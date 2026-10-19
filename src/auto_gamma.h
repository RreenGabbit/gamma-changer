#pragma once

#include <cmath>

namespace gamma_changer {

// The gap between these thresholds prevents a scene hovering near the switch
// point from repeatedly changing the display gamma.
constexpr double kAutoSwitchToNightLuminance = 0.020;
constexpr double kAutoSwitchToDayLuminance = 0.055;
constexpr double kAutoLuminanceSmoothing = 0.35;
constexpr int kAutoNightConfirmationSamples = 2;
constexpr int kAutoDayConfirmationSamples = 3;

class AutoGammaDetector {
public:
    void Reset() noexcept {
        initialized_ = false;
        night_ = false;
        confirmations_ = 0;
        filteredLuminance_ = 0.0;
    }

    bool Update(const double luminance) noexcept {
        if (!std::isfinite(luminance)) {
            Reset();
            return false;
        }

        if (!initialized_) {
            filteredLuminance_ = luminance;
            initialized_ = true;
        } else {
            filteredLuminance_ +=
                (luminance - filteredLuminance_) * kAutoLuminanceSmoothing;
        }

        if (!night_) {
            if (filteredLuminance_ <= kAutoSwitchToNightLuminance &&
                luminance <= kAutoSwitchToNightLuminance) {
                ++confirmations_;
                if (confirmations_ >= kAutoNightConfirmationSamples) {
                    night_ = true;
                    confirmations_ = 0;
                }
            } else {
                confirmations_ = 0;
            }
        } else if (filteredLuminance_ >= kAutoSwitchToDayLuminance &&
                   luminance >= kAutoSwitchToDayLuminance) {
            ++confirmations_;
            if (confirmations_ >= kAutoDayConfirmationSamples) {
                night_ = false;
                confirmations_ = 0;
            }
        } else {
            confirmations_ = 0;
        }
        return night_;
    }

    bool IsNight() const noexcept { return night_; }
    double FilteredLuminance() const noexcept { return filteredLuminance_; }

private:
    bool initialized_ = false;
    bool night_ = false;
    int confirmations_ = 0;
    double filteredLuminance_ = 0.0;
};

inline double AutomaticGammaTarget(
    const bool useNightGamma,
    const double maximumGamma) noexcept {
    if (!useNightGamma || !std::isfinite(maximumGamma)) {
        return 1.0;
    }
    return maximumGamma > 1.0 ? maximumGamma : 1.0;
}

}  // namespace gamma_changer
