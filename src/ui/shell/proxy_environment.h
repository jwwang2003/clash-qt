#pragma once

#include <QString>
#include "core/types.h"

namespace ui {

enum class Shell { Posix, PowerShell };

// Empty when there is no usable proxy listener. The controller secret is never included.
QString proxyEnvironment(const core::Endpoint &endpoint, const core::BaseConfig &config, Shell shell);

}  // namespace ui
