#pragma once
// =============================================================================
//  sf2_engine.h  —  SoundFont2 synthesis engine
//
//  Built on TinySoundFont (tsf.h) by Bernhard Schelling.
//  Single-header, no external dependencies, renders SF2 / SFZ files directly.
//  Public domain (TinySoundFont) — see src/tsf.h.
//
//  Tuning
//  ------
//  A4 is retuned to 432 Hz globally by offsetting every channel by
//  -0.3177 semitones (= 12 * log2(432/440) = -31.77 cents).
//  Because the underlying samples are from a real instrument, the result
//  is perceptually a true 432 Hz recording, not a digital pitch-shift.
//
//  Thread safety
//  -------------
//  All methods (except render()) must be called with the audio mutex HELD.
//  render() is called from the audio callback — also under the audio mutex.
//
//  Usage
//  -----
//    Sf2Engine sf2;
//    sf2.load("Salamander.sf2", sample_rate);   // from main / MIDI thread
//    sf2.note_on(69, 0.8f);                     // play A4 at 80 % velocity
//    // ... in audioCallback:
//    float scratch[nFrames * 2];
//    sf2.render(scratch, nFrames);              // mix into your output
//    sf2.note_off(69);
// =============================================================================

// TSF_IMPLEMENTATION must be defined exactly once across all translation units.
// Since this header is included only in main.cpp, it is safe here.
#define TSF_IMPLEMENTATION
#include "tsf.h"

#include <cmath>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
//  Tuning constant: semitones to shift from A=440 to A=432
//  A4_432_SEMITONES = 12 * log2(432 / 440) = -0.31769...
// ---------------------------------------------------------------------------
static inline float sf2TuningSemitones() {
    return static_cast<float>(12.0 * std::log2(432.0 / 440.0));
}

// ---------------------------------------------------------------------------
//  Sf2Engine
// ---------------------------------------------------------------------------
struct Sf2Engine {
    tsf*        sf2       = nullptr;
    bool        loaded    = false;
    int         preset    = 0;       // active preset index (0 = first = Grand Piano)
    uint32_t    sr        = 48000u;
    float       tuning_semitones = -0.31769f;  // 12*log2(432/440); updated by retune()
    std::string path;

    // -----------------------------------------------------------------------
    //  install — configure and activate a pre-loaded tsf* under the lock.
    //
    //  Intended for the two-phase load pattern where the caller opens the file
    //  with tsf_load_filename() OUTSIDE the audio mutex (avoiding a long
    //  disk-I/O stall on the audio callback), then calls install() while
    //  holding g_audio.mutex to swap in the new instance atomically.
    //
    //  Ownership of t_new is transferred: install() is responsible for
    //  closing it on failure or on the next unload().
    //  Returns true on success; on failure t_new is closed and false returned.
    // -----------------------------------------------------------------------
    bool install(tsf* t_new, const std::string& sf2_path, uint32_t sample_rate)
    {
        unload();   // close previous instance (safe: caller holds audio mutex)

        if (!t_new) {
            std::printf("[!] SF2: cannot open '%s'\n", sf2_path.c_str());
            return false;
        }

        // 1.5 dB boost to match InstrVoice volume level
        tsf_set_output(t_new, TSF_STEREO_INTERLEAVED,
                       static_cast<int>(sample_rate), 1.5f);

        tsf_set_max_voices(t_new, 64);

        // Apply tuning on all 16 MIDI channels.
        // Also pre-initialize channel 9 (GM percussion) to bank 128 / program 0
        // (Standard Drum Kit) so drums work immediately without an explicit
        // Program Change event in the MIDI file.
        for (int ch = 0; ch < 16; ++ch)
            tsf_channel_set_tuning(t_new, ch, tuning_semitones);
        tsf_channel_set_presetnumber(t_new, 9, 0, 1 /*flag_mididrums*/);

        sf2    = t_new;
        loaded = true;
        sr     = sample_rate;
        path   = sf2_path;
        preset = 0;

        const int nPresets = tsf_get_presetcount(sf2);
        std::printf("[+] SF2 loaded: '%s'\n", sf2_path.c_str());
        std::printf("    %d preset(s).  Active: [0] %s\n",
                    nPresets, tsf_get_presetname(sf2, 0));
        std::printf("    Tuning: A4 = %.2f Hz  (%.4f semitones from 440)\n",
                    440.0 * std::pow(2.0, static_cast<double>(tuning_semitones) / 12.0),
                    static_cast<double>(tuning_semitones));
        return true;
    }

    // -----------------------------------------------------------------------
    //  load — open an SF2/SFZ file and prepare for playback.
    //  Returns true on success.  Performs disk I/O internally; prefer the
    //  two-phase pattern (tsf_load_filename outside lock + install() inside
    //  lock) for hot-reload paths where blocking the audio mutex is undesirable.
    // -----------------------------------------------------------------------
    bool load(const std::string& sf2_path, uint32_t sample_rate)
    {
        tsf* t = tsf_load_filename(sf2_path.c_str());
        return install(t, sf2_path, sample_rate);
    }

    // -----------------------------------------------------------------------
    //  unload — release all resources.
    // -----------------------------------------------------------------------
    void unload()
    {
        if (sf2) { tsf_close(sf2); sf2 = nullptr; }
        loaded = false;
    }

    // -----------------------------------------------------------------------
    //  update_sr — call after a device switch to reset the sample rate.
    //  Re-loads the SF2 if already loaded.
    //  Note: channel GM assignments are lost on reload.  If MIDI playback
    //  is active, the playback thread will re-issue all program-changes on
    //  the next note event; silence is the only audible artifact.
    // -----------------------------------------------------------------------
    void update_sr(uint32_t new_sr)
    {
        if (!loaded || new_sr == sr) return;
        const std::string saved_path = path;
        const int saved_preset = preset;  // preserve user preset choice
        unload();
        load(saved_path, new_sr);
        preset = saved_preset;  // restore without touching MIDI channels
    }

    // -----------------------------------------------------------------------
    //  retune — update A4 reference frequency and re-apply to all channels.
    //  Call whenever g_tuning.a4 changes (interactive command or device switch).
    // -----------------------------------------------------------------------
    void retune(double a4_hz)
    {
        tuning_semitones = static_cast<float>(12.0 * std::log2(a4_hz / 440.0));
        if (!sf2) return;
        for (int ch = 0; ch < 16; ++ch)
            tsf_channel_set_tuning(sf2, ch, tuning_semitones);
    }

    // -----------------------------------------------------------------------
    //  select_preset — switch to a preset by index (single-note demo mode).
    //
    //  This intentionally updates ONLY this->preset, which is used by the
    //  non-channel note_on() / note_off() calls (tsf_note_on with preset index).
    //  It must NOT write to any tsf_channel_set_presetindex() because that
    //  would overwrite the per-channel GM program assignments that
    //  channel_program() / channel_note_on() depend on during MIDI playback.
    // -----------------------------------------------------------------------
    void select_preset(int idx)
    {
        if (!sf2) return;
        const int n = tsf_get_presetcount(sf2);
        preset = (idx % n + n) % n;
    }

    // -----------------------------------------------------------------------
    //  Interactive note-on / note-off (preset-based, no channel routing).
    //  key  = MIDI note number 0-127.
    //  vel  = 0.0 – 1.0 (normalized velocity).
    // -----------------------------------------------------------------------
    void note_on(int key, float vel)
    {
        if (!sf2) return;
        tsf_note_on(sf2, preset, key, vel);
    }

    void note_off(int key)
    {
        if (!sf2) return;
        tsf_note_off(sf2, preset, key);
    }

    // -----------------------------------------------------------------------
    //  MIDI-file note-on / note-off (channel-based, honours GM program table).
    // -----------------------------------------------------------------------
    void channel_note_on(int channel, int key, float vel)
    {
        if (!sf2) return;
        tsf_channel_note_on(sf2, channel, key, vel);
    }

    void channel_note_off(int channel, int key)
    {
        if (!sf2) return;
        tsf_channel_note_off(sf2, channel, key);
    }

    // MIDI CC forwarding (mod wheel, sustain pedal, volume, pan, expression, etc.)
    void channel_control(int channel, int cc, int value)
    {
        if (!sf2) return;
        tsf_channel_midi_control(sf2, channel, cc, value);
        // CC 121 (All Controllers Off) resets TSF's internal channel tuning to
        // 0 semitones (A=440 Hz).  Immediately reapply the global 432 Hz offset
        // so the channel doesn't drift back to standard pitch mid-song.
        if (cc == 121)
            tsf_channel_set_tuning(sf2, channel, tuning_semitones);
    }

    // Pitch Bend — pitch_wheel is the raw 14-bit value (0-16383; centre = 8192).
    void channel_pitch_bend(int channel, int pitch_wheel)
    {
        if (!sf2) return;
        tsf_channel_set_pitchwheel(sf2, channel, pitch_wheel);
    }

    // Channel (mono) aftertouch — maps to CC11 (expression) for TSF compatibility.
    // TSF has no dedicated aftertouch path; expression is the closest equivalent
    // for dynamics-sensitive instruments.
    void channel_aftertouch(int channel, int pressure)
    {
        if (!sf2) return;
        tsf_channel_midi_control(sf2, channel, 11 /*EXPRESSION_MSB*/, pressure);
    }

    // MIDI Program Change — maps GM bank/program to the nearest SF2 preset.
    // Channel 9 is the GM percussion channel: flag_mididrums=1 directs TSF
    // to search bank 128 (the SF2 drum bank) instead of melodic bank 0.
    // Without this flag, channel 9 would silently inherit preset 0 (piano).
    void channel_program(int channel, int program)
    {
        if (!sf2) return;
        const int flag_drums = (channel == 9) ? 1 : 0;
        tsf_channel_set_presetnumber(sf2, channel, program, flag_drums);
        // Reapply tuning after preset switch
        tsf_channel_set_tuning(sf2, channel, tuning_semitones);
    }

    // -----------------------------------------------------------------------
    //  silence — stop all sound immediately.
    // -----------------------------------------------------------------------
    void silence()
    {
        if (!sf2) return;
        tsf_note_off_all(sf2);
    }

    // -----------------------------------------------------------------------
    //  render — fill `buf` with `nFrames` stereo interleaved FLOAT samples.
    //  Called from the audio callback thread (mutex already held).
    //  flag_mixing = 0 → replace buf contents.
    // -----------------------------------------------------------------------
    void render(float* buf, uint32_t nFrames)
    {
        if (!sf2) return;
        tsf_render_float(sf2, buf, static_cast<int>(nFrames), 0);
    }

    // -----------------------------------------------------------------------
    //  print_presets — list all presets to stdout.
    // -----------------------------------------------------------------------
    void print_presets() const
    {
        if (!sf2) { std::printf("[!] No SF2 loaded.\n"); return; }
        const int n = tsf_get_presetcount(sf2);
        std::printf("\n  SF2: %s\n  %d preset(s):\n", path.c_str(), n);
        for (int i = 0; i < n; ++i)
            std::printf("    [%3d]%s %s\n", i,
                        (i == preset ? "*" : " "),
                        tsf_get_presetname(sf2, i));
        std::printf("\n");
    }

    ~Sf2Engine() { unload(); }
};

// Global instance — defined once here (included only in main.cpp).
static Sf2Engine g_sf2_engine;
