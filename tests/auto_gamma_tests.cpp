#include "auto_gamma.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using gamma_changer::AutoGammaDetector;
using gamma_changer::AutomaticGammaTarget;

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    AutoGammaDetector detector;
    Require(
        !detector.Update(0.004),
        "one dark sample is not enough to switch");
    Require(
        detector.Update(0.004),
        "consecutive dark samples switch to night gamma");
    Require(detector.IsNight(), "night state is retained");

    Require(
        detector.Update(0.030),
        "the dead zone holds night mode");
    Require(
        detector.Update(0.500),
        "a brief bright flash does not immediately leave night mode");
    Require(
        detector.Update(0.004),
        "the detector stays in night mode after the flash");

    for (int sample = 0; sample < gamma_changer::kAutoDayConfirmationSamples;
         ++sample) {
        const bool night = detector.Update(0.500);
        if (sample + 1 < gamma_changer::kAutoDayConfirmationSamples) {
            Require(night, "day switch waits for sustained brightness");
        } else {
            Require(!night, "sustained brightness returns to day gamma");
        }
    }
    Require(!detector.IsNight(), "day state is retained");

    detector.Reset();
    Require(
        !detector.Update(0.030),
        "a day scene inside the dead zone stays neutral");
    Require(
        !detector.Update(0.030),
        "the dead zone prevents switching to night");

    Require(
        AutomaticGammaTarget(true, 2.4) == 2.4,
        "night mode uses the configured maximum");
    Require(
        AutomaticGammaTarget(false, 2.4) == 1.0,
        "day mode uses gamma one");
    Require(
        AutomaticGammaTarget(true, 0.8) == 1.0,
        "automatic mode never darkens below neutral");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Require(
        !detector.Update(nan),
        "invalid luminance is treated as day");
    Require(!detector.IsNight(), "invalid luminance resets night detection");
    Require(
        AutomaticGammaTarget(true, nan) == 1.0,
        "invalid maximum falls back to neutral");

    std::cout << "auto_gamma_tests: all tests passed\n";
    return 0;
}
