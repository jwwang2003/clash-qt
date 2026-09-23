#include "integrations/component/module_backend.h"

#include <utility>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "integrations/component/marshal/backend_marshal.h"
#include "integrations/component/marshal/com_objects.h"
#include "integrations/component/marshal/connection_snapshot.h"

namespace clashqt::integration {
namespace {

const marshal::ByteWriter &noArgs() {
    static const marshal::ByteWriter empty;
    return empty;
}

/// The module speaks component-r1's Result vocabulary; the application speaks
/// backend-r4's ErrorCode. One translation, in one place, so a boundary failure
/// arrives at the UI as something it already knows how to render.
cb::ErrorCode errorCodeFor(com::Result status) noexcept {
    switch (status) {
        case com::kOk:
        case com::kFalse:
            return cb::ErrorCode::None;
        case com::kNoInterface:
        case com::kNotImplemented:
        case com::kUnsupportedVersion:
            return cb::ErrorCode::NotSupported;
        case com::kInvalidArgument:
            return cb::ErrorCode::InvalidArgument;
        case com::kNotFound:
            return cb::ErrorCode::NotFound;
        case com::kTimeout:
            return cb::ErrorCode::Timeout;
        case com::kCancelled:
            return cb::ErrorCode::Cancelled;
        case com::kInvalidState:
        case com::kAlreadyClosed:
            return cb::ErrorCode::InvalidState;
        default:
            return cb::ErrorCode::Unspecified;
    }
}

QString jsonText(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QJsonObject jsonObjectFrom(const QString &text) {
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
    return document.isObject() ? document.object() : QJsonObject{};
}

}  // namespace

// ------------------------------------------------------------------- Host

com::Result ModuleBackend::Host::QueryInterface(const com::InterfaceId &id, void **out) noexcept {
    const com::InterfaceEntry entries[] = {
        com::MakeInterfaceEntry(static_cast<abi::IBackendHost *>(this)),
        com::InterfaceEntry{&com::kIObjectId,
                            static_cast<void *>(static_cast<com::IObject *>(
                                static_cast<abi::IBackendHost *>(this)))},
    };
    return com::ResolveInterface(id, out, entries, sizeof(entries) / sizeof(entries[0]),
                                 static_cast<abi::IBackendHost *>(this));
}

std::int32_t ModuleBackend::Host::AddRef() noexcept { return references_.Increment(); }

std::int32_t ModuleBackend::Host::Release() noexcept {
    const std::int32_t remaining = references_.Decrement();
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

com::Result ModuleBackend::Host::Notify(std::uint32_t event, const void *data,
                                        std::size_t size) noexcept {
    if (owner_ == nullptr) {
        // The backend is gone and the module has not finished letting go. Not
        // a failure of the module's: answering is what keeps it from treating
        // this as a protocol error.
        return com::kAlreadyClosed;
    }
    try {
        owner_->enqueue(event, data, size);
    } catch (...) {
        // Copying the payload is the only thing here that can fail, and this is
        // a COM entry point the module calls: an exception leaving it would
        // unwind into the module's runtime, which is undefined rather than
        // merely lossy. The module is told the event was not accepted.
        return com::kFail;
    }
    return com::kOk;
}

com::Result ModuleBackend::Host::Invoke(std::uint32_t command, const void *data, std::size_t size,
                                        com::IBuffer **reply) noexcept {
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (owner_ == nullptr) {
        return com::kAlreadyClosed;
    }
    try {
        return owner_->handleHostCommand(command, data, size, reply);
    } catch (...) {
        if (reply != nullptr) {
            *reply = nullptr;
        }
        return com::kFail;
    }
}

// ---------------------------------------------------------- construction

ModuleBackend::ModuleBackend(com::ComPtr<abi::IBackendSession> session,
                             core::PrivilegedCoreService *service)
    : session_(std::move(session)), service_(service) {
    if (!session_) {
        lastError_ = {cb::ErrorCode::InvalidState, QStringLiteral("no module session")};
        return;
    }
    host_ = new Host(this);  // created with one reference, owned here
    const com::Result status = session_->SetHost(host_);
    if (com::IsFailure(status)) {
        lastError_ = {errorCodeFor(status), QStringLiteral("the module refused the host")};
        host_->detachOwner();
        host_->Release();
        host_ = nullptr;
        session_.Reset();
        return;
    }
    valid_ = true;
}

ModuleBackend::~ModuleBackend() {
    // Close first, so nothing in the module can call back into an object whose
    // members are already gone. Then detach the host, so a module that has not
    // released its reference yet reaches a live object that refuses.
    shutdown(/*stopManagedCore=*/true, /*timeoutMs=*/3000);
    if (host_ != nullptr) {
        host_->detachOwner();
        host_->Release();
        host_ = nullptr;
    }
    session_.Reset();
}

bool ModuleBackend::shutdown(bool stopManagedCore, int timeoutMs) {
    if (!session_) {
        return true;
    }
    const std::uint32_t flags =
        stopManagedCore ? abi::kCloseStopManagedCore : abi::kCloseDefault;
    const com::Result status =
        session_->Close(flags, static_cast<std::uint32_t>(timeoutMs < 0 ? 0 : timeoutMs));
    valid_ = false;
    // The module detaches its own listener as part of closing, which is the
    // order that matters: its adapter stops being reachable before the host
    // reference goes. This is the fallback for a module that did NOT - the
    // host must not be left holding a listener into a closed session.
    //
    // KNOWN SURVIVING MUTANT, for the same reason as the delivery guard above:
    // the shipping module's CoreProcess detaches itself in its destructor and
    // the test double never attaches, so deleting these four lines fails
    // nothing today. It is the half of the seam that does not depend on the
    // module behaving, and a host that trusted the module here would hand a
    // dangling listener to the privileged client.
    if (service_ != nullptr && listenerActive_) {
        service_->setListener(nullptr);
        listenerActive_ = false;
    }
    // Whatever the module produced on the way out is still the consumer's to
    // see - including an unconfirmed stop, which is what blocks quit.
    drain(timeoutMs);
    if (status == com::kAlreadyClosed) {
        return true;
    }
    if (com::IsFailure(status)) {
        lastError_ = {errorCodeFor(status), QStringLiteral("the module did not close cleanly")};
        return false;
    }
    return true;
}

std::int32_t ModuleBackend::outstandingWork() const {
    return session_ ? session_->OutstandingWork() : 0;
}

std::int32_t ModuleBackend::pendingNativeWork() const {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdPendingNativeWork, noArgs(), &reply))) {
        // A module older than wire revision 3 answers kNotImplemented. Saying
        // -1 rather than 0 keeps "I cannot tell you" distinct from "nothing is
        // running": a caller that treated them alike would unmap on the
        // strength of an answer it never got.
        return -1;
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const std::int32_t pending = in.i32();
    return in.finished() ? pending : -1;
}

// ------------------------------------------------------- command plumbing

void ModuleBackend::noteFailure(std::uint32_t command, com::Result status) const {
    QString message = QStringLiteral("command 0x%1 failed (%2)").arg(command, 0, 16).arg(status);
    if (session_) {
        com::IErrorInfo *error = nullptr;
        if (com::IsSuccess(session_->LastError(&error)) && error != nullptr) {
            // component-r1: the diagnostic comes from the failing object, not
            // from thread-local state that cannot survive a boundary.
            const QString detail = marshal::ErrorMessageOf(error);
            error->Release();
            if (!detail.isEmpty()) {
                message = detail;
            }
        }
    }
    lastError_ = {errorCodeFor(status), message};
}

com::Result ModuleBackend::call(std::uint32_t command, const marshal::ByteWriter &args,
                                std::vector<std::uint8_t> *reply) const {
    if (!session_) {
        lastError_ = {cb::ErrorCode::InvalidState, QStringLiteral("no module session")};
        return com::kInvalidState;
    }
    CommandScope scope(this);
    com::IBuffer *buffer = nullptr;
    const com::Result status = session_->Invoke(
        command, args.isEmpty() ? nullptr : args.data().data(), args.size(),
        reply == nullptr ? nullptr : &buffer);
    if (com::IsFailure(status)) {
        if (buffer != nullptr) {
            buffer->Release();
        }
        noteFailure(command, status);
        return status;
    }
    if (reply != nullptr) {
        *reply = marshal::ReadBuffer(buffer);
    }
    if (buffer != nullptr) {
        // The module allocated it; the module frees it when this reference
        // goes. Copying out first is what keeps that true.
        buffer->Release();
    }
    lastError_ = {};
    return status;
}

cb::RequestId ModuleBackend::requestCall(std::uint32_t command,
                                         const marshal::ByteWriter &args) const {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(command, args, &reply))) {
        return cb::RequestId::Invalid;
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const cb::RequestId id = marshal::readRequestId(in);
    return in.finished() ? id : cb::RequestId::Invalid;
}

bool ModuleBackend::flagCall(std::uint32_t command, const marshal::ByteWriter &args) const {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(command, args, &reply))) {
        return false;
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const bool value = in.boolean();
    return in.finished() && value;
}

QString ModuleBackend::textCall(std::uint32_t command, const marshal::ByteWriter &args) const {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(command, args, &reply))) {
        return {};
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const QString value = in.text();
    return in.finished() ? value : QString();
}

std::uint8_t ModuleBackend::byteCall(std::uint32_t command, const marshal::ByteWriter &args,
                                     std::uint8_t fallback) const {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(command, args, &reply))) {
        return fallback;
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const std::uint8_t value = in.u8();
    return in.finished() ? value : fallback;
}

// ------------------------------------------------------------- observers

std::uint64_t ModuleBackend::producedSequence() const {
    // Asked of the MODULE, not counted here. The module's wrapped backend is
    // where events are produced; this side only ever sees them arrive, and the
    // gap between the two is exactly the window an arrival counter would get
    // wrong. A module that cannot answer reports 0, and 0 admits everything -
    // the behaviour before the envelope existed, degraded honestly.
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdProducedSequence, marshal::ByteWriter{}, &reply))) {
        return 0;
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const std::uint64_t sequence = in.u64();
    return in.finished() ? sequence : 0;
}

bool ModuleBackend::addObserver(cb::BackendObserver *observer) noexcept {
    if (observer == nullptr) {
        return false;
    }
    for (cb::BackendObserver *known : observers_) {
        if (known == observer) {
            return false;
        }
    }
    // Always the module's PRODUCED sequence, including from inside a delivery.
    // That is exactly what the in-process dispatcher records - "sequence_ + 1",
    // the next event it has not produced yet - so a mid-delivery registration
    // is excluded from the rest of the batch on both sides, and so are events
    // the module has produced but not yet forwarded. Using the sequence of the
    // event being delivered instead would admit the remainder of the batch and
    // make the module-backed backend more generous than the direct one.
    std::uint64_t admitAfter = 0;
    try {
        admitAfter = producedSequence();
        observers_.push_back(observer);
        admittedFrom_.emplace_back(observer, admitAfter);
    } catch (...) {
        // backend-r4 declares this noexcept. A registration that could not be
        // recorded is reported as a refusal, not as a half-registered observer
        // that would receive events with no admission rule attached.
        if (!observers_.empty() && observers_.back() == observer) {
            observers_.pop_back();
        }
        return false;
    }
    return true;
}

bool ModuleBackend::removeObserver(cb::BackendObserver *observer) noexcept {
    for (auto it = observers_.begin(); it != observers_.end(); ++it) {
        if (*it == observer) {
            // Erasing from the live list is what makes removal during delivery
            // safe: the delivery loop re-checks membership before every call,
            // so a removed observer receives nothing further, including events
            // already queued.
            observers_.erase(it);
            for (auto entry = admittedFrom_.begin(); entry != admittedFrom_.end(); ++entry) {
                if (entry->first == observer) {
                    admittedFrom_.erase(entry);
                    break;
                }
            }
            return true;
        }
    }
    return false;
}

template <typename Callback>
void ModuleBackend::forEachObserver(Callback callback) const {
    const std::vector<cb::BackendObserver *> snapshot = observers_;
    for (cb::BackendObserver *observer : snapshot) {
        bool stillRegistered = false;
        for (cb::BackendObserver *live : observers_) {
            if (live == observer) {
                stillRegistered = true;
                break;
            }
        }
        if (!stillRegistered) {
            continue;
        }
        // Admitted by PRODUCTION. An observer registered after the module
        // produced this event does not receive it, however long the event spent
        // in transit or in this queue.
        bool admitted = true;
        for (const auto &entry : admittedFrom_) {
            if (entry.first == observer) {
                // Sequence 0 is wire.h's "this module does not sequence its
                // events". Admitting everything then is the behaviour that
                // existed before the envelope, which is a degradation a
                // consumer can reason about; admitting nothing would be a
                // silent failure that looks like a working backend.
                admitted = deliveringSequence_ == 0 || deliveringSequence_ > entry.second;
                break;
            }
        }
        if (admitted) {
            callback(*observer);
        }
    }
}

// ---------------------------------------------------------------- events

void ModuleBackend::enqueue(std::uint32_t event, const void *data, std::size_t size) {
    const auto *first = static_cast<const std::uint8_t *>(data);
    // wire.h's envelope comes off HERE, once, so nothing below this line has to
    // know it exists - including the packed connection snapshot, whose decoder
    // validates from offset 0 of what it is given.
    QueuedEvent queued;
    if (first != nullptr && size >= abi::kEventEnvelopeBytes) {
        for (std::size_t byte = 0; byte < abi::kEventEnvelopeBytes; ++byte) {
            queued.sequence |= static_cast<std::uint64_t>(first[byte]) << (byte * 8);
        }
        queued.payload.assign(first + abi::kEventEnvelopeBytes, first + size);
    } else if (size != 0) {
        // Too short to carry an envelope. Counted as unknown rather than
        // decoded from the middle: a payload that does not start where the
        // protocol says it does is a peer disagreeing about the protocol.
        ++unknownEvents_;
        return;
    }
    queued.event = event;
    queue_.push_back(std::move(queued));
    if (commandDepth_ > 0 || delivering_) {
        // Inside one of our own calls, or already draining: the queue is
        // drained when that unwinds. This is the host half of backend-r4's
        // no-re-entrant-delivery rule.
        //
        // KNOWN SURVIVING MUTANT, deliberately kept. Deleting the
        // `commandDepth_ > 0` term does not fail the suite, because both
        // modules in this tree wrap a backend that already queues its
        // callbacks, so no Notify ever arrives inside a command and the term
        // is never reached. It is defence in depth for a module that delivers
        // synchronously - which the ABI permits, and across a raw boundary the
        // non-re-entrancy guarantee has to be re-established rather than
        // inherited. Recorded rather than removed: a surviving mutant is
        // either a coverage gap or a redundancy, and this one is a redundancy
        // only for the modules that exist today.
        if (commandDepth_ == 0 && !drainScheduled_) {
            drainScheduled_ = true;
            QTimer::singleShot(0, &pump_, [this] {
                drainScheduled_ = false;
                deliverQueued();
            });
        }
        return;
    }
    deliverQueued();
}

void ModuleBackend::deliverQueued() const {
    if (delivering_ || commandDepth_ > 0) {
        return;
    }
    delivering_ = true;
    while (!queue_.empty()) {
        const QueuedEvent event = std::move(queue_.front());
        queue_.pop_front();
        // Published for the whole of this dispatch: forEachObserver compares it
        // against what each observer was admitted from, and addObserver uses it
        // for anything that registers from inside a callback.
        deliveringSequence_ = event.sequence;
        dispatchEvent(event.event, event.payload);
    }
    deliveringSequence_ = 0;
    delivering_ = false;
}

bool ModuleBackend::drain(int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    // QEventLoop::processEvents reports whether it did anything;
    // QCoreApplication's does not, and a drain that cannot tell an idle loop
    // from a busy one either spins for the whole timeout or returns too early.
    QEventLoop loop;
    do {
        const bool worked = loop.processEvents(QEventLoop::AllEvents);
        deliverQueued();
        if (queue_.empty() && !worked) {
            break;
        }
    } while (elapsed.elapsed() < static_cast<qint64>(timeoutMs));
    deliverQueued();
    return queue_.empty();
}

void ModuleBackend::dispatchEvent(std::uint32_t event,
                                  const std::vector<std::uint8_t> &payload) const {
    marshal::ByteReader in(payload.data(), payload.size());

    switch (event) {
        case abi::kEvtCoreStateChanged: {
            const cb::Generation generation = marshal::readGeneration(in);
            const auto state = static_cast<cb::CoreState>(in.u8());
            const auto ownership = static_cast<cb::Ownership>(in.u8());
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.coreStateChanged(generation, state, ownership);
            });
            return;
        }
        case abi::kEvtCoreReady: {
            const cb::Completion completion = marshal::readCompletion(in);
            const cb::Endpoint endpoint = marshal::readEndpoint(in);
            if (!in.finished()) break;
            forEachObserver(
                [&](cb::BackendObserver &observer) { observer.coreReady(completion, endpoint); });
            return;
        }
        case abi::kEvtCoreLogLine: {
            const cb::Generation generation = marshal::readGeneration(in);
            const QString line = in.text();
            if (!in.finished()) break;
            forEachObserver(
                [&](cb::BackendObserver &observer) { observer.coreLogLine(generation, line); });
            return;
        }
        case abi::kEvtCoreFailed: {
            const cb::Completion completion = marshal::readCompletion(in);
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) { observer.coreFailed(completion); });
            return;
        }
        case abi::kEvtCoreStopped: {
            const cb::Generation generation = marshal::readGeneration(in);
            if (!in.finished()) break;
            forEachObserver(
                [&](cb::BackendObserver &observer) { observer.coreStopped(generation); });
            return;
        }
        case abi::kEvtStopCompleted: {
            const cb::StopCompleted result = marshal::readStopCompleted(in);
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) { observer.stopCompleted(result); });
            return;
        }
        case abi::kEvtEndpointChanged: {
            const cb::Generation generation = marshal::readGeneration(in);
            const cb::Endpoint endpoint = marshal::readEndpoint(in);
            const auto ownership = static_cast<cb::Ownership>(in.u8());
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.endpointChanged(generation, endpoint, ownership);
            });
            return;
        }
        case abi::kEvtConnectedChanged: {
            const cb::Generation generation = marshal::readGeneration(in);
            const bool connected = in.boolean();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.connectedChanged(generation, connected);
            });
            return;
        }
        case abi::kEvtConfigReceived: {
            const cb::Completion completion = marshal::readCompletion(in);
            const cb::BaseConfig config = marshal::readBaseConfig(in);
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.configReceived(completion, config);
            });
            return;
        }
        case abi::kEvtModeChanged: {
            const cb::Completion completion = marshal::readCompletion(in);
            const QString mode = in.text();
            if (!in.finished()) break;
            forEachObserver(
                [&](cb::BackendObserver &observer) { observer.modeChanged(completion, mode); });
            return;
        }
        case abi::kEvtTunChangeCompleted: {
            const cb::TunChangeCompleted result = marshal::readTunChangeCompleted(in);
            if (!in.finished()) break;
            forEachObserver(
                [&](cb::BackendObserver &observer) { observer.tunChangeCompleted(result); });
            return;
        }
        case abi::kEvtNodeSelected: {
            const cb::Completion completion = marshal::readCompletion(in);
            const QString group = in.text();
            const QString node = in.text();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.nodeSelected(completion, group, node);
            });
            return;
        }
        case abi::kEvtGeoDatabasesUpdated: {
            const cb::Completion completion = marshal::readCompletion(in);
            if (!in.finished()) break;
            forEachObserver(
                [&](cb::BackendObserver &observer) { observer.geoDatabasesUpdated(completion); });
            return;
        }
        case abi::kEvtDnsQueryFinished: {
            const cb::Completion completion = marshal::readCompletion(in);
            const QString name = in.text();
            const QString resultJson = in.text();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.dnsQueryFinished(completion, name, resultJson);
            });
            return;
        }
        case abi::kEvtDnsCacheFlushed: {
            const cb::Completion completion = marshal::readCompletion(in);
            const bool fakeIp = in.boolean();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.dnsCacheFlushed(completion, fakeIp);
            });
            return;
        }
        case abi::kEvtVersionReceived: {
            const cb::Completion completion = marshal::readCompletion(in);
            const QString version = in.text();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.versionReceived(completion, version);
            });
            return;
        }
        case abi::kEvtProxiesUpdated: {
            const cb::Completion completion = marshal::readCompletion(in);
            const QVector<cb::ProxyGroup> groups = marshal::readVector<cb::ProxyGroup>(
                in, marshal::readProxyGroup, marshal::kMinProxyGroupBytes);
            const QVector<cb::ProxyNode> nodes = marshal::readVector<cb::ProxyNode>(
                in, marshal::readProxyNode, marshal::kMinProxyNodeBytes);
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                // The spans borrow these local containers, which outlive the
                // callback - exactly the lifetime rule backend-r4 section 9
                // puts on every collection handed to a consumer.
                observer.proxiesUpdated(completion, cb::makeSpan(groups), cb::makeSpan(nodes));
            });
            return;
        }
        case abi::kEvtRulesUpdated: {
            const cb::Completion completion = marshal::readCompletion(in);
            const QVector<cb::Rule> rules =
                marshal::readVector<cb::Rule>(in, marshal::readRule, marshal::kMinRuleBytes);
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.rulesUpdated(completion, cb::makeSpan(rules));
            });
            return;
        }
        case abi::kEvtTrafficSample: {
            const cb::Generation generation = marshal::readGeneration(in);
            const quint64 up = in.u64();
            const quint64 down = in.u64();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.trafficSample(generation, up, down);
            });
            return;
        }
        case abi::kEvtMemorySample: {
            const cb::Generation generation = marshal::readGeneration(in);
            const quint64 inuse = in.u64();
            const quint64 oslimit = in.u64();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.memorySample(generation, inuse, oslimit);
            });
            return;
        }
        case abi::kEvtConnectionsUpdated: {
            marshal::ConnectionSnapshot snapshot;
            QString reason;
            if (!marshal::unpackConnectionSnapshot(payload.data(), payload.size(), &snapshot,
                                                   &reason)) {
                lastError_ = {cb::ErrorCode::Protocol,
                              QStringLiteral("malformed connections snapshot: %1").arg(reason)};
                forEachObserver([&](cb::BackendObserver &observer) {
                    observer.errorOccurred(cb::Completion{cb::RequestId::Invalid,
                                                          cb::Generation::Initial,
                                                          cb::CompletionStatus::Failed, lastError_});
                });
                return;
            }
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.connectionsUpdated(snapshot.generation,
                                            cb::makeSpan(snapshot.connections),
                                            snapshot.uploadTotal, snapshot.downloadTotal);
            });
            return;
        }
        case abi::kEvtLogReceived: {
            const cb::Generation generation = marshal::readGeneration(in);
            const cb::LogEntry entry = marshal::readLogEntry(in);
            if (!in.finished()) break;
            forEachObserver(
                [&](cb::BackendObserver &observer) { observer.logReceived(generation, entry); });
            return;
        }
        case abi::kEvtProvidersReceived: {
            const cb::Completion completion = marshal::readCompletion(in);
            const bool rules = in.boolean();
            const QVector<cb::Provider> providers = marshal::readVector<cb::Provider>(
                in, marshal::readProvider, marshal::kMinProviderBytes);
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.providersReceived(completion, rules, cb::makeSpan(providers));
            });
            return;
        }
        case abi::kEvtProviderBusyChanged: {
            const cb::Generation generation = marshal::readGeneration(in);
            const bool busy = in.boolean();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.providerBusyChanged(generation, busy);
            });
            return;
        }
        case abi::kEvtProviderOperationFinished: {
            const cb::Completion completion = marshal::readCompletion(in);
            const QString message = in.text();
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.providerOperationFinished(completion, message);
            });
            return;
        }
        case abi::kEvtPrivilegedServiceStatus: {
            const cb::Completion completion = marshal::readCompletion(in);
            const cb::PrivilegedServiceStatus status = marshal::readPrivilegedServiceStatus(in);
            if (!in.finished()) break;
            forEachObserver([&](cb::BackendObserver &observer) {
                observer.privilegedServiceStatus(completion, status);
            });
            return;
        }
        case abi::kEvtErrorOccurred: {
            const cb::Completion completion = marshal::readCompletion(in);
            if (!in.finished()) break;
            forEachObserver(
                [&](cb::BackendObserver &observer) { observer.errorOccurred(completion); });
            return;
        }
        default:
            // An event code this host does not know. Counted, not absorbed:
            // with a matching handshake it can only mean a producer bug.
            ++unknownEvents_;
            return;
    }

    // Fell out of a case: the payload did not decode. Reported on the error
    // channel rather than dropped, because a malformed packet that nobody
    // mentions is how a producer bug survives to the next release.
    lastError_ = {cb::ErrorCode::Protocol,
                  QStringLiteral("event 0x%1 did not decode").arg(event, 0, 16)};
    const cb::ErrorInfo error = lastError_;
    forEachObserver([&](cb::BackendObserver &observer) {
        observer.errorOccurred(cb::Completion{cb::RequestId::Invalid, cb::Generation::Initial,
                                              cb::CompletionStatus::Failed, error});
    });
}

// ------------------------------------------- the privileged reverse seam

com::Result ModuleBackend::handleHostCommand(std::uint32_t command, const void *data,
                                             std::size_t size, com::IBuffer **reply) {
    marshal::ByteReader in(data, size);
    marshal::ByteWriter out;

    const auto answerFlag = [&](bool value) {
        out.boolean(value);
        return com::kOk;
    };

    com::Result status = com::kNotImplemented;
    switch (command) {
        case abi::kHostPrivilegedSetListenerActive: {
            const bool active = in.boolean();
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            if (service_ == nullptr) {
                return com::kInvalidState;
            }
            if (active && listenerActive_) {
                // ONE connection only. Two live connections to one privileged
                // socket is a correctness hazard; the boundary does not get to
                // reintroduce it.
                return com::kInvalidState;
            }
            if (!active && !listenerActive_) {
                // Already detached. Answering kOk rather than re-detaching
                // keeps the host's service from seeing a second setListener
                // for one detach: the component's own teardown and this
                // object's both travel this path.
                return com::kOk;
            }
            service_->setListener(active ? this : nullptr);
            listenerActive_ = active;
            return com::kOk;
        }
        case abi::kHostPrivilegedIsSupported:
            status = answerFlag(service_ != nullptr && service_->isSupported());
            break;
        case abi::kHostPrivilegedIsAvailable:
            status = answerFlag(service_ != nullptr && service_->isAvailable());
            break;
        case abi::kHostPrivilegedIsConnected:
            status = answerFlag(service_ != nullptr && service_->isConnected());
            break;
        case abi::kHostPrivilegedIsBusy:
            status = answerFlag(service_ != nullptr && service_->isBusy());
            break;
        case abi::kHostPrivilegedConnectionError:
            out.text(service_ == nullptr ? QString() : service_->connectionError());
            status = com::kOk;
            break;
        case abi::kHostPrivilegedRequestStatus:
            if (service_ == nullptr) return com::kInvalidState;
            service_->requestStatus();
            status = com::kOk;
            break;
        case abi::kHostPrivilegedRequestLogs:
            if (service_ == nullptr) return com::kInvalidState;
            service_->requestLogs();
            status = com::kOk;
            break;
        case abi::kHostPrivilegedStartCore: {
            const QString json = in.text();
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            if (service_ == nullptr) return com::kInvalidState;
            service_->startCore(jsonObjectFrom(json));
            return com::kOk;
        }
        case abi::kHostPrivilegedStopCore:
            if (service_ == nullptr) return com::kInvalidState;
            service_->stopCore();
            status = com::kOk;
            break;
        case abi::kHostPrivilegedClose:
            if (service_ == nullptr) return com::kInvalidState;
            service_->close();
            status = com::kOk;
            break;
        default:
            return com::kNotImplemented;
    }

    if (com::IsFailure(status)) {
        return status;
    }
    if (!in.finished()) {
        return com::kInvalidArgument;
    }
    if (reply != nullptr && !out.isEmpty()) {
        auto *buffer = marshal::ByteBuffer::Create(out.data().data(), out.size());
        if (buffer == nullptr) {
            return com::kFail;
        }
        // Allocated by the HOST, released by the module: the mirror image of
        // every reply the module sends, and for the same reason.
        *reply = static_cast<com::IBuffer *>(buffer);
    }
    return com::kOk;
}

void ModuleBackend::sendToModule(std::uint32_t command, const marshal::ByteWriter &args) {
    if (!session_) {
        return;
    }
    session_->Invoke(command, args.isEmpty() ? nullptr : args.data().data(), args.size(), nullptr);
}

void ModuleBackend::privilegedConnectedChanged(bool connected) {
    marshal::ByteWriter args;
    args.boolean(connected);
    sendToModule(abi::kCmdPrivilegedConnectedChanged, args);
}

void ModuleBackend::privilegedStatusReceived(const QJsonObject &status) {
    marshal::ByteWriter args;
    args.text(jsonText(status));
    sendToModule(abi::kCmdPrivilegedStatusReceived, args);
}

void ModuleBackend::privilegedCoreStarted(const QJsonObject &endpoint) {
    marshal::ByteWriter args;
    args.text(jsonText(endpoint));
    sendToModule(abi::kCmdPrivilegedCoreStarted, args);
}

void ModuleBackend::privilegedCoreStopped() {
    sendToModule(abi::kCmdPrivilegedCoreStopped, noArgs());
}

void ModuleBackend::privilegedLogsReceived(const QString &logs) {
    marshal::ByteWriter args;
    args.text(logs);
    sendToModule(abi::kCmdPrivilegedLogsReceived, args);
}

void ModuleBackend::privilegedRequestFinished(const QString &operation, bool success,
                                              const QString &error) {
    marshal::ByteWriter args;
    args.text(operation);
    args.boolean(success);
    args.text(error);
    sendToModule(abi::kCmdPrivilegedRequestFinished, args);
}

// ------------------------------------------------------- MihomoBackend

cb::Generation ModuleBackend::generation() const noexcept {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdGeneration, noArgs(), &reply))) {
        return cb::Generation::Initial;
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const cb::Generation value = marshal::readGeneration(in);
    return in.finished() ? value : cb::Generation::Initial;
}

QString ModuleBackend::discoverBinary() const noexcept {
    return textCall(abi::kCmdDiscoverBinary, noArgs());
}

void ModuleBackend::setBinaryPath(const QString &path) noexcept {
    marshal::ByteWriter args;
    args.text(path);
    call(abi::kCmdSetBinaryPath, args, nullptr);
}

QString ModuleBackend::binaryPath() const noexcept { return textCall(abi::kCmdBinaryPath, noArgs()); }

bool ModuleBackend::setExecutionMode(cb::ExecutionMode mode) noexcept {
    marshal::ByteWriter args;
    args.u8(static_cast<std::uint8_t>(mode));
    return flagCall(abi::kCmdSetExecutionMode, args);
}

cb::ExecutionMode ModuleBackend::executionMode() const noexcept {
    return static_cast<cb::ExecutionMode>(
        byteCall(abi::kCmdExecutionMode, noArgs(), static_cast<std::uint8_t>(cb::ExecutionMode::Managed)));
}

bool ModuleBackend::usesPrivilegedService() const noexcept {
    return flagCall(abi::kCmdUsesPrivilegedService, noArgs());
}

cb::RequestId ModuleBackend::start(const QString &configPath, const QString &workDir) noexcept {
    marshal::ByteWriter args;
    args.text(configPath);
    args.text(workDir);
    return requestCall(abi::kCmdStart, args);
}

cb::RequestId ModuleBackend::stop() noexcept { return requestCall(abi::kCmdStop, noArgs()); }

cb::CoreState ModuleBackend::state() const noexcept {
    return static_cast<cb::CoreState>(
        byteCall(abi::kCmdState, noArgs(), static_cast<std::uint8_t>(cb::CoreState::Failed)));
}

cb::Ownership ModuleBackend::ownership() const noexcept {
    return static_cast<cb::Ownership>(
        byteCall(abi::kCmdOwnership, noArgs(), static_cast<std::uint8_t>(cb::Ownership::None)));
}

cb::Endpoint ModuleBackend::managedEndpoint() const noexcept {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdManagedEndpoint, noArgs(), &reply))) {
        return {};
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const cb::Endpoint endpoint = marshal::readEndpoint(in);
    return in.finished() ? endpoint : cb::Endpoint{};
}

QStringList ModuleBackend::activeConfigPaths() const noexcept {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdActiveConfigPaths, noArgs(), &reply))) {
        return {};
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const QStringList paths = in.textList();
    return in.finished() ? paths : QStringList{};
}

bool ModuleBackend::isRestartPending() const noexcept {
    return flagCall(abi::kCmdIsRestartPending, noArgs());
}

bool ModuleBackend::isManagedCoreActive() const noexcept {
    return flagCall(abi::kCmdIsManagedCoreActive, noArgs());
}

cb::Endpoint ModuleBackend::discoverEndpoint() const noexcept {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdDiscoverEndpoint, noArgs(), &reply))) {
        return {};
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const cb::Endpoint endpoint = marshal::readEndpoint(in);
    return in.finished() ? endpoint : cb::Endpoint{};
}

bool ModuleBackend::endpointFromConfigFile(const QString &path,
                                           cb::Endpoint *out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    marshal::ByteWriter args;
    args.text(path);
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdEndpointFromConfigFile, args, &reply))) {
        return false;
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const bool parsed = in.boolean();
    const cb::Endpoint endpoint = marshal::readEndpoint(in);
    if (!in.finished() || !parsed) {
        // *out is left untouched when the answer is false, exactly as
        // BackendAttachment documents.
        return false;
    }
    *out = endpoint;
    return true;
}

cb::RequestId ModuleBackend::attach(const cb::Endpoint &endpoint) noexcept {
    marshal::ByteWriter args;
    marshal::writeEndpoint(args, endpoint);
    return requestCall(abi::kCmdAttach, args);
}

cb::RequestId ModuleBackend::detach() noexcept { return requestCall(abi::kCmdDetach, noArgs()); }

cb::Endpoint ModuleBackend::currentEndpoint() const noexcept {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdCurrentEndpoint, noArgs(), &reply))) {
        return {};
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const cb::Endpoint endpoint = marshal::readEndpoint(in);
    return in.finished() ? endpoint : cb::Endpoint{};
}

bool ModuleBackend::isAttached() const noexcept { return flagCall(abi::kCmdIsAttached, noArgs()); }

bool ModuleBackend::isConnected() const noexcept { return flagCall(abi::kCmdIsConnected, noArgs()); }

cb::Ownership ModuleBackend::attachmentOwnership() const noexcept {
    return static_cast<cb::Ownership>(byteCall(abi::kCmdAttachmentOwnership, noArgs(),
                                               static_cast<std::uint8_t>(cb::Ownership::None)));
}

bool ModuleBackend::isExternalControllerConnected() const noexcept {
    return flagCall(abi::kCmdIsExternalControllerConnected, noArgs());
}

cb::RequestId ModuleBackend::refreshConfig() noexcept {
    return requestCall(abi::kCmdRefreshConfig, noArgs());
}

cb::RequestId ModuleBackend::setMode(const QString &mode) noexcept {
    marshal::ByteWriter args;
    args.text(mode);
    return requestCall(abi::kCmdSetMode, args);
}

cb::RequestId ModuleBackend::setTunEnabled(bool enabled) noexcept {
    marshal::ByteWriter args;
    args.boolean(enabled);
    return requestCall(abi::kCmdSetTunEnabled, args);
}

bool ModuleBackend::isTunChangePending() const noexcept {
    return flagCall(abi::kCmdIsTunChangePending, noArgs());
}

cb::RequestId ModuleBackend::selectNode(const QString &group, const QString &node) noexcept {
    marshal::ByteWriter args;
    args.text(group);
    args.text(node);
    return requestCall(abi::kCmdSelectNode, args);
}

cb::RequestId ModuleBackend::resetGroupSelection(const QString &group) noexcept {
    marshal::ByteWriter args;
    args.text(group);
    return requestCall(abi::kCmdResetGroupSelection, args);
}

cb::RequestId ModuleBackend::testGroupDelay(const QString &group) noexcept {
    marshal::ByteWriter args;
    args.text(group);
    return requestCall(abi::kCmdTestGroupDelay, args);
}

cb::RequestId ModuleBackend::testNodeDelay(const QString &node) noexcept {
    marshal::ByteWriter args;
    args.text(node);
    return requestCall(abi::kCmdTestNodeDelay, args);
}

cb::RequestId ModuleBackend::closeConnection(const QString &id) noexcept {
    marshal::ByteWriter args;
    args.text(id);
    return requestCall(abi::kCmdCloseConnection, args);
}

cb::RequestId ModuleBackend::closeAllConnections() noexcept {
    return requestCall(abi::kCmdCloseAllConnections, noArgs());
}

cb::RequestId ModuleBackend::updateGeoDatabases() noexcept {
    return requestCall(abi::kCmdUpdateGeoDatabases, noArgs());
}

cb::RequestId ModuleBackend::queryDns(const QString &name, const QString &type) noexcept {
    marshal::ByteWriter args;
    args.text(name);
    args.text(type);
    return requestCall(abi::kCmdQueryDns, args);
}

cb::RequestId ModuleBackend::flushDnsCache(bool fakeIp) noexcept {
    marshal::ByteWriter args;
    args.boolean(fakeIp);
    return requestCall(abi::kCmdFlushDnsCache, args);
}

cb::RequestId ModuleBackend::refreshVersion() noexcept {
    return requestCall(abi::kCmdRefreshVersion, noArgs());
}

cb::RequestId ModuleBackend::refreshProxies() noexcept {
    return requestCall(abi::kCmdRefreshProxies, noArgs());
}

cb::RequestId ModuleBackend::refreshRules() noexcept {
    return requestCall(abi::kCmdRefreshRules, noArgs());
}

cb::RequestId ModuleBackend::openTrafficStream() noexcept {
    return requestCall(abi::kCmdOpenTrafficStream, noArgs());
}

cb::RequestId ModuleBackend::closeTrafficStream() noexcept {
    return requestCall(abi::kCmdCloseTrafficStream, noArgs());
}

cb::RequestId ModuleBackend::openConnectionsStream() noexcept {
    return requestCall(abi::kCmdOpenConnectionsStream, noArgs());
}

cb::RequestId ModuleBackend::closeConnectionsStream() noexcept {
    return requestCall(abi::kCmdCloseConnectionsStream, noArgs());
}

cb::RequestId ModuleBackend::openLogStream(const QString &level) noexcept {
    marshal::ByteWriter args;
    args.text(level);
    return requestCall(abi::kCmdOpenLogStream, args);
}

cb::RequestId ModuleBackend::closeLogStream() noexcept {
    return requestCall(abi::kCmdCloseLogStream, noArgs());
}

cb::RequestId ModuleBackend::openMemoryStream() noexcept {
    return requestCall(abi::kCmdOpenMemoryStream, noArgs());
}

cb::RequestId ModuleBackend::closeMemoryStream() noexcept {
    return requestCall(abi::kCmdCloseMemoryStream, noArgs());
}

cb::RequestId ModuleBackend::fetchProviders(bool rules) noexcept {
    marshal::ByteWriter args;
    args.boolean(rules);
    return requestCall(abi::kCmdFetchProviders, args);
}

cb::RequestId ModuleBackend::updateProvider(bool rules, const QString &name) noexcept {
    marshal::ByteWriter args;
    args.boolean(rules);
    args.text(name);
    return requestCall(abi::kCmdUpdateProvider, args);
}

cb::RequestId ModuleBackend::healthCheckProvider(const QString &name) noexcept {
    marshal::ByteWriter args;
    args.text(name);
    return requestCall(abi::kCmdHealthCheckProvider, args);
}

bool ModuleBackend::isProviderBusy() const noexcept {
    return flagCall(abi::kCmdIsProviderBusy, noArgs());
}

cb::BackendIdentity ModuleBackend::identity() const noexcept {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdIdentity, noArgs(), &reply))) {
        return {};
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const cb::BackendIdentity identity = marshal::readIdentity(in);
    return in.finished() ? identity : cb::BackendIdentity{};
}

cb::FeatureSet ModuleBackend::features() const noexcept {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdFeatures, noArgs(), &reply))) {
        return {};
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const std::uint32_t bits = in.u32();
    return in.finished() ? cb::FeatureSet{bits} : cb::FeatureSet{};
}

bool ModuleBackend::serviceSupported() const noexcept {
    return flagCall(abi::kCmdServiceSupported, noArgs());
}

bool ModuleBackend::serviceAvailable() const noexcept {
    return flagCall(abi::kCmdServiceAvailable, noArgs());
}

cb::BackendTimings ModuleBackend::timings() const noexcept {
    std::vector<std::uint8_t> reply;
    if (com::IsFailure(call(abi::kCmdTimings, noArgs(), &reply))) {
        return {};
    }
    marshal::ByteReader in(reply.data(), reply.size());
    const cb::BackendTimings timings = marshal::readTimings(in);
    return in.finished() ? timings : cb::BackendTimings{};
}

cb::RequestId ModuleBackend::requestPrivilegedServiceStatus() noexcept {
    return requestCall(abi::kCmdRequestPrivilegedServiceStatus, noArgs());
}

}  // namespace clashqt::integration
