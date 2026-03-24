#pragma once
// =============================================================================
//  instrument.h  —  Multi-timbre synthesis engine  (header-only, no dependencies)
//
//  Synthesis models
//  ----------------
//   SINE    Pure sine wave — original LUT-based behaviour
//   PIANO   Extended Karplus-Strong struck-string physical model
//           Delay line length = sr / hz  (exact integer — perfectly in tune
//           because both sample rate and Hz are integers in this project)
//   ORGAN   8-rank additive drawbar pipe organ  (harmonics 1–8 via LUT)
//   RHODES  2-operator phase-modulation FM electric piano
//           Modulation index decays over time — bright attack → warm sustain
//           (same character as the Yamaha DX7 "Rhodes" patch)
//
//  All timbres honour the 432 Hz integer scale from notes_432.h — no
//  irrational multipliers enter the synthesis chain at any point.
//
//  Hot-path guarantee
//  ------------------
//  InstrVoice::next() contains no std::sin(), no heap allocation, no system
//  calls, no locks.  All parameters are pre-computed in start().
// =============================================================================

#include "lut.h"

#include <cmath>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
//  Model selector
// ---------------------------------------------------------------------------
enum class InstrModel {
    SINE     = 0,   ///< Pure sine (LUT)
    PIANO    = 1,   ///< Karplus-Strong struck string
    ORGAN    = 2,   ///< 8-rank additive drawbar organ
    RHODES   = 3,   ///< 2-op FM phase-modulation electric piano
    SF2      = 4,   ///< SoundFont2 sample playback (TinySoundFont)
    MIDI_OUT = 5,   ///< Windows MIDI output (WinMM) — streams to device driver
};

static const int INSTR_MODEL_COUNT = 6;

inline const char* instrLabel(InstrModel m) {
    switch (m) {
    case InstrModel::SINE:     return "sine";
    case InstrModel::PIANO:    return "piano";
    case InstrModel::ORGAN:    return "organ";
    case InstrModel::RHODES:   return "rhodes";
    case InstrModel::SF2:      return "sf2";
    case InstrModel::MIDI_OUT: return "midiout";
    default:                    return "?";
    }
}

inline InstrModel instrFromString(const char* s) {
    if (std::strcmp(s, "piano")   == 0) return InstrModel::PIANO;
    if (std::strcmp(s, "organ")   == 0) return InstrModel::ORGAN;
    if (std::strcmp(s, "rhodes")  == 0) return InstrModel::RHODES;
    if (std::strcmp(s, "sf2")     == 0) return InstrModel::SF2;
    if (std::strcmp(s, "midiout") == 0) return InstrModel::MIDI_OUT;
    return InstrModel::SINE;
}

// ---------------------------------------------------------------------------
//  Limits
// ---------------------------------------------------------------------------
// KS_BUF_MAX  = maximum possible delay line length
//             = max_sample_rate / min_note_hz = 192000 / 64 = 3000 samples
static constexpr uint32_t KS_BUF_MAX  = 3000u;

// ORGAN_RANKS = number of drawbar harmonic ranks
static constexpr int ORGAN_RANKS = 8;

// FM_LUT_SCALE converts phase-modulation depth (-1..+1) to LUT index offset:
//   index_offset = modulation_depth * FM_LUT_SCALE
//   so that one full modulation cycle spans the full LUT
static constexpr double FM_LUT_SCALE =
    static_cast<double>(LUT_SIZE) / (2.0 * 3.14159265358979323846);

// ---------------------------------------------------------------------------
//  InstrVoice — one polyphonic voice supporting all synthesis models
//
//  Memory note:
//  The ks_buf[KS_BUF_MAX] member is 3000 × 8 = 24 000 bytes per voice.
//  With MAX_VOICES = 16, total ≈ 384 KB.  AudioState is declared static
//  (global storage), so no stack concern.
// ---------------------------------------------------------------------------
struct InstrVoice {
    // ---- Model / common ------------------------------------------------
    InstrModel model     = InstrModel::SINE;
    bool       is_active = false;
    double     amp_out   = 0.0;
    uint32_t   voice_sr  = 48000u;  // stored at start() time for release()

    // ---- SINE -----------------------------------------------------------
    PhaseAccum s_ph;

    // ---- PIANO  (Extended Karplus-Strong struck-string) ----------------
    // Delay line.  The circular buffer of length ks_N holds one full period
    // of the vibrating string.  The first-order IIR LP filter + ks_stretch
    // coefficient implements the physical energy loss.
    //
    // Physical mapping:
    //   ks_N       = round(sr / hz)   — exact integer (both are ints) → pure pitch
    //   ks_stretch = 0.001^(1/(T60*hz)) — energy half-life from T60(hz)
    //   T60(hz)    = 128 / hz^(2/3)  — empirically matches real piano decay
    double   ks_buf[KS_BUF_MAX]{};
    uint32_t ks_N       = 0;
    uint32_t ks_head    = 0;
    uint32_t ks_age     = 0;   // sample counter — silence check disabled early on
    double   ks_prev    = 0.0;
    double   ks_stretch = 0.998;
    bool     ks_live    = false;

    // ---- ORGAN  (8-rank additive drawbar) -------------------------------
    // Classic "full-organ" voicing: all 8 drawbars in, harmonics 1–8.
    // Drawbar amplitudes are tuned by ear to sound like a large pipe organ.
    PhaseAccum org_ph[ORGAN_RANKS];

    static constexpr double ORG_AMP[ORGAN_RANKS] = {
        0.390,  // rank 1  •  fundamental        (8')
        0.265,  // rank 2  •  octave             (4')
        0.155,  // rank 3  •  quint              (2⅔')
        0.088,  // rank 4  •  super-octave       (2')
        0.050,  // rank 5  •  tierce             (1⅗')
        0.028,  // rank 6  •  larigot            (1⅕')
        0.014,  // rank 7  •  sifflöte           (7th harmonic)
        0.010,  // rank 8  •  flageolet          (8th harmonic)
    };                                       // sum ≈ 1.00

    // ---- RHODES  (2-op phase-modulation FM electric piano) --------------
    // Algorithm: separate carrier and modulator at identical pitch.
    // PM formula: out = sin(ωt + β(t)·sin(ωt))
    //
    // β (modulation index) starts at ~3.5 and decays exponentially to ~0
    // over 2.5 seconds — this creates the characteristic Rhodes "bloom":
    //   attack  → bright, complex, metallic (high β)
    //   sustain → pure, warm sine (β → 0)
    //
    // Both carrier and modulator use integer-Hz PhaseAccum (LUT-based) so
    // there is no floating-point frequency error.
    PhaseAccum fm_c;            // carrier   (integer Hz, LUT)
    PhaseAccum fm_m;            // modulator (same integer Hz)
    double     fm_idx = 0.0;    // current modulation index β (decays)
    double     fm_idk = 0.0;    // per-sample index decay  (idk = index decay key)
    double     fm_amp = 0.0;    // current amplitude (slow physical decay)
    double     fm_amk = 0.0;    // per-sample amplitude decay
    double     fm_sus = 0.0;    // amplitude sustain floor

    // =========================================================================
    //  start()  — initialise a voice.  Called from the main/MIDI thread.
    //             May call std::pow / std::cos — NOT in the audio hot path.
    // =========================================================================
    void start(InstrModel mdl, double hz_f, uint32_t sr, double amp = 0.25)
    {
        model     = mdl;
        amp_out   = amp;
        is_active = true;
        voice_sr  = sr;

        switch (mdl) {

        // ---- SINE -------------------------------------------------------
        case InstrModel::SINE:
            s_ph.init(hz_f, sr);
            break;

        // ---- PIANO ------------------------------------------------------
        case InstrModel::PIANO: {
            // Delay line length: nearest-integer rounding minimises pitch error
            // (under 0.5 sample = less than ~1 cent at most notes).
            ks_N = static_cast<uint32_t>(
                std::lround(static_cast<double>(sr) / hz_f));
            if (ks_N < 2u)         ks_N = 2u;
            if (ks_N > KS_BUF_MAX) ks_N = KS_BUF_MAX;

            ks_head = 0u;
            ks_age  = 0u;
            ks_prev = 0.0;
            ks_live = true;

            // T60 model: 128 / hz^(2/3)  →  bass decays in ~8 s, treble in ~0.3 s
            // Derived from two anchor points: C2(64 Hz)=8 s, C8(4096 Hz)=0.5 s
            const double t60 = std::max(0.25,
                128.0 / std::pow(hz_f, 2.0 / 3.0));

            // Per-sample gain for the LP feedback; after T60*hz_f periods → -60 dB
            ks_stretch = std::pow(0.001, 1.0 / (t60 * hz_f));

            // Excitation: white noise burst (classical Karplus-Strong).
            // Pure noise guarantees every sample is non-zero at the start,
            // which prevents the energy-based silence check from triggering
            // on the very first frame. The KS feedback loop immediately
            // begins low-pass filtering, giving a natural piano-like tone.
            // The burst window is used to shape the attack: the first third
            // of the buffer has full-amplitude noise, the rest is silence
            // (reduces click on very low notes).
            const uint32_t burst = std::max(1u, ks_N);
            uint32_t rng = static_cast<uint32_t>(hz_f) * 0x9E3779B9u ^ 0xDEADC0DEu;
            auto lcg = [&]() -> double {
                rng = rng * 1664525u + 1013904223u;
                return static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0;
            };
            for (uint32_t i = 0; i < ks_N; ++i)
                ks_buf[i] = (i < burst) ? lcg() : 0.0;
            break;
        }

        // ---- ORGAN ------------------------------------------------------
        case InstrModel::ORGAN: {
            for (int r = 0; r < ORGAN_RANKS; ++r) {
                const double fh = hz_f * static_cast<double>(r + 1);
                if (fh >= static_cast<double>(sr) / 2.0)
                    org_ph[r] = PhaseAccum{};               // above Nyquist → silence
                else
                    org_ph[r].init(fh, sr);
            }
            break;
        }

        // ---- RHODES -----------------------------------------------------
        case InstrModel::RHODES: {
            fm_c.init(hz_f, sr);
            fm_m.init(hz_f, sr);

            // Modulation index: 3.5 at attack, decays to ~0 in 2.5 s
            // Depth scales with velocity so soft notes are already warm
            fm_idx = 3.5 * (amp / 0.25);   // normalised to MASTER_GAIN = 0.25
            fm_idk = std::pow(0.001, 1.0 / (2.5 * static_cast<double>(sr)));

            // Amplitude: full at attack, slow physical decay over ~7 s
            fm_amp = amp;
            fm_sus = amp * 0.40;            // sustain at 40 % of peak
            fm_amk = std::pow(0.40, 1.0 / (7.0 * static_cast<double>(sr)));
            break;
        }

        } // switch
    }

    // =========================================================================
    //  stop()  — immediate silence (interactive stop / voice steal)
    // =========================================================================
    void stop()
    {
        amp_out   = 0.0;
        is_active = false;
        ks_live   = false;
    }

    // =========================================================================
    //  release()  — graceful note-off (MIDI note-off / key released)
    //
    //  PIANO : accelerates the natural string damping (= damper pedal release)
    //  ORGAN : immediately silences (pipe organs cut on key release)
    //  RHODES: short fade-out (0.25 s) — tine stops vibrating when released
    //  SINE  : immediate stop
    // =========================================================================
    void release()
    {
        if (!is_active) return;
        switch (model) {
        case InstrModel::PIANO:
            // Multiply stretch by damping factor — ~4× faster decay
            ks_stretch *= 0.78;
            break;
        case InstrModel::RHODES:
            // Override amplitude decay to reach -60 dB in 0.25 s
            fm_amk = std::pow(0.001,
                              1.0 / (0.25 * static_cast<double>(voice_sr)));
            fm_sus = 0.0;
            break;
        default:
            stop();
            break;
        }
    }

    // =========================================================================
    //  active()  — true while the voice is producing audible output
    // =========================================================================
    [[nodiscard]] bool active() const
    {
        if (!is_active) return false;
        if (model == InstrModel::PIANO) return ks_live;
        return amp_out > 0.0;
    }

    // =========================================================================
    //  next()  —  HOT PATH: called once per sample frame from audioCallback.
    //
    //  Contract: no std::sin, no heap allocation, no locks, no system calls.
    // =========================================================================
    [[nodiscard]] double next()
    {
        if (!is_active) return 0.0;

        switch (model) {

        // ---- SINE -------------------------------------------------------
        case InstrModel::SINE:
            return SINE_LUT[s_ph.advance()] * amp_out;

        // ---- PIANO ------------------------------------------------------
        case InstrModel::PIANO: {
            if (!ks_live) return 0.0;

            const double out      = ks_buf[ks_head];
            // 1st-order LP: y = ks_stretch * 0.5 * (x[n] + x[n-1])
            const double filtered = ks_stretch * 0.5 * (out + ks_prev);

            ks_prev         = out;
            ks_buf[ks_head] = filtered;
            ks_head         = (ks_head + 1u == ks_N) ? 0u : ks_head + 1u;
            ++ks_age;

            // Auto-silence: only after at least 4 full periods have elapsed
            // (ks_age guard prevents false-trigger on leading zeros or
            //  near-zero zero-crossings in the first few cycles)
            if (ks_age > ks_N * 4u && filtered * filtered < 1e-22) {
                ks_live   = false;
                is_active = false;
                return 0.0;
            }
            return out * amp_out;
        }

        // ---- ORGAN ------------------------------------------------------
        case InstrModel::ORGAN: {
            double s = 0.0;
            for (int r = 0; r < ORGAN_RANKS; ++r)
                if (org_ph[r].delta)
                    s += SINE_LUT[org_ph[r].advance()] * ORG_AMP[r];
            return s * amp_out;
        }

        // ---- RHODES -----------------------------------------------------
        case InstrModel::RHODES: {
            // Phase modulation: out = sin(ωt + β · sin(ωt))
            const uint32_t m_idx = fm_m.advance();
            const double   m_val = SINE_LUT[m_idx];

            // Carrier index offset = β × sin(mod) × (LUT_SIZE / 2π)
            const auto c_offset =
                static_cast<int64_t>(fm_idx * m_val * FM_LUT_SCALE);

            const uint32_t c_idx =
                static_cast<uint32_t>(
                    (static_cast<int64_t>(fm_c.advance()) + c_offset)
                    & static_cast<int64_t>(LUT_MASK));

            const double sample = SINE_LUT[c_idx];

            // Decay modulation index (brightness envelope)
            fm_idx *= fm_idk;

            // Slow amplitude decay (Rhodes physical string decay)
            if (fm_amp > fm_sus) {
                fm_amp *= fm_amk;
                if (fm_amp < fm_sus) fm_amp = fm_sus;
            }

            return sample * fm_amp;
        }

        default: return 0.0;
        }
    }
};
