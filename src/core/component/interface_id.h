#ifndef CLASHQT_CORE_COMPONENT_INTERFACE_ID_H
#define CLASHQT_CORE_COMPONENT_INTERFACE_ID_H

// The 128-bit identifier that names an interface, a class or a module.
// Contract: docs/module-api.md revision component-r1.
//
// An id names an IMMUTABLE vtable: once published, method order and signatures
// never change. An incompatible interface gets a new id; both may be exposed
// through QueryInterface during a migration.

#include <cstddef>
#include <cstdint>

namespace clashqt::com {

struct InterfaceId {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data4[8];
};

constexpr bool operator==(const InterfaceId& a, const InterfaceId& b) noexcept {
    if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3) {
        return false;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (a.data4[i] != b.data4[i]) {
            return false;
        }
    }
    return true;
}

constexpr bool operator!=(const InterfaceId& a, const InterfaceId& b) noexcept { return !(a == b); }

// A total order, so ids can key an ordered container. Not a public promise about
// which id sorts first; only that the order is strict, total and stable.
constexpr bool operator<(const InterfaceId& a, const InterfaceId& b) noexcept {
    if (a.data1 != b.data1) return a.data1 < b.data1;
    if (a.data2 != b.data2) return a.data2 < b.data2;
    if (a.data3 != b.data3) return a.data3 < b.data3;
    for (std::size_t i = 0; i < 8; ++i) {
        if (a.data4[i] != b.data4[i]) return a.data4[i] < b.data4[i];
    }
    return false;
}

namespace detail {

// Compile-time only. An invalid digit makes the call a non-constant expression,
// which is a compile error at the point of use rather than a silent zero id.
constexpr std::uint8_t HexDigit(char c) {
    return (c >= '0' && c <= '9')   ? static_cast<std::uint8_t>(c - '0')
           : (c >= 'a' && c <= 'f') ? static_cast<std::uint8_t>(c - 'a' + 10)
           : (c >= 'A' && c <= 'F') ? static_cast<std::uint8_t>(c - 'A' + 10)
                                    : throw "interface id: not a hexadecimal digit";
}

constexpr std::uint8_t HexByte(const char* p) {
    return static_cast<std::uint8_t>((HexDigit(p[0]) << 4) | HexDigit(p[1]));
}

constexpr char RequireDash(char c) {
    return c == '-' ? c : throw "interface id: expected '-' in 8-4-4-4-12 form";
}

}  // namespace detail

// Builds an id from its canonical 8-4-4-4-12 text, e.g.
// MakeInterfaceId("247a1b90-ece9-43a9-ab82-5739bdff6445").
// consteval: an id is always a compile-time constant, and a malformed literal or
// one of the wrong length fails to compile.
consteval InterfaceId MakeInterfaceId(const char (&text)[37]) {
    detail::RequireDash(text[8]);
    detail::RequireDash(text[13]);
    detail::RequireDash(text[18]);
    detail::RequireDash(text[23]);
    return InterfaceId{
        static_cast<std::uint32_t>((static_cast<std::uint32_t>(detail::HexByte(text + 0)) << 24) |
                                   (static_cast<std::uint32_t>(detail::HexByte(text + 2)) << 16) |
                                   (static_cast<std::uint32_t>(detail::HexByte(text + 4)) << 8) |
                                   static_cast<std::uint32_t>(detail::HexByte(text + 6))),
        static_cast<std::uint16_t>((detail::HexByte(text + 9) << 8) | detail::HexByte(text + 11)),
        static_cast<std::uint16_t>((detail::HexByte(text + 14) << 8) | detail::HexByte(text + 16)),
        {detail::HexByte(text + 19), detail::HexByte(text + 21), detail::HexByte(text + 24),
         detail::HexByte(text + 26), detail::HexByte(text + 28), detail::HexByte(text + 30),
         detail::HexByte(text + 32), detail::HexByte(text + 34)}};
}

// Number of bytes FormatInterfaceId writes, including the terminating NUL.
inline constexpr std::size_t kInterfaceIdTextSize = 37;

// Writes the canonical lower-case 8-4-4-4-12 text plus a NUL. A caller-provided
// buffer, so no string type crosses the published boundary.
void FormatInterfaceId(const InterfaceId& id, char (&out)[kInterfaceIdTextSize]) noexcept;

// The same 128-bit type in two other roles, exactly as GUID/IID/CLSID are one
// type in the model this follows.
using ClassId = InterfaceId;   // names a creatable class inside a module
using ModuleId = InterfaceId;  // names a loaded module

}  // namespace clashqt::com

#endif  // CLASHQT_CORE_COMPONENT_INTERFACE_ID_H
