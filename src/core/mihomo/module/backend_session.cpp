#include "core/mihomo/module/backend_session.h"

#include <cstring>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QThread>

#include "core/mihomo/module/backend_module.h"
#include "integrations/component/marshal/backend_marshal.h"
#include "integrations/component/marshal/com_objects.h"
#include "integrations/component/marshal/connection_snapshot.h"

namespace core::module {
namespace {

QJsonObject jsonObjectFrom(const QString &text) {
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
    return document.isObject() ? document.object() : QJsonObject{};
}

/// The requests backend-r4 documents as settling with an observer callback
/// carrying their id. Everything else returns an id for correlation only:
/// a stream open produces no completion at all (telemetry.h), closeConnection
/// reports only failures (control.h), and attach/detach announce themselves
/// with endpointChanged/connectedChanged rather than with a completion. Those
/// are deliberately NOT tracked - a counter that only ever goes up is worse
/// than no counter, because Close would then always time out.
bool settlesWithACompletion(std::uint32_t command) noexcept {
    switch (command) {
        case abi::kCmdStart:
        case abi::kCmdStop:
        case abi::kCmdRefreshConfig:
        case abi::kCmdRefreshVersion:
        case abi::kCmdRefreshProxies:
        case abi::kCmdRefreshRules:
        case abi::kCmdSetMode:
        case abi::kCmdSetTunEnabled:
        case abi::kCmdSelectNode:
        case abi::kCmdResetGroupSelection:
        case abi::kCmdTestGroupDelay:
        case abi::kCmdTestNodeDelay:
        case abi::kCmdUpdateGeoDatabases:
        case abi::kCmdQueryDns:
        case abi::kCmdFlushDnsCache:
        case abi::kCmdFetchProviders:
        case abi::kCmdUpdateProvider:
        case abi::kCmdHealthCheckProvider:
        case abi::kCmdRequestPrivilegedServiceStatus:
            return true;
        default:
            return false;
    }
}

/// The event that asks the owner thread to perform a teardown a foreign thread
/// is not allowed to perform. Registered rather than hard-coded, so it cannot
/// collide with a host's own user events.
QEvent::Type retireEventType() noexcept {
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}

/// How long a cleanup that found a module-owned runnable still in flight waits
/// before looking again. Short enough that a released session is reclaimed
/// promptly, long enough that the owner's event loop is not starved by a
/// re-post per iteration - the object is counted throughout, so nothing is
/// unsafe while it elapses.
constexpr int kCleanupRetryMs = 5;

}  // namespace

/// A QObject with the owner thread's affinity and nothing else. It exists so a
/// deferred cleanup has somewhere to run; Qt delivers the event on the owner
/// thread and the teardown happens there.
///
/// No Q_OBJECT, deliberately: it declares no signals or slots, so the module
/// needs no moc pass for it and the build registration stays exactly as it was.
class BackendSession::OwnerThreadRetire final : public QObject {
  public:
    explicit OwnerThreadRetire(BackendSession *session) noexcept : session_(session) {}

    /// Asks for another cleanup pass shortly. Returns false when no timer could
    /// be started, which is the caller's cue to ask through the event queue
    /// instead - never its cue to destroy anything early.
    bool rearm() noexcept {
        if (timer_ != 0) {
            return true;
        }
        timer_ = startTimer(kCleanupRetryMs);
        return timer_ != 0;
    }

  protected:
    bool event(QEvent *incoming) override {
        if (incoming->type() == retireEventType()) {
            BackendSession *session = session_;
            // May destroy the session, and with it this object - the same shape
            // as Qt's own DeferredDelete, which calls `delete` on the receiver
            // from inside its event(). Nothing of `this` is touched afterwards.
            session->runOwnerThreadCleanup();
            return true;
        }
        return QObject::event(incoming);
    }

    void timerEvent(QTimerEvent *incoming) override {
        if (incoming->timerId() != timer_) {
            QObject::timerEvent(incoming);
            return;
        }
        // Killed BEFORE the pass, because the pass may delete this object and
        // because a pass that finds work still running re-arms deliberately.
        killTimer(timer_);
        timer_ = 0;
        BackendSession *session = session_;
        session->runOwnerThreadCleanup();
    }

  private:
    BackendSession *session_;
    int timer_ = 0;
};

BackendSession::ModuleCount::~ModuleCount() {
    if (owner_ != nullptr) {
        owner_->objectDestroyed();
    }
}

BackendSession::BackendSession(BackendModule *owner, BackendFactory factory) noexcept
    : moduleCount_(owner), owner_(owner), factory_(std::move(factory)) {}

BackendSession::~BackendSession() {
    // A session that is still open when its last reference goes is closed
    // first: the destructor must not leave a managed core running behind a
    // released object, and must not leave the wrapped backend delivering into
    // a half-destroyed observer.
    if (!closed_) {
        Close(abi::kCloseDefault, 0);
    }
    // The module's count is NOT released here. moduleCount_ is the first member
    // declared, so it is the last destroyed, and the count survives until
    // retire_, wrapped_, privileged_ and every other member have gone. See the
    // note at the top of the header.
}

void BackendSession::setCommandExtension(CommandExtension extension) {
    extension_ = std::move(extension);
}

marshal::ObjectAnchor *BackendSession::anchor() const noexcept { return owner_; }

com::Result BackendSession::QueryInterface(const com::InterfaceId &id, void **out) noexcept {
    const com::InterfaceEntry entries[] = {
        com::MakeInterfaceEntry(static_cast<abi::IBackendSession *>(this)),
        com::InterfaceEntry{&com::kIObjectId,
                            static_cast<void *>(static_cast<com::IObject *>(
                                static_cast<abi::IBackendSession *>(this)))},
    };
    return com::ResolveInterface(id, out, entries, sizeof(entries) / sizeof(entries[0]),
                                 static_cast<abi::IBackendSession *>(this));
}

std::int32_t BackendSession::AddRef() noexcept { return references_.Increment(); }

std::int32_t BackendSession::Release() noexcept {
    const std::int32_t remaining = references_.Decrement();
    if (remaining != 0) {
        return remaining;
    }
    // Zero holders: this thread now owns the object outright. Nobody else can
    // be inside AddRef, because nobody else has a pointer it would be legal to
    // call it on, so the count below is this thread's to move.
    if (!mustDeferFinalCleanup()) {
        delete this;
        // component-r1's zero, and it means what it says: the destructor has
        // run and the storage is gone.
        return 0;
    }
    // It could not be destroyed here. Saying "0" now would report a destruction
    // that has not happened and will not happen on this thread; instead the
    // cleanup takes a REAL reference and the count reports it. The object,
    // the module's count for it and therefore the mapping all survive until
    // that reference is dropped - which is the same interval as before, now
    // described honestly.
    const std::int32_t held = references_.Increment();
    scheduleOwnerThreadCleanup();
    return held;
}

bool BackendSession::mustDeferFinalCleanup() const noexcept {
    // component-r1 permits Release from ANY thread and this class does not
    // narrow that. What it cannot do is DESTROY from any thread: an activated
    // session owns a QProcess, its socket notifiers and a
    // QNetworkAccessManager, and tearing those down off their own thread is
    // undefined - it was observed as a SIGSEGV inside QSocketNotifier, after
    // the object had already reported itself gone.
    QThread *const owner = ownerThread_.load(std::memory_order_acquire);
    if (owner == nullptr) {
        // No host was ever installed, so there is no Qt collaborator and no
        // pool: an inert object, synchronously destructible anywhere.
        return false;
    }
    if (retire_ == nullptr || QCoreApplication::instance() == nullptr) {
        // Nothing to defer TO. retire_ is created with ownerThread_ so the
        // first half cannot happen; the second means Qt's own machinery has
        // already gone, and with it the notifiers whose affinity is at issue.
        return false;
    }
    if (owner != QThread::currentThread()) {
        return true;
    }
    // The owner thread, but the wrapped backend still has runnables on a pool
    // it owns. They execute instructions in this image, so the object outlives
    // them - and it does so by being counted rather than by blocking here.
    // Close(timeoutMs) is the call that exists to drain them on a budget; a
    // Release that stalled the owner's loop for the length of a config parse
    // would be a worse answer than an honest count.
    return nativeWorkCount() > 0;
}

void BackendSession::scheduleOwnerThreadCleanup() noexcept {
    try {
        QCoreApplication::postEvent(retire_.get(), new QEvent(retireEventType()));
    } catch (...) {
        // Out of memory while asking for the cleanup. The object is left ALIVE
        // and COUNTED rather than destroyed on a thread that may not destroy
        // it: a loader is told one object is outstanding and refuses to unmap,
        // which is the safe answer even though this one is never reclaimed.
    }
}

void BackendSession::runOwnerThreadCleanup() noexcept {
    if (nativeWorkCount() > 0) {
        // Not yet. A runnable the wrapped backend submitted is still executing
        // code in this image; destroying now would run this object's members
        // out from under it. Keep the cleanup reference and look again.
        if (!retire_->rearm()) {
            scheduleOwnerThreadCleanup();
        }
        return;
    }
    dropCleanupReference();
}

void BackendSession::dropCleanupReference() noexcept {
    // The reference Release transferred here. It is the last one, so this
    // decrement is the one that reaches zero - on the thread that may legally
    // destroy this object's Qt collaborators.
    if (references_.Decrement() == 0) {
        delete this;
    }
}

std::int32_t BackendSession::nativeWorkCount() const noexcept {
    if (!wrapped_.pendingNativeWork) {
        return 0;
    }
    const int pending = wrapped_.pendingNativeWork();
    return pending > 0 ? static_cast<std::int32_t>(pending) : 0;
}

com::Result BackendSession::SetHost(abi::IBackendHost *host) noexcept {
    try {
        return setHostImpl(host);
    } catch (...) {
        return com::kFail;
    }
}

com::Result BackendSession::setHostImpl(abi::IBackendHost *host) {
    if (closed_) {
        return com::kAlreadyClosed;
    }
    if (host == nullptr) {
        setError(com::kInvalidArgument, QStringLiteral("a session needs a host"),
                 QStringLiteral("SetHost"));
        return com::kInvalidArgument;
    }
    if (host_ != nullptr) {
        setError(com::kInvalidState, QStringLiteral("this session already has a host"),
                 QStringLiteral("SetHost"));
        return com::kInvalidState;
    }

    host_ = host;
    host_->AddRef();

    // The backend is built HERE and not in the constructor, because the
    // privileged-execution seam is the host's and there is nothing to adapt
    // until a host exists. module-r1: no default real helper construction in
    // the module.
    //
    // Everything from here to addObserver allocates, and this is a COM entry
    // point: an exception that escapes it crosses into a caller compiled by a
    // different runtime, which is undefined rather than merely wrong.
    try {
        privileged_ = std::make_unique<HostPrivilegedService>(host_);
        // Allocated BEFORE the backend, because from the moment the backend
        // exists this session owns Qt objects with this thread's affinity and a
        // cross-thread final Release needs somewhere to post to. Created here
        // rather than in the constructor so it belongs to the thread that will
        // actually own those objects, which is the one calling SetHost.
        retire_ = std::make_unique<OwnerThreadRetire>(this);
        wrapped_ = factory_ ? factory_(privileged_.get()) : WrappedBackend{};
    } catch (...) {
        wrapped_ = WrappedBackend{};
    }
    if (!wrapped_.backend) {
        retire_.reset();
        privileged_.reset();
        host_->Release();
        host_ = nullptr;
        setError(com::kFail, QStringLiteral("the module could not construct a backend"),
                 QStringLiteral("SetHost"));
        return com::kFail;
    }
    wrapped_.backend->addObserver(this);
    // Published only once the backend exists and retire_ is in place, so a
    // concurrent Release never sees an owner thread without a landing pad.
    ownerThread_.store(QThread::currentThread(), std::memory_order_release);
    clearError();
    return com::kOk;
}

void BackendSession::setError(com::Result code, const QString &message, const QString &source) {
    lastErrorCode_ = code;
    lastErrorMessage_ = message;
    lastErrorSource_ = source;
}

void BackendSession::clearError() {
    lastErrorCode_ = com::kOk;
    lastErrorMessage_.clear();
    lastErrorSource_.clear();
}

com::Result BackendSession::LastError(com::IErrorInfo **out) noexcept {
    try {
        return lastErrorImpl(out);
    } catch (...) {
        if (out != nullptr) {
            *out = nullptr;
        }
        return com::kFail;
    }
}

com::Result BackendSession::lastErrorImpl(com::IErrorInfo **out) {
    if (out == nullptr) {
        return com::kInvalidArgument;
    }
    *out = nullptr;
    if (com::IsSuccess(lastErrorCode_)) {
        return com::kNotFound;  // no diagnostic is normal, not a failure
    }
    // Anchored on the module: a consumer may hold this diagnostic - and the
    // buffers it produces for its message and source - long after releasing the
    // session it came from, and every one of them has its vtable in this image.
    auto *error = marshal::ErrorInfoObject::Create(lastErrorCode_, lastErrorMessage_,
                                                   lastErrorSource_, owner_);
    if (error == nullptr) {
        return com::kFail;
    }
    *out = static_cast<com::IErrorInfo *>(error);
    return com::kOk;
}

cb::RequestId BackendSession::track(cb::RequestId id) {
    if (id != cb::RequestId::Invalid) {
        inFlight_.insert(cb::number(id));
    }
    return id;
}

void BackendSession::settle(cb::RequestId id) {
    if (id != cb::RequestId::Invalid) {
        inFlight_.remove(cb::number(id));
        if (id == closingStop_) {
            closingStopSettled_ = true;
        }
    }
}

std::int32_t BackendSession::OutstandingWork() noexcept {
    // Requests that have not settled, PLUS runnables the wrapped backend has on
    // its own pool. The second term is not bookkeeping pedantry: those
    // runnables execute instructions in this module's image, so a host that saw
    // zero here could unmap the code that is running.
    return static_cast<std::int32_t>(inFlight_.size()) + nativeWorkCount();
}

com::Result BackendSession::Invoke(std::uint32_t command, const void *args, std::size_t size,
                                   com::IBuffer **reply) noexcept {
    try {
        return invokeImpl(command, args, size, reply);
    } catch (...) {
        // The output stays null, as it was set before anything could throw, and
        // the caller gets a code instead of an exception crossing the boundary.
        if (reply != nullptr) {
            *reply = nullptr;
        }
        return com::kFail;
    }
}

com::Result BackendSession::invokeImpl(std::uint32_t command, const void *args, std::size_t size,
                                       com::IBuffer **reply) {
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (closed_) {
        return com::kAlreadyClosed;
    }
    // Cleared HERE, not only on success. LastError() is documented as the
    // diagnostic for the LAST failed call, and a caller that reads it after a
    // fresh failure must not be handed the previous one: a stale message names
    // the wrong command and sends the reader looking in the wrong place. Every
    // exit below either sets a diagnostic or leaves this cleared state.
    clearError();
    if (command >= abi::kCmdTestControlBase) {
        // The shipping module has no extension, so the test-control range is
        // kNotImplemented there. This is the whole mechanism that keeps a
        // failure-injection switch out of the shipping ABI.
        if (!extension_) {
            setError(com::kNotImplemented,
                     QStringLiteral("command 0x%1 is in the test-control range, which a shipping "
                                    "module does not implement")
                         .arg(command, 0, 16),
                     QStringLiteral("Invoke"));
            return com::kNotImplemented;
        }
        return extension_(*this, command, args, size, reply);
    }
    if (wrapped_.backend == nullptr) {
        setError(com::kInvalidState, QStringLiteral("no host is installed"),
                 QStringLiteral("Invoke"));
        return com::kInvalidState;
    }

    marshal::ByteReader in(args, size);
    marshal::ByteWriter out;
    const com::Result status = dispatch(command, in, out);
    if (com::IsFailure(status)) {
        // A decode that failed INSIDE dispatch used to return here silently,
        // leaving LastError() answering kNotFound - or, worse, a diagnostic
        // from some earlier call - for a failure that had just happened. Every
        // refusal names itself now, and a dispatch that already set a more
        // specific message keeps it.
        if (com::IsSuccess(lastErrorCode_)) {
            setError(status,
                     status == com::kNotImplemented
                         ? QStringLiteral("command 0x%1 is not implemented by this module")
                               .arg(command, 0, 16)
                         : QStringLiteral("arguments for command 0x%1 did not decode")
                               .arg(command, 0, 16),
                     QStringLiteral("Invoke"));
        }
        return status;
    }
    if (!in.finished()) {
        // Either the argument block was short - which the reader already
        // latched - or it carried bytes this command does not know, which
        // means the two sides disagree about the encoding. Both are the peer's
        // bug, and both are reported rather than absorbed.
        setError(com::kInvalidArgument,
                 QStringLiteral("arguments for command 0x%1 did not decode")
                     .arg(command, 0, 16),
                 QStringLiteral("Invoke"));
        return com::kInvalidArgument;
    }
    if (reply != nullptr && !out.isEmpty()) {
        // Anchored on the module for the same reason the diagnostic is: the
        // consumer decides how long this buffer lives, and it is this image's
        // vtable it is calling into for the whole of that time.
        auto *buffer = marshal::ByteBuffer::Create(out.data().data(), out.size(), owner_);
        if (buffer == nullptr) {
            setError(com::kFail, QStringLiteral("the reply could not be allocated"),
                     QStringLiteral("Invoke"));
            return com::kFail;
        }
        *reply = static_cast<com::IBuffer *>(buffer);
    }
    clearError();
    return com::kOk;
}

com::Result BackendSession::dispatch(std::uint32_t command, marshal::ByteReader &in,
                                     marshal::ByteWriter &out) {
    cb::MihomoBackend &backend = *wrapped_.backend;

    if (command == abi::kCmdProducedSequence) {
        // Not a backend-r4 operation: the number the host needs to admit an
        // observer by production rather than by arrival. Answered before the
        // switch so it cannot be mistaken for one.
        out.u64(wrapped_.producedSequence ? wrapped_.producedSequence() : 0);
        return com::kOk;
    }

    if (command == abi::kCmdPendingNativeWork) {
        // Also not a backend-r4 operation: what the module has running on its
        // own pool, so a host can ask before it decides to unload rather than
        // inferring quiescence from a request counter that never knew about it.
        out.i32(nativeWorkCount());
        return com::kOk;
    }

    const auto requestReply = [&](cb::RequestId id) {
        if (settlesWithACompletion(command)) {
            track(id);
        }
        marshal::writeRequestId(out, id);
    };

    switch (command) {
        // ---- BackendLifecycle
        case abi::kCmdDiscoverBinary:
            out.text(backend.discoverBinary());
            return com::kOk;
        case abi::kCmdSetBinaryPath:
            backend.setBinaryPath(in.text());
            return in.ok() ? com::kOk : com::kInvalidArgument;
        case abi::kCmdBinaryPath:
            out.text(backend.binaryPath());
            return com::kOk;
        case abi::kCmdSetExecutionMode: {
            const auto mode = static_cast<cb::ExecutionMode>(in.u8());
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            out.boolean(backend.setExecutionMode(mode));
            return com::kOk;
        }
        case abi::kCmdExecutionMode:
            out.u8(static_cast<std::uint8_t>(backend.executionMode()));
            return com::kOk;
        case abi::kCmdUsesPrivilegedService:
            out.boolean(backend.usesPrivilegedService());
            return com::kOk;
        case abi::kCmdStart: {
            const QString configPath = in.text();
            const QString workDir = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.start(configPath, workDir));
            return com::kOk;
        }
        case abi::kCmdStop:
            requestReply(backend.stop());
            return com::kOk;
        case abi::kCmdState:
            out.u8(static_cast<std::uint8_t>(backend.state()));
            return com::kOk;
        case abi::kCmdOwnership:
            out.u8(static_cast<std::uint8_t>(backend.ownership()));
            return com::kOk;
        case abi::kCmdManagedEndpoint:
            marshal::writeEndpoint(out, backend.managedEndpoint());
            return com::kOk;
        case abi::kCmdActiveConfigPaths:
            out.textList(backend.activeConfigPaths());
            return com::kOk;
        case abi::kCmdIsRestartPending:
            out.boolean(backend.isRestartPending());
            return com::kOk;
        case abi::kCmdIsManagedCoreActive:
            out.boolean(backend.isManagedCoreActive());
            return com::kOk;

        // ---- BackendAttachment
        case abi::kCmdDiscoverEndpoint:
            marshal::writeEndpoint(out, backend.discoverEndpoint());
            return com::kOk;
        case abi::kCmdEndpointFromConfigFile: {
            const QString path = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            cb::Endpoint endpoint;
            const bool parsed = backend.endpointFromConfigFile(path, &endpoint);
            out.boolean(parsed);
            // The endpoint is written either way so the reply has one shape;
            // the flag, not the payload, is what the caller branches on, and
            // *out is documented as untouched when the answer is false.
            marshal::writeEndpoint(out, endpoint);
            return com::kOk;
        }
        case abi::kCmdAttach: {
            const cb::Endpoint endpoint = marshal::readEndpoint(in);
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.attach(endpoint));
            return com::kOk;
        }
        case abi::kCmdDetach:
            requestReply(backend.detach());
            return com::kOk;
        case abi::kCmdCurrentEndpoint:
            marshal::writeEndpoint(out, backend.currentEndpoint());
            return com::kOk;
        case abi::kCmdIsAttached:
            out.boolean(backend.isAttached());
            return com::kOk;
        case abi::kCmdIsConnected:
            out.boolean(backend.isConnected());
            return com::kOk;
        case abi::kCmdAttachmentOwnership:
            out.u8(static_cast<std::uint8_t>(backend.attachmentOwnership()));
            return com::kOk;
        case abi::kCmdIsExternalControllerConnected:
            out.boolean(backend.isExternalControllerConnected());
            return com::kOk;
        case abi::kCmdRefreshConfig:
            requestReply(backend.refreshConfig());
            return com::kOk;

        // ---- BackendControl
        case abi::kCmdSetMode: {
            const QString mode = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.setMode(mode));
            return com::kOk;
        }
        case abi::kCmdSetTunEnabled: {
            const bool enabled = in.boolean();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.setTunEnabled(enabled));
            return com::kOk;
        }
        case abi::kCmdIsTunChangePending:
            out.boolean(backend.isTunChangePending());
            return com::kOk;
        case abi::kCmdSelectNode: {
            const QString group = in.text();
            const QString node = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.selectNode(group, node));
            return com::kOk;
        }
        case abi::kCmdResetGroupSelection: {
            const QString group = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.resetGroupSelection(group));
            return com::kOk;
        }
        case abi::kCmdTestGroupDelay: {
            const QString group = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.testGroupDelay(group));
            return com::kOk;
        }
        case abi::kCmdTestNodeDelay: {
            const QString node = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.testNodeDelay(node));
            return com::kOk;
        }
        case abi::kCmdCloseConnection: {
            const QString id = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.closeConnection(id));
            return com::kOk;
        }
        case abi::kCmdCloseAllConnections:
            requestReply(backend.closeAllConnections());
            return com::kOk;
        case abi::kCmdUpdateGeoDatabases:
            requestReply(backend.updateGeoDatabases());
            return com::kOk;
        case abi::kCmdQueryDns: {
            const QString name = in.text();
            const QString type = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.queryDns(name, type));
            return com::kOk;
        }
        case abi::kCmdFlushDnsCache: {
            const bool fakeIp = in.boolean();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.flushDnsCache(fakeIp));
            return com::kOk;
        }

        // ---- BackendTelemetry
        case abi::kCmdRefreshVersion:
            requestReply(backend.refreshVersion());
            return com::kOk;
        case abi::kCmdRefreshProxies:
            requestReply(backend.refreshProxies());
            return com::kOk;
        case abi::kCmdRefreshRules:
            requestReply(backend.refreshRules());
            return com::kOk;
        case abi::kCmdOpenTrafficStream:
            requestReply(backend.openTrafficStream());
            return com::kOk;
        case abi::kCmdCloseTrafficStream:
            requestReply(backend.closeTrafficStream());
            return com::kOk;
        case abi::kCmdOpenConnectionsStream:
            requestReply(backend.openConnectionsStream());
            return com::kOk;
        case abi::kCmdCloseConnectionsStream:
            requestReply(backend.closeConnectionsStream());
            return com::kOk;
        case abi::kCmdOpenLogStream: {
            const QString level = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.openLogStream(level));
            return com::kOk;
        }
        case abi::kCmdCloseLogStream:
            requestReply(backend.closeLogStream());
            return com::kOk;
        case abi::kCmdOpenMemoryStream:
            requestReply(backend.openMemoryStream());
            return com::kOk;
        case abi::kCmdCloseMemoryStream:
            requestReply(backend.closeMemoryStream());
            return com::kOk;
        case abi::kCmdFetchProviders: {
            const bool rules = in.boolean();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.fetchProviders(rules));
            return com::kOk;
        }
        case abi::kCmdUpdateProvider: {
            const bool rules = in.boolean();
            const QString name = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.updateProvider(rules, name));
            return com::kOk;
        }
        case abi::kCmdHealthCheckProvider: {
            const QString name = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            requestReply(backend.healthCheckProvider(name));
            return com::kOk;
        }
        case abi::kCmdIsProviderBusy:
            out.boolean(backend.isProviderBusy());
            return com::kOk;

        // ---- BackendCapabilities
        case abi::kCmdIdentity:
            marshal::writeIdentity(out, backend.identity());
            return com::kOk;
        case abi::kCmdFeatures:
            out.u32(backend.features().bits);
            return com::kOk;
        case abi::kCmdServiceSupported:
            out.boolean(backend.serviceSupported());
            return com::kOk;
        case abi::kCmdServiceAvailable:
            out.boolean(backend.serviceAvailable());
            return com::kOk;
        case abi::kCmdTimings:
            marshal::writeTimings(out, backend.timings());
            return com::kOk;
        case abi::kCmdRequestPrivilegedServiceStatus:
            requestReply(backend.requestPrivilegedServiceStatus());
            return com::kOk;

        // ---- MihomoBackend
        case abi::kCmdGeneration:
            marshal::writeGeneration(out, backend.generation());
            return com::kOk;

        default:
            break;
    }

    if (command >= abi::kCmdPrivilegedConnectedChanged &&
        command <= abi::kCmdPrivilegedRequestFinished) {
        return dispatchPrivileged(command, in);
    }

    // An unknown code from a newer host is NORMAL. It is answered, not
    // absorbed, so the host can decide whether the feature was optional.
    setError(com::kNotImplemented,
             QStringLiteral("command 0x%1 is not implemented by this module").arg(command, 0, 16),
             QStringLiteral("Invoke"));
    return com::kNotImplemented;
}

com::Result BackendSession::dispatchPrivileged(std::uint32_t command, marshal::ByteReader &in) {
    if (!privileged_) {
        return com::kInvalidState;
    }
    switch (command) {
        case abi::kCmdPrivilegedConnectedChanged: {
            const bool connected = in.boolean();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            privileged_->deliverConnectedChanged(connected);
            return com::kOk;
        }
        case abi::kCmdPrivilegedStatusReceived: {
            const QString json = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            privileged_->deliverStatusReceived(jsonObjectFrom(json));
            return com::kOk;
        }
        case abi::kCmdPrivilegedCoreStarted: {
            const QString json = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            privileged_->deliverCoreStarted(jsonObjectFrom(json));
            return com::kOk;
        }
        case abi::kCmdPrivilegedCoreStopped:
            privileged_->deliverCoreStopped();
            return com::kOk;
        case abi::kCmdPrivilegedLogsReceived: {
            const QString logs = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            privileged_->deliverLogsReceived(logs);
            return com::kOk;
        }
        case abi::kCmdPrivilegedRequestFinished: {
            const QString operation = in.text();
            const bool success = in.boolean();
            const QString error = in.text();
            if (!in.ok()) {
                return com::kInvalidArgument;
            }
            privileged_->deliverRequestFinished(operation, success, error);
            return com::kOk;
        }
        default:
            return com::kNotImplemented;
    }
}

com::Result BackendSession::Close(std::uint32_t flags, std::uint32_t timeoutMs) noexcept {
    try {
        return closeImpl(flags, timeoutMs);
    } catch (...) {
        // Teardown must still happen: the alternative to an untidy close is a
        // wrapped backend that keeps delivering into a released observer.
        closed_ = true;
        wrapped_ = WrappedBackend{};
        privileged_.reset();
        if (host_ != nullptr) {
            host_->Release();
            host_ = nullptr;
        }
        return com::kFail;
    }
}

com::Result BackendSession::closeImpl(std::uint32_t flags, std::uint32_t timeoutMs) {
    if (closed_) {
        return com::kAlreadyClosed;  // idempotent, and not an error to guard against
    }

    com::Result outcome = com::kOk;
    if (wrapped_.backend != nullptr) {
        if ((flags & abi::kCloseStopManagedCore) != 0 && wrapped_.backend->isManagedCoreActive()) {
            // An unconfirmed stop is reported through the ordinary
            // stopCompleted event (backend-r4 section 6); Close does not
            // convert it into a success and does not swallow it.
            closingStop_ = wrapped_.backend->stop();
            closingStopSettled_ = closingStop_ == cb::RequestId::Invalid;
            if (closingStop_ != cb::RequestId::Invalid) {
                inFlight_.insert(cb::number(closingStop_));
            }
        }

        // Drain on the shared event loop. The wrapped backend queues its
        // deliveries, so pumping here is what lets them arrive; a bare sleep
        // would not. Native pool work is drained against the same deadline: a
        // finished watcher or a cancel flag proves the HOST's half is done, not
        // that the module-defined runnable has exited.
        QElapsedTimer elapsed;
        elapsed.start();
        while ((!inFlight_.isEmpty() || nativeWorkCount() > 0) &&
               elapsed.elapsed() < static_cast<qint64>(timeoutMs)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        const std::int32_t stillNative = nativeWorkCount();
        if (!inFlight_.isEmpty() || stillNative > 0) {
            // Honest: work was abandoned. Teardown still happens below,
            // because leaving the observer attached to a backend nobody owns
            // is worse than an abandoned request. The teardown itself does NOT
            // abandon the native runnable - destroying the wrapped backend
            // blocks until its pool has drained - so a kTimeout here means
            // "this took longer than you allowed", never "code is still running
            // in an image you may now unmap".
            outcome = com::kTimeout;
            setError(com::kTimeout,
                     QStringLiteral("%1 request(s) and %2 native task(s) were still outstanding "
                                    "at close")
                         .arg(inFlight_.size())
                         .arg(stillNative),
                     QStringLiteral("Close"));
        }
        wrapped_.backend->removeObserver(this);
    }

    closed_ = true;
    // Order matters and is the whole point of this function: the backend goes
    // first, so nothing can produce an event; then the privileged adapter
    // detaches its listener; only then is the host released. "Host and
    // callbacks outlive pending work" (module-r1).
    wrapped_ = WrappedBackend{};
    if (privileged_) {
        privileged_->detachHost();
        privileged_.reset();
    }
    if (host_ != nullptr) {
        host_->Release();
        host_ = nullptr;
    }
    inFlight_.clear();
    return outcome;
}

// ------------------------------------------------------------------ events

std::uint64_t BackendSession::currentProductionSequence() const noexcept {
    // The sequence of the event the wrapped backend is delivering right now.
    // Every observer callback of this class runs inside one of those
    // deliveries, so this is that event's production number and not an
    // approximation of it. A factory that supplies no accessor reports 0, which
    // wire.h defines as "admit everything" on the host side.
    return wrapped_.deliverySequence ? wrapped_.deliverySequence() : 0;
}

void BackendSession::emitEventBytes(std::uint32_t event, const void *data,
                                    std::size_t size) noexcept {
    if (host_ == nullptr) {
        return;
    }
    const std::uint64_t sequence = currentProductionSequence();
    try {
        // wire.h's envelope: the production sequence, little-endian, then the
        // payload untouched. Built in a member buffer that keeps its capacity,
        // so a 500-row connections snapshot costs one memcpy per delivery and
        // not an allocation.
        envelope_.resize(abi::kEventEnvelopeBytes + size);
        for (std::size_t byte = 0; byte < abi::kEventEnvelopeBytes; ++byte) {
            envelope_[byte] = static_cast<std::uint8_t>((sequence >> (byte * 8)) & 0xFF);
        }
        if (size != 0 && data != nullptr) {
            std::memcpy(envelope_.data() + abi::kEventEnvelopeBytes, data, size);
        }
    } catch (...) {
        // An event that cannot be framed is not delivered as a malformed one.
        return;
    }
    host_->Notify(event, envelope_.data(), envelope_.size());
}

void BackendSession::emitEvent(std::uint32_t event, const marshal::ByteWriter &payload) noexcept {
    emitEventBytes(event, payload.data().empty() ? nullptr : payload.data().data(), payload.size());
}

void BackendSession::coreStateChanged(cb::Generation generation, cb::CoreState state,
                                      cb::Ownership ownership) noexcept {
    managedCoreActive_ = state == cb::CoreState::Running || state == cb::CoreState::Starting;
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    out.u8(static_cast<std::uint8_t>(state));
    out.u8(static_cast<std::uint8_t>(ownership));
    emitEvent(abi::kEvtCoreStateChanged, out);
}

void BackendSession::coreReady(const cb::Completion &completion,
                               const cb::Endpoint &endpoint) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    marshal::writeEndpoint(out, endpoint);
    emitEvent(abi::kEvtCoreReady, out);
}

void BackendSession::coreLogLine(cb::Generation generation, const QString &line) noexcept {
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    out.text(line);
    emitEvent(abi::kEvtCoreLogLine, out);
}

void BackendSession::coreFailed(const cb::Completion &completion) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    emitEvent(abi::kEvtCoreFailed, out);
}

void BackendSession::coreStopped(cb::Generation generation) noexcept {
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    emitEvent(abi::kEvtCoreStopped, out);
}

void BackendSession::stopCompleted(const cb::StopCompleted &result) noexcept {
    settle(result.request);
    marshal::ByteWriter out;
    marshal::writeStopCompleted(out, result);
    emitEvent(abi::kEvtStopCompleted, out);
}

void BackendSession::endpointChanged(cb::Generation generation, const cb::Endpoint &endpoint,
                                     cb::Ownership ownership) noexcept {
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    marshal::writeEndpoint(out, endpoint);
    out.u8(static_cast<std::uint8_t>(ownership));
    emitEvent(abi::kEvtEndpointChanged, out);
}

void BackendSession::connectedChanged(cb::Generation generation, bool connected) noexcept {
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    out.boolean(connected);
    emitEvent(abi::kEvtConnectedChanged, out);
}

void BackendSession::configReceived(const cb::Completion &completion,
                                    const cb::BaseConfig &config) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    marshal::writeBaseConfig(out, config);
    emitEvent(abi::kEvtConfigReceived, out);
}

void BackendSession::modeChanged(const cb::Completion &completion, const QString &mode) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    out.text(mode);
    emitEvent(abi::kEvtModeChanged, out);
}

void BackendSession::tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept {
    settle(result.request);
    marshal::ByteWriter out;
    marshal::writeTunChangeCompleted(out, result);
    emitEvent(abi::kEvtTunChangeCompleted, out);
}

void BackendSession::nodeSelected(const cb::Completion &completion, const QString &group,
                                  const QString &node) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    out.text(group);
    out.text(node);
    emitEvent(abi::kEvtNodeSelected, out);
}

void BackendSession::geoDatabasesUpdated(const cb::Completion &completion) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    emitEvent(abi::kEvtGeoDatabasesUpdated, out);
}

void BackendSession::dnsQueryFinished(const cb::Completion &completion, const QString &name,
                                      const QString &resultJson) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    out.text(name);
    out.text(resultJson);
    emitEvent(abi::kEvtDnsQueryFinished, out);
}

void BackendSession::dnsCacheFlushed(const cb::Completion &completion, bool fakeIp) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    out.boolean(fakeIp);
    emitEvent(abi::kEvtDnsCacheFlushed, out);
}

void BackendSession::versionReceived(const cb::Completion &completion,
                                     const QString &version) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    out.text(version);
    emitEvent(abi::kEvtVersionReceived, out);
}

void BackendSession::proxiesUpdated(const cb::Completion &completion,
                                    cb::Span<cb::ProxyGroup> groups,
                                    cb::Span<cb::ProxyNode> nodes) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    marshal::writeSpan(out, groups, marshal::writeProxyGroup);
    marshal::writeSpan(out, nodes, marshal::writeProxyNode);
    emitEvent(abi::kEvtProxiesUpdated, out);
}

void BackendSession::rulesUpdated(const cb::Completion &completion,
                                  cb::Span<cb::Rule> rules) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    marshal::writeSpan(out, rules, marshal::writeRule);
    emitEvent(abi::kEvtRulesUpdated, out);
}

void BackendSession::trafficSample(cb::Generation generation, quint64 up, quint64 down) noexcept {
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    out.u64(up);
    out.u64(down);
    emitEvent(abi::kEvtTrafficSample, out);
}

void BackendSession::memorySample(cb::Generation generation, quint64 inuse,
                                  quint64 oslimit) noexcept {
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    out.u64(inuse);
    out.u64(oslimit);
    emitEvent(abi::kEvtMemorySample, out);
}

void BackendSession::connectionsUpdated(cb::Generation generation,
                                        cb::Span<cb::Connection> connections, quint64 uploadTotal,
                                        quint64 downloadTotal) noexcept {
    // The one payload that does not use the general encoding: one packed
    // buffer with fixed-layout records indexing a shared UTF-8 blob (D8).
    const std::vector<std::uint8_t> packed =
        marshal::packConnectionSnapshot(generation, connections, uploadTotal, downloadTotal);
    emitEventBytes(abi::kEvtConnectionsUpdated, packed.data(), packed.size());
}

void BackendSession::logReceived(cb::Generation generation, const cb::LogEntry &entry) noexcept {
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    marshal::writeLogEntry(out, entry);
    emitEvent(abi::kEvtLogReceived, out);
}

void BackendSession::providersReceived(const cb::Completion &completion, bool rules,
                                       cb::Span<cb::Provider> providers) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    out.boolean(rules);
    marshal::writeSpan(out, providers, marshal::writeProvider);
    emitEvent(abi::kEvtProvidersReceived, out);
}

void BackendSession::providerBusyChanged(cb::Generation generation, bool busy) noexcept {
    marshal::ByteWriter out;
    marshal::writeGeneration(out, generation);
    out.boolean(busy);
    emitEvent(abi::kEvtProviderBusyChanged, out);
}

void BackendSession::providerOperationFinished(const cb::Completion &completion,
                                               const QString &message) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    out.text(message);
    emitEvent(abi::kEvtProviderOperationFinished, out);
}

void BackendSession::privilegedServiceStatus(const cb::Completion &completion,
                                             const cb::PrivilegedServiceStatus &status) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    marshal::writePrivilegedServiceStatus(out, status);
    emitEvent(abi::kEvtPrivilegedServiceStatus, out);
}

void BackendSession::errorOccurred(const cb::Completion &completion) noexcept {
    settle(completion.request);
    marshal::ByteWriter out;
    marshal::writeCompletion(out, completion);
    emitEvent(abi::kEvtErrorOccurred, out);
}

}  // namespace core::module
