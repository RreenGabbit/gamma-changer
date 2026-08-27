#include "gamma_math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gamma_changer {

double ClampGamma(const double gamma) noexcept {
    if (!std::isfinite(gamma)) {
        return 1.0;
    }
    return std::clamp(gamma, kMinimumGamma, kMaximumGamma);
}

GammaRamp MakeIdentityRamp() noexcept {
    GammaRamp ramp{};
    for (auto& channel : ramp.channels) {
        for (std::size_t index = 0; index < channel.size(); ++index) {
            // 257 maps the 8-bit endpoints exactly onto the full 16-bit range.
            channel[index] = static_cast<std::uint16_t>(index * 257U);
        }
    }
    return ramp;
}

GammaRamp ApplyRelativeGamma(const GammaRamp& baseline, const double requestedGamma) noexcept {
    const double gamma = ClampGamma(requestedGamma);
    if (std::abs(gamma - 1.0) < std::numeric_limits<double>::epsilon()) {
        return baseline;
    }

    GammaRamp result{};
    constexpr double lastIndex = static_cast<double>(kGammaRampEntries - 1U);

    for (std::size_t channelIndex = 0; channelIndex < kGammaRampChannels; ++channelIndex) {
        const auto& source = baseline.channels[channelIndex];
        auto& destination = result.channels[channelIndex];

        for (std::size_t index = 0; index < kGammaRampEntries; ++index) {
            const double normalized = static_cast<double>(index) / lastIndex;
            const double sourcePosition = std::pow(normalized, 1.0 / gamma) * lastIndex;
            const auto lower = static_cast<std::size_t>(std::floor(sourcePosition));
            const auto upper = std::min(lower + 1U, kGammaRampEntries - 1U);
            const double fraction = sourcePosition - static_cast<double>(lower);
            const double interpolated =
                static_cast<double>(source[lower]) * (1.0 - fraction) +
                static_cast<double>(source[upper]) * fraction;
            destination[index] = static_cast<std::uint16_t>(
                std::clamp(std::lround(interpolated), 0L, 65535L));
        }
    }

    return result;
}

bool RampsApproximatelyEqual(
    const GammaRamp& left,
    const GammaRamp& right,
    const std::uint16_t tolerance) noexcept {
    for (std::size_t channel = 0; channel < kGammaRampChannels; ++channel) {
        for (std::size_t index = 0; index < kGammaRampEntries; ++index) {
            const auto a = static_cast<int>(left.channels[channel][index]);
            const auto b = static_cast<int>(right.channels[channel][index]);
            if (std::abs(a - b) > static_cast<int>(tolerance)) {
                return false;
            }
        }
    }
    return true;
}

bool IsMonotonic(const GammaRamp& ramp) noexcept {
    for (const auto& channel : ramp.channels) {
        for (std::size_t index = 1; index < channel.size(); ++index) {
            if (channel[index] < channel[index - 1U]) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace gamma_changer
