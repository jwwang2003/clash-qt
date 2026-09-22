#ifndef CLASHQT_CORE_BACKEND_ATTACHMENT_H
#define CLASHQT_CORE_BACKEND_ATTACHMENT_H

// BackendAttachment: the controller this component talks to, which may be the
// managed core's or any endpoint the user or discovery pointed us at.
// Contract: docs/module-api.md revision backend-r4, sections 1, 2, 5.
// An aborted request's completion is MARKED Superseded rather than left for the
// consumer to infer, and the snapshot re-issue obligation that follows every
// generation bump is discharged by an endpoint change on its own.

#include <QString>

#include "core/backend/types.h"

namespace core::backend {

// Thread affinity: the owning thread, as BackendLifecycle.
class BackendAttachment {
  public:
    // The endpoint startup attaches to before any managed launch (main.cpp:269).
    // Always returns a value; the localhost default when nothing is discovered.
    virtual Endpoint discoverEndpoint() const noexcept = 0;

    // Parses an external-controller out of a config file. Returns false and
    // leaves *out untouched when the file has none or cannot be read. An
    // out-parameter rather than std::optional, and false rather than a throw:
    // the configuration path is a yaml-cpp exception site and nothing may
    // unwind across this interface (contract section 9).
    // kInvalidArgument equivalent: out == nullptr returns false.
    virtual bool endpointFromConfigFile(const QString &path, Endpoint *out) const noexcept = 0;

    // Attaches to `endpoint`, overwriting any current attachment. A managed
    // launch overwrites a discovered attachment this way (main.cpp:111-112),
    // and that sequence must stay expressible.
    //
    // Bumps the generation BEFORE aborting the in-flight requests of the
    // previous endpoint (contract section 2's ordering rule), then publishes
    // the cleared live state, then re-opens whatever streams were open.
    // No observer is invoked before this call returns.
    //
    // Every request that abort abandons completes with
    // CompletionStatus::Superseded and ErrorCode::Superseded.
    // The consumer's generation comparison cannot see them on its own: they are
    // queued ahead of the endpointChanged event that announces the bump.
    //
    // This call also re-issues the snapshot set, which is what discharges the
    // re-issue obligation for an endpoint change rather than deferring it.
    virtual RequestId attach(const Endpoint &endpoint) noexcept = 0;

    // Stops talking to the current controller: bumps the generation, aborts
    // in-flight work (marking each abandoned completion Superseded),
    // clears live state, closes the streams. Nothing is re-issued: there is no
    // endpoint left to fetch from.
    //
    // NEVER terminates the controller. Detaching from an attached controller
    // leaves it running; only BackendLifecycle::stop() terminates anything, and
    // only a managed core (contract section 1).
    virtual RequestId detach() noexcept = 0;

    // By value. core::MihomoClient::endpoint() returns a const reference today;
    // contract section 9 forbids returning a reference into the component.
    virtual Endpoint currentEndpoint() const noexcept = 0;

    virtual bool isAttached() const noexcept = 0;
    virtual bool isConnected() const noexcept = 0;

    // Answers "is the controller we are attached to our own managed child?"
    // directly, instead of making the consumer compare host and port as
    // ui/main_window.cpp:324-328 does today.
    //   Managed  - attached to the endpoint of a core this component runs
    //   Attached - attached to something this component did not start
    //   None     - not attached
    virtual Ownership attachmentOwnership() const noexcept = 0;

    // The second question the UI asks today (ui/main_window.cpp:340): connected
    // to a controller that is not ours. Equivalent to
    //   isConnected() && attachmentOwnership() == Ownership::Attached
    // and published so the consumer does not have to know that.
    virtual bool isExternalControllerConnected() const noexcept = 0;

    // Completion: observer.configReceived() / observer.errorOccurred().
    virtual RequestId refreshConfig() noexcept = 0;

  protected:
    ~BackendAttachment() = default;
};

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_ATTACHMENT_H
