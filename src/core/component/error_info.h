#ifndef CLASHQT_CORE_COMPONENT_ERROR_INFO_H
#define CLASHQT_CORE_COMPONENT_ERROR_INFO_H

// IErrorInfo: an optional diagnostic that accompanies a failure Result.
// Contract: docs/module-api.md revision component-r1.

#include "core/component/buffer.h"
#include "core/component/object.h"
#include "core/component/result.h"

namespace clashqt::com {

// Retrieval
//   A diagnostic is retrieved FROM THE FAILING OBJECT, by querying it for
//   kIErrorInfoId - never from thread-local or process-global state, which
//   cannot survive a module or thread boundary and silently reports the wrong
//   error. kNoInterface from that query means the object carries no diagnostic:
//   the absence of error info is normal and is not itself a failure.
//
// Thread affinity
//   An IErrorInfo is an immutable snapshot taken when the failure occurred, so
//   every method is callable from any thread for as long as the reference is
//   held. Retrieving one from the failing object has that object's affinity, not
//   this one's.
struct IErrorInfo : IObject {
    // The failure Result this diagnostic describes. kInvalidArgument when out is
    // null; the value written is always a failure code.
    virtual Result GetCode(Result* out) noexcept = 0;

    // A UTF-8 message, not NUL-terminated; its length is the buffer's Size().
    // The buffer arrives with one reference already owned by the caller.
    //   kInvalidArgument  out is null
    //   kNotFound         no message; *out is nullptr. Normal, not a failure of
    //                     the diagnostic itself.
    virtual Result GetMessage(IBuffer** out) noexcept = 0;

    // An optional UTF-8 source tag (a subsystem or call site), same rules.
    virtual Result GetSource(IBuffer** out) noexcept = 0;

  protected:
    ~IErrorInfo() = default;
};

}  // namespace clashqt::com

CLASHQT_COM_DECLARE_INTERFACE_ID(::clashqt::com::IErrorInfo,
                                 "e83b4037-e096-4ae8-8f94-0f56ad29568a")

namespace clashqt::com {
inline constexpr const InterfaceId& kIErrorInfoId = InterfaceTraits<IErrorInfo>::kId;
}

#endif  // CLASHQT_CORE_COMPONENT_ERROR_INFO_H
