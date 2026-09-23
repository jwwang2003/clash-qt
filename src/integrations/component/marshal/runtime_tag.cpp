#include "integrations/component/marshal/runtime_tag.h"

#include <QString>
#include <QtGlobal>

#include "core/component/abi/target_abi.h"
#include "core/component/abi/wire.h"

namespace clashqt::integration::marshal {

std::uint64_t runtimeTag() noexcept {
    namespace abi = ::clashqt::com::abi;

    // qVersion() is the library actually loaded; QT_VERSION_STR is what these
    // sources were compiled against. A module built against 6.11 headers and
    // running against a 6.9 library differs from the host in the first value
    // and matches in the second, so both are in the tag.
    std::uint64_t tag = abi::detail::HashText(QT_VERSION_STR, abi::detail::kHashSeed);
    tag = abi::detail::HashText(qVersion(), tag);
    tag = abi::detail::HashValue(static_cast<std::uint64_t>(sizeof(QString)), tag);
    // The marshalling revision. Two sides that share a Qt build but not a codec
    // revision are just as incompatible, and this is the cheapest place to say
    // so - before any object is created.
    tag = abi::detail::HashValue(static_cast<std::uint64_t>(abi::kWireRevision), tag);
    return tag;
}

}  // namespace clashqt::integration::marshal
