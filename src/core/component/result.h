#ifndef CLASHQT_CORE_COMPONENT_RESULT_H
#define CLASHQT_CORE_COMPONENT_RESULT_H

// Status codes for the portable component object model.
// Contract: .refactor/COMPONENT_CONTRACT.md revision component-r1.
//
// Result is SIGNED on purpose. The reviewed design reference declares its status
// type as an unsigned 32-bit integer and then tests failure with `value < 0`,
// which is never true, so every failure there reads as success and the negative
// constants wrap to large positive values. Keeping the type signed and the
// predicates as inline functions (never macros) is what makes an expected-failure
// test able to observe a genuine failure.

#include <cstdint>

namespace clashqt::com {

using Result = std::int32_t;

static_assert(static_cast<Result>(-1) < 0,
              "Result must be signed; an unsigned status type makes IsFailure inoperative.");

// Success codes: non-negative.
inline constexpr Result kOk = 0;      // Operation succeeded.
inline constexpr Result kFalse = 1;   // Succeeded, and the answer is negative.

// Failure codes: negative, each value distinct and permanently stable.
inline constexpr Result kFail = -1;                // Unspecified failure.
inline constexpr Result kNoInterface = -2;         // The object does not implement the requested id.
inline constexpr Result kNotImplemented = -3;      // In the vtable, not implemented by this object.
inline constexpr Result kInvalidArgument = -4;     // A caller-supplied argument is unusable.
inline constexpr Result kNotFound = -5;            // A named entity does not exist.
inline constexpr Result kTimeout = -6;             // A bounded wait expired.
inline constexpr Result kCancelled = -7;           // Cancelled before completion.
inline constexpr Result kUnsupportedVersion = -8;  // ABI or interface version negotiation failed.
inline constexpr Result kInvalidState = -9;        // The object is not in a state that permits this.
inline constexpr Result kAlreadyClosed = -10;      // The object has been closed or drained.

// Inline functions, not macros: they obey scope, respect the signed type and can
// be used in constant expressions.
constexpr bool IsSuccess(Result r) noexcept { return r >= 0; }
constexpr bool IsFailure(Result r) noexcept { return r < 0; }

static_assert(IsSuccess(kOk) && IsSuccess(kFalse), "kOk and kFalse are successes.");
static_assert(IsFailure(kFail) && IsFailure(kAlreadyClosed), "negative codes are failures.");
static_assert(kOk != kFalse, "a successful \"no\" must be distinguishable from a plain success.");
static_assert(kInvalidArgument != kNotFound && kInvalidArgument != kNoInterface,
              "every failure code is distinct: the reference gives two conditions the same value.");

}  // namespace clashqt::com

#endif  // CLASHQT_CORE_COMPONENT_RESULT_H
