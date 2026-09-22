#pragma once

// The privileged-execution reverse seam, module side.
//
// DECISION D2 AND module-r1, TOGETHER. D2 made privileged execution an
// interface the component publishes and the composition root implements, so
// that no platform type appears on the component's surface. module-r1 keeps
// that arrangement across the binary boundary: "Privileged execution stays
// host-owned. Marshal the existing injected service through an ABI-safe reverse
// interface; one connection only, no default real helper construction in
// module."
//
// So this class is a core::PrivilegedCoreService that owns NOTHING. Every query
// and every command becomes one IBackendHost::Invoke; the real
// PrivilegedServiceClient, its socket and its lease stay in the host process's
// composition root, exactly where they are today. A module that constructed its
// own client would be the second live connection to one privileged socket that
// decision D3 was written to remove.

#include <QJsonObject>
#include <QString>

#include "core/backend/privileged_core_service.h"
#include "core/component/abi/backend_abi.h"
#include "core/component/com_ptr.h"

namespace core::module {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;

class HostPrivilegedService final : public PrivilegedCoreService {
  public:
    /// Borrows the host; the session that owns both guarantees the host
    /// outlives this object.
    explicit HostPrivilegedService(abi::IBackendHost *host) noexcept;
    ~HostPrivilegedService() override;

    HostPrivilegedService(const HostPrivilegedService &) = delete;
    HostPrivilegedService &operator=(const HostPrivilegedService &) = delete;

    // ---- core::PrivilegedCoreService
    void setListener(PrivilegedCoreServiceListener *listener) override;
    bool isSupported() const override;
    bool isAvailable() const override;
    bool isConnected() const override;
    bool isBusy() const override;
    QString connectionError() const override;
    void requestStatus() override;
    void requestLogs() override;
    void startCore(const QJsonObject &config) override;
    void stopCore() override;
    void close() override;

    // ---- delivery of the host's listener callbacks, decoded by the session
    void deliverConnectedChanged(bool connected);
    void deliverStatusReceived(const QJsonObject &status);
    void deliverCoreStarted(const QJsonObject &endpoint);
    void deliverCoreStopped();
    void deliverLogsReceived(const QString &logs);
    void deliverRequestFinished(const QString &operation, bool success, const QString &error);

    /// Stops using the host. Called during Close, before the host reference is
    /// released, so nothing can call back into a host that is going away.
    void detachHost() noexcept;

  private:
    bool queryFlag(std::uint32_t command) const;
    void send(std::uint32_t command, const void *data, std::size_t size) const;

    abi::IBackendHost *host_ = nullptr;
    PrivilegedCoreServiceListener *listener_ = nullptr;
};

}  // namespace core::module
