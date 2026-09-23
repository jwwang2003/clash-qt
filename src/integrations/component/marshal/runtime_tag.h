#pragma once

// The half of the handshake that target_abi.h cannot compute.
//
// No published ABI header includes a Qt header - that is what makes the ABI
// header set publishable to a module built elsewhere. But module-r1 requires
// the host and the module to share one Qt build, because they hand Qt-owned
// memory to each other inside their private marshalling and because a
// QCoreApplication lives on one side and the event loop it drives is used by
// the other. So each side computes this tag with its own private code and the
// handshake compares two numbers.
//
// It is a RUNTIME value, deliberately: comparing QT_VERSION at compile time
// would pass for a module compiled against the same headers and linked against
// a different library, which is the failure this is meant to catch.

#include <cstdint>

namespace clashqt::integration::marshal {

/// Mixes the Qt version this binary was COMPILED against with the one it is
/// RUNNING against, plus the size of the Qt string type. Two binaries agree
/// only when all three agree.
std::uint64_t runtimeTag() noexcept;

}  // namespace clashqt::integration::marshal
