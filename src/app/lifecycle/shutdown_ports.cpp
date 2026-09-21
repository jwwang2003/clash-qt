#include "app/lifecycle/shutdown_ports.h"

#include <utility>

#include <QThreadPool>

namespace app::lifecycle {

FunctionGate::FunctionGate(QString name, std::function<bool()> isBusy)
    : name_(std::move(name)), isBusy_(std::move(isBusy)) {}

bool FunctionGate::isBusy() const {
    // A gate with no predicate is not a gate that blocks forever: an absent
    // busy source is not a busy one.
    return isBusy_ ? isBusy_() : false;
}

FunctionProxyShutdown::FunctionProxyShutdown(std::function<void()> request)
    : request_(std::move(request)) {}

void FunctionProxyShutdown::requestShutdown() {
    if (request_) request_();
}

int GlobalThreadPoolDrain::activeTaskCount() const {
    return QThreadPool::globalInstance()->activeThreadCount();
}

}  // namespace app::lifecycle
