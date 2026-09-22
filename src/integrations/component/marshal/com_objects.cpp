#include "integrations/component/marshal/com_objects.h"

#include <cstring>
#include <new>

namespace clashqt::integration::marshal {
namespace {

std::vector<std::uint8_t> Utf8Of(const QString &text) {
    const QByteArray utf8 = text.toUtf8();
    return std::vector<std::uint8_t>(utf8.constBegin(), utf8.constEnd());
}

}  // namespace

// ------------------------------------------------------------- ByteBuffer

ByteBuffer *ByteBuffer::Create(const void *data, std::size_t size, ObjectAnchor *anchor) noexcept {
    // nothrow: an exception must not unwind out of a factory the boundary calls.
    auto *buffer = new (std::nothrow) ByteBuffer();
    if (buffer == nullptr) {
        return nullptr;
    }
    // Taken BEFORE the object can be handed out or destroyed, so the module is
    // pinned for the whole of this object's life rather than from the moment
    // the first caller happens to look.
    buffer->anchor_ = anchor;
    if (anchor != nullptr) {
        anchor->objectCreated();
    }
    if (size != 0 && data != nullptr) {
        const auto *first = static_cast<const std::uint8_t *>(data);
        // The one allocation that can fail after construction. Reported as a
        // null factory result rather than a half-filled buffer.
        try {
            buffer->bytes_.assign(first, first + size);
        } catch (...) {
            buffer->Release();  // runs ~ByteBuffer, which releases the anchor
            return nullptr;
        }
    }
    return buffer;
}

ByteBuffer *ByteBuffer::Create(const std::vector<std::uint8_t> &bytes,
                               ObjectAnchor *anchor) noexcept {
    return Create(bytes.empty() ? nullptr : bytes.data(), bytes.size(), anchor);
}

ByteBuffer::~ByteBuffer() {
    if (anchor_ != nullptr) {
        anchor_->objectDestroyed();
        anchor_ = nullptr;
    }
}

com::Result ByteBuffer::QueryInterface(const com::InterfaceId &id, void **out) noexcept {
    const com::InterfaceEntry entries[] = {
        com::MakeInterfaceEntry(static_cast<com::IBuffer *>(this)),
        com::InterfaceEntry{&com::kIObjectId, static_cast<void *>(static_cast<com::IObject *>(this))},
    };
    return com::ResolveInterface(id, out, entries, sizeof(entries) / sizeof(entries[0]),
                                 static_cast<com::IObject *>(this));
}

std::int32_t ByteBuffer::AddRef() noexcept { return references_.Increment(); }

std::int32_t ByteBuffer::Release() noexcept {
    const std::int32_t remaining = references_.Decrement();
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

std::size_t ByteBuffer::Size() noexcept { return bytes_.size(); }

const void *ByteBuffer::Data() noexcept { return bytes_.empty() ? nullptr : bytes_.data(); }

void *ByteBuffer::MutableData() noexcept { return bytes_.empty() ? nullptr : bytes_.data(); }

com::Result ByteBuffer::Resize(std::size_t newSize) noexcept {
    try {
        bytes_.resize(newSize);
    } catch (...) {
        return com::kFail;  // unchanged, per IBuffer's documented contract
    }
    return com::kOk;
}

// --------------------------------------------------------- ErrorInfoObject

ErrorInfoObject *ErrorInfoObject::Create(com::Result code, const QString &message,
                                         const QString &source, ObjectAnchor *anchor) noexcept {
    auto *error = new (std::nothrow) ErrorInfoObject();
    if (error == nullptr) {
        return nullptr;
    }
    error->anchor_ = anchor;
    if (anchor != nullptr) {
        anchor->objectCreated();
    }
    error->code_ = code;
    try {
        error->message_ = Utf8Of(message);
        error->source_ = Utf8Of(source);
    } catch (...) {
        // A diagnostic that cannot allocate still carries its code, which is
        // the part a caller branches on.
        error->message_.clear();
        error->source_.clear();
    }
    return error;
}

ErrorInfoObject::~ErrorInfoObject() {
    if (anchor_ != nullptr) {
        anchor_->objectDestroyed();
        anchor_ = nullptr;
    }
}

com::Result ErrorInfoObject::QueryInterface(const com::InterfaceId &id, void **out) noexcept {
    const com::InterfaceEntry entries[] = {
        com::MakeInterfaceEntry(static_cast<com::IErrorInfo *>(this)),
        com::InterfaceEntry{&com::kIObjectId,
                            static_cast<void *>(static_cast<com::IObject *>(this))},
    };
    return com::ResolveInterface(id, out, entries, sizeof(entries) / sizeof(entries[0]),
                                 static_cast<com::IObject *>(this));
}

std::int32_t ErrorInfoObject::AddRef() noexcept { return references_.Increment(); }

std::int32_t ErrorInfoObject::Release() noexcept {
    const std::int32_t remaining = references_.Decrement();
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

com::Result ErrorInfoObject::GetCode(com::Result *out) noexcept {
    if (out == nullptr) {
        return com::kInvalidArgument;
    }
    *out = code_;
    return com::kOk;
}

com::Result ErrorInfoObject::GetMessage(com::IBuffer **out) noexcept {
    if (out == nullptr) {
        return com::kInvalidArgument;
    }
    *out = nullptr;
    if (message_.empty()) {
        return com::kNotFound;  // no message is normal, not a failure
    }
    // The nested buffer carries the anchor too: it is produced by this module,
    // it can outlive this diagnostic, and a consumer that keeps only the
    // message must still keep the module mapped.
    ByteBuffer *buffer = ByteBuffer::Create(message_, anchor_);
    if (buffer == nullptr) {
        return com::kFail;
    }
    *out = buffer;
    return com::kOk;
}

com::Result ErrorInfoObject::GetSource(com::IBuffer **out) noexcept {
    if (out == nullptr) {
        return com::kInvalidArgument;
    }
    *out = nullptr;
    if (source_.empty()) {
        return com::kNotFound;
    }
    ByteBuffer *buffer = ByteBuffer::Create(source_, anchor_);
    if (buffer == nullptr) {
        return com::kFail;
    }
    *out = buffer;
    return com::kOk;
}

// ------------------------------------------------------------- free helpers

std::vector<std::uint8_t> ReadBuffer(com::IBuffer *buffer) {
    if (buffer == nullptr) {
        return {};
    }
    const std::size_t size = buffer->Size();
    const auto *data = static_cast<const std::uint8_t *>(buffer->Data());
    if (size == 0 || data == nullptr) {
        return {};
    }
    return std::vector<std::uint8_t>(data, data + size);
}

QString ErrorMessageOf(com::IErrorInfo *error) {
    if (error == nullptr) {
        return {};
    }
    com::IBuffer *message = nullptr;
    if (com::IsFailure(error->GetMessage(&message)) || message == nullptr) {
        return {};
    }
    const auto *data = static_cast<const char *>(message->Data());
    const QString text =
        data == nullptr ? QString() : QString::fromUtf8(data, static_cast<qsizetype>(message->Size()));
    message->Release();
    return text;
}

}  // namespace clashqt::integration::marshal
