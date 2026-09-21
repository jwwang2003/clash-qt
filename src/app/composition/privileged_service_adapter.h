#pragma once

// Composition-root adapter: platform::PrivilegedServiceClient -> the component's
// core::PrivilegedCoreService seam (DECISION D2).
//
// Lives in the composition root, which is the only layer permitted to see both
// sides: IR-CORE-NOT-PLATFORM keeps the seam out of src/core/**, and
// IR-PLATFORM-NOT-CORE keeps it out of src/platform/**. The seam interface is
// published at core/backend/privileged_core_service.h, which src/app/** may
// include because it is not component-private core/mihomo/** code.
// include.
//
// The adapter is what keeps clash_mihomo_impl free of platform symbols, which
// is what removes the PUBLIC clash_mihomo_impl -> clash_platform edge
// (D2-mihomo-impl-links-platform) without absorbing the privileged client into
// the component: G2 keeps privileged execution a separate, OS-owned service.
//
// Header-only and deliberately NOT a Q_OBJECT. A Q_OBJECT here would be moc'd
// into whichever target scans this directory - clash_mihomo_impl - and the
// generated code would drag the platform symbols straight back in. Signals are
// what put a platform type on CoreProcess's surface in the first place; this
// adapter connects to the client's signals through a plain QObject context and
// forwards them to the seam's listener, so no new build target is required.

#include <QJsonObject>
#include <QFileInfo>
#include <QObject>
#include <QString>

#include "core/backend/privileged_core_service.h"
#include "platform/service/privileged_service_client.h"

namespace core {

/// Wraps a platform::PrivilegedServiceClient the caller owns. The client must
/// outlive the adapter; the adapter never deletes it.
class PrivilegedServiceClientAdapter final : public PrivilegedCoreService {
public:
    /// `socketPath` names the helper socket this client was pointed at. Empty
    /// means the platform default, and support is then the platform's answer. A
    /// non-empty one is an explicit choice by whoever built the client - a test
    /// harness, or a host with a relocated helper - and a named helper is a
    /// supported one.
    explicit PrivilegedServiceClientAdapter(platform::PrivilegedServiceClient *client,
                                            QString socketPath = QString())
        : client_(client), socketPath_(std::move(socketPath)) {
        if (!client_) return;
        QObject::connect(client_, &platform::PrivilegedServiceClient::connectedChanged, &context_,
                         [this](bool connected) {
                             if (listener_) listener_->privilegedConnectedChanged(connected);
                         });
        QObject::connect(client_, &platform::PrivilegedServiceClient::statusReceived, &context_,
                         [this](const QJsonObject &status) {
                             if (listener_) listener_->privilegedStatusReceived(status);
                         });
        QObject::connect(client_, &platform::PrivilegedServiceClient::coreStarted, &context_,
                         [this](const QJsonObject &endpoint) {
                             if (listener_) listener_->privilegedCoreStarted(endpoint);
                         });
        QObject::connect(client_, &platform::PrivilegedServiceClient::coreStopped, &context_,
                         [this] {
                             if (listener_) listener_->privilegedCoreStopped();
                         });
        QObject::connect(client_, &platform::PrivilegedServiceClient::logsReceived, &context_,
                         [this](const QString &logs) {
                             if (listener_) listener_->privilegedLogsReceived(logs);
                         });
        QObject::connect(client_, &platform::PrivilegedServiceClient::requestFinished, &context_,
                         [this](const QString &operation, bool success, const QString &error) {
                             if (listener_)
                                 listener_->privilegedRequestFinished(operation, success, error);
                         });
    }

    ~PrivilegedServiceClientAdapter() override {
        if (client_) client_->disconnect(&context_);
    }

    PrivilegedServiceClientAdapter(const PrivilegedServiceClientAdapter &) = delete;
    PrivilegedServiceClientAdapter &operator=(const PrivilegedServiceClientAdapter &) = delete;

    void setListener(PrivilegedCoreServiceListener *listener) override { listener_ = listener; }

    bool isSupported() const override {
        if (!client_) return false;
        // A named socket IS the statement that a helper exists here.
        return !socketPath_.isEmpty() || platform::PrivilegedServiceClient::isSupported();
    }
    bool isAvailable() const override {
        if (!isSupported()) return false;
        return QFileInfo::exists(socketPath_.isEmpty()
                                     ? platform::PrivilegedServiceClient::defaultSocketPath()
                                     : socketPath_);
    }
    bool isConnected() const override { return client_ && client_->isConnected(); }
    bool isBusy() const override { return client_ && client_->isBusy(); }
    QString connectionError() const override {
        return client_ ? client_->connectionError() : QString();
    }

    void requestStatus() override { if (client_) client_->requestStatus(); }
    void requestLogs() override { if (client_) client_->requestLogs(); }
    void startCore(const QJsonObject &config) override { if (client_) client_->startCore(config); }
    void stopCore() override { if (client_) client_->stopCore(); }
    void close() override { if (client_) client_->close(); }

private:
    QObject context_;
    platform::PrivilegedServiceClient *client_ = nullptr;
    QString socketPath_;
    PrivilegedCoreServiceListener *listener_ = nullptr;
};

}  // namespace core
