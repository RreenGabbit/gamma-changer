#pragma once

#include <cmath>
#include <locale>
#include <sstream>
#include <string>

#include "gamma_math.h"

namespace gamma_changer {

inline bool ParseToggleGamma(const std::wstring& text, double& gamma) {
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    double value = 0.0;
    const bool parsed = static_cast<bool>(input >> value);
    input >> std::ws;
    if (!parsed || !input.eof() || !std::isfinite(value) ||
        value < kMinimumGamma || value > kMaximumGamma) {
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
