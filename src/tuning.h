#pragma once
// =============================================================================
//  tuning.h  —  Runtime A4 pitch and scale-division configuration
//
//  Two modes
//  ---------
//  TuneMode::INTEGER  (default)
//    Intervals are pure rational numbers rooted at A4 = a4 Hz.
//    C4 is derived from A4 via the Pythagorean major sixth (27:16):
//        C4 = A4 × 16/27
//    All other pitches use the just-intonation / Pythagorean chromatic ratios
//    below.  When a4 = 432:  C4 = 256, D4 = 288, A4 = 432 —  exact integers.
//    For any other A4 the frequencies are rational multiples (not integers) but
//    preserve all pure-interval relationships.
//
//    Chromatic ratios from C (one octave):
//      C   1/1        C#  17/16      D   9/8       D#  19/16
//      E   5/4        F   4/3        F#  45/32     G   3/2
//      G#  405/256    A   27/16      A#  225/128   B   15/8
//    (G# comes from A×15/16; at A4=432 → 432×15/16 = 405 Hz ✓)
//
//  TuneMode::EQUAL
//    Standard 12-TET equal temperament.  A4 = a4 Hz.
//        f(n) = a4 × 2^((n − 69) / 12)   (n = MIDI note number, A4 = 69)
//
//  SF2 detuning
//    For TinySoundFont channel tuning (both modes):
//        detune_semitones = 12 × log2(a4 / 440)
//    Applied to all 16 MIDI channels so sample playback is transposed to a4.
//
//  Comparison utility
//    printScale()      — one octave (C4–C5) with INTEGER vs EQUAL side-by-side
//    printComparison() — three-column: Integer-432, Equal-432, Equal-440
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
enum class TuneMode { INTEGER, EQUAL };

// ---------------------------------------------------------------------------
namespace tuning_detail {

// Chromatic ratios from C — INTEGER mode.
// Reproduce exact Hz values in notes_432.h when A4 = 432 (C4 = 256).
static constexpr double RATIO[12] = {
     1.0,           //  0  C   = 1/1
    17.0 / 16.0,    //  1  C#  = 17/16
     9.0 /  8.0,    //  2  D   = 9/8
    19.0 / 16.0,    //  3  D#  = 19/16
     5.0 /  4.0,    //  4  E   = 5/4
     4.0 /  3.0,    //  5  F   = 4/3         (341.333… → 341 Hz in the static table)
    45.0 / 32.0,    //  6  F#  = 45/32
     3.0 /  2.0,    //  7  G   = 3/2
   405.0 / 256.0,   //  8  G#  = 405/256     = A4 × 15/16
    27.0 / 16.0,    //  9  A   = 27/16        Pythagorean major sixth from C
   225.0 / 128.0,   // 10  A#  = 225/128
    15.0 /  8.0     // 11  B   = 15/8
};

static constexpr int MIDI_A4 = 69;  // A4
static constexpr int MIDI_C4 = 60;  // C4
static constexpr int MIDI_C2 = 36;  // lowest note in NOTE_TABLE

static constexpr const char* CHROMA[12] = {
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};

} // namespace tuning_detail

// ---------------------------------------------------------------------------
//  TuningConfig
// ---------------------------------------------------------------------------
struct TuningConfig {
    double   a4   = 432.0;          // A4 reference frequency in Hz
    TuneMode mode = TuneMode::INTEGER;

    // -----------------------------------------------------------------------
    //  noteFreq — Hz for MIDI note n (0–127)
    // -----------------------------------------------------------------------
    double noteFreq(int midi) const noexcept
    {
        using namespace tuning_detail;

        if (mode == TuneMode::EQUAL)
            return a4 * std::pow(2.0, (midi - MIDI_A4) / 12.0);

        // INTEGER mode: build from C4 via rational ratios.
        // C4 = A4 × 16/27  (inverse of the Pythagorean major sixth 27/16)
        const double c4 = a4 * (16.0 / 27.0);
        const int    d  = midi - MIDI_C4;   // semitones from Middle C (may be neg.)

        int oct = 0, chr = d;
        if (chr < 0) {
            const int up = (-chr + 11) / 12;
            chr += up * 12;
            oct -= up;
        }
        oct += chr / 12;
        chr  = chr % 12;

        return c4 * RATIO[chr] * std::pow(2.0, static_cast<double>(oct));
    }

    // Rounded integer Hz — for display / legacy NOTE_TABLE comparisons
    uint32_t noteHz(int midi) const noexcept {
        return static_cast<uint32_t>(std::lround(noteFreq(midi)));
    }

    // -----------------------------------------------------------------------
    //  sf2Detune — semitones to shift vs 440 Hz (for TinySoundFont channels)
    //  Valid for both INTEGER and EQUAL modes: A4 is always a4 Hz.
    // -----------------------------------------------------------------------
    double sf2Detune() const noexcept {
        return 12.0 * std::log2(a4 / 440.0);
    }

    const char* modeName() const noexcept {
        return mode == TuneMode::INTEGER ? "integer" : "equal";
    }
    static const char* modeList() noexcept { return "integer | equal"; }

    // -----------------------------------------------------------------------
    //  printScale — C4–C5 table, current mode active, side-by-side comparison
    // -----------------------------------------------------------------------
    void printScale() const
    {
        using namespace tuning_detail;
        TuningConfig eq; eq.a4 = a4; eq.mode = TuneMode::EQUAL;
        TuningConfig in; in.a4 = a4; in.mode = TuneMode::INTEGER;

        std::printf("\n==========================================================\n");
        std::printf("  Tuning  A4 = %.4f Hz   Mode: %s\n", a4, modeName());
        std::printf("==========================================================\n");
        std::printf("  %-5s  %9s  %9s  %8s  %s\n",
                    "Note", "Integer", "Equal", "Diff", "Active");
        std::printf("  %-5s  %9s  %9s  %8s  %s\n",
                    "-----", "-------", "-----", "------", "------");

        for (int semi = 0; semi <= 12; ++semi) {
            const int    midi  = MIDI_C4 + semi;
            const double f_int = in.noteFreq(midi);
            const double f_eq  = eq.noteFreq(midi);
            const double cents = 1200.0 * std::log2(f_int / f_eq);
            const double f_cur = noteFreq(midi);

            char name[8];
            std::snprintf(name, sizeof(name), "%s%d",
                          CHROMA[semi % 12], 4 + semi / 12);

            const bool is_int = (mode == TuneMode::INTEGER);
            std::printf("  %-5s  %9.4f  %9.4f  %+7.2fc  [%s]\n",
                        name, f_int, f_eq, cents,
                        is_int ? "integer" : "equal  ");
            (void)f_cur;
        }
        std::printf("==========================================================\n\n");
    }

    // -----------------------------------------------------------------------
    //  printComparison — three-column: Integer-432, Equal-432, Equal-440
    //  Shows cents deviation of Integer-432 vs both equal-temperament refs.
    // -----------------------------------------------------------------------
    void printComparison() const
    {
        using namespace tuning_detail;
        TuningConfig i432; i432.a4 = 432.0; i432.mode = TuneMode::INTEGER;
        TuningConfig e432; e432.a4 = 432.0; e432.mode = TuneMode::EQUAL;
        TuningConfig e440; e440.a4 = 440.0; e440.mode = TuneMode::EQUAL;

        std::printf("\n");
        std::printf("=================================================================\n");
        std::printf("  Tuning comparison  C4 – C5\n");
        std::printf("  Columns: Integer-432 Hz  |  Equal-432 Hz  |  Equal-440 Hz\n");
        std::printf("  Diff = Integer-432 vs reference  (+ = sharper,  - = flatter)\n");
        std::printf("=================================================================\n");
        std::printf("  %-5s  %9s  %9s  %9s  %8s  %8s\n",
                    "Note", "Int-432", "Eq-432", "Eq-440",
                    "vsEq-432", "vsEq-440");
        std::printf("  %-5s  %9s  %9s  %9s  %8s  %8s\n",
                    "-----", "-------", "------", "------",
                    "(cents)", "(cents)");
        std::printf("  %s\n", std::string(67, '-').c_str());

        for (int semi = 0; semi <= 12; ++semi) {
            const int    midi = MIDI_C4 + semi;
            const double fi   = i432.noteFreq(midi);
            const double e32  = e432.noteFreq(midi);
            const double e40  = e440.noteFreq(midi);
            const double c32  = 1200.0 * std::log2(fi / e32);
            const double c40  = 1200.0 * std::log2(fi / e40);

            char name[8];
            std::snprintf(name, sizeof(name), "%s%d",
                          CHROMA[semi % 12], 4 + semi / 12);

            std::printf("  %-5s  %9.4f  %9.4f  %9.4f  %+7.2fc  %+7.2fc\n",
                        name, fi, e32, e40, c32, c40);
        }
        std::printf("=================================================================\n");
        std::printf("  A4 Integer-432 vs Equal-432: %.2f cents\n",
                    1200.0 * std::log2(432.0 / e432.noteFreq(69)));
        std::printf("  A4 Integer-432 vs Equal-440: %.2f cents\n\n",
                    1200.0 * std::log2(432.0 / e440.noteFreq(69)));
    }
};

// ---------------------------------------------------------------------------
//  Global instance.  Written only from the main console thread; read from
//  audio callback (hot path) and MIDI thread — no lock needed because changes
//  take effect on the next note-on (in-flight voices are unaffected).
// ---------------------------------------------------------------------------
static TuningConfig g_tuning;
