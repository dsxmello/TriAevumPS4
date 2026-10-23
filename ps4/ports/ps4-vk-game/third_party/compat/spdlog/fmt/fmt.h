// Minimal {fmt} shim for the TriAevum PS4 port.
//
// The desktop build links spdlog/fmt for the Azahar-derived PICA shader
// decompiler (tools/oot3d/third_party/azahar_pica/.../glsl_shader_decompiler.cpp).
// Only a small subset of the API reaches the PS4 closure:
//   * fmt::format("... {} ... {1} ... {{ }} ...", args...)
//     - "{}" substitutes the next argument sequentially
//     - "{N}" substitutes argument N positionally
//     - "{{" and "}}" produce literal braces
//   * fmt::format_string<Args...> as the format parameter type
// This header implements exactly that subset with runtime substitution.
// Floating-point values use the shortest decimal form that round-trips
// through strtof/strtod, matching {fmt}'s default formatting contract.
#ifndef TRIAEVUM_PS4_COMPAT_FMT
#define TRIAEVUM_PS4_COMPAT_FMT

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fmt {

template <class... Args>
using format_string = std::string_view;

namespace detail {

inline std::string StringifyFloat(double value) {
    for (int precision = 1; precision <= 17; ++precision) {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) {
            return buffer;
        }
    }
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.17g", value);
    return buffer;
}

template <class T>
std::string Stringify(const T &value) {
    if constexpr (std::is_integral_v<std::decay_t<T>>) {
        if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
            return value ? "true" : "false";
        } else if constexpr (std::is_same_v<std::decay_t<T>, char>) {
            return std::string(1, value);
        } else {
            return std::to_string(
                static_cast<long long>(value));
        }
    } else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
        return StringifyFloat(static_cast<double>(value));
    } else if constexpr (std::is_constructible_v<std::string, const T &>) {
        return std::string(value);
    } else if constexpr (std::is_enum_v<std::decay_t<T>>) {
        return std::to_string(static_cast<long long>(value));
    } else {
        static_assert(sizeof(T) == 0,
                      "fmt shim: unsupported argument type");
        return {};
    }
}

inline void AppendFormatted(std::string &out, std::string_view text,
                            const std::vector<std::string> &args) {
    std::size_t nextArgument = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char current = text[index];
        if (current == '{') {
            if (index + 1 < text.size() && text[index + 1] == '{') {
                out += '{';
                ++index;
                continue;
            }
            const std::size_t end = text.find('}', index);
            if (end == std::string_view::npos) {
                break;
            }
            const std::string_view spec = text.substr(index + 1, end - index - 1);
            std::size_t argument = nextArgument;
            if (!spec.empty()) {
                // The closure only uses empty specs and pure indices.
                argument = 0;
                for (const char digit : spec) {
                    argument = argument * 10 + static_cast<std::size_t>(digit - '0');
                }
            } else {
                ++nextArgument;
            }
            if (argument >= args.size()) {
                continue;
            }
            out += args[argument];
            index = end;
        } else if (current == '}') {
            if (index + 1 < text.size() && text[index + 1] == '}') {
                out += '}';
                ++index;
                continue;
            }
            out += '}';
        } else {
            out += current;
        }
    }
}

}  // namespace detail

inline std::string format(std::string_view text) {
    std::string out;
    detail::AppendFormatted(out, text, {});
    return out;
}

template <class... Args>
std::string format(std::string_view text, Args &&...args) {
    const std::vector<std::string> stringified = {
        detail::Stringify(args)...,
    };
    std::string out;
    out.reserve(text.size() + text.size() / 4);
    detail::AppendFormatted(out, text, stringified);
    return out;
}

}  // namespace fmt

#endif
