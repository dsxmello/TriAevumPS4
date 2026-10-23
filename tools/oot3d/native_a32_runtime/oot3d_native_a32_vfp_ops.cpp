#include "oot3d_native_a32_vfp_ops.h"

// Compile the pinned implementation as the single authoritative scalar VFP
// translation unit, then expose operation-level entry points for generated
// AOT code without changing the provenance-checked snapshot.
#include "upstream/recomp/a32_vfp_scalar.cpp"

#include <bit>
#include <cmath>

namespace oot3d::recomp::a32 {

namespace {

// Host-float fast paths with the authoritative result and flags (the same
// bounds as oot3d_native_a32_vfp_fast.h): round-to-nearest where the
// operation rounds, operands +/-0 or normal with a biased exponent of at
// least 24, nonzero results normal with a biased exponent of at least 2.
// Everything else takes the software implementation.
constexpr std::uint32_t kFastRoundingModeMask = 0x00C00000U;

constexpr std::uint32_t FastExponent(std::uint32_t bits) noexcept {
    return (bits >> 23U) & 0xFFU;
}

constexpr bool FastOperand(std::uint32_t bits) noexcept {
    const std::uint32_t exponent = FastExponent(bits);
    return (exponent >= 24U && exponent != 0xFFU) || (bits & 0x7FFFFFFFU) == 0U;
}

constexpr bool FastNonzeroResult(std::uint32_t bits) noexcept {
    const std::uint32_t exponent = FastExponent(bits);
    return exponent >= 2U && exponent != 0xFFU;
}

bool FastDivide(std::uint32_t left, std::uint32_t right, std::uint32_t fpscr,
                VfpBinary32Result& result) noexcept {
    if ((fpscr & kFastRoundingModeMask) != 0U || !FastOperand(left) ||
        !FastOperand(right) || (right & 0x7FFFFFFFU) == 0U) {
        return false;
    }
    const float numerator = std::bit_cast<float>(left);
    const float denominator = std::bit_cast<float>(right);
    const float quotient = numerator / denominator;
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(quotient);
    if ((left & 0x7FFFFFFFU) == 0U) {
        result = {bits, 0U};
        return true;
    }
    if (!FastNonzeroResult(bits)) {
        return false;
    }
    // Exact in double: 24 x 24 significand bits.
    const bool exact = static_cast<double>(quotient) * static_cast<double>(denominator) ==
                       static_cast<double>(numerator);
    result = {bits, exact ? 0U : kFpscrIxc};
    return true;
}

bool FastSquareRoot(std::uint32_t value, std::uint32_t fpscr,
                    VfpBinary32Result& result) noexcept {
    if ((fpscr & kFastRoundingModeMask) != 0U || !FastOperand(value)) {
        return false;
    }
    if ((value & 0x7FFFFFFFU) == 0U) {
        result = {value, 0U};  // sqrt(+/-0) = +/-0
        return true;
    }
    if ((value & kSignBit) != 0U) {
        return false;
    }
    const float operand = std::bit_cast<float>(value);
    const float root = std::sqrt(operand);
    const bool exact = static_cast<double>(root) * static_cast<double>(root) ==
                       static_cast<double>(operand);
    result = {std::bit_cast<std::uint32_t>(root), exact ? 0U : kFpscrIxc};
    return true;
}

// VCVT to integer rounds toward zero whatever FPSCR says; only the
// flush-to-zero of a denormal input depends on it (left to the fallback).
bool FastToSigned(std::uint32_t value, VfpBinary32Result& result) noexcept {
    const std::uint32_t exponent = FastExponent(value);
    if (exponent == 0U) {
        if ((value & 0x7FFFFFFFU) != 0U) return false;
        result = {0U, 0U};
        return true;
    }
    if (exponent >= 127U + 31U) {
        return false;
    }
    const float operand = std::bit_cast<float>(value);
    const auto integer = static_cast<std::int32_t>(operand);
    result = {static_cast<std::uint32_t>(integer),
              static_cast<float>(integer) == operand ? 0U : kFpscrIxc};
    return true;
}

bool FastToUnsigned(std::uint32_t value, VfpBinary32Result& result) noexcept {
    const std::uint32_t exponent = FastExponent(value);
    if (exponent == 0U) {
        if ((value & 0x7FFFFFFFU) != 0U) return false;
        result = {0U, 0U};
        return true;
    }
    if ((value & kSignBit) != 0U || exponent >= 127U + 32U) {
        return false;
    }
    const float operand = std::bit_cast<float>(value);
    const auto integer = static_cast<std::uint32_t>(operand);
    result = {integer,
              static_cast<double>(integer) == static_cast<double>(operand) ? 0U : kFpscrIxc};
    return true;
}

bool FastFromUnsigned(std::uint32_t value, std::uint32_t fpscr,
                      VfpBinary32Result& result) noexcept {
    if ((fpscr & kFastRoundingModeMask) != 0U) {
        return false;
    }
    const float converted = static_cast<float>(value);
    result = {std::bit_cast<std::uint32_t>(converted),
              static_cast<double>(converted) == static_cast<double>(value) ? 0U : kFpscrIxc};
    return true;
}

} // namespace

VfpBinary32Result VfpBinary32Add(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    const Result result = F32Add(left, right, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32Subtract(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    const Result result = F32Sub(left, right, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32Divide(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    if (VfpBinary32Result fast{}; FastDivide(left, right, fpscr, fast)) [[likely]] {
        return fast;
    }
    const Result result = F32Div(left, right, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32MultiplySubtract(
    std::uint32_t accumulator,
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    const Result result =
        F32Mls(accumulator, left, right, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32NegativeMultiplyAccumulate(
    std::uint32_t accumulator,
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    const Result result =
        F32Nmla(accumulator, left, right, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32NegativeMultiplySubtract(
    std::uint32_t accumulator,
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    const Result result =
        F32Nmls(accumulator, left, right, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32NegativeMultiply(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    const Result result = F32Nmul(left, right, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32SquareRoot(
    std::uint32_t value,
    std::uint32_t fpscr) noexcept {
    if (VfpBinary32Result fast{}; FastSquareRoot(value, fpscr, fast)) [[likely]] {
        return fast;
    }
    const Result result = F32Sqrt(value, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32FromUnsigned(
    std::uint32_t value,
    std::uint32_t fpscr) noexcept {
    if (VfpBinary32Result fast{}; FastFromUnsigned(value, fpscr, fast)) [[likely]] {
        return fast;
    }
    const Result result = F32FromUnsigned(value, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32ToUnsigned(
    std::uint32_t value,
    std::uint32_t fpscr) noexcept {
    if (VfpBinary32Result fast{}; FastToUnsigned(value, fast)) [[likely]] {
        return fast;
    }
    const Result result = F32ToInteger(
        value, false, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

VfpBinary32Result VfpBinary32ToSigned(
    std::uint32_t value,
    std::uint32_t fpscr) noexcept {
    if (VfpBinary32Result fast{}; FastToSigned(value, fast)) [[likely]] {
        return fast;
    }
    const Result result = F32ToInteger(
        value, true, ControlFromFpscr(fpscr));
    return {result.value, result.flags & kExceptionFlags};
}

} // namespace oot3d::recomp::a32
