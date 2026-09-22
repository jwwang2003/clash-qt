#include "integrations/component/marshal/connection_snapshot.h"

#include <cstring>

#include <QByteArray>
#include <QDateTime>
#include <QHash>

#include "core/component/abi/wire.h"

namespace clashqt::integration::marshal {
namespace {

namespace abi = ::clashqt::com::abi;

// The validator walks the record's 12 TextRefs as an array, which is only
// legitimate because they are declared consecutively with no padding between
// them. Checked here rather than assumed, so reordering the record in wire.h
// breaks the build instead of the bounds check.
static_assert(offsetof(abi::ConnectionRecord, processPath) ==
                  offsetof(abi::ConnectionRecord, id) + 11 * sizeof(abi::TextRef),
              "the 12 TextRef fields must stay consecutive and unpadded");
static_assert(offsetof(abi::ConnectionRecord, chainFirst) ==
                  offsetof(abi::ConnectionRecord, id) + 12 * sizeof(abi::TextRef),
              "the TextRef run ends where chainFirst begins");

// A blob big enough to need a 64-bit offset is not a telemetry snapshot, it is
// a bug. The cap is checked on the way in as well as on the way out.
constexpr std::size_t kMaxBlobBytes = 256u * 1024u * 1024u;

/// Appends UTF-8 to the blob, reusing an identical run when one is already
/// there. Interning is what makes "tcp" cost 3 bytes per snapshot instead of 3
/// bytes per row.
class BlobBuilder {
  public:
    explicit BlobBuilder(std::size_t reserveBytes) { blob_.reserve(reserveBytes); }

    abi::TextRef add(const QString &text) {
        if (text.isEmpty()) {
            return abi::TextRef{0, 0};
        }
        const auto known = interned_.constFind(text);
        if (known != interned_.constEnd()) {
            return *known;
        }
        const QByteArray utf8 = text.toUtf8();
        const abi::TextRef ref{static_cast<std::uint32_t>(blob_.size()),
                               static_cast<std::uint32_t>(utf8.size())};
        blob_.insert(blob_.end(), utf8.constBegin(), utf8.constEnd());
        interned_.insert(text, ref);
        return ref;
    }

    const std::vector<std::uint8_t> &bytes() const noexcept { return blob_; }

  private:
    std::vector<std::uint8_t> blob_;
    QHash<QString, abi::TextRef> interned_;
};

bool refWithinBlob(const abi::TextRef &ref, std::uint32_t blobSize) noexcept {
    // Written as a subtraction so a length near UINT32_MAX cannot wrap the sum.
    return ref.offset <= blobSize && ref.length <= blobSize - ref.offset;
}

QString textOf(const abi::TextRef &ref, const std::uint8_t *blob) {
    if (ref.length == 0) {
        return {};
    }
    return QString::fromUtf8(reinterpret_cast<const char *>(blob + ref.offset),
                             static_cast<qsizetype>(ref.length));
}

QDateTime timeOf(std::int64_t ms) {
    return ms == abi::kNoTimestamp ? QDateTime() : QDateTime::fromMSecsSinceEpoch(ms);
}

std::int64_t msOf(const QDateTime &value) {
    return value.isValid() ? value.toMSecsSinceEpoch() : abi::kNoTimestamp;
}

bool fail(QString *reason, const char *text) {
    if (reason != nullptr) {
        *reason = QString::fromLatin1(text);
    }
    return false;
}

}  // namespace

std::vector<std::uint8_t> packConnectionSnapshot(cb::Generation generation,
                                                 cb::Span<cb::Connection> connections,
                                                 quint64 uploadTotal, quint64 downloadTotal) {
    const std::size_t count = connections.size();

    std::size_t chainSlots = 0;
    for (std::size_t index = 0; index < count; ++index) {
        chainSlots += static_cast<std::size_t>(connections[index].chains.size());
    }

    // One estimate, one reserve. 64 bytes of text per row is a guess; being
    // wrong costs one reallocation, not one per field.
    BlobBuilder blob(count * 64);
    std::vector<abi::ConnectionRecord> records;
    records.reserve(count);
    std::vector<abi::TextRef> chains;
    chains.reserve(chainSlots);

    for (std::size_t index = 0; index < count; ++index) {
        const cb::Connection &connection = connections[index];
        abi::ConnectionRecord record{};
        record.id = blob.add(connection.id);
        record.host = blob.add(connection.host);
        record.network = blob.add(connection.network);
        record.connectionType = blob.add(connection.connectionType);
        record.rule = blob.add(connection.rule);
        record.rulePayload = blob.add(connection.rulePayload);
        record.sourceIp = blob.add(connection.sourceIp);
        record.sourcePort = blob.add(connection.sourcePort);
        record.destinationIp = blob.add(connection.destinationIp);
        record.destinationPort = blob.add(connection.destinationPort);
        record.process = blob.add(connection.process);
        record.processPath = blob.add(connection.processPath);
        record.chainFirst = static_cast<std::uint32_t>(chains.size());
        record.chainCount = static_cast<std::uint32_t>(connection.chains.size());
        for (const QString &hop : connection.chains) {
            chains.push_back(blob.add(hop));
        }
        record.upload = connection.upload;
        record.download = connection.download;
        record.uploadRate = connection.uploadRate;
        record.downloadRate = connection.downloadRate;
        record.startMs = msOf(connection.start);
        record.endMs = msOf(connection.end);
        records.push_back(record);
    }

    const std::size_t recordOffset = sizeof(abi::ConnectionSnapshotHeader);
    const std::size_t recordBytes = records.size() * sizeof(abi::ConnectionRecord);
    const std::size_t chainOffset = recordOffset + recordBytes;
    const std::size_t chainBytes = chains.size() * sizeof(abi::TextRef);
    const std::size_t blobOffset = chainOffset + chainBytes;
    const std::size_t total = blobOffset + blob.bytes().size();

    abi::ConnectionSnapshotHeader header{};
    header.magic = abi::kConnectionSnapshotMagic;
    header.version = abi::kConnectionSnapshotVersion;
    header.recordSize = static_cast<std::uint16_t>(sizeof(abi::ConnectionRecord));
    header.count = static_cast<std::uint32_t>(records.size());
    header.chainSlots = static_cast<std::uint32_t>(chains.size());
    header.recordOffset = static_cast<std::uint32_t>(recordOffset);
    header.chainOffset = static_cast<std::uint32_t>(chainOffset);
    header.blobOffset = static_cast<std::uint32_t>(blobOffset);
    header.blobSize = static_cast<std::uint32_t>(blob.bytes().size());
    header.generation = cb::number(generation);
    header.uploadTotal = uploadTotal;
    header.downloadTotal = downloadTotal;

    std::vector<std::uint8_t> packed(total);
    std::memcpy(packed.data(), &header, sizeof(header));
    if (recordBytes != 0) {
        std::memcpy(packed.data() + recordOffset, records.data(), recordBytes);
    }
    if (chainBytes != 0) {
        std::memcpy(packed.data() + chainOffset, chains.data(), chainBytes);
    }
    if (!blob.bytes().empty()) {
        std::memcpy(packed.data() + blobOffset, blob.bytes().data(), blob.bytes().size());
    }
    return packed;
}

bool unpackConnectionSnapshot(const void *data, std::size_t size, ConnectionSnapshot *out,
                              QString *reason) {
    if (out == nullptr) {
        return fail(reason, "null output");
    }
    if (data == nullptr || size < sizeof(abi::ConnectionSnapshotHeader)) {
        return fail(reason, "shorter than the header");
    }
    const auto *bytes = static_cast<const std::uint8_t *>(data);

    abi::ConnectionSnapshotHeader header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != abi::kConnectionSnapshotMagic) {
        return fail(reason, "wrong magic");
    }
    if (header.version != abi::kConnectionSnapshotVersion) {
        return fail(reason, "unknown snapshot version");
    }
    // A producer with wider records is a newer producer; reading its records
    // with this build's stride would silently shear every field.
    if (header.recordSize != sizeof(abi::ConnectionRecord)) {
        return fail(reason, "record size does not match this build");
    }
    if (header.blobSize > kMaxBlobBytes) {
        return fail(reason, "blob larger than any real snapshot");
    }

    // Section bounds. Every arithmetic step is done in std::size_t on values
    // already known to be 32-bit, so nothing can wrap.
    const std::size_t recordBytes =
        static_cast<std::size_t>(header.count) * sizeof(abi::ConnectionRecord);
    const std::size_t chainBytes =
        static_cast<std::size_t>(header.chainSlots) * sizeof(abi::TextRef);
    if (header.recordOffset != sizeof(abi::ConnectionSnapshotHeader)) {
        return fail(reason, "records do not follow the header");
    }
    if (static_cast<std::size_t>(header.recordOffset) + recordBytes != header.chainOffset) {
        return fail(reason, "chain section does not follow the records");
    }
    if (static_cast<std::size_t>(header.chainOffset) + chainBytes != header.blobOffset) {
        return fail(reason, "blob does not follow the chain section");
    }
    if (static_cast<std::size_t>(header.blobOffset) + header.blobSize != size) {
        return fail(reason, "buffer length does not match the sections");
    }

    const auto *records = reinterpret_cast<const abi::ConnectionRecord *>(bytes + header.recordOffset);
    const auto *chains = reinterpret_cast<const abi::TextRef *>(bytes + header.chainOffset);
    const std::uint8_t *blob = bytes + header.blobOffset;

    // Validate EVERYTHING before constructing anything: a half-built snapshot
    // delivered alongside an error is worse than no snapshot.
    for (std::uint32_t index = 0; index < header.chainSlots; ++index) {
        if (!refWithinBlob(chains[index], header.blobSize)) {
            return fail(reason, "a chain entry points outside the blob");
        }
    }
    for (std::uint32_t index = 0; index < header.count; ++index) {
        const abi::ConnectionRecord &record = records[index];
        const abi::TextRef *fields = &record.id;
        for (std::size_t field = 0; field < 12; ++field) {
            if (!refWithinBlob(fields[field], header.blobSize)) {
                return fail(reason, "a record field points outside the blob");
            }
        }
        if (record.chainFirst > header.chainSlots ||
            record.chainCount > header.chainSlots - record.chainFirst) {
            return fail(reason, "a chain run is outside the chain section");
        }
    }

    ConnectionSnapshot snapshot;
    snapshot.generation = static_cast<cb::Generation>(header.generation);
    snapshot.uploadTotal = header.uploadTotal;
    snapshot.downloadTotal = header.downloadTotal;
    snapshot.connections.reserve(static_cast<qsizetype>(header.count));
    for (std::uint32_t index = 0; index < header.count; ++index) {
        const abi::ConnectionRecord &record = records[index];
        cb::Connection connection;
        connection.id = textOf(record.id, blob);
        connection.host = textOf(record.host, blob);
        connection.network = textOf(record.network, blob);
        connection.connectionType = textOf(record.connectionType, blob);
        connection.rule = textOf(record.rule, blob);
        connection.rulePayload = textOf(record.rulePayload, blob);
        connection.sourceIp = textOf(record.sourceIp, blob);
        connection.sourcePort = textOf(record.sourcePort, blob);
        connection.destinationIp = textOf(record.destinationIp, blob);
        connection.destinationPort = textOf(record.destinationPort, blob);
        connection.process = textOf(record.process, blob);
        connection.processPath = textOf(record.processPath, blob);
        connection.chains.reserve(static_cast<qsizetype>(record.chainCount));
        for (std::uint32_t hop = 0; hop < record.chainCount; ++hop) {
            connection.chains.append(textOf(chains[record.chainFirst + hop], blob));
        }
        connection.upload = record.upload;
        connection.download = record.download;
        connection.uploadRate = record.uploadRate;
        connection.downloadRate = record.downloadRate;
        connection.start = timeOf(record.startMs);
        connection.end = timeOf(record.endMs);
        snapshot.connections.append(connection);
    }

    *out = std::move(snapshot);
    return true;
}

}  // namespace clashqt::integration::marshal
