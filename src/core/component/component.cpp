#include "core/component/interface_id.h"
#include "core/component/object_support.h"

#include <cstddef>
#include <cstdint>

namespace clashqt::com {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

char* WriteByte(char* out, std::uint8_t value) noexcept {
    *out++ = kHexDigits[(value >> 4) & 0x0F];
    *out++ = kHexDigits[value & 0x0F];
    return out;
}

}  // namespace

void FormatInterfaceId(const InterfaceId& id, char (&out)[kInterfaceIdTextSize]) noexcept {
    char* cursor = out;
    cursor = WriteByte(cursor, static_cast<std::uint8_t>((id.data1 >> 24) & 0xFF));
    cursor = WriteByte(cursor, static_cast<std::uint8_t>((id.data1 >> 16) & 0xFF));
    cursor = WriteByte(cursor, static_cast<std::uint8_t>((id.data1 >> 8) & 0xFF));
    cursor = WriteByte(cursor, static_cast<std::uint8_t>(id.data1 & 0xFF));
    *cursor++ = '-';
    cursor = WriteByte(cursor, static_cast<std::uint8_t>((id.data2 >> 8) & 0xFF));
    cursor = WriteByte(cursor, static_cast<std::uint8_t>(id.data2 & 0xFF));
    *cursor++ = '-';
    cursor = WriteByte(cursor, static_cast<std::uint8_t>((id.data3 >> 8) & 0xFF));
    cursor = WriteByte(cursor, static_cast<std::uint8_t>(id.data3 & 0xFF));
    *cursor++ = '-';
    cursor = WriteByte(cursor, id.data4[0]);
    cursor = WriteByte(cursor, id.data4[1]);
    *cursor++ = '-';
    for (std::size_t i = 2; i < 8; ++i) {
        cursor = WriteByte(cursor, id.data4[i]);
    }
    *cursor = '\0';
}

Result ResolveInterface(const InterfaceId& id, void** out, const InterfaceEntry* entries,
                        std::size_t count, IObject* identity) noexcept {
    // Rule 1 first, and on its own: a null out parameter is answered without
    // writing anything anywhere and without touching the reference count.
    if (out == nullptr) {
        return kInvalidArgument;
    }
    if (identity == nullptr || (entries == nullptr && count != 0)) {
        *out = nullptr;
        return kInvalidArgument;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (entries[i].id != nullptr && *entries[i].id == id) {
            // Rule 3: write, then take exactly one new strong reference.
            *out = entries[i].pointer;
            identity->AddRef();
            return kOk;
        }
    }
    // Rule 2: the write happens BEFORE the return, so a caller that ignores the
    // code cannot read an uninitialised pointer.
    *out = nullptr;
    return kNoInterface;
}

}  // namespace clashqt::com
