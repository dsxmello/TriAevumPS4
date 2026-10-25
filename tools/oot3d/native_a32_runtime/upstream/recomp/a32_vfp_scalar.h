#pragma once

#include <cstdint>

#include "a32_runtime.h"

namespace oot3d::recomp::a32 {

struct VfpBinary32Result {
    std::uint32_t value{};
    std::uint32_t exception_flags{};
};

VfpBinary32Result VfpBinary32Multiply(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept;
VfpBinary32Result VfpBinary32MultiplyAccumulate(
    std::uint32_t accumulator,
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept;
VfpBinary32Result VfpBinary32FromSigned(
    std::uint32_t value,
    std::uint32_t fpscr) noexcept;
// VCMP/VCMPE. Inline: translated code compares often and the exact result
// is a few integer tests (the same as F32Compare in a32_vfp_scalar.cpp).
inline VfpBinary32Result VfpBinary32Compare(
    std::uint32_t left,
    std::uint32_t right,
    bool signal_all_nans) noexcept {
    constexpr std::uint32_t kSign = 0x80000000U;
    constexpr std::uint32_t kExponent = 0x7F800000U;
    constexpr std::uint32_t kFraction = 0x007FFFFFU;
    constexpr std::uint32_t kQuiet = 0x00400000U;
    constexpr std::uint32_t kEqual = 0x60000000U;
    constexpr std::uint32_t kLess = 0x80000000U;
    constexpr std::uint32_t kGreater = 0x20000000U;
    constexpr std::uint32_t kUnordered = 0x30000000U;
    constexpr std::uint32_t kIoc = 1U << 0U;
    const bool left_nan = (left & kExponent) == kExponent && (left & kFraction) != 0U;
    const bool right_nan = (right & kExponent) == kExponent && (right & kFraction) != 0U;
    if (left_nan || right_nan) {
        const bool invalid = (left_nan && (left & kQuiet) == 0U) ||
                             (right_nan && (right & kQuiet) == 0U) ||
                             signal_all_nans;
        return {kUnordered, invalid ? kIoc : 0U};
    }
    if (((left | right) & ~kSign) == 0U || left == right) {
        return {kEqual, 0U};
    }
    const bool left_sign = (left & kSign) != 0U;
    const bool right_sign = (right & kSign) != 0U;
    if (left_sign != right_sign) {
        return {left_sign ? kLess : kGreater, 0U};
    }
    const bool less = left_sign ? (left & ~kSign) > (right & ~kSign) : left < right;
    return {less ? kLess : kGreater, 0U};
}

// Execute one instruction from the exact scalar binary32/binary64 VFP subset.
// The A32 condition has already been evaluated by ExecuteBlock.  Unsupported
// encodings and dynamic FPSCR modes return Unsupported without committing a
// scalar result, allowing the caller to retain the existing fallback path.
ExecutionResult ExecuteVfpScalar(
    std::uint32_t raw,
    std::uint32_t pc,
    GuestState& state);

}  // namespace oot3d::recomp::a32
