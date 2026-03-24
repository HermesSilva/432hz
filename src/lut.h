#pragma once
// =============================================================================
//  lut.h  —  64-bit double-precision sine Look-Up Table
//
//  Design goals
//  ------------
//  • The LUT is computed ONCE at startup using the standard library sin(),
//    so any floating-point imprecision is confined to build-time.
//  • At runtime, audio is synthesised using pure integer phase arithmetic —
//    no sin() calls, no floating-point error accumulation in the hot path.
//  • LUT_SIZE must be a power of two so that the phase mask wraps for free.
//
//  Phase indexing
//  --------------
//  For a note at frequency F (integer Hz) and sample rate SR:
//
//      phase_increment = (F * LUT_SIZE) / SR
//
//  Because F, LUT_SIZE and SR are all integers, phase_increment is computed
//  once as a 64-bit integer.  The fractional part of a non-integer quotient
//  is handled by carrying the remainder in a 64-bit fixed-point accumulator:
//
//      accumulator is Q32.32: high 32 bits = LUT index, low 32 = sub-sample
//
//  This gives sub-sample accuracy without any floating-point math in the
//  audio callback.
// =============================================================================

#include <array>
#include <cstdint>
#include <cmath>

// LUT size — 2^16 = 65536 entries gives ~0.0055° angular resolution,
// more than sufficient for audio.  Increase to 2^20 for even lower
// inter-sample error, at the cost of ~8 MB.
inline constexpr uint32_t LUT_SIZE       = 65536u;          // 2^16
inline constexpr uint32_t LUT_MASK       = LUT_SIZE - 1u;
inline constexpr double   TWO_PI         = 6.283185307179586476925;

// The table itself: one full cycle stored as doubles in [-1.0, +1.0].
// Declared extern so the compiler won't duplicate it across TUs.
extern const std::array<double, LUT_SIZE> SINE_LUT;

// ---------------------------------------------------------------------------
//  Fixed-point phase accumulator (Q32.32)
//  Each voice carries one of these.
// ---------------------------------------------------------------------------
struct PhaseAccum {
    uint64_t value = 0;          // Q32.32 accumulator
    uint64_t delta = 0;          // Q32.32 increment per sample

    // Initialise for a given integer frequency at a given sample rate.
    // Both must be positive integers; no floating-point division at runtime.
    void init(uint32_t freq_hz, uint32_t sample_rate) {
        // delta = freq_hz * LUT_SIZE / sample_rate  (as Q32.32)
        // We scale freq_hz by 2^32 first to get the fractional bits.
        const uint64_t numerator = static_cast<uint64_t>(freq_hz) *
                                   static_cast<uint64_t>(LUT_SIZE) << 32u;
        delta = numerator / static_cast<uint64_t>(sample_rate);
        value = 0;
    }

    // Double-precision variant for non-integer frequencies (equal temperament).
    // Uses floating-point to compute the Q32.32 delta; accuracy is ~2^-52
    // relative error — negligible over the lifetime of any audio buffer.
    void init(double freq_hz, uint32_t sample_rate) {
        // Q32.32 scale factor = LUT_SIZE * 2^32
        // delta = (freq_hz / sample_rate) * LUT_SIZE * 2^32
        const double scale =
            static_cast<double>(static_cast<uint64_t>(LUT_SIZE) << 32u);
        delta = static_cast<uint64_t>(
            (freq_hz / static_cast<double>(sample_rate)) * scale);
        value = 0;
    }

    // Advance by one sample and return the current LUT index (upper 16 bits).
    [[nodiscard]] inline uint32_t advance() {
        value += delta;
        return static_cast<uint32_t>(value >> 32u) & LUT_MASK;
    }

    // Read the current sample without advancing.
    [[nodiscard]] inline double sample() const {
        const uint32_t idx = static_cast<uint32_t>(value >> 32u) & LUT_MASK;
        return SINE_LUT[idx];
    }
};
