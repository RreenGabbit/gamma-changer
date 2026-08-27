#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gamma_changer {

constexpr std::size_t kGammaRampEntries = 256;
constexpr std::size_t kGammaRampChannels = 3;
constexpr double kMinimumGamma = 0.50;
constexpr double kMaximumGamma = 3.00;

using GammaChannel = std::array<std::uint16_t, kGammaRampEntries>;

struct GammaRamp {
    std::array<GammaChannel, kGammaRampChannels> channels{};
};

double ClampGamma(double gamma) noexcept;
GammaRamp MakeIdentityRamp() noexcept;

// Composes a power curve with an existing calibration ramp. A gamma of 1.0
// returns the baseline byte-for-byte. Values above 1.0 brighten midtones;
// values below 1.0 darken them.
GammaRamp ApplyRelativeGamma(const GammaRamp& baseline, double gamma) noexcept;

bool RampsApproximatelyEqual(
    const GammaRamp& left,
    const GammaRamp& right,
    std::uint16_t tolerance = 1024) noexcept;

bool IsMonotonic(const GammaRamp& ramp) noexcept;

}  // namespace gamma_changer
