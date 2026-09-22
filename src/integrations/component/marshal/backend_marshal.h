#pragma once

// Every value type of backend-r4, as bytes. Private, compiled on both sides of
// the boundary.
//
// LOSSLESSNESS IS THE WHOLE REQUIREMENT. module-r1: "all operations/events of
// backend-r4 must round-trip losslessly including 64-bit IDs/counters". So
// RequestId and Generation are u64 and never narrowed; rates are IEEE-754 bit
// patterns and never re-parsed from text; a QDateTime keeps its invalidity as a
// distinguished value rather than collapsing to the epoch, because
// Connection::end is invalid for every LIVE connection and an epoch timestamp
// there would render as a connection that closed in 1970.

#include <cstdint>

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include "core/backend/backend.h"
#include "integrations/component/marshal/codec.h"

namespace clashqt::integration::marshal {

namespace cb = ::core::backend;

// ---------------------------------------------------------------- scalars

inline void writeRequestId(ByteWriter &out, cb::RequestId id) { out.u64(cb::number(id)); }
inline cb::RequestId readRequestId(ByteReader &in) {
    return static_cast<cb::RequestId>(in.u64());
}

inline void writeGeneration(ByteWriter &out, cb::Generation generation) {
    out.u64(cb::number(generation));
}
inline cb::Generation readGeneration(ByteReader &in) {
    return static_cast<cb::Generation>(in.u64());
}

/// A timestamp travels as its INSTANT PLUS ITS REPRESENTATION - epoch
/// milliseconds, wire.h's TimeRepresentation, the offset from UTC in seconds
/// and, for a named zone, its IANA identifier. An invalid QDateTime is a
/// distinguished value, not the epoch, and stays invalid.
///
/// WHY THE REPRESENTATION AND NOT JUST THE INSTANT. QDateTime::operator==
/// compares instants, so a codec that dropped the zone would pass every
/// equality test and still change what the connections page prints with
/// toString(Qt::ISODate). A malformed representation - an unknown spec, an
/// out-of-range offset, a zone identifier this build does not know - fails the
/// reader, so the command carrying it is refused rather than silently retimed.
void writeDateTime(ByteWriter &out, const QDateTime &value);
QDateTime readDateTime(ByteReader &in);

/// The three representation pieces, shared with the packed connection snapshot
/// so the two encodings cannot drift in what they preserve.
std::uint8_t timeRepresentationOf(const QDateTime &value) noexcept;
/// Empty unless the value's representation is a NAMED zone.
QString timeZoneIdOf(const QDateTime &value);
/// Rebuilds a QDateTime from the parts, validating the enum, the offset bound
/// and the zone identifier. `ok` is false when the parts are not decodable;
/// `kNoTimestamp` yields an invalid QDateTime with `ok` true, because an
/// invalid timestamp is a value and not a malformed packet.
QDateTime dateTimeFromParts(std::int64_t ms, std::uint8_t representation,
                            std::int32_t offsetSeconds, const QString &zoneId, bool *ok);

// ------------------------------------------------------------ value types

void writeEndpoint(ByteWriter &out, const cb::Endpoint &endpoint);
cb::Endpoint readEndpoint(ByteReader &in);

void writeErrorInfo(ByteWriter &out, const cb::ErrorInfo &error);
cb::ErrorInfo readErrorInfo(ByteReader &in);

void writeCompletion(ByteWriter &out, const cb::Completion &completion);
cb::Completion readCompletion(ByteReader &in);

void writeStopCompleted(ByteWriter &out, const cb::StopCompleted &result);
cb::StopCompleted readStopCompleted(ByteReader &in);

void writeTunChangeCompleted(ByteWriter &out, const cb::TunChangeCompleted &result);
cb::TunChangeCompleted readTunChangeCompleted(ByteReader &in);

void writeBaseConfig(ByteWriter &out, const cb::BaseConfig &config);
cb::BaseConfig readBaseConfig(ByteReader &in);

void writeProxyGroup(ByteWriter &out, const cb::ProxyGroup &group);
cb::ProxyGroup readProxyGroup(ByteReader &in);

void writeProxyNode(ByteWriter &out, const cb::ProxyNode &node);
cb::ProxyNode readProxyNode(ByteReader &in);

void writeRule(ByteWriter &out, const cb::Rule &rule);
cb::Rule readRule(ByteReader &in);

void writeProvider(ByteWriter &out, const cb::Provider &provider);
cb::Provider readProvider(ByteReader &in);

void writeLogEntry(ByteWriter &out, const cb::LogEntry &entry);

/// Field-by-field, for the general encoding. Telemetry snapshots do NOT use
/// this - they use the packed layout in connection_snapshot.h, which is what
/// keeps a hundreds-of-entries snapshot off the per-string allocation path.
/// This exists so a single connection can be described in an ordinary argument
/// block.
void writeConnection(ByteWriter &out, const cb::Connection &connection);
cb::LogEntry readLogEntry(ByteReader &in);
cb::Connection readConnection(ByteReader &in);

void writeIdentity(ByteWriter &out, const cb::BackendIdentity &identity);
cb::BackendIdentity readIdentity(ByteReader &in);

void writeTimings(ByteWriter &out, const cb::BackendTimings &timings);
cb::BackendTimings readTimings(ByteReader &in);

void writePrivilegedServiceStatus(ByteWriter &out, const cb::PrivilegedServiceStatus &status);
cb::PrivilegedServiceStatus readPrivilegedServiceStatus(ByteReader &in);

// ------------------------------------------------------------- collections
//
// A Span is BORROWED for the duration of the callback that carries it
// (backend-r4 section 9), so encoding copies eagerly: by the time the bytes
// reach the other side the producer's storage may be gone.

template <typename T, typename WriteOne>
void writeSpan(ByteWriter &out, cb::Span<T> span, WriteOne writeOne) {
    out.u32(static_cast<std::uint32_t>(span.size()));
    for (std::size_t index = 0; index < span.size(); ++index) {
        writeOne(out, span[index]);
    }
}

/// `minimumBytesEach` is what stops a corrupted count from being believed.
template <typename T, typename ReadOne>
QVector<T> readVector(ByteReader &in, ReadOne readOne, std::size_t minimumBytesEach) {
    const std::uint32_t size = in.count(minimumBytesEach);
    QVector<T> values;
    values.reserve(static_cast<qsizetype>(size));
    for (std::uint32_t index = 0; index < size && in.ok(); ++index) {
        values.append(readOne(in));
    }
    return in.ok() ? values : QVector<T>{};
}

/// The smallest possible encoding of each record, for the count guard above.
/// A string costs at least its 4-byte length, so these are counts of fields.
///
/// A timestamp costs its i64 instant PLUS its representation: the u8 spec, the
/// i32 offset and at least the 4-byte length of an empty zone identifier.
inline constexpr std::size_t kTimestampRepresentationBytes = 1 + 4 + 4;
inline constexpr std::size_t kMinProxyGroupBytes = 4 * 5;
inline constexpr std::size_t kMinProxyNodeBytes = 4 * 2 + 4;
inline constexpr std::size_t kMinRuleBytes = 4 * 3;
inline constexpr std::size_t kMinProviderBytes =
    4 * 4 + 4 + 8 + 8 + 8 + 8 + 2 * kTimestampRepresentationBytes;
inline constexpr std::size_t kMinTextBytes = 4;
inline constexpr std::size_t kMinConnectionBytes =
    4 * 13 + 8 * 6 + 2 * kTimestampRepresentationBytes;

}  // namespace clashqt::integration::marshal
