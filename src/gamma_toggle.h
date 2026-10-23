#pragma once

#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <string>

#include "gamma_math.h"

namespace gamma_changer {

inline bool ParseToggleGamma(const std::wstring& text, double& gamma) {
    // Accept [ws][+-]digits[.digits][e[+-]digits][ws] only; strtod alone would
    // also take hex floats, "inf" and "nan". Parsing stays locale-independent
    // because the application never calls setlocale.
    std::string ascii;
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::iswspace(text[begin])) {
        ++begin;
    }
    while (end > begin && std::iswspace(text[end - 1U])) {
        --end;
    }
    if (begin == end) {
        return false;
    }
    for (std::size_t index = begin; index < end; ++index) {
        const wchar_t c = text[index];
        if (c > 0x7F) {
            return false;
        }
        ascii.push_back(static_cast<char>(c));
    }

    std::size_t i = 0;
    const auto digits = [&] {
        const std::size_t start = i;
        while (i < ascii.size() && ascii[i] >= '0' && ascii[i] <= '9') {
            ++i;
        }
        return i - start;
    };
    if (ascii[i] == '+' || ascii[i] == '-') {
        ++i;
    }
    std::size_t mantissa = digits();
    if (i < ascii.size() && ascii[i] == '.') {
        ++i;
        mantissa += digits();
    }
    if (mantissa == 0U) {
        return false;
    }
    if (i < ascii.size() && (ascii[i] == 'e' || ascii[i] == 'E')) {
        ++i;
        if (i < ascii.size() && (ascii[i] == '+' || ascii[i] == '-')) {
            ++i;
        }
        if (digits() == 0U) {
            return false;
        }
    }
    if (i != ascii.size()) {
        return false;
    }

    const double value = std::strtod(ascii.c_str(), nullptr);
    if (!std::isfinite(value) || value < kMinimumGamma || value > kMaximumGamma) {
        return false;
    }
    gamma = std::round(value * 100.0) / 100.0;
    return true;
}

// From any other slider value, the first press selects the second endpoint.
inline double NextToggleGamma(double current, double first, double second) noexcept {
    return std::abs(current - second) < 0.0001 ? first : second;
}

}  // namespace gamma_changer
