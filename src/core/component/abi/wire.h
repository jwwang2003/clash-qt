#ifndef CLASHQT_CORE_COMPONENT_ABI_WIRE_H
#define CLASHQT_CORE_COMPONENT_ABI_WIRE_H

// The versioned command/event protocol carried over the two fixed vtables in
// backend_abi.h, and the fixed record layouts telemetry uses.
// Contract: .refactor/P4_ABI_CONTRACT.md revision module-r1.
//
// WHY A COMMAND PROTOCOL RATHER THAN SIXTY VTABLE SLOTS
//   module-r1 permits it, and backend-r4 has 61 operations and 25 events. Sixty
//   slots would freeze the ORDER of every one of them: adding an operation in
//   backend-r5 would mean a new interface id and a migration, for a change that
//   is additive in the C++ contract. A command code is data, so an unknown code
//   is answered kNotImplemented by an older peer instead of being a call into
//   the wrong slot. The vtables stay three methods wide and immutable, which is
//   what the id promises.
//
// ENCODING RULES - all of them, because a decoder that guesses is a crash
//   * Little-endian, fixed width. No native `long`, no `size_t`, no bitfields,
//     no padding assumptions: every field is written and read one at a time.
//   * A string is u32 byteLength followed by that many UTF-8 bytes. Never
//     NUL-terminated, never assumed valid UTF-8 by the reader.
//   * A list is u32 count followed by count encoded elements. A decoder that
//     reads past the end of its input fails; it does not clamp and continue.
//   * A timestamp is an INSTANT PLUS ITS REPRESENTATION: i64 milliseconds since
//     the epoch (kNoTimestamp for an invalid one), u8 TimeRepresentation, i32
//     offset-from-UTC seconds, and the IANA zone identifier as a string. See
//     "TIMESTAMPS CARRY THEIR REPRESENTATION" below for why the instant alone
//     is not enough.
//   * Every decode failure is reported to the caller as kInvalidArgument with a
//     diagnostic naming the command or event - a malformed packet is a bug in
//     the peer, and silence is how it survives to the next release.
//
// The connection snapshot does NOT use this encoding: it is one packed buffer
// with fixed-layout records indexing a shared UTF-8 blob (decision D8's second
// binding constraint). Its layout is at the bottom of this file.
//
// THE EVENT ENVELOPE
//   Every payload passed to IBackendHost::Notify begins with ONE fixed-width
//   u64: the sequence the module's wrapped backend assigned to the event when
//   it PRODUCED it. Everything after those eight bytes is the event's own
//   payload in the rules above - including the packed connection snapshot,
//   whose magic therefore sits at offset 8, not 0.
//
//   It is there because backend-r4 admits an observer by PRODUCTION, not by
//   arrival: an observer added after an event was produced does not receive it.
//   In process that costs nothing, because the producing queue and the observer
//   list are the same object. Across the boundary they are not - the module
//   produces and queues, the host receives later - so a host that stamped
//   events on arrival would hand a newly registered observer events produced
//   before it existed. The sequence travels so the host applies the SAME rule,
//   and kCmdProducedSequence is how the host learns the number to compare
//   against at registration time.
//
// TIMESTAMPS CARRY THEIR REPRESENTATION, NOT JUST THEIR INSTANT
//   A QDateTime is an instant AND a time representation, and the host renders
//   the second one: the connections page prints start/end with
//   toString(Qt::ISODate), so a value that arrives as local time where it left
//   as UTC is a visible text change even though operator== - which compares
//   instants - still succeeds. Moving a backend behind this boundary must not
//   change a single character the user reads, so every timestamp on this wire
//   carries the epoch milliseconds, a TimeRepresentation, the offset from UTC
//   in seconds, and the IANA zone identifier when the representation is a named
//   zone. A decoder rebuilds the same representation or refuses the packet.

#include <cstddef>
#include <cstdint>

namespace clashqt::com::abi {

// The revision of everything in this file. Bumped when a code's MEANING or a
// payload's layout changes; adding a new code at the end does not bump it,
// which is the entire point of the unknown-code rule.
//
// 2: every Notify payload gained the production-sequence envelope above.
// 3: every timestamp - in the general encoding AND in the packed connection
//    snapshot - gained its time representation, so the host-visible rendering
//    of a QDateTime survives the boundary. The packed record grew with it, so
//    ConnectionSnapshotHeader::version moved to 2 at the same time.
inline constexpr std::uint32_t kWireRevision = 3;

// The size of the event envelope, in bytes, before the event's own payload.
inline constexpr std::size_t kEventEnvelopeBytes = 8;

// backend-r4's BackendIdentity::interfaceRevision. Not the wire revision and
// not the module ABI version: it moves only when the C++ contract's vtable
// does, which backend-r4 records as still 1 across r1-r4.
inline constexpr std::uint32_t kBackendInterfaceRevision = 1;

// --------------------------------------------------------------- commands
//
// Host -> module, through IBackendSession::Invoke. One code per published
// operation of backend-r4, plus the reverse-seam callbacks the host delivers
// into the module's privileged-service adapter.

enum Command : std::uint32_t {
    // ---- BackendLifecycle
    kCmdDiscoverBinary = 0x0100,
    kCmdSetBinaryPath = 0x0101,
    kCmdBinaryPath = 0x0102,
    kCmdSetExecutionMode = 0x0103,
    kCmdExecutionMode = 0x0104,
    kCmdUsesPrivilegedService = 0x0105,
    kCmdStart = 0x0106,
    kCmdStop = 0x0107,
    kCmdState = 0x0108,
    kCmdOwnership = 0x0109,
    kCmdManagedEndpoint = 0x010A,
    kCmdActiveConfigPaths = 0x010B,
    kCmdIsRestartPending = 0x010C,
    kCmdIsManagedCoreActive = 0x010D,

    // ---- BackendAttachment
    kCmdDiscoverEndpoint = 0x0200,
    kCmdEndpointFromConfigFile = 0x0201,
    kCmdAttach = 0x0202,
    kCmdDetach = 0x0203,
    kCmdCurrentEndpoint = 0x0204,
    kCmdIsAttached = 0x0205,
    kCmdIsConnected = 0x0206,
    kCmdAttachmentOwnership = 0x0207,
    kCmdIsExternalControllerConnected = 0x0208,
    kCmdRefreshConfig = 0x0209,

    // ---- BackendControl
    kCmdSetMode = 0x0300,
    kCmdSetTunEnabled = 0x0301,
    kCmdIsTunChangePending = 0x0302,
    kCmdSelectNode = 0x0303,
    kCmdResetGroupSelection = 0x0304,
    kCmdTestGroupDelay = 0x0305,
    kCmdTestNodeDelay = 0x0306,
    kCmdCloseConnection = 0x0307,
    kCmdCloseAllConnections = 0x0308,
    kCmdUpdateGeoDatabases = 0x0309,
    kCmdQueryDns = 0x030A,
    kCmdFlushDnsCache = 0x030B,

    // ---- BackendTelemetry
    kCmdRefreshVersion = 0x0400,
    kCmdRefreshProxies = 0x0401,
    kCmdRefreshRules = 0x0402,
    kCmdOpenTrafficStream = 0x0403,
    kCmdCloseTrafficStream = 0x0404,
    kCmdOpenConnectionsStream = 0x0405,
    kCmdCloseConnectionsStream = 0x0406,
    kCmdOpenLogStream = 0x0407,
    kCmdCloseLogStream = 0x0408,
    kCmdOpenMemoryStream = 0x0409,
    kCmdCloseMemoryStream = 0x040A,
    kCmdFetchProviders = 0x040B,
    kCmdUpdateProvider = 0x040C,
    kCmdHealthCheckProvider = 0x040D,
    kCmdIsProviderBusy = 0x040E,

    // ---- BackendCapabilities
    kCmdIdentity = 0x0500,
    kCmdFeatures = 0x0501,
    kCmdServiceSupported = 0x0502,
    kCmdServiceAvailable = 0x0503,
    kCmdTimings = 0x0504,
    kCmdRequestPrivilegedServiceStatus = 0x0505,

    // ---- MihomoBackend itself
    kCmdGeneration = 0x0600,
    // The sequence of the last event the module's wrapped backend PRODUCED.
    // The host records it when an observer registers and admits only events
    // whose envelope sequence is greater, which is backend-r4's admission rule
    // stated in the only terms that survive a boundary. A module whose backend
    // cannot report one answers 0, and a host that gets 0 admits everything -
    // the pre-envelope behaviour, and an honest degradation rather than a
    // silent one.
    kCmdProducedSequence = 0x0601,
    // How many pieces of NATIVE ASYNCHRONOUS WORK the module has submitted and
    // not yet seen exit: runnables on a pool the module owns, whose code lives
    // in the module's image. A session's OutstandingWork() already includes this
    // number, and Close() drains against it, but a host that wants to ask before
    // deciding to unload can ask directly. Answered 0 by a module whose backend
    // submits none, which is the honest pre-r3 answer and not a claim.
    kCmdPendingNativeWork = 0x0602,

    // ---- the privileged-execution reverse seam, host -> module.
    // Privileged execution stays host-owned (module-r1): the module never
    // constructs a real helper client. These are the listener callbacks of
    // core::PrivilegedCoreServiceListener, delivered into the module's adapter.
    kCmdPrivilegedConnectedChanged = 0x0700,
    kCmdPrivilegedStatusReceived = 0x0701,
    kCmdPrivilegedCoreStarted = 0x0702,
    kCmdPrivilegedCoreStopped = 0x0703,
    kCmdPrivilegedLogsReceived = 0x0704,
    kCmdPrivilegedRequestFinished = 0x0705,

    // ---- reserved for a module that is not the shipping one.
    // The real module answers everything at or above this kNotImplemented, so a
    // test-only control surface cannot become a failure-injection switch in the
    // shipping ABI (module-r1: "Do not add failure-inversion switches to
    // shipping ABI"). The fake module implements them; the real one refuses.
    kCmdTestControlBase = 0xF0000000,
};

// --------------------------------------------------------- host operations
//
// Module -> host, through IBackendHost::Invoke. The privileged-service seam
// only: ONE connection, owned by the host, marshalled here.

enum HostCommand : std::uint32_t {
    kHostPrivilegedIsSupported = 0x0800,
    kHostPrivilegedIsAvailable = 0x0801,
    kHostPrivilegedIsConnected = 0x0802,
    kHostPrivilegedIsBusy = 0x0803,
    kHostPrivilegedConnectionError = 0x0804,
    kHostPrivilegedRequestStatus = 0x0805,
    kHostPrivilegedRequestLogs = 0x0806,
    kHostPrivilegedStartCore = 0x0807,
    kHostPrivilegedStopCore = 0x0808,
    kHostPrivilegedClose = 0x0809,
    // The module's adapter attaches and detaches itself as THE listener. The
    // host refuses a second attach: one connection only.
    kHostPrivilegedSetListenerActive = 0x080A,
};

// ------------------------------------------------------------------ events
//
// Module -> host, through IBackendHost::Notify. One per BackendObserver
// callback of backend-r4, delivered in production order, each payload behind
// the u64 envelope described at the top of this file.

enum Event : std::uint32_t {
    kEvtCoreStateChanged = 0x0900,
    kEvtCoreReady = 0x0901,
    kEvtCoreLogLine = 0x0902,
    kEvtCoreFailed = 0x0903,
    kEvtCoreStopped = 0x0904,
    kEvtStopCompleted = 0x0905,

    kEvtEndpointChanged = 0x0910,
    kEvtConnectedChanged = 0x0911,
    kEvtConfigReceived = 0x0912,

    kEvtModeChanged = 0x0920,
    kEvtTunChangeCompleted = 0x0921,
    kEvtNodeSelected = 0x0922,
    kEvtGeoDatabasesUpdated = 0x0923,
    kEvtDnsQueryFinished = 0x0924,
    kEvtDnsCacheFlushed = 0x0925,

    kEvtVersionReceived = 0x0930,
    kEvtProxiesUpdated = 0x0931,
    kEvtRulesUpdated = 0x0932,
    kEvtTrafficSample = 0x0933,
    kEvtMemorySample = 0x0934,
    // Payload is the packed snapshot below, not the general encoding.
    kEvtConnectionsUpdated = 0x0935,
    kEvtLogReceived = 0x0936,

    kEvtProvidersReceived = 0x0940,
    kEvtProviderBusyChanged = 0x0941,
    kEvtProviderOperationFinished = 0x0942,

    kEvtPrivilegedServiceStatus = 0x0950,
    kEvtErrorOccurred = 0x0960,
};

// ------------------------------------------------------------- close flags

enum CloseFlags : std::uint32_t {
    // Stop accepting commands, cancel what can be cancelled, release the host
    // reference. Pending work that cannot be cancelled is still awaited.
    kCloseDefault = 0,
    // Additionally stop any managed core this session started. The host uses
    // this on shutdown; an unconfirmed stop is still reported as unconfirmed.
    kCloseStopManagedCore = 1u << 0,
};

// --------------------------------------------------------------- timestamps
//
// How a QDateTime's time representation travels, in BOTH encodings. The numbers
// are this wire's own and are deliberately not Qt::TimeSpec's: a peer built
// against a Qt whose enum is renumbered must still decode what this header
// describes. Mapping to and from Qt happens once, in the codec.
enum TimeRepresentation : std::uint8_t {
    kTimeLocal = 0,          // the receiver's own zone, offset is informational
    kTimeUtc = 1,            // offset is 0
    kTimeOffsetFromUtc = 2,  // offset is the whole representation
    kTimeNamedZone = 3,      // the IANA identifier is the representation
};

// Anything above this is a producer this build does not understand, and a
// decoder refuses it rather than guessing at LocalTime.
inline constexpr std::uint8_t kTimeRepresentationMax = kTimeNamedZone;

// Qt rejects a zone offset outside +/- 16 hours, so a decoder validates the
// same bound instead of handing QDateTime a value it will silently discard.
inline constexpr std::int32_t kMaxUtcOffsetSeconds = 16 * 3600;

// An invalid QDateTime, distinguishable from the epoch. 64-bit because a 32-bit
// field silently truncates in 2038, and a truncated `end` reads as a connection
// that closed before it opened.
inline constexpr std::int64_t kNoTimestamp = INT64_MIN;

// ---------------------------------------------------- packed connection set
//
// D8's second binding constraint: "One buffer per snapshot with fixed-layout
// structs indexing into it, and a case in the benchmark lane before the claim
// is made." A snapshot can be hundreds of entries and arrives at the engine's
// emission rate, so it is the one payload where per-string allocation would be
// a real regression.
//
// Layout, in one buffer, in this order and with no padding between sections:
//
//   [ ConnectionSnapshotHeader ]          fixed, 64 bytes
//   [ ConnectionRecord * count ]          fixed, 184 bytes each
//   [ TextRef * chainSlotCount ]          the flattened proxy chains
//   [ UTF-8 blob ]                        every string, no separators
//
// A TextRef is an offset and a length INTO THE BLOB, relative to blobOffset.
// Nothing in the buffer is a pointer, so the buffer is position independent and
// a consumer can validate it fully before reading one byte of text.
//
// Timestamps keep their representation here too, and they do it WITHOUT giving
// up the fixed layout: the instant, the TimeRepresentation and the UTC offset
// are fixed-width fields of the record, and a named zone's IANA identifier is a
// TextRef into the same interned blob every other string uses. A snapshot whose
// rows share one zone therefore costs that identifier once, not once per row,
// and the buffer is still exactly one owned allocation.

// Every offset and length is validated against the blob's bounds before use.
struct TextRef {
    std::uint32_t offset;
    std::uint32_t length;
};

static_assert(sizeof(TextRef) == 8, "TextRef is two fixed-width fields");

inline constexpr std::uint32_t kConnectionSnapshotMagic = 0x43514E53;  // 'CQNS'
// 2: ConnectionRecord carries each timestamp's representation as well as its
//    instant, so the host renders what the backend produced.
inline constexpr std::uint16_t kConnectionSnapshotVersion = 2;

struct ConnectionSnapshotHeader {
    std::uint32_t magic;         // kConnectionSnapshotMagic
    std::uint16_t version;       // kConnectionSnapshotVersion
    std::uint16_t recordSize;    // sizeof(ConnectionRecord), so a newer producer
                                 // with wider records is detected, not misread
    std::uint32_t count;         // connection records
    std::uint32_t chainSlots;    // TextRefs in the chain section
    std::uint32_t recordOffset;  // from the start of the buffer
    std::uint32_t chainOffset;
    std::uint32_t blobOffset;
    std::uint32_t blobSize;
    std::uint64_t generation;  // core::backend::Generation, 64-bit, lossless
    std::uint64_t uploadTotal;
    std::uint64_t downloadTotal;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
};

static_assert(sizeof(ConnectionSnapshotHeader) == 64, "header layout is fixed");
static_assert(alignof(ConnectionSnapshotHeader) == 8, "no implicit padding at the end");

// core::Connection, flattened. The 14 strings are TextRefs - the 12 of the
// connection plus the two zone identifiers - and the proxy chain is a run of
// TextRefs in the chain section. Each timestamp is four fields: the instant in
// milliseconds since the epoch (kNoTimestamp when the QDateTime is invalid),
// its TimeRepresentation, its offset from UTC in seconds, and, for a named
// zone, the TextRef naming it.
struct ConnectionRecord {
    TextRef id;
    TextRef host;
    TextRef network;
    TextRef connectionType;
    TextRef rule;
    TextRef rulePayload;
    TextRef sourceIp;
    TextRef sourcePort;
    TextRef destinationIp;
    TextRef destinationPort;
    TextRef process;
    TextRef processPath;
    // Empty unless the matching spec is kTimeNamedZone. Interned like every
    // other string, so one zone shared by 500 rows is stored once.
    TextRef startZone;
    TextRef endZone;
    std::uint32_t chainFirst;  // index into the chain section
    std::uint32_t chainCount;
    std::uint64_t upload;
    std::uint64_t download;
    double uploadRate;
    double downloadRate;
    std::int64_t startMs;
    std::int64_t endMs;
    std::int32_t startOffsetSeconds;
    std::int32_t endOffsetSeconds;
    std::uint8_t startSpec;  // TimeRepresentation
    std::uint8_t endSpec;    // TimeRepresentation
    // Declared rather than left to the compiler: a record whose tail is implicit
    // padding travels with whatever happened to be on the producer's stack, and
    // a fixed layout means every byte is accounted for.
    std::uint8_t reserved[6];
};

static_assert(sizeof(ConnectionRecord) == 184, "record layout is fixed");
static_assert(alignof(ConnectionRecord) == 8, "no implicit padding at the end");
static_assert(sizeof(double) == 8, "the rate fields are IEEE-754 binary64");

}  // namespace clashqt::com::abi

#endif  // CLASHQT_CORE_COMPONENT_ABI_WIRE_H
