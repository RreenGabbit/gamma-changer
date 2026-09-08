#include "gamma_toggle.h"

#include <cstdlib>
#include <iostream>

int main() {
    const auto check = [](bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    };
    double gamma = 1.0;
    for (const auto* text : {L"", L" ", L"abc", L"1.5x", L"1 2", L"nan", L"inf",
                             L"0.49", L"3.01", L"-1", L"1,5", L"1e999"}) {
        check(!gamma_changer::ParseToggleGamma(text, gamma), "Invalid value accepted");
        check(gamma == 1.0, "Invalid input changed saved value");
    }
    check(gamma_changer::ParseToggleGamma(L" 1.756 ", gamma) && gamma == 1.76,
          "Decimal rounding or whitespace failed");
    check(gamma_changer::ParseToggleGamma(L"0.50", gamma) && gamma == 0.5,
          "Minimum rejected");
    check(gamma_changer::ParseToggleGamma(L"3.00", gamma) && gamma == 3.0,
          "Maximum rejected");
    using gamma_changer::NextToggleGamma;
    check(NextToggleGamma(1.0, 0.8, 2.2) == 2.2, "First press from another value failed");
    check(NextToggleGamma(2.2, 0.8, 2.2) == 0.8, "Return to custom first value failed");
    check(NextToggleGamma(0.8, 0.8, 2.2) == 2.2, "Toggle back failed");
    check(NextToggleGamma(0.8, 2.2, 0.8) == 2.2, "Reversed endpoints failed");
    check(NextToggleGamma(1.5, 1.5, 1.5) == 1.5, "Equal endpoints failed");
    check(NextToggleGamma(3.0, 1.0, 3.0) == 1.0, "Baseline toggle failed");
}
