// A BackendObserver that records what it was told, and that APPLIES THE
// CONSUMER OBLIGATION of backend-r1 section 2 so the tests can assert on it:
// an event whose generation is older than the newest one already observed is
// rejected, as is a completion the backend marked Superseded.
//
// Rejected events are counted, not discarded silently, so "the guard fired" and
// "nothing arrived" are different observations.
#ifndef CLASHQT_TESTS_SUPPORT_BACKEND_RECORDING_OBSERVER_H
#define CLASHQT_TESTS_SUPPORT_BACKEND_RECORDING_OBSERVER_H

#include <functional>
#include <vector>

#include <QString>
#include <QStringList>
#include <QVector>

#include "core/backend/backend.h"

namespace testsupport::backend {

namespace cb = core::backend;

class FakeBackend;

class RecordingObserver final : public cb::BackendObserver {
  public:
    // `backend` is only used to assert that no callback arrives from inside a
    // mutating call; it is never kept past this object's lifetime.
    explicit RecordingObserver(FakeBackend *backend = nullptr, QString name = QStringLiteral("o"));
    ~RecordingObserver() override;

    struct StateEvent {
        cb::Generation generation = cb::Generation::Initial;
        cb::CoreState state = cb::CoreState::Stopped;
        cb::Ownership ownership = cb::Ownership::None;
    };
    struct CompletionEvent {
        QString channel;
        cb::Completion completion;
        QString payload;  // a printable digest, enough to prove which data arrived
    };

    // --- what arrived and was accepted
    std::vector<StateEvent> states;
    std::vector<CompletionEvent> accepted;
    // --- what the section 2 guard turned away
    std::vector<CompletionEvent> rejectedByGeneration;
    std::vector<CompletionEvent> supersededByBackend;

    std::vector<cb::StopCompleted> stops;
    std::vector<cb::TunChangeCompleted> tunChanges;
    std::vector<cb::Completion> failures;
    QStringList logLines;
    std::vector<cb::Endpoint> endpoints;
    std::vector<bool> connects;
    std::vector<cb::Endpoint> readyEndpoints;
    int stoppedCount = 0;
    int liveStateClears = 0;

    // The re-entrancy witness. Set when a callback ran while a mutating call of
    // the backend was still on the stack, which the contract forbids.
    bool sawReentrantDelivery = false;
    int callbackCount = 0;

    // Runs inside the next callback, once. Used to remove an observer during
    // delivery.
    std::function<void()> onNextEvent;

    cb::Generation lastObserved() const noexcept { return lastObserved_; }
    bool acceptedChannel(const QString &channel) const;
    int acceptedCount(const QString &channel) const;
    void clear();

    // --- BackendObserver
    void coreStateChanged(cb::Generation generation, cb::CoreState state,
                          cb::Ownership ownership) noexcept override;
    void coreReady(const cb::Completion &completion, const cb::Endpoint &endpoint) noexcept override;
    void coreLogLine(cb::Generation generation, const QString &line) noexcept override;
    void coreFailed(const cb::Completion &completion) noexcept override;
    void coreStopped(cb::Generation generation) noexcept override;
    void stopCompleted(const cb::StopCompleted &result) noexcept override;
    void endpointChanged(cb::Generation generation, const cb::Endpoint &endpoint,
                         cb::Ownership ownership) noexcept override;
    void connectedChanged(cb::Generation generation, bool connected) noexcept override;
    void configReceived(const cb::Completion &completion,
                        const cb::BaseConfig &config) noexcept override;
    void modeChanged(const cb::Completion &completion, const QString &mode) noexcept override;
    void tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept override;
    void nodeSelected(const cb::Completion &completion, const QString &group,
                      const QString &node) noexcept override;
    void versionReceived(const cb::Completion &completion,
                         const QString &version) noexcept override;
    void proxiesUpdated(const cb::Completion &completion, cb::Span<cb::ProxyGroup> groups,
                        cb::Span<cb::ProxyNode> nodes) noexcept override;
    void rulesUpdated(const cb::Completion &completion,
                      cb::Span<cb::Rule> rules) noexcept override;
    void providersReceived(const cb::Completion &completion, bool rules,
                           cb::Span<cb::Provider> providers) noexcept override;
    void errorOccurred(const cb::Completion &completion) noexcept override;

  private:
    void witness() noexcept;
    // Returns true when the event may be acted on. Applies the section 2 rule.
    bool admit(const QString &channel, const cb::Completion &completion,
               const QString &payload) noexcept;
    void observe(cb::Generation generation) noexcept;

    FakeBackend *backend_;
    QString name_;
    cb::Generation lastObserved_ = cb::Generation::Initial;
};

}  // namespace testsupport::backend

#endif  // CLASHQT_TESTS_SUPPORT_BACKEND_RECORDING_OBSERVER_H
