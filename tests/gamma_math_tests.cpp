#include "gamma_math.h"

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
    TestWindowsSafetyEnvelope();
    std::cout << "gamma_math_tests: all tests passed\n";
    return 0;
}
