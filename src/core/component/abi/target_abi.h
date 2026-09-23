#ifndef CLASHQT_CORE_COMPONENT_ABI_TARGET_ABI_H
#define CLASHQT_CORE_COMPONENT_ABI_TARGET_ABI_H

// The compile-time description of the binary target a translation unit was
// built for, reduced to one 64-bit tag the handshake can compare.
// Contract: docs/module-api.md.
//
// WHY A TAG AND NOT A VERSION NUMBER
//   component-r1 is explicit that QueryInterface plus a C factory does not by
//   itself make a C++ vtable portable across compilers and runtimes. A module
//   built by a different compiler, for a different architecture, or against a
//   different C++ ABI does not have the same vtable layout, and loading it is
//   undefined behaviour rather than a version mismatch. The tag is how a host
//   REFUSES such a module instead of calling into it.
//
// WHAT IS AND IS NOT IN THE TAG
//   Only what this header can see from predefined macros: architecture, pointer
//   width, endianness, compiler identity and major version, and the C++ ABI
//   flavour. The Qt build is deliberately NOT here - no published ABI header
//   includes a Qt header - so it travels as a separate runtime tag that the
//   host and the module each compute from their own private code and the
//   handshake compares. Two tags, both checked; see module_entry.h.
//
// QUALIFICATION
//   Values are produced on every target this compiles for, but a MATCH only
//   means "the two sides agree about what they were built for". The project
//   qualifies macOS arm64 only; every other target is unqualified regardless of
//   what the tag says. Nothing here was compiled or run on Windows or Linux:
//   the MSVC and clang-cl branches exist so the header does not FAIL to compile
//   there, and that is the entire claim being made about them.
//
// PORTABILITY OF THE MACROS THIS USES
//   Only <bit> and the compiler-identity macros. __BYTE_ORDER__ is a GCC/Clang
//   extension that MSVC does not define, so endianness comes from
//   std::endian instead - which every C++20 implementation has.

#include <bit>
#include <cstddef>
#include <cstdint>

namespace clashqt::com::abi {

namespace detail {

// FNV-1a over a NUL-terminated literal. constexpr, so a tag costs nothing at
// run time and can be compared as a plain integer.
constexpr std::uint64_t HashText(const char* text, std::uint64_t seed) noexcept {
    std::uint64_t hash = seed;
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(text[i]));
        hash *= 0x00000100000001B3ull;
    }
    return hash;
}

constexpr std::uint64_t HashValue(std::uint64_t value, std::uint64_t seed) noexcept {
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < 8; ++i) {
        hash ^= (value >> (i * 8)) & 0xFFull;
        hash *= 0x00000100000001B3ull;
    }
    return hash;
}

constexpr std::uint64_t kHashSeed = 0xCBF29CE484222325ull;

}  // namespace detail

// ------------------------------------------------------------- architecture

#if defined(__aarch64__) || defined(_M_ARM64)
inline constexpr const char* kTargetArchitecture = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
inline constexpr const char* kTargetArchitecture = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
inline constexpr const char* kTargetArchitecture = "x86";
#elif defined(__arm__) || defined(_M_ARM)
inline constexpr const char* kTargetArchitecture = "arm";
#else
inline constexpr const char* kTargetArchitecture = "unknown-architecture";
#endif

#if defined(__APPLE__)
inline constexpr const char* kTargetPlatform = "darwin";
#elif defined(_WIN32)
inline constexpr const char* kTargetPlatform = "windows";
#elif defined(__linux__)
inline constexpr const char* kTargetPlatform = "linux";
#else
inline constexpr const char* kTargetPlatform = "unknown-platform";
#endif

// ----------------------------------------------------------------- compiler

#if defined(__clang__)
inline constexpr const char* kTargetCompiler = "clang";
inline constexpr std::uint64_t kTargetCompilerMajor = __clang_major__;
#elif defined(_MSC_VER)
inline constexpr const char* kTargetCompiler = "msvc";
inline constexpr std::uint64_t kTargetCompilerMajor = _MSC_VER / 100;
#elif defined(__GNUC__)
inline constexpr const char* kTargetCompiler = "gcc";
inline constexpr std::uint64_t kTargetCompilerMajor = __GNUC__;
#else
inline constexpr const char* kTargetCompiler = "unknown-compiler";
inline constexpr std::uint64_t kTargetCompilerMajor = 0;
#endif

// The C++ ABI flavour: vtable layout, name mangling and the this-adjustment
// under multiple inheritance all follow from it, and every interface in this
// project uses multiple inheritance somewhere.
//
// clang-cl defines BOTH __clang__ and _MSC_VER and follows the MICROSOFT C++
// ABI, not the Itanium one. Keying this on "_MSC_VER is defined at all" is the
// difference between refusing such a module and calling into it with the wrong
// vtable layout. The compiler IDENTITY above still reports clang, because that
// is what it is, and the two fields are hashed separately.
#if defined(_MSC_VER)
inline constexpr const char* kTargetCxxAbi = "msvc";
#else
inline constexpr const char* kTargetCxxAbi = "itanium";
#endif

// ------------------------------------------------------------------ the tag

// Endianness, from the standard library rather than from a GCC/Clang-only
// predefined macro, so this header compiles under MSVC and clang-cl too.
inline constexpr std::uint64_t kTargetEndianness =
    std::endian::native == std::endian::little    ? 1
    : std::endian::native == std::endian::big     ? 2
                                                  : 3;  // mixed: distinct, and refused by a match

inline constexpr std::uint64_t kTargetAbiTag = detail::HashValue(
    static_cast<std::uint64_t>(sizeof(void*)) | (static_cast<std::uint64_t>(sizeof(long)) << 8) |
        (kTargetEndianness << 16) |
        (kTargetCompilerMajor << 24),
    detail::HashText(
        kTargetCxxAbi,
        detail::HashText(
            kTargetCompiler,
            detail::HashText(kTargetPlatform,
                             detail::HashText(kTargetArchitecture, detail::kHashSeed)))));

static_assert(kTargetAbiTag != 0, "a zero target tag would compare equal to an uninitialised one");

// A host may load a module on any target whose two tags agree. That is a
// statement about layout agreement, NOT a qualification claim: the project has
// measured macOS arm64 and nothing else, and this constant is false everywhere
// else so a consumer can say so in its own diagnostics rather than inferring it.
// It is deliberately not "every compiler on macOS arm64 is fine" either -
// qualification belongs to a measured toolchain/OS/runtime record, and this
// constant only says which target that record was made on.
inline constexpr bool kTargetIsQualified =
#if defined(__APPLE__) && (defined(__aarch64__) || defined(_M_ARM64))
    sizeof(void*) == 8 && std::endian::native == std::endian::little;
#else
    false;
#endif

}  // namespace clashqt::com::abi

#endif  // CLASHQT_CORE_COMPONENT_ABI_TARGET_ABI_H
