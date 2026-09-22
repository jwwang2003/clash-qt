#include "integrations/component/marshal/backend_marshal.h"

#include <QTimeZone>

#include "core/component/abi/wire.h"

namespace clashqt::integration::marshal {
namespace {

namespace abi = ::clashqt::com::abi;

// A QDateTime that is not valid is written as this, and read back as an
// invalid QDateTime rather than as a moment in time. Shared with the packed
// connection snapshot, which has the same problem for the same field.
constexpr std::int64_t kNoTimestamp = abi::kNoTimestamp;

template <typename Enum>
Enum readEnum8(ByteReader &in) {
    return static_cast<Enum>(in.u8());
}

}  // namespace

std::uint8_t timeRepresentationOf(const QDateTime &value) noexcept {
    switch (value.timeSpec()) {
        case Qt::UTC:
            return abi::kTimeUtc;
        case Qt::OffsetFromUTC:
            return abi::kTimeOffsetFromUtc;
        case Qt::TimeZone:
            return abi::kTimeNamedZone;
        case Qt::LocalTime:
        default:
            return abi::kTimeLocal;
    }
}

QString timeZoneIdOf(const QDateTime &value) {
    // Only a NAMED zone needs its identifier; asking a local-time value for its
    // zone would answer the machine's current one and silently turn "local"
    // into "Europe/Berlin" on the far side, which is the opposite of preserving
    // the representation.
    if (value.timeSpec() != Qt::TimeZone) {
        return {};
    }
    return QString::fromUtf8(value.timeRepresentation().id());
}

QDateTime dateTimeFromParts(std::int64_t ms, std::uint8_t representation,
                            std::int32_t offsetSeconds, const QString &zoneId, bool *ok) {
    const auto refuse = [&]() -> QDateTime {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    };
    if (ok != nullptr) {
        *ok = true;
    }
    if (ms == kNoTimestamp) {
        return {};  // invalid stays invalid, and that is not a decode failure
    }
    if (representation > abi::kTimeRepresentationMax) {
        return refuse();
    }
    switch (representation) {
        case abi::kTimeUtc:
            return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
        case abi::kTimeOffsetFromUtc: {
            if (offsetSeconds < -abi::kMaxUtcOffsetSeconds ||
                offsetSeconds > abi::kMaxUtcOffsetSeconds) {
                return refuse();
            }
            return QDateTime::fromMSecsSinceEpoch(
                ms, QTimeZone::fromSecondsAheadOfUtc(offsetSeconds));
        }
        case abi::kTimeNamedZone: {
            if (zoneId.isEmpty()) {
                return refuse();
            }
            const QTimeZone zone(zoneId.toUtf8());
            if (!zone.isValid()) {
                // A zone this build's database does not know is refused rather
                // than degraded to local time: degrading is exactly the silent
                // representation change this encoding exists to stop.
                return refuse();
            }
            return QDateTime::fromMSecsSinceEpoch(ms, zone);
        }
        case abi::kTimeLocal:
        default:
            return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::LocalTime);
    }
}

void writeDateTime(ByteWriter &out, const QDateTime &value) {
    out.i64(value.isValid() ? value.toMSecsSinceEpoch() : kNoTimestamp);
    out.u8(timeRepresentationOf(value));
    out.i32(value.isValid() ? value.offsetFromUtc() : 0);
    out.text(timeZoneIdOf(value));
}

QDateTime readDateTime(ByteReader &in) {
    const std::int64_t ms = in.i64();
    const std::uint8_t representation = in.u8();
    const std::int32_t offsetSeconds = in.i32();
    const QString zoneId = in.text();
    if (!in.ok()) {
        return {};
    }
    bool decoded = false;
    const QDateTime value = dateTimeFromParts(ms, representation, offsetSeconds, zoneId, &decoded);
    if (!decoded) {
        // Latched on the reader, so the command that contains this field is
        // refused with kInvalidArgument rather than delivering a timestamp the
        // producer did not send.
        in.fail();
        return {};
    }
    return value;
}

void writeEndpoint(ByteWriter &out, const cb::Endpoint &endpoint) {
    out.text(endpoint.host);
    out.u16(endpoint.port);
    out.text(endpoint.secret);
}

cb::Endpoint readEndpoint(ByteReader &in) {
    cb::Endpoint endpoint;
    endpoint.host = in.text();
    endpoint.port = in.u16();
    endpoint.secret = in.text();
    return endpoint;
}

void writeErrorInfo(ByteWriter &out, const cb::ErrorInfo &error) {
    out.i32(static_cast<std::int32_t>(error.code));
    out.text(error.message);
}

cb::ErrorInfo readErrorInfo(ByteReader &in) {
    cb::ErrorInfo error;
    error.code = static_cast<cb::ErrorCode>(in.i32());
    error.message = in.text();
    return error;
}

void writeCompletion(ByteWriter &out, const cb::Completion &completion) {
    writeRequestId(out, completion.request);
    writeGeneration(out, completion.generation);
    out.u8(static_cast<std::uint8_t>(completion.status));
    writeErrorInfo(out, completion.error);
}

cb::Completion readCompletion(ByteReader &in) {
    cb::Completion completion;
    completion.request = readRequestId(in);
    completion.generation = readGeneration(in);
    completion.status = readEnum8<cb::CompletionStatus>(in);
    completion.error = readErrorInfo(in);
    return completion;
}

void writeStopCompleted(ByteWriter &out, const cb::StopCompleted &result) {
    writeRequestId(out, result.request);
    writeGeneration(out, result.generation);
    out.u8(static_cast<std::uint8_t>(result.status));
    out.boolean(result.confirmed);
    writeErrorInfo(out, result.reason);
}

cb::StopCompleted readStopCompleted(ByteReader &in) {
    cb::StopCompleted result;
    result.request = readRequestId(in);
    result.generation = readGeneration(in);
    result.status = readEnum8<cb::CompletionStatus>(in);
    result.confirmed = in.boolean();
    result.reason = readErrorInfo(in);
    return result;
}

void writeTunChangeCompleted(ByteWriter &out, const cb::TunChangeCompleted &result) {
    writeRequestId(out, result.request);
    writeGeneration(out, result.generation);
    out.u8(static_cast<std::uint8_t>(result.status));
    out.boolean(result.requested);
    out.boolean(result.actual);
    writeErrorInfo(out, result.error);
}

cb::TunChangeCompleted readTunChangeCompleted(ByteReader &in) {
    cb::TunChangeCompleted result;
    result.request = readRequestId(in);
    result.generation = readGeneration(in);
    result.status = readEnum8<cb::CompletionStatus>(in);
    result.requested = in.boolean();
    result.actual = in.boolean();
    result.error = readErrorInfo(in);
    return result;
}

void writeBaseConfig(ByteWriter &out, const cb::BaseConfig &config) {
    out.text(config.mode);
    out.text(config.logLevel);
    out.u16(config.mixedPort);
    out.u16(config.httpPort);
    out.u16(config.socksPort);
    out.u16(config.redirPort);
    out.u16(config.tproxyPort);
    out.boolean(config.allowLan);
    out.boolean(config.ipv6);
    out.boolean(config.tunEnabled);
}

cb::BaseConfig readBaseConfig(ByteReader &in) {
    cb::BaseConfig config;
    config.mode = in.text();
    config.logLevel = in.text();
    config.mixedPort = in.u16();
    config.httpPort = in.u16();
    config.socksPort = in.u16();
    config.redirPort = in.u16();
    config.tproxyPort = in.u16();
    config.allowLan = in.boolean();
    config.ipv6 = in.boolean();
    config.tunEnabled = in.boolean();
    return config;
}

void writeProxyGroup(ByteWriter &out, const cb::ProxyGroup &group) {
    out.text(group.name);
    out.text(group.type);
    out.text(group.now);
    out.textList(group.all);
    out.text(group.fixed);
}

cb::ProxyGroup readProxyGroup(ByteReader &in) {
    cb::ProxyGroup group;
    group.name = in.text();
    group.type = in.text();
    group.now = in.text();
    group.all = in.textList();
    group.fixed = in.text();
    return group;
}

void writeProxyNode(ByteWriter &out, const cb::ProxyNode &node) {
    out.text(node.name);
    out.text(node.type);
    // -1 is "untested" and 0 is "timeout": two distinct values a narrowing or
    // an unsigned encoding would confuse.
    out.i32(node.delay);
}

cb::ProxyNode readProxyNode(ByteReader &in) {
    cb::ProxyNode node;
    node.name = in.text();
    node.type = in.text();
    node.delay = in.i32();
    return node;
}

void writeRule(ByteWriter &out, const cb::Rule &rule) {
    out.text(rule.type);
    out.text(rule.payload);
    out.text(rule.proxy);
}

cb::Rule readRule(ByteReader &in) {
    cb::Rule rule;
    rule.type = in.text();
    rule.payload = in.text();
    rule.proxy = in.text();
    return rule;
}

void writeProvider(ByteWriter &out, const cb::Provider &provider) {
    out.text(provider.name);
    out.text(provider.type);
    out.text(provider.vehicle);
    out.text(provider.behavior);
    out.i32(provider.count);
    writeDateTime(out, provider.updated);
    out.u64(provider.used);
    out.u64(provider.total);
    writeDateTime(out, provider.expires);
}

cb::Provider readProvider(ByteReader &in) {
    cb::Provider provider;
    provider.name = in.text();
    provider.type = in.text();
    provider.vehicle = in.text();
    provider.behavior = in.text();
    provider.count = in.i32();
    provider.updated = readDateTime(in);
    provider.used = in.u64();
    provider.total = in.u64();
    provider.expires = readDateTime(in);
    return provider;
}

void writeLogEntry(ByteWriter &out, const cb::LogEntry &entry) {
    out.text(entry.level);
    out.text(entry.payload);
    writeDateTime(out, entry.time);
}

cb::LogEntry readLogEntry(ByteReader &in) {
    cb::LogEntry entry;
    entry.level = in.text();
    entry.payload = in.text();
    entry.time = readDateTime(in);
    return entry;
}

void writeConnection(ByteWriter &out, const cb::Connection &connection) {
    out.text(connection.id);
    out.text(connection.host);
    out.text(connection.network);
    out.text(connection.connectionType);
    out.textList(connection.chains);
    out.text(connection.rule);
    out.text(connection.rulePayload);
    out.text(connection.sourceIp);
    out.text(connection.sourcePort);
    out.text(connection.destinationIp);
    out.text(connection.destinationPort);
    out.text(connection.process);
    out.text(connection.processPath);
    out.u64(connection.upload);
    out.u64(connection.download);
    out.f64(connection.uploadRate);
    out.f64(connection.downloadRate);
    writeDateTime(out, connection.start);
    writeDateTime(out, connection.end);
}

cb::Connection readConnection(ByteReader &in) {
    cb::Connection connection;
    connection.id = in.text();
    connection.host = in.text();
    connection.network = in.text();
    connection.connectionType = in.text();
    connection.chains = in.textList();
    connection.rule = in.text();
    connection.rulePayload = in.text();
    connection.sourceIp = in.text();
    connection.sourcePort = in.text();
    connection.destinationIp = in.text();
    connection.destinationPort = in.text();
    connection.process = in.text();
    connection.processPath = in.text();
    connection.upload = in.u64();
    connection.download = in.u64();
    connection.uploadRate = in.f64();
    connection.downloadRate = in.f64();
    connection.start = readDateTime(in);
    connection.end = readDateTime(in);
    return connection;
}

void writeIdentity(ByteWriter &out, const cb::BackendIdentity &identity) {
    out.text(identity.name);
    out.u32(identity.moduleAbiVersion);
    out.u32(identity.interfaceRevision);
}

cb::BackendIdentity readIdentity(ByteReader &in) {
    cb::BackendIdentity identity;
    identity.name = in.text();
    identity.moduleAbiVersion = in.u32();
    identity.interfaceRevision = in.u32();
    return identity;
}

void writeTimings(ByteWriter &out, const cb::BackendTimings &timings) {
    out.u32(timings.idleDeadlineMs);
    out.u32(timings.serviceIdleDeadlineMs);
    out.u32(timings.hardCapMs);
    out.u32(timings.probeIntervalMs);
    out.u32(timings.probeTimeoutMs);
    out.u32(timings.terminateWaitMs);
}

cb::BackendTimings readTimings(ByteReader &in) {
    cb::BackendTimings timings;
    timings.idleDeadlineMs = in.u32();
    timings.serviceIdleDeadlineMs = in.u32();
    timings.hardCapMs = in.u32();
    timings.probeIntervalMs = in.u32();
    timings.probeTimeoutMs = in.u32();
    timings.terminateWaitMs = in.u32();
    return timings;
}

void writePrivilegedServiceStatus(ByteWriter &out, const cb::PrivilegedServiceStatus &status) {
    out.u8(static_cast<std::uint8_t>(status.state));
    out.text(status.version);
    // The field whose absence once left the helper's uninstall guard inert -
    // read in three places and written by nothing. It crosses the boundary like
    // any other value, and the ABI suite asserts that it does.
    out.boolean(status.coreRunning);
    writeErrorInfo(out, status.error);
}

cb::PrivilegedServiceStatus readPrivilegedServiceStatus(ByteReader &in) {
    cb::PrivilegedServiceStatus status;
    status.state = readEnum8<cb::ServiceState>(in);
    status.version = in.text();
    status.coreRunning = in.boolean();
    status.error = readErrorInfo(in);
    return status;
}

}  // namespace clashqt::integration::marshal
