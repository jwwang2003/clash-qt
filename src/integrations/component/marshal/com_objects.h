#pragma once

// The two component objects the boundary produces: an owned byte range and a
// diagnostic. Private, compiled on BOTH sides, and that is the point:
// component-r1 requires the producing module to own and free what it hands
// over, so each side allocates with its own copy of this code and destroys
// through its own vtable. A buffer allocated by the module and freed by the
// host's allocator is the crash this arrangement exists to prevent.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QString>

#include "core/component/buffer.h"
#include "core/component/error_info.h"
#include "core/component/object_support.h"
#include "core/component/result.h"

namespace clashqt::integration::marshal {

namespace com = ::clashqt::com;

/// What an object produced by a MODULE reports its own existence to, so the
/// module can refuse to be unmapped while it exists.
///
/// component-r1 puts ownership of a produced object on the producing module,
/// and the loader decides whether the image may go by asking the module what is
/// still alive. A reply buffer or a diagnostic has its vtable in the module's
/// image exactly as a session does, and a consumer may hold either one long
/// after the session it came from was released - so "sessions are all gone"
/// is not the same statement as "nothing points into this image".
///
/// Objects the HOST produces pass no anchor: there is no module to keep alive,
/// and the same two classes are compiled into both sides.
class ObjectAnchor {
  public:
    /// Called once before the object is handed out.
    virtual void objectCreated() noexcept = 0;
    /// Called once from the object's destructor.
    virtual void objectDestroyed() noexcept = 0;

  protected:
    ~ObjectAnchor() = default;
};

/// A resizable, reference-counted byte range. Created with one reference, which
/// the caller owns.
class ByteBuffer final : public com::IBuffer {
  public:
    static ByteBuffer *Create(const void *data, std::size_t size,
                              ObjectAnchor *anchor = nullptr) noexcept;
    static ByteBuffer *Create(const std::vector<std::uint8_t> &bytes,
                              ObjectAnchor *anchor = nullptr) noexcept;

    com::Result QueryInterface(const com::InterfaceId &id, void **out) noexcept override;
    std::int32_t AddRef() noexcept override;
    std::int32_t Release() noexcept override;

    std::size_t Size() noexcept override;
    const void *Data() noexcept override;
    void *MutableData() noexcept override;
    com::Result Resize(std::size_t newSize) noexcept override;

  private:
    ByteBuffer() = default;
    ~ByteBuffer();

    com::ReferenceCount references_;
    ObjectAnchor *anchor_ = nullptr;
    std::vector<std::uint8_t> bytes_;
};

/// An immutable failure snapshot. `source` is the subsystem that failed, which
/// on this boundary is the command or event name - the one piece of context
/// that turns "kInvalidArgument" into something actionable.
class ErrorInfoObject final : public com::IErrorInfo {
  public:
    static ErrorInfoObject *Create(com::Result code, const QString &message, const QString &source,
                                   ObjectAnchor *anchor = nullptr) noexcept;

    com::Result QueryInterface(const com::InterfaceId &id, void **out) noexcept override;
    std::int32_t AddRef() noexcept override;
    std::int32_t Release() noexcept override;

    com::Result GetCode(com::Result *out) noexcept override;
    com::Result GetMessage(com::IBuffer **out) noexcept override;
    com::Result GetSource(com::IBuffer **out) noexcept override;

  private:
    ErrorInfoObject() = default;
    ~ErrorInfoObject();

    com::ReferenceCount references_;
    /// Held so GetMessage/GetSource can pass it to the buffers THEY produce.
    /// Those nested buffers outlive this object as easily as this object
    /// outlives its session, and each one pins the module in its own right.
    ObjectAnchor *anchor_ = nullptr;
    com::Result code_ = com::kFail;
    std::vector<std::uint8_t> message_;
    std::vector<std::uint8_t> source_;
};

/// Reads an IBuffer's bytes into a vector the caller owns. Null is an empty
/// result, not a failure: component-r1 makes an absent buffer normal.
std::vector<std::uint8_t> ReadBuffer(com::IBuffer *buffer);

/// Retrieves the message of an IErrorInfo as text. Empty when there is none.
QString ErrorMessageOf(com::IErrorInfo *error);

}  // namespace clashqt::integration::marshal
