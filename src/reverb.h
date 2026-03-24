#pragma once
// =============================================================================
//  reverb.h  —  Freeverb stereo room reverb  (public domain)
//
//  Original algorithm: "Freeverb" by Jezar at Dreampoint
//  This C++ port:
//    • Scales all delay lines to the actual sample rate (works at 44.1, 48,
//      96, 192 kHz without resampling)
//    • Processes stereo-interleaved double-precision buffers (in-place)
//    • Exposes four hand-tuned named presets suited to piano performance
//    • All parameters are changed at runtime with zero clicks
//
//  Architecture (per channel):
//    input  →  8 comb-filters in parallel  →  4 allpass-filters in series
//    output  =  dry*input  +  wet*verb
//    L and R use different delay lengths (stereo spread) for width
//
//  Comb filter (IIR loop with LP feedback — Schroeder model):
//    y[n] = x[n-N]  +  feedback * LPF(y[n-N])
//    LPF: last = out*damp2 + last*damp1
//
//  Allpass filter (pure diffusor):
//    y[n] = -x[n]  +  buf[n-N]  +  0.5 * buf[n-N]
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rv_detail {

// ---------------------------------------------------------------------------
//  Comb filter  (Schroeder-Moorer allpole resonator with LP feedback)
// ---------------------------------------------------------------------------
struct CombFilter {
    std::vector<float> buf;
    int   pos  = 0;
    float last = 0.f;
    float fb   = 0.f;   // feedback amount
    float d1   = 0.f;   // damp LP coefficient
    float d2   = 1.f;   // = 1 - d1

    void init(int n) { buf.assign(static_cast<size_t>(n), 0.f); pos = 0; last = 0.f; }
    void set_feedback(float v) { fb = v; }
    void set_damp(float v)     { d1 = v; d2 = 1.f - v; }

    inline float tick(float x) {
        const float out = buf[pos];
        last       = out * d2 + last * d1;   // low-pass damps high frequencies
        buf[pos]   = x + last * fb;
        if (++pos == static_cast<int>(buf.size())) pos = 0;
        return out;
    }
};

// ---------------------------------------------------------------------------
//  All-pass filter  (diffusor)
// ---------------------------------------------------------------------------
struct AllpassFilter {
    std::vector<float> buf;
    int pos = 0;

    void init(int n) { buf.assign(static_cast<size_t>(n), 0.f); pos = 0; }

    inline float tick(float x) {
        const float b = buf[pos];
        buf[pos]      = x + b * 0.5f;
        if (++pos == static_cast<int>(buf.size())) pos = 0;
        return b - x;
    }
};

} // namespace rv_detail

// =============================================================================
//  Reverb  —  public interface
// =============================================================================
struct Reverb {
    // ---- parameters (call apply_params() after changing roomSize or damp) ---
    bool  enabled  = false;
    float roomSize = 0.80f;   ///< 0.0 (tiny) … 1.0 (huge hall)
    float damp     = 0.30f;   ///< 0.0 (bright) … 1.0 (dark)
    float wet      = 0.26f;   ///< wet reverb level  0.0 – 1.0
    float dry      = 0.82f;   ///< dry direct level   0.0 – 1.0
    float width    = 1.00f;   ///< stereo width       0.0 – 1.0

    // ---- internals -----------------------------------------------------------
    static constexpr int NUM_COMB    = 8;
    static constexpr int NUM_ALLPASS = 4;
    //  Delay lengths at 44100 Hz reference rate
    static constexpr int REF_COMB[8]    = {1116,1188,1277,1356,1422,1491,1557,1617};
    static constexpr int REF_AP[4]      = { 556, 441, 341, 225};
    static constexpr int STEREO_SPREAD  = 23;
    //  Input gain: normalises 8-comb parallel sum to ~unity
    static constexpr float FIXED_GAIN   = 0.015f;

    rv_detail::CombFilter    combL[NUM_COMB],    combR[NUM_COMB];
    rv_detail::AllpassFilter apL[NUM_ALLPASS],   apR[NUM_ALLPASS];
    uint32_t sr = 0;

    // -------------------------------------------------------------------------
    //  init — allocate delay lines scaled to sample_rate.
    //  Must be called once after the audio stream is opened and again whenever
    //  the sample rate changes (e.g. device switch).
    // -------------------------------------------------------------------------
    void init(uint32_t sample_rate)
    {
        sr = sample_rate;
        const float scale = static_cast<float>(sample_rate) / 44100.f;
        for (int i = 0; i < NUM_COMB; ++i) {
            combL[i].init(static_cast<int>(std::roundf(REF_COMB[i] * scale)));
            combR[i].init(static_cast<int>(std::roundf((REF_COMB[i] + STEREO_SPREAD) * scale)));
        }
        for (int i = 0; i < NUM_ALLPASS; ++i) {
            apL[i].init(static_cast<int>(std::roundf(REF_AP[i] * scale)));
            apR[i].init(static_cast<int>(std::roundf((REF_AP[i] + STEREO_SPREAD) * scale)));
        }
        apply_params();
    }

    // -------------------------------------------------------------------------
    //  apply_params — push current roomSize and damp into the comb filters.
    //  Call after any change to roomSize or damp.
    // -------------------------------------------------------------------------
    void apply_params()
    {
        //  Freeverb feedback range: [0.70, 0.98]  (linear stretch of roomSize)
        const float fb = roomSize * 0.28f + 0.70f;
        //  LP damp:  0.0 = transparent, 0.0–0.4 range keeps highs present
        const float d  = damp * 0.40f;
        for (int i = 0; i < NUM_COMB; ++i) {
            combL[i].set_feedback(fb);  combL[i].set_damp(d);
            combR[i].set_feedback(fb);  combR[i].set_damp(d);
        }
    }

    // -------------------------------------------------------------------------
    //  set_preset — apply a named preset and enable reverb.
    //  Names: hall | stage | studio | plate | cathedral | off
    // -------------------------------------------------------------------------
    void set_preset(const char* name)
    {
        if (!std::strcmp(name, "hall")) {
            //  Large concert hall: wide, bright, natural bloom — ideal for piano
            roomSize=0.82f; damp=0.28f; wet=0.26f; dry=0.82f; width=1.00f;
        } else if (!std::strcmp(name, "stage")) {
            //  Live stage: medium room, more presence, punchy attack
            roomSize=0.72f; damp=0.38f; wet=0.20f; dry=0.88f; width=0.85f;
        } else if (!std::strcmp(name, "studio")) {
            //  Recording studio: intimate, tight, controlled
            roomSize=0.48f; damp=0.55f; wet=0.14f; dry=0.92f; width=0.65f;
        } else if (!std::strcmp(name, "plate")) {
            //  Steel plate reverb: dense, coloured, characteristic of vintage records
            roomSize=0.68f; damp=0.20f; wet=0.30f; dry=0.80f; width=0.55f;
        } else if (!std::strcmp(name, "cathedral")) {
            //  Huge stone space: enormous tail, very bright, ethereal
            roomSize=0.95f; damp=0.10f; wet=0.42f; dry=0.68f; width=1.00f;
        } else if (!std::strcmp(name, "off") || !std::strcmp(name, "none")) {
            enabled = false;
            return;
        } else {
            return;   // unknown preset — no change
        }
        enabled = true;
        apply_params();
    }

    static const char* preset_list() {
        return "hall | stage | studio | plate | cathedral | off";
    }

    // -------------------------------------------------------------------------
    //  process — process `nFrames` stereo interleaved double samples in-place.
    //  HOT PATH: no allocation, no locks, no system calls.
    // -------------------------------------------------------------------------
    void process(double* buf, uint32_t nFrames)
    {
        if (!enabled) return;

        //  Wet mix coefficients for stereo width
        //  w1 = centre share, w2 = cross-feed share
        const float w1 = wet * (0.5f + 0.5f * width);
        const float w2 = wet * (0.5f - 0.5f * width);

        for (uint32_t f = 0; f < nFrames; ++f) {
            const double dL = buf[f * 2u];
            const double dR = buf[f * 2u + 1u];
            const float  iL = static_cast<float>(dL) * FIXED_GAIN;
            const float  iR = static_cast<float>(dR) * FIXED_GAIN;

            float outL = 0.f, outR = 0.f;

            // 8 comb filters in parallel
            for (int i = 0; i < NUM_COMB; ++i) {
                outL += combL[i].tick(iL);
                outR += combR[i].tick(iR);
            }
            // 4 allpass filters in series
            for (int i = 0; i < NUM_ALLPASS; ++i) {
                outL = apL[i].tick(outL);
                outR = apR[i].tick(outR);
            }

            // Wet/dry mix with stereo width
            buf[f * 2u]      = dL * dry + (outL * w1 + outR * w2);
            buf[f * 2u + 1u] = dR * dry + (outR * w1 + outL * w2);
        }
    }
};

// Global reverb instance — one shared room for all timbres.
static Reverb g_reverb;
