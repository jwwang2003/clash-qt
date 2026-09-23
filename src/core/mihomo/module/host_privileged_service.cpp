#include "core/mihomo/module/host_privileged_service.h"

#include <QJsonDocument>

#include "integrations/component/marshal/codec.h"
#include "integrations/component/marshal/com_objects.h"

namespace core::module {
namespace {

namespace marshal = ::clashqt::integration::marshal;

QString jsonText(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

}  // namespace

HostPrivilegedService::HostPrivilegedService(abi::IBackendHost *host) noexcept : host_(host) {
    if (host_ != nullptr) {
        host_->AddRef();
    }
}

HostPrivilegedService::~HostPrivilegedService() { detachHost(); }

void HostPrivilegedService::detachHost() noexcept {
    if (host_ == nullptr) {
        return;
    }
    // Tell the host to stop delivering listener callbacks BEFORE dropping the
    // reference: the callback path is the one thing that outlives a naive
    // teardown.
    marshal::ByteWriter args;
    args.boolean(false);
    host_->Invoke(abi::kHostPrivilegedSetListenerActive, args.data().data(), args.size(), nullptr);
    host_->Release();
    host_ = nullptr;
}

void HostPrivilegedService::send(std::uint32_t command, const void *data,
                                 std::size_t size) const {
    if (host_ == nullptr) {
        return;
    }
    host_->Invoke(command, data, size, nullptr);
}

bool HostPrivilegedService::queryFlag(std::uint32_t command) const {
    if (host_ == nullptr) {
        return false;
    }
    com::IBuffer *reply = nullptr;
    const com::Result status = host_->Invoke(command, nullptr, 0, &reply);
    if (com::IsFailure(status) || reply == nullptr) {
        // A host that cannot answer is a host with no privileged service. The
        // fail-safe direction matters: answering "true" here would arm a guard
        // on a service nobody is talking to.
        if (reply != nullptr) {
            reply->Release();
        }
        return false;
    }
    marshal::ByteReader in(reply->Data(), reply->Size());
    const bool value = in.boolean();
    reply->Release();
    return in.ok() && value;
}

void HostPrivilegedService::setListener(PrivilegedCoreServiceListener *listener) {
    listener_ = listener;
    if (host_ == nullptr) {
        return;
    }
    marshal::ByteWriter args;
    args.boolean(listener != nullptr);
    // One connection only: the host refuses a second activation, and the
    // session never creates a second adapter.
    host_->Invoke(abi::kHostPrivilegedSetListenerActive, args.data().data(), args.size(), nullptr);
}

bool HostPrivilegedService::isSupported() const {
    return queryFlag(abi::kHostPrivilegedIsSupported);
}

bool HostPrivilegedService::isAvailable() const {
    return queryFlag(abi::kHostPrivilegedIsAvailable);
}

bool HostPrivilegedService::isConnected() const {
    return queryFlag(abi::kHostPrivilegedIsConnected);
}

bool HostPrivilegedService::isBusy() const { return queryFlag(abi::kHostPrivilegedIsBusy); }

QString HostPrivilegedService::connectionError() const {
    if (host_ == nullptr) {
        return {};
    }
    com::IBuffer *reply = nullptr;
    const com::Result status = host_->Invoke(abi::kHostPrivilegedConnectionError, nullptr, 0, &reply);
    if (com::IsFailure(status) || reply == nullptr) {
        if (reply != nullptr) {
            reply->Release();
        }
        return {};
    }
    marshal::ByteReader in(reply->Data(), reply->Size());
    const QString text = in.text();
    reply->Release();
    return in.ok() ? text : QString();
}

void HostPrivilegedService::requestStatus() {
    send(abi::kHostPrivilegedRequestStatus, nullptr, 0);
}

void HostPrivilegedService::requestLogs() { send(abi::kHostPrivilegedRequestLogs, nullptr, 0); }

void HostPrivilegedService::startCore(const QJsonObject &config) {
    // JSON text, not a QJsonObject: a parsed document cannot cross a module
    // boundary, and the host parses it back with its own Qt.
    marshal::ByteWriter args;
    args.text(jsonText(config));
    send(abi::kHostPrivilegedStartCore, args.data().data(), args.size());
}

void HostPrivilegedService::stopCore() { send(abi::kHostPrivilegedStopCore, nullptr, 0); }

void HostPrivilegedService::close() { send(abi::kHostPrivilegedClose, nullptr, 0); }

void HostPrivilegedService::deliverConnectedChanged(bool connected) {
    if (listener_ != nullptr) {
        listener_->privilegedConnectedChanged(connected);
    }
}

void HostPrivilegedService::deliverStatusReceived(const QJsonObject &status) {
    if (listener_ != nullptr) {
        listener_->privilegedStatusReceived(status);
    }
}

void HostPrivilegedService::deliverCoreStarted(const QJsonObject &endpoint) {
    if (listener_ != nullptr) {
        listener_->privilegedCoreStarted(endpoint);
    }
}

void HostPrivilegedService::deliverCoreStopped() {
    if (listener_ != nullptr) {
        listener_->privilegedCoreStopped();
    }
}

void HostPrivilegedService::deliverLogsReceived(const QString &logs) {
    if (listener_ != nullptr) {
        listener_->privilegedLogsReceived(logs);
    }
}

void HostPrivilegedService::deliverRequestFinished(const QString &operation, bool success,
                                                   const QString &error) {
    if (listener_ != nullptr) {
        listener_->privilegedRequestFinished(operation, success, error);
    }
}

}  // namespace core::module
