// =============================================================================
//  lut.cpp  —  Sine LUT initialisation
//
//  The table is filled at program startup via a global constructor.
//  This is the ONLY place sin() is ever called in the entire program.
// =============================================================================

#include "lut.h"
#include <cmath>

// Build the LUT.  Using a lambda + immediately-invoked to keep it const-
// qualified while still allowing the loop to fill the array.
const std::array<double, LUT_SIZE> SINE_LUT = []() {
    std::array<double, LUT_SIZE> t{};
    for (uint32_t i = 0; i < LUT_SIZE; ++i) {
        // angle in [0, 2*pi)
        t[i] = std::sin((TWO_PI * static_cast<double>(i))
                        / static_cast<double>(LUT_SIZE));
    }
    return t;
}();
