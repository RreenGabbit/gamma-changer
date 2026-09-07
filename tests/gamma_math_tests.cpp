#include "gamma_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

using gamma_changer::ApplyRelativeGamma;
using gamma_changer::ClampGamma;
using gamma_changer::GammaRamp;
using gamma_changer::IsMonotonic;
using gamma_changer::MakeIdentityRamp;
using gamma_changer::RampsApproximatelyEqual;

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void TestIdentityRamp() {
    const GammaRamp identity = MakeIdentityRamp();
    for (const auto& channel : identity.channels) {
        Require(channel.front() == 0U, "identity starts at zero");
        Require(channel.back() == 65535U, "identity reaches the full 16-bit endpoint");
        Require(channel[128] == 32896U, "identity maps 8-bit values with x257 scaling");
    }
    Require(IsMonotonic(identity), "identity is monotonic");
}

void TestNeutralIsByteExact() {
    GammaRamp custom = MakeIdentityRamp();
    custom.channels[0][63] = 12345U;
    custom.channels[1][127] = 30001U;
    custom.channels[2][191] = 50002U;
    const GammaRamp neutral = ApplyRelativeGamma(custom, 1.0);
    Require(RampsApproximatelyEqual(custom, neutral, 0U), "neutral is byte-exact");
}

void TestDirectionAndEndpoints() {
    const GammaRamp identity = MakeIdentityRamp();
    const GammaRamp brighter = ApplyRelativeGamma(identity, 2.0);
    const GammaRamp darker = ApplyRelativeGamma(identity, 0.5);

    for (std::size_t channel = 0; channel < 3U; ++channel) {
        Require(brighter.channels[channel].front() == 0U, "brighter curve keeps black");
        Require(brighter.channels[channel].back() == 65535U, "brighter curve keeps white");
        Require(darker.channels[channel].front() == 0U, "darker curve keeps black");
        Require(darker.channels[channel].back() == 65535U, "darker curve keeps white");
        Require(
            brighter.channels[channel][128] > identity.channels[channel][128],
            "gamma above one brightens midtones");
        Require(
            darker.channels[channel][128] < identity.channels[channel][128],
            "gamma below one darkens midtones");
    }
    Require(IsMonotonic(brighter), "brighter curve is monotonic");
    Require(IsMonotonic(darker), "darker curve is monotonic");
}

void TestKnownMidpoint() {
    const GammaRamp identity = MakeIdentityRamp();
    const GammaRamp gammaTwo = ApplyRelativeGamma(identity, 2.0);
    const double expected = std::sqrt(128.0 / 255.0) * 65535.0;
    Require(
        std::abs(static_cast<double>(gammaTwo.channels[0][128]) - expected) < 2.0,
        "gamma-two midpoint matches the power curve");
}

void TestClamping() {
    Require(ClampGamma(0.01) == gamma_changer::kMinimumGamma, "low gamma is clamped");
    Require(ClampGamma(99.0) == gamma_changer::kMaximumGamma, "high gamma is clamped");
    Require(
        ClampGamma(std::numeric_limits<double>::quiet_NaN()) == 1.0,
        "non-finite gamma becomes neutral");
}

void TestCalibratedInterpolation() {
    // Distinct piecewise-linear calibrations exercise interpolation, clipping,
    // plateaus, and nonzero black points without deriving the expected values
    // from the implementation's sample-index calculation.
    GammaRamp baseline{};
    for (std::size_t index = 0; index < gamma_changer::kGammaRampEntries; ++index) {
        baseline.channels[0][index] = static_cast<std::uint16_t>(
            100U + std::min(index, std::size_t{128}) * 400U);
        baseline.channels[1][index] = static_cast<std::uint16_t>(3000U + index * 200U);
        baseline.channels[2][index] = static_cast<std::uint16_t>(
            4096U + (index > 96U ? (index - 96U) * 300U : 0U));
    }

    for (int step = 50; step <= 300; ++step) {
        const double gamma = static_cast<double>(step) / 100.0;
        const GammaRamp adjusted = ApplyRelativeGamma(baseline, gamma);
        Require(IsMonotonic(adjusted), "calibrated curves remain monotonic at every gamma");
        for (std::size_t channel = 0; channel < gamma_changer::kGammaRampChannels; ++channel) {
            Require(
                adjusted.channels[channel].front() == baseline.channels[channel].front() &&
                    adjusted.channels[channel].back() == baseline.channels[channel].back(),
                "correction preserves each channel's calibrated black and white points");
        }
        for (std::size_t index = 0; index < gamma_changer::kGammaRampEntries; ++index) {
            const long double position =
                std::pow(static_cast<long double>(index) / 255.0L, 1.0L / gamma) * 255.0L;
            const std::array<long double, 3> expected = {
                100.0L + std::min(position, 128.0L) * 400.0L,
                3000.0L + position * 200.0L,
                4096.0L + std::max(position - 96.0L, 0.0L) * 300.0L,
            };
            for (std::size_t channel = 0; channel < expected.size(); ++channel) {
                Require(
                    std::abs(static_cast<long>(adjusted.channels[channel][index]) -
                             std::lround(expected[channel])) <= 1L,
                    "all RGB channels follow their own calibrated curve within rounding");
            }
        }
    }
}

void TestInvalidGammaPreservesBaseline() {
    GammaRamp baseline = MakeIdentityRamp();
    baseline.channels[0][63] = 12345U;
    baseline.channels[1][127] = 30001U;
    baseline.channels[2][191] = 50002U;
    const std::array<double, 3> invalid = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    for (const double gamma : invalid) {
        Require(
            RampsApproximatelyEqual(baseline, ApplyRelativeGamma(baseline, gamma), 0U),
            "invalid gamma returns the exact calibration baseline");
    }
    Require(
        RampsApproximatelyEqual(
            ApplyRelativeGamma(baseline, 0.01),
            ApplyRelativeGamma(baseline, gamma_changer::kMinimumGamma),
            0U),
        "out-of-range darkening matches the minimum gamma");
    Require(
        RampsApproximatelyEqual(
            ApplyRelativeGamma(baseline, 99.0),
            ApplyRelativeGamma(baseline, gamma_changer::kMaximumGamma),
            0U),
        "out-of-range brightening matches the maximum gamma");
}

void TestReadbackTolerance() {
    const GammaRamp baseline = MakeIdentityRamp();
    GammaRamp actual = baseline;
    actual.channels[2][254] -= 1024U;
    Require(RampsApproximatelyEqual(baseline, actual), "readback accepts the tolerance boundary");
    Require(RampsApproximatelyEqual(actual, baseline), "readback tolerance is symmetric");
    Require(!RampsApproximatelyEqual(baseline, actual, 0U), "exact comparison rejects quantization");
    --actual.channels[2][254];
    Require(!RampsApproximatelyEqual(baseline, actual), "readback rejects changes beyond tolerance");
    Require(!RampsApproximatelyEqual(actual, baseline), "readback rejects differences in either direction");
}

void TestWindowsSafetyEnvelope() {
    const GammaRamp identity = MakeIdentityRamp();
    for (int step = 50; step <= 300; ++step) {
        const GammaRamp adjusted = ApplyRelativeGamma(identity, static_cast<double>(step) / 100.0);
        Require(IsMonotonic(adjusted), "every selectable gamma is monotonic");
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            for (std::size_t index = 0; index < 256U; ++index) {
                const auto difference = std::abs(
                    static_cast<int>(adjusted.channels[channel][index]) -
                    static_cast<int>(identity.channels[channel][index]));
                // GDI requires each value to remain within 32768 of identity.
                Require(
                    difference <= 32768,
                    "every selectable gamma stays inside the GDI safety envelope");
            }
        }
    }
}

}  // namespace

int main() {
    TestIdentityRamp();
    TestNeutralIsByteExact();
    TestDirectionAndEndpoints();
    TestKnownMidpoint();
    TestClamping();
    TestCalibratedInterpolation();
    TestInvalidGammaPreservesBaseline();
    TestReadbackTolerance();
    TestWindowsSafetyEnvelope();
    std::cout << "gamma_math_tests: all tests passed\n";
    return 0;
}
