#pragma once

// The privileged-execution seam, owned by clash_mihomo_impl.
//
// platform::PrivilegedServiceClient used to appear in CoreProcess's constructor
// signature, which put a platform type on the component's public surface and
// forced a PUBLIC clash_mihomo_impl -> clash_platform edge. The client is NOT
// absorbed into the component: privileged execution stays a separate, OS-owned
// service, so the shipped component is never made responsible for privileged
// IPC. Instead the component publishes this abstract interface and the
// composition root adapts the concrete client onto it. That adapter cannot live
// beside this file, because src/core/** may not include a platform header and
// src/main.cpp may not include the component-private core/mihomo/** headers;
// this published seam is what lets the composition root see both sides. The
// adapter lives at src/app/composition/privileged_service_adapter.h.
//
// Deliberately not a QObject: signals are what dragged platform in. Delivery is
// a plain listener interface, invoked on the owning thread.

#include <QJsonObject>
#include <QString>

namespace core {

/// What a privileged core service reports back to its single listener.
/// Every callback arrives on the thread that owns the service.
class PrivilegedCoreServiceListener {
public:
    virtual void privilegedConnectedChanged(bool connected) = 0;
    virtual void privilegedStatusReceived(const QJsonObject &status) = 0;
    virtual void privilegedCoreStarted(const QJsonObject &endpoint) = 0;
    virtual void privilegedCoreStopped() = 0;
    virtual void privilegedLogsReceived(const QString &logs) = 0;
    virtual void privilegedRequestFinished(const QString &operation, bool success,
                                           const QString &error) = 0;

protected:
    // A listener is a view, not an owner: the service never deletes it.
    ~PrivilegedCoreServiceListener() = default;
};

/// A lease on a privileged core. One live connection per service instance;
/// dropping it makes the helper stop the core it started.
class PrivilegedCoreService {
public:
    virtual ~PrivilegedCoreService() = default;

    /// Set once by the owner, cleared with nullptr before the listener dies.
    virtual void setListener(PrivilegedCoreServiceListener *listener) = 0;

    /// Instance queries, never process-global statics (backend contract
    /// section 9): a loaded module gets its own copy of every static.
    virtual bool isSupported() const = 0;
    virtual bool isAvailable() const = 0;
    virtual bool isConnected() const = 0;
    virtual bool isBusy() const = 0;
    /// Available before privilegedConnectedChanged(false); may be empty.
    virtual QString connectionError() const = 0;

    virtual void requestStatus() = 0;
    virtual void requestLogs() = 0;
    virtual void startCore(const QJsonObject &config) = 0;
    virtual void stopCore() = 0;
    virtual void close() = 0;
};

/// The default when nothing is injected. It reports the platform as having no
/// privileged service, so selecting service mode is REFUSED rather than
/// silently accepted and then wedged. A component that was handed no privileged
/// service genuinely has none; pretending otherwise is how a stop that can never
/// be confirmed gets reported as a success.
class NullPrivilegedCoreService final : public PrivilegedCoreService {
public:
    void setListener(PrivilegedCoreServiceListener *listener) override { (void)listener; }
    bool isSupported() const override { return false; }
    bool isAvailable() const override { return false; }
    bool isConnected() const override { return false; }
    bool isBusy() const override { return false; }
    QString connectionError() const override { return {}; }
    void requestStatus() override {}
    void requestLogs() override {}
    void startCore(const QJsonObject &config) override { (void)config; }
    void stopCore() override {}
    void close() override {}
};

}  // namespace core
