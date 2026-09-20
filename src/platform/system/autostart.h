#pragma once

#include <QString>

namespace platform {

/// Launch-at-login registration.
///
/// Contract with the ui module. Extend, do not reshape.
class Autostart {
public:
    static bool isSupported();
    static bool isEnabled();
    static bool setEnabled(bool enabled);
    static QString lastError();
};

}  // namespace platform
