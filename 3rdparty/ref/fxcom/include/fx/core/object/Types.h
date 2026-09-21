#pragma once
#include <cstdint>
#include <string.h>

/// Unique identification structures
namespace fx {

/// Unique interface identifier
struct GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];

    bool operator==(const GUID& rhs) const noexcept {
        return memcmp(&Data1, &rhs.Data1, sizeof(rhs)) == 0;
    }
    bool operator!=(const GUID& rhs) const noexcept { return !(*this == rhs); }
};

namespace details {
constexpr inline uint8_t ascii_to_hex(char c) { return c >= 97 ? c - 87 : c - 48; }

constexpr uint8_t str_to_uint8(const char* s) {
    return (ascii_to_hex(s[0]) << 4) | ascii_to_hex(s[1]);
}

constexpr uint16_t str_to_uint16(const char* s) {
    return (ascii_to_hex(s[0]) << 12) | (ascii_to_hex(s[1]) << 8) |
           (ascii_to_hex(s[2]) << 4) | ascii_to_hex(s[3]);
}

constexpr uint32_t str_to_uint32(const char* s) {
    return (ascii_to_hex(s[0]) << 28) | (ascii_to_hex(s[1]) << 24) |
           (ascii_to_hex(s[2]) << 20) | (ascii_to_hex(s[3]) << 16) |
           (ascii_to_hex(s[4]) << 12) | (ascii_to_hex(s[5]) << 8) |
           (ascii_to_hex(s[6]) << 4) | ascii_to_hex(s[7]);
}

constexpr GUID str_to_guid(const char* s) {
    return GUID{str_to_uint32(s),
                str_to_uint16(s + 9),
                str_to_uint16(s + 14),
                {str_to_uint8(s + 19), str_to_uint8(s + 21), str_to_uint8(s + 24),
                 str_to_uint8(s + 26), str_to_uint8(s + 28), str_to_uint8(s + 30),
                 str_to_uint8(s + 32), str_to_uint8(s + 34)}};
}
}  // namespace details

// GUID literals
namespace literals {
constexpr GUID operator"" _fx_guid(const char* str, size_t N) {
    return details::str_to_guid(str);
}
}  // namespace literals

using namespace literals;

using FIID = GUID;
using FREFIID = const FIID&;
using FCLSID = GUID;
using FBOOL = int32_t;
using FLONG = int32_t;
using FRESULT = uint32_t;

#define FSUCCEEDED(hr) (((fx::FRESULT)(hr)) >= 0)
#define FFAILED(hr) (((fx::FRESULT)(hr)) < 0)

#define FIID_PPV_ARGS(pInterface) \
    __uuid_of<std::decay<decltype(**(pInterface))>::type>(), (void**)(pInterface)

}  // namespace fx

#if !defined(_MSC_VER)
// GCC and Clang reject the globally-qualified explicit specialization used by
// the MSVC implementation below. Use an interface-provided UUID accessor.
struct UUIDTraits {
    template <typename Interface>
    static constexpr const fx::GUID& uuid_of() {
        return Interface::this_uuid();
    }
};

#define FX_IID(Interface, StrIID) \
    static constexpr fx::GUID IID_##Interface = StrIID##_fx_guid;

#define FX_DECLARE_UUID_TRAITS(Interface) \
    friend struct ::UUIDTraits;              \
    static constexpr const fx::GUID& this_uuid() { return IID_##Interface; }

template <typename Interface>
constexpr const fx::GUID& __uuid_of(const Interface* pv = nullptr) {
    return ::UUIDTraits::uuid_of<Interface>();
}

#else
// Keep the original explicit-specialization implementation for MSVC.
template <typename Interface>
struct UUIDTraits;

#define FX_IID(Interface, StrIID)                                              \
    static constexpr fx::GUID IID_##Interface = StrIID##_fx_guid;           \
    template <>                                                                   \
    struct ::UUIDTraits<struct Interface> {                                       \
        static constexpr const fx::GUID& uuid_of() { return IID_##Interface; } \
    };

template <typename Interface>
constexpr const fx::GUID& __uuid_of(const Interface* pv = nullptr) {
    return ::UUIDTraits<Interface>::uuid_of();
}

// For compatible with old version of gcc and clang
#define FX_DECLARE_UUID_TRAITS(Interface)

#endif

namespace fx {

/// Base interface for all dynamic objects in the engine
FX_IID(IObject, "00000000-0000-0000-0000-000000000000")
struct IObject {
    FX_DECLARE_UUID_TRAITS(IObject)
    /// Queries the specific interface.

    /// \param [in] IID - Unique identifier of the requested interface.
    /// \param [out] ppInterface - Memory address where the pointer to the requested
    /// interface will be written.
    ///                            If the interface is not supported, null pointer will
    ///                            be returned.
    /// \remark The method increments the number of strong references by 1. The
    /// interface must be
    ///         released by a call to Release() method when it is no longer needed.
    virtual FRESULT QueryInterface(FREFIID riid, void** ppInterface) = 0;

    /// Increments the number of strong references by 1.

    /// \remark This method is equivalent to GetReferenceCounters()->AddStrongRef().\n
    ///         The method is thread-safe and does not require explicit synchronization.
    /// \return The number of strong references after incrementing the counter.
    /// \note   In a multithreaded environment, the returned number may not be reliable
    ///         as other threads may simultaneously change the actual value of the
    ///         counter.
    virtual FLONG AddRef() = 0;

    /// Decrements the number of strong references by 1 and destroys the object when the
    /// counter reaches zero.

    /// \remark This method is equivalent to
    /// GetReferenceCounters()->ReleaseStrongRef().\n
    ///         The method is thread-safe and does not require explicit synchronization.
    /// \return The number of strong references after decrementing the counter.
    /// \note   In a multithreaded environment, the returned number may not be reliable
    ///         as other threads may simultaneously change the actual value of the
    ///         counter. The only reliable value is 0 as the object is destroyed when
    ///         the last strong reference is released.
    virtual FLONG Release() = 0;
};

FX_IID(IWeakReference, "00000000-0000-0000-0000-000000000004")
struct IWeakReference : public IObject {
    FX_DECLARE_UUID_TRAITS(IWeakReference)
    virtual FRESULT Resolve(FREFIID riid, void** ppv) = 0;

    virtual FLONG GetNumStrongRefs() const = 0;

    virtual FBOOL IsExpired() const = 0;
};

FX_IID(IWeakable, "00000000-0000-0000-0000-000000000005")
struct IWeakable : public IObject {
    FX_DECLARE_UUID_TRAITS(IWeakable)
    virtual IWeakReference* GetWeakReference() = 0;
};

// Common Status Code
constexpr FRESULT FS_OK = 0;
constexpr FRESULT FE_GENERIC_ERROR = -1;
constexpr FRESULT FE_NOINTERFACE = -2;
constexpr FRESULT FE_NOT_IMPLEMENT = -3;
constexpr FRESULT FE_INVALID_ARGS = -4;
constexpr FRESULT FE_NOT_ALIVE_OBJECT = -4;
constexpr FRESULT FE_NOT_FOUND = -5;
constexpr FRESULT FE_WAIT_TIMEOUT = -6;

/// Binary data blob
// {F578FF0D-ABD2-4514-9D32-7CB454D4A73B}
FX_IID(IDataBlob, "f578ff0d-abd2-4514-9d32-7cb454d4a73b")
struct IDataBlob : public IObject {
    /// Sets the size of the internal data buffer
    virtual void Resize(size_t NewSize) = 0;

    /// Returns the size of the internal data buffer
    virtual size_t GetSize() = 0;

    /// Returns the pointer to the internal data buffer
    virtual void* GetDataPtr() = 0;
};

FRESULT CreateBlob(size_t Size, IDataBlob** ppBlob);

FRESULT CreateStringBlob(size_t Size, IDataBlob** ppBlob);

FRESULT CreateProxyBlob(size_t Size, const void* pData, IDataBlob** ppBlob);

FRESULT CreateProxyBlobFromSource(IDataBlob* pSource, size_t Offset, size_t Size,
                                  IDataBlob** ppBlob);

}  // namespace fx

// Use fx_guid literals
using namespace fx::literals;
