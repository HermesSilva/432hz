// =============================================================================
//  main.cpp  -  432 Hz Bit-Perfect Synthesiser
//
//  Multi-API design
//  ----------------
//  Compiled APIs (in order of preference):
//    1. ASIO   - lowest latency, truly exclusive, bit-perfect
//    2. WASAPI - Windows native, exclusive mode = bit-perfect, very robust
//
//  At startup the app enumerates ALL output devices across ALL compiled APIs,
//  then walks a priority-ordered candidate list (api, device, rate) until
//  one combination succeeds.
//
//  CLI flags
//  ---------
//    --list            Print device table and exit.
//    --api  asio|wasapi  Restrict to one API.
//    --device <id>     Force a specific device ID from the table.
//
//  Interactive commands
//  --------------------
//    A4 / C4 / ...     Play a note from the integer-scale table.
//    x <hz>            Play any integer frequency (e.g. x 300).
//    chord <n> <n2>    Play multiple notes simultaneously.
//    scale <note> [up|down] [ms]  Play a chromatic octave.
//    probe [ms]        Play A4 on each device — find the audible one.
//    timbre <name>     Switch instrument: sine | piano | organ | rhodes.
//    stop              Silence all voices.
//    list              Print the full note table.
//    devices           Re-print the device capability table.
//    sr                Show negotiated sample rate / API / device.
//    quit              Exit.
// =============================================================================

#include <RtAudio.h>

#include "lut.h"
#include "notes_432.h"
#include "tuning.h"

#include "instrument.h"
#include "midi_loader.h"
#include "reverb.h"
#include "sf2_engine.h"
#include "midi_out_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <timeapi.h>   // timeBeginPeriod / timeEndPeriod (winmm, already linked)

#include <d3d11.h>      // DirectX 11 (for Dear ImGui DX11 backend)
#include <filesystem>   // std::filesystem::path
#include <functional>   // std::function (device-switch callback)

#include "debug_console.h"

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------
static constexpr uint32_t PREFERRED_RATES[] = {
    192000u, 176400u, 96000u, 88200u, 48000u, 44100u
};
static constexpr RtAudio::Api API_PREF[] = {
    RtAudio::WINDOWS_ASIO,
    RtAudio::WINDOWS_WASAPI,
};
static constexpr uint32_t FRAMES_PER_BUFFER = 256u;
static constexpr uint32_t MAX_VOICES        = 16u;
static constexpr double   MASTER_GAIN       = 0.25;

// ---------------------------------------------------------------------------
//  Shared audio state
// ---------------------------------------------------------------------------
struct AudioState {
    std::mutex mutex;
    InstrVoice voices[MAX_VOICES]{};
    uint32_t   sample_rate = 0;
    uint32_t   channels    = 2;   // actual device output channel count
};
static AudioState g_audio;

// Current synthesis model (read from main thread, read from MIDI thread).
// Changed only from the main thread; no lock needed for read in MIDI thread.
static InstrModel g_instr_model = InstrModel::SINE;

// Scale sequencer state
static std::atomic<bool> g_scale_stop{false};
static std::thread       g_scale_thread;

// MIDI playback state
static std::atomic<bool> g_midi_stop{false};
static std::thread       g_midi_thread;

// ---------------------------------------------------------------------------
//  Extended MIDI state (read by GUI)
// ---------------------------------------------------------------------------
static std::atomic<bool>     g_midi_paused{false};
static std::atomic<uint64_t> g_midi_elapsed_us{0};
static std::atomic<uint64_t> g_midi_total_us{0};
static std::atomic<bool>     g_midi_playing{false};
static std::atomic<uint64_t> g_seek_target_us{~uint64_t{0}};  // UINT64_MAX = no seek
static std::mutex            g_now_playing_mutex;
static std::string           g_now_playing_name;
static std::string           g_current_midi_path;  // full path of the active MIDI file

// MIDI instruments info — written by playMidiFile, read by GUI
static std::mutex  g_midi_info_mutex;
static int         g_midi_info_programs[16]{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
static bool        g_midi_info_used[16]{};

// ---------------------------------------------------------------------------
//  Per-channel spatial mixer   (GUI writes, audio thread reads)
// ---------------------------------------------------------------------------
struct MixChannel {
    float vol   = 1.0f;   // channel volume 0.0 – 1.4 (1.0 = unity)
    float pan   = 0.0f;   // pan: -1.0 (L) … 0 (C) … +1.0 (R)
    float send  = 0.25f;  // reverb send 0.0 – 1.0
    bool  mute  = false;
    bool  solo  = false;
};
static MixChannel         g_mix[16];
static std::atomic<float> g_ch_vu[16]; // per-channel peak (0–1); set on note-on, decays in GUI

// ---------------------------------------------------------------------------
//  Windows MIDI output
// ---------------------------------------------------------------------------
static MidiOutEngine              g_midiout;
static std::vector<MidiOutDevice> g_midiout_devices;

// ---------------------------------------------------------------------------
//  VU meter  (written by audio callback, read by GUI — lock-free)
//  Master volume  (written by GUI, read by audio callback)
// ---------------------------------------------------------------------------
static std::atomic<float>    g_vu_peak_l{0.0f};
static std::atomic<float>    g_vu_peak_r{0.0f};
static std::atomic<float>    g_master_volume{1.0f};

// ---------------------------------------------------------------------------
//  RtAudio callback  -  HOT PATH  (no sin, no alloc, no system calls)
// ---------------------------------------------------------------------------
static int audioCallback(void*        outputBuffer,
                         void*        /*inputBuffer*/,
                         unsigned int nFrames,
                         double       /*streamTime*/,
                         RtAudioStreamStatus status,
                         void*        userData)
{
    (void)status;

    auto* out    = static_cast<double*>(outputBuffer);
    auto& state  = *static_cast<AudioState*>(userData);
    const uint32_t nCh = state.channels;   // actual device channel count

    std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::memset(out, 0, nFrames * nCh * sizeof(double));
        return 0;
    }

    // All synthesis runs on a stereo scratch buffer; expanded to nCh at the end.
    // This keeps reverb.h (stereo-only) and TSF (stereo-only) working unchanged.
    double stereo[FRAMES_PER_BUFFER * 2];
    std::memset(stereo, 0, nFrames * 2u * sizeof(double));

    for (uint32_t f = 0; f < nFrames; ++f) {
        double s = 0.0;
        for (auto& v : state.voices) {
            if (!v.active()) continue;
            s += v.next();
        }
        stereo[f * 2u]      = s;
        stereo[f * 2u + 1u] = s;
    }

    // Mix SoundFont2 (TinySoundFont) output when loaded
    if (g_sf2_engine.loaded) {
        // Safe scratch buffer: 4096 frames max (driver never requests more)
        float scratch[4096 * 2];
        std::memset(scratch, 0, nFrames * 2u * sizeof(float));
        g_sf2_engine.render(scratch, nFrames);
        for (uint32_t f = 0; f < nFrames; ++f) {
            stereo[f * 2u]      += static_cast<double>(scratch[f * 2u]);
            stereo[f * 2u + 1u] += static_cast<double>(scratch[f * 2u + 1u]);
        }
    }

    // Room reverb — stereo in-place
    g_reverb.process(stereo, nFrames);

    // Apply master volume
    const float mv = g_master_volume.load(std::memory_order_relaxed);
    if (mv != 1.0f) {
        for (uint32_t f = 0; f < nFrames; ++f) {
            stereo[f * 2u]      *= mv;
            stereo[f * 2u + 1u] *= mv;
        }
    }

    // Update VU meter peaks (GUI reads these atomically)
    float peakL = 0.f, peakR = 0.f;
    for (uint32_t f = 0; f < nFrames; ++f) {
        const float sL = std::abs(static_cast<float>(stereo[f * 2u]));
        const float sR = std::abs(static_cast<float>(stereo[f * 2u + 1u]));
        if (sL > peakL) peakL = sL;
        if (sR > peakR) peakR = sR;
    }
    {
        float cur = g_vu_peak_l.load(std::memory_order_relaxed);
        if (peakL > cur) g_vu_peak_l.store(peakL, std::memory_order_relaxed);
    }
    {
        float cur = g_vu_peak_r.load(std::memory_order_relaxed);
        if (peakR > cur) g_vu_peak_r.store(peakR, std::memory_order_relaxed);
    }

    // Expand stereo mix to the device's N-channel output.
    // The spatial disc Y-axis (g_mix[ch].send: +1=front, -1=rear) determines
    // how much of the L/R signal goes to front vs surround speakers.
    // Left-side and right-side fractions are computed independently so that
    // each instrument's depth position actually routes it to the correct speakers.
    if (nCh == 2) {
        std::memcpy(out, stereo, nFrames * 2u * sizeof(double));
    } else {
        // Determine solo state so inactive channels don't skew the weighting.
        bool anySolo = false;
        for (int ch = 0; ch < 16; ++ch)
            if (g_mix[ch].solo) { anySolo = true; break; }

        // Per-side weighted front fraction.
        // wL = how much channel ch contributes to the L stereo signal.
        // wR = how much channel ch contributes to the R stereo signal.
        // fF maps send [-1,+1] to [0,1]  (0=rear, 1=front).
        float sumL = 0.f, sumLF = 0.f, sumR = 0.f, sumRF = 0.f;
        for (int ch = 0; ch < 16; ++ch) {
            const bool  act = anySolo ? g_mix[ch].solo : !g_mix[ch].mute;
            const float vol = act ? g_mix[ch].vol : 0.f;
            if (vol < 1e-6f) continue;
            const float pan = g_mix[ch].pan;              // -1 (L) ... +1 (R)
            const float dep = g_mix[ch].send;             // +1 (front) ... -1 (rear)
            const float wL  = vol * (1.f - pan) * 0.5f;
            const float wR  = vol * (1.f + pan) * 0.5f;
            const float fF  = (dep + 1.f) * 0.5f;        // 0=rear  1=front
            sumL  += wL;   sumLF += wL * fF;
            sumR  += wR;   sumRF += wR * fF;
        }
        const float ffL = sumL > 1e-6f ? sumLF / sumL : 1.f; // 0=all-rear ... 1=all-front
        const float ffR = sumR > 1e-6f ? sumRF / sumR : 1.f;

        // Constant-power crossfade: cos -> front gain, sin -> surround gain.
        const double aL  = static_cast<double>((1.f - ffL) * 1.5707963f);
        const double aR  = static_cast<double>((1.f - ffR) * 1.5707963f);
        const double fgL = std::cos(aL);   // front gain L side
        const double fgR = std::cos(aR);   // front gain R side
        const double sgL = std::sin(aL);   // surround gain L side
        const double sgR = std::sin(aR);   // surround gain R side

        std::memset(out, 0, nFrames * nCh * sizeof(double));
        for (uint32_t f = 0; f < nFrames; ++f) {
            const double L = stereo[f * 2u];
            const double R = stereo[f * 2u + 1u];
            out[f * nCh + 0] = L * fgL;   // Front Left
            out[f * nCh + 1] = R * fgR;   // Front Right
            // [2] = Centre: silent (phantom centre via FL+FR)
            // [3] = LFE:    silent (bass management belongs to the receiver)
            if (nCh == 4) {
                // Quad:  FL FR RL RR
                out[f * nCh + 2] = L * sgL;
                out[f * nCh + 3] = R * sgR;
            } else if (nCh >= 6) {
                // 5.1:   FL FR C LFE BL BR
                out[f * nCh + 4] = L * sgL;   // Back/Surround Left
                out[f * nCh + 5] = R * sgR;   // Back/Surround Right
                if (nCh >= 8) {
                    // 7.1:   + Side Left/Right at -3 dB relative to back
                    out[f * nCh + 6] = L * sgL * 0.7071;
                    out[f * nCh + 7] = R * sgR * 0.7071;
                }
            }
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
//  API helpers
// ---------------------------------------------------------------------------
static const char* apiLabel(RtAudio::Api a) {
    switch (a) {
    case RtAudio::WINDOWS_ASIO:   return "ASIO  ";
    case RtAudio::WINDOWS_WASAPI: return "WASAPI";
    case RtAudio::WINDOWS_DS:     return "DS    ";
    default:                       return "?     ";
    }
}

static RtAudio::Api apiFromString(const std::string& s) {
    if (s == "asio")   return RtAudio::WINDOWS_ASIO;
    if (s == "wasapi") return RtAudio::WINDOWS_WASAPI;
    if (s == "ds")     return RtAudio::WINDOWS_DS;
    return RtAudio::UNSPECIFIED;
}

// ---------------------------------------------------------------------------
//  Device record  -  pairs DeviceInfo with its originating API
// ---------------------------------------------------------------------------
struct DevRec {
    RtAudio::Api        api{};
    uint32_t            id{};
    RtAudio::DeviceInfo info{};
};

// ---------------------------------------------------------------------------
//  Module-level device / stream state  (shared with GuiApp via extern)
// ---------------------------------------------------------------------------
static std::vector<DevRec>      g_devices;
static RtAudio::Api             g_opened_api       = RtAudio::UNSPECIFIED;
static uint32_t                 g_opened_device_id  = 0;
static std::string              g_opened_name;
static uint32_t                 g_negotiated_rate   = 0;
static uint32_t                 g_buffer_frames     = FRAMES_PER_BUFFER;
// Injected at runtime from main() after open; called by switchToDeviceGlobal.
static std::function<bool(RtAudio::Api, uint32_t)> g_switch_device_fn;

bool switchToDeviceGlobal(RtAudio::Api api, uint32_t id)
{
    return g_switch_device_fn ? g_switch_device_fn(api, id) : false;
}

static std::vector<DevRec> enumerateOutputDevices() {
    std::vector<RtAudio::Api> compiled;
    RtAudio::getCompiledApi(compiled);

    std::vector<DevRec> result;
    for (auto api : compiled) {
        try {
            RtAudio probe(api);
            for (auto id : probe.getDeviceIds()) {
                DevRec r;
                r.api  = api;
                r.id   = id;
                r.info = probe.getDeviceInfo(id);
                if (r.info.outputChannels >= 2)
                    result.push_back(std::move(r));
            }
        } catch (...) {}
    }
    return result;
}


// ---------------------------------------------------------------------------
//  Try to open a stream at all preferred rates.
// ---------------------------------------------------------------------------
static bool tryOpenStream(RtAudio&     audio,
                          uint32_t     deviceId,
                          RtAudio::Api api,
                          uint32_t     nChannels,
                          uint32_t&    negotiatedRate,
                          uint32_t&    bufferFrames)
{
    RtAudio::StreamParameters params;
    params.deviceId     = deviceId;
    params.nChannels    = nChannels;   // use all channels the device offers
    params.firstChannel = 0;

    RtAudio::StreamOptions opts;
    opts.streamName = "432hz_player";
    opts.flags      = RTAUDIO_MINIMIZE_LATENCY | RTAUDIO_SCHEDULE_REALTIME;
    if (api == RtAudio::WINDOWS_WASAPI)
        opts.flags |= RTAUDIO_HOG_DEVICE;   // WASAPI exclusive = bit-perfect

    for (uint32_t rate : PREFERRED_RATES) {
        bufferFrames = FRAMES_PER_BUFFER;

        const bool err = audio.openStream(
            &params, nullptr, RTAUDIO_FLOAT64,
            rate, &bufferFrames,
            &audioCallback, &g_audio, &opts);

        if (!err) {
            negotiatedRate = rate;
            return true;
        }
    }
    return false;
}

// Map 432 Hz integer table Hz value back to MIDI note number.
// NOTE_TABLE[0] = C2 = 64 Hz = MIDI 36 (C2).  Returns -1 if not found.
static int hzToMidiNote(uint32_t hz)
{
    static constexpr int MIDI_C2 = 36;
    for (size_t i = 0; i < NOTE_TABLE.size(); ++i)
        if (NOTE_TABLE[i].hz == hz) return static_cast<int>(i) + MIDI_C2;
    return -1;
}

// ---------------------------------------------------------------------------
//  Scale sequencer
// ---------------------------------------------------------------------------
static void stopScaleThread() {
    g_scale_stop.store(true);
    if (g_scale_thread.joinable()) g_scale_thread.join();
    g_scale_stop.store(false);
}

// Runs in its own thread.
// startIdx : index into NOTE_TABLE
// dir      : +1 ascending, -1 descending
// durationMs: milliseconds per note
static void playScaleThread(size_t startIdx, int dir, uint32_t durationMs) {
    const int OCTAVE = 12;  // chromatic octave = 12 semitones
    for (int step = 0; step <= OCTAVE; ++step) {
        if (g_scale_stop.load()) break;

        const int idx = static_cast<int>(startIdx) + step * dir;
        if (idx < 0 || idx >= static_cast<int>(NOTE_TABLE.size())) break;

        const Note&    note  = NOTE_TABLE[static_cast<size_t>(idx)];
        const int      midi  = 36 + idx;   // MIDI note number (MIDI_C2 = 36)
        const double   hz_f  = g_tuning.noteFreq(midi);
        const uint32_t sr    = g_audio.sample_rate;

        {
            std::lock_guard<std::mutex> lk(g_audio.mutex);
            for (auto& v : g_audio.voices) v.stop();
            // SF2: trigger via MIDI note; channel tuning offset handles pitch.
            // InstrVoice: compute frequency from tuning config.
            if (g_instr_model == InstrModel::SF2) {
                g_sf2_engine.silence();
                g_sf2_engine.note_on(midi, 0.80f);
            } else if (hz_f > 0.0 && hz_f < static_cast<double>(sr) / 2.0) {
                g_audio.voices[0].start(g_instr_model, hz_f, sr);
            }
        }

        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(durationMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (g_scale_stop.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_audio.mutex);
        for (auto& v : g_audio.voices) v.stop();
    }
}

// ---------------------------------------------------------------------------
//  MIDI helpers
// ---------------------------------------------------------------------------

// Map MIDI note number to tuned frequency in Hz using the global tuning config.
// Returns 0.0 for out-of-range notes.
static double midiNoteToHz(uint8_t midiNote)
{
    if (midiNote > 127) return 0.0;
    return g_tuning.noteFreq(static_cast<int>(midiNote));
}

// Update the GUI's MIDI channel/instrument table.
static void printMidiInfo(const MidiFile& mf)
{
    std::lock_guard<std::mutex> lk(g_midi_info_mutex);
    for (int ch = 0; ch < 16; ++ch) {
        g_midi_info_used[ch]     = mf.channelUsed[ch];
        g_midi_info_programs[ch] = mf.channelProgram[ch];
    }
}

// Apply per-channel mixer state to the SF2 engine via MIDI CCs.
// Must be called while g_audio.mutex IS NOT held (it acquires it internally).
// Safe to call from the GUI thread or the MIDI thread (outside the locked region).
static void applyMixerToSF2()
{
    if (!g_sf2_engine.loaded) return;

    // Determine effective solo state
    bool anySolo = false;
    for (int ch = 0; ch < 16; ++ch)
        if (g_mix[ch].solo) { anySolo = true; break; }

    std::lock_guard<std::mutex> lk(g_audio.mutex);
    for (int ch = 0; ch < 16; ++ch) {
        const bool active = anySolo ? g_mix[ch].solo : !g_mix[ch].mute;
        // CC7  = Channel Volume  (0-127;  100 = nominal/unity gain)
        const int cc7  = static_cast<int>(active ? g_mix[ch].vol * 100.f + 0.5f : 0.f);
        // CC10 = Pan             (0=L, 64=C, 127=R)
        const int cc10 = static_cast<int>((g_mix[ch].pan + 1.0f) * 0.5f * 127.f + 0.5f);
        // CC91 = Reverb send
        const int cc91 = static_cast<int>(g_mix[ch].send * 127.f + 0.5f);
        g_sf2_engine.channel_control(ch, 7,  std::clamp(cc7,  0, 127));
        g_sf2_engine.channel_control(ch, 10, std::clamp(cc10, 0, 127));
        g_sf2_engine.channel_control(ch, 91, std::clamp(cc91, 0, 127));
    }
}

static void stopMidiThread() {
    g_midi_stop.store(true);
    if (g_midi_thread.joinable()) g_midi_thread.join();
    g_midi_stop.store(false);
    g_midiout.silence_all();  // silence MIDI-out device immediately
}

// Runs in a background thread — owns its MidiFile copy.
static void playMidiThread(MidiFile midi)
{
    // Voice tracking for InstrVoice path: (channel, note) → voice index
    struct ActiveNote { uint8_t channel; uint8_t note; uint32_t vi; };
    std::vector<ActiveNote> active;
    active.reserve(MAX_VOICES);

    uint32_t voiceAge[MAX_VOICES] = {};
    uint32_t ageCounter = 0;

    const uint32_t sr = g_audio.sample_rate;
    const auto     t0 = std::chrono::steady_clock::now();

    g_midi_playing.store(true);
    g_midi_elapsed_us.store(0);

    // Accumulated pause duration — shifts the effective time origin forward.
    std::chrono::microseconds pause_offset{0};

    // ---- SF2: set per-channel GM programs before the first note sounds ----
    // Channel 9 (GM percussion) is included here; channel_program() passes
    // flag_mididrums=1 for channel 9 so TSF searches bank 128 (drum bank).
    if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
        std::lock_guard<std::mutex> lk(g_audio.mutex);
        for (int ch = 0; ch < 16; ++ch) {
            if (midi.channelUsed[ch])
                g_sf2_engine.channel_program(ch, midi.channelProgram[ch]);
        }
    }
    // ---- MIDI-out: init all active channels (PB range, 432 Hz offset, program)
    if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
        for (int ch = 0; ch < 16; ++ch)
            if (midi.channelUsed[ch] || ch == 9)
                g_midiout.init_channel(ch, midi.channelProgram[ch]);
    }
    // Apply mixer initial state (volume, pan, reverb send per channel)
    applyMixerToSF2();

    // Iterate the unified event list — contains notes, CC, pitch bend,
    // aftertouch, program changes, all sorted by absolute time.
    for (const auto& ev : midi.events) {
        if (g_midi_stop.load()) break;

        // ---- Seek handling -----------------------------------------------
        // Skip events before the seek target; re-anchor time origin when we
        // reach the first event at or after the requested position.
        {
            const uint64_t seekTarget = g_seek_target_us.load();
            if (seekTarget != ~uint64_t{0}) {
                if (ev.absUs < seekTarget) {
                    g_midi_elapsed_us.store(ev.absUs, std::memory_order_relaxed);
                    continue;
                }
                // First event at or after the target.
                const auto now = std::chrono::steady_clock::now();
                pause_offset = std::chrono::duration_cast<std::chrono::microseconds>(
                    (now - t0) - std::chrono::microseconds(ev.absUs));
                g_midi_elapsed_us.store(seekTarget);
                g_seek_target_us.store(~uint64_t{0});
                {
                    std::lock_guard<std::mutex> lk(g_audio.mutex);
                    for (auto& v : g_audio.voices) v.stop();
                    g_sf2_engine.silence();
                    active.clear();
                }
                g_midiout.silence_all();
                if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
                    std::lock_guard<std::mutex> lk(g_audio.mutex);
                    for (int ch2 = 0; ch2 < 16; ++ch2) {
                        if (midi.channelUsed[ch2]) {
                            g_sf2_engine.channel_program(ch2, midi.channelProgram[ch2]);
                            // Reset per-channel controller state so stale values
                            // (sustain held, modulation, reduced expression, off-centre
                            // pitch bend) from before the seek point don't bleed through.
                            g_sf2_engine.channel_control(ch2,  64, 0);    // CC64  sustain pedal off
                            g_sf2_engine.channel_control(ch2,   1, 0);    // CC1   modulation wheel = 0
                            g_sf2_engine.channel_control(ch2,  11, 127);  // CC11  expression = full
                            g_sf2_engine.channel_pitch_bend(ch2, 8192);   // pitch bend = centre
                        }
                    }
                }
                // Re-apply spatial mixer (volume/pan/reverb) after seek
                applyMixerToSF2();
                // MIDI-out: re-init channels after seek
                if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
                    for (int ch2 = 0; ch2 < 16; ++ch2)
                        if (midi.channelUsed[ch2])
                            g_midiout.init_channel(ch2, midi.channelProgram[ch2]);
                }
            }
        }

        // Hybrid sleep+spin wait for this event's scheduled time.
        // pause_offset accumulates total paused duration so it is excluded.
        bool stopped = false;
        const auto abs_target = t0 + std::chrono::microseconds(ev.absUs);
        for (;;) {
            if (g_midi_stop.load()) { stopped = true; break; }

            // ---- Pause handling ----
            if (g_midi_paused.load()) {
                const auto ps = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lk(g_audio.mutex);
                    for (auto& v : g_audio.voices) v.stop();
                    g_sf2_engine.silence();
                }
                g_midiout.silence_all();
                while (g_midi_paused.load() && !g_midi_stop.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (g_midi_stop.load()) { stopped = true; break; }
                pause_offset += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - ps);
                // Re-send GM programs so SF2 voices are ready on resume
                if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
                    std::lock_guard<std::mutex> lk(g_audio.mutex);
                    for (int ch2 = 0; ch2 < 16; ++ch2)
                        if (midi.channelUsed[ch2])
                            g_sf2_engine.channel_program(ch2, midi.channelProgram[ch2]);
                }
                // MIDI-out: re-init channels on resume
                if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
                    for (int ch2 = 0; ch2 < 16; ++ch2)
                        if (midi.channelUsed[ch2])
                            g_midiout.init_channel(ch2, midi.channelProgram[ch2]);
                }
                // Re-apply spatial mixer after resume
                applyMixerToSF2();
            }

            const auto now             = std::chrono::steady_clock::now();
            const auto effective_target = abs_target + pause_offset;
            if (now >= effective_target) break;
            const auto us_left = std::chrono::duration_cast<std::chrono::microseconds>(
                                     effective_target - now).count();
            if (us_left > 2000)
                // Cap sleep to 5 ms so g_midi_stop is checked frequently.
                // This makes track switching feel instant (≤5 ms latency).
                std::this_thread::sleep_for(std::chrono::microseconds(
                    std::min<int64_t>(us_left - 1500, 5000)));
        }
        if (stopped) break;

        // Update progress bar
        g_midi_elapsed_us.store(ev.absUs, std::memory_order_relaxed);

        const int ch = ev.channel;

        // ------------------------------------------------------------------
        //  Dispatch every MIDI channel event type
        // ------------------------------------------------------------------
        switch (ev.type) {

        // --- Note On / Note Off -------------------------------------------
        case 0x9u:   // Note On  (vel==0 treated as note-off by hardware convention)
        case 0x8u: { // Note Off
            const bool noteOn = (ev.type == 0x9u && ev.data2 > 0);

            if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                if (noteOn) {
                    g_sf2_engine.channel_note_on(ch, ev.data1, ev.data2 / 127.0f);
                    // Update per-channel VU level (GUI reads and decays it)
                    const float vel = ev.data2 / 127.f;
                    float cur = g_ch_vu[ch].load(std::memory_order_relaxed);
                    if (vel > cur) g_ch_vu[ch].store(vel, std::memory_order_relaxed);
                } else {
                    g_sf2_engine.channel_note_off(ch, ev.data1);
                }
            } else if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
                if (noteOn) {
                    g_midiout.note_on(ch, ev.data1, ev.data2);
                    const float vel = ev.data2 / 127.f;
                    float cur = g_ch_vu[ch].load(std::memory_order_relaxed);
                    if (vel > cur) g_ch_vu[ch].store(vel, std::memory_order_relaxed);
                } else {
                    g_midiout.note_off(ch, ev.data1);
                }
            } else {
                // InstrVoice path — skip percussion without SF2
                if (ch == 9) break;

                if (noteOn) {
                    const double hz = midiNoteToHz(ev.data1);
                    if (hz <= 0.0 || hz >= static_cast<double>(sr) / 2.0) break;
                    const double amp = MASTER_GAIN * ev.data2 / 127.0;

                    std::lock_guard<std::mutex> lk(g_audio.mutex);
                    uint32_t vi = MAX_VOICES;
                    for (uint32_t i = 0; i < MAX_VOICES; ++i)
                        if (!g_audio.voices[i].active()) { vi = i; break; }
                    if (vi == MAX_VOICES) {
                        uint32_t oldest = voiceAge[0]; vi = 0;
                        for (uint32_t i = 1; i < MAX_VOICES; ++i)
                            if (voiceAge[i] < oldest) { oldest = voiceAge[i]; vi = i; }
                        for (auto it = active.begin(); it != active.end(); ++it)
                            if (it->vi == vi) { active.erase(it); break; }
                    }
                    g_audio.voices[vi].start(g_instr_model, hz, sr, amp);
                    voiceAge[vi] = ++ageCounter;
                    active.push_back({static_cast<uint8_t>(ch), ev.data1, vi});
                } else {
                    std::lock_guard<std::mutex> lk(g_audio.mutex);
                    for (auto it = active.begin(); it != active.end(); ++it) {
                        if (it->channel == ch && it->note == ev.data1) {
                            g_audio.voices[it->vi].release();
                            voiceAge[it->vi] = 0;
                            active.erase(it);
                            break;
                        }
                    }
                }
            }
            break;
        }

        // --- Control Change (CC) ------------------------------------------
        // CC7/CC10/CC91 are intercepted and remapped through the per-channel
        // spatial mixer. All other CCs pass through unchanged.
        case 0xBu:
            if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                const int cc = ev.data1;
                if (cc == 7) {
                    // Scale MIDI file volume by mixer volume; honour mute/solo
                    bool anySolo = false;
                    for (int c2 = 0; c2 < 16; ++c2) if (g_mix[c2].solo) { anySolo = true; break; }
                    const bool active = anySolo ? g_mix[ch].solo : !g_mix[ch].mute;
                    const int scaled = static_cast<int>(
                        active ? ev.data2 * g_mix[ch].vol + 0.5f : 0.f);
                    g_sf2_engine.channel_control(ch, 7, std::clamp(scaled, 0, 127));
                } else if (cc == 10) {
                    // Override pan with mixer pan (MIDI file pan ignored)
                    const int cc10 = static_cast<int>((g_mix[ch].pan + 1.0f) * 0.5f * 127.f + 0.5f);
                    g_sf2_engine.channel_control(ch, 10, std::clamp(cc10, 0, 127));
                } else if (cc == 91) {
                    // Override reverb send with mixer send
                    const int cc91 = static_cast<int>(g_mix[ch].send * 127.f + 0.5f);
                    g_sf2_engine.channel_control(ch, 91, std::clamp(cc91, 0, 127));
                } else {
                    g_sf2_engine.channel_control(ch, cc, ev.data2);
                }
            } else if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
                const int cc = ev.data1;
                if (cc == 7) {
                    bool anySolo = false;
                    for (int c2 = 0; c2 < 16; ++c2) if (g_mix[c2].solo) { anySolo = true; break; }
                    const bool act = anySolo ? g_mix[ch].solo : !g_mix[ch].mute;
                    const int scaled = static_cast<int>(act ? ev.data2 * g_mix[ch].vol + 0.5f : 0.f);
                    g_midiout.control_change(ch, 7, std::clamp(scaled, 0, 127));
                } else if (cc == 10) {
                    const int cc10 = static_cast<int>((g_mix[ch].pan + 1.0f) * 0.5f * 127.f + 0.5f);
                    g_midiout.control_change(ch, 10, std::clamp(cc10, 0, 127));
                } else {
                    g_midiout.control_change(ch, cc, ev.data2);
                }
            }
            break;

        // --- Program Change -----------------------------------------------
        case 0xCu:
            if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                g_sf2_engine.channel_program(ch, ev.data1);
            } else if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
                g_midiout.program_change(ch, ev.data1);
            }
            break;

        // --- Pitch Bend ---------------------------------------------------
        // data1 = LSB, data2 = MSB  →  14-bit value: centre = 8192
        case 0xEu:
            if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
                const int pitchWheel = ev.data1 | (ev.data2 << 7);
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                g_sf2_engine.channel_pitch_bend(ch, pitchWheel);
            } else if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
                const int pitchWheel = ev.data1 | (ev.data2 << 7);
                g_midiout.pitch_bend(ch, pitchWheel);
            }
            break;

        // --- Channel Aftertouch (mono pressure) ---------------------------
        case 0xDu:
            if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                g_sf2_engine.channel_aftertouch(ch, ev.data1);
            } else if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
                g_midiout.aftertouch(ch, ev.data1);
            }
            break;

        // --- Polyphonic Aftertouch ----------------------------------------
        // TSF has no per-key pressure; ignore silently.
        case 0xAu:
            break;

        default:
            break;
        }
    }  // end for (ev : midi.events)

    // Silence all voices
    {
        std::lock_guard<std::mutex> lk(g_audio.mutex);
        for (auto& v : g_audio.voices) v.stop();
        g_sf2_engine.silence();
    }
    g_midiout.silence_all();
    g_midi_playing.store(false);
    if (!g_midi_stop.load())
        g_midi_elapsed_us.store(g_midi_total_us.load());  // show full at end
}  // end playMidiThread

static void printScale() {
    g_tuning.printScale();
}

// ---------------------------------------------------------------------------
//  High-level playback helpers  (called from GuiApp and from main)
// ---------------------------------------------------------------------------

// Play a MIDI file: stop current playback instantly, then parse and play
// entirely on a background thread so the GUI thread never blocks.
void playMidiFile(const std::string& path)
{
    // 1. Stop and silence immediately (<5 ms with the 5-ms sleep cap).
    stopMidiThread();
    stopScaleThread();
    {
        std::lock_guard<std::mutex> lk(g_audio.mutex);
        for (auto& v : g_audio.voices) v.stop();
        g_sf2_engine.silence();
    }
    g_midiout.silence_all();

    // 2. Update visible state right away so the UI reflects the new song.
    g_midi_paused.store(false);
    g_midi_playing.store(false);
    g_midi_elapsed_us.store(0);
    g_midi_total_us.store(0);
    {
        std::lock_guard<std::mutex> lk(g_now_playing_mutex);
        g_now_playing_name = std::filesystem::path(path).filename().string();
    }
    g_current_midi_path = path;

    // 3. Parse the MIDI file and start playback on a background thread.
    //    Large files can take > 1 s to parse; doing it here would freeze the GUI.
    g_midi_thread = std::thread([path]() {
        const std::string name = std::filesystem::path(path).filename().string();
        AppLogInfo("MIDI: Opening \"" + name + "\"");

        MidiFile mf = loadMidiFile(path);
        if (!mf.valid) {
            const std::string err = mf.error.empty() ? "unknown error" : mf.error;
            AppLogError("MIDI: Failed to open \"" + name + "\" - " + err);
            for (const auto& d : mf.diagnostics)
                AppLogWarn("  " + d);
            return;
        }
        if (g_midi_stop.load()) return;

        // Log success/warning summary
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "MIDI: OK \"%s\" - %zu events, %zu notes, %zu program changes",
                name.c_str(),
                mf.events.size(),
                mf.notes.size(),
                mf.programChanges.size());
            if (mf.events.empty())
                AppLogWarn(buf);
            else
                AppLogInfo(buf);
        }
        if (!mf.standard.empty())
            AppLogInfo("  Standard: " + mf.standard);

        // Always log diagnostics; use warn level when something looks wrong
        {
            const bool hasIssue = mf.events.empty();
            for (const auto& d : mf.diagnostics) {
                const bool isWarn = (d.rfind("WARNING", 0) == 0 ||
                                     d.rfind("PROBLEM", 0) == 0 ||
                                     d.find("WARNING") != std::string::npos);
                if (hasIssue || isWarn)
                    AppLogWarn("  " + d);
                else
                    AppLogInfo("  " + d);
            }
        }

        printMidiInfo(mf);
        const uint64_t totalUs = mf.events.empty() ? 0u
                               : mf.events.back().absUs + 1'000'000u;
        g_midi_total_us.store(totalUs);
        g_midi_elapsed_us.store(0);

        playMidiThread(std::move(mf));
    });
}

// Load a SoundFont2 file and resume MIDI playback from the current position.
void loadSf2File(const std::string& path)
{
    // Snapshot playback state before stopping anything.
    const bool        wasPlaying = g_midi_playing.load();
    const bool        wasPaused  = g_midi_paused.load();
    const uint64_t    resumePos  = g_midi_elapsed_us.load();
    const std::string midiPath   = g_current_midi_path;

    stopMidiThread();
    stopScaleThread();
    {
        std::lock_guard<std::mutex> lk(g_audio.mutex);
        for (auto& v : g_audio.voices) v.stop();
        g_sf2_engine.silence();
    }
    g_midiout.silence_all();
    g_midiout.close();

    // Load the SF2 file from disk OUTSIDE the audio mutex.  tsf_load_filename
    // can take several seconds for large soundfonts; keeping it outside the
    // lock lets the audio callback keep running (rendering silence) instead of
    // stalling for the entire I/O duration.
    tsf* new_tsf = tsf_load_filename(path.c_str());

    // Install the new SF2 instance under the lock (fast — no disk I/O).
    {
        std::lock_guard<std::mutex> lk(g_audio.mutex);
        if (g_sf2_engine.install(new_tsf, path, g_audio.sample_rate)) {
            g_sf2_engine.retune(g_tuning.a4);
            g_instr_model = InstrModel::SF2;
        }
    }

    // If a MIDI was playing, restart it from the saved position.
    if (wasPlaying && !midiPath.empty()) {
        playMidiFile(midiPath);
        if (resumePos > 0)
            g_seek_target_us.store(resumePos);  // seek jumps to where we were
        if (wasPaused)
            g_midi_paused.store(true);
    }
}

// Switch to a Windows MIDI Out device, resuming playback if a song was playing.
// Mirrors the resume logic in loadSf2File().
void switchToMidiOut(int device_id)
{
    const bool        wasPlaying = g_midi_playing.load();
    const bool        wasPaused  = g_midi_paused.load();
    const uint64_t    resumePos  = g_midi_elapsed_us.load();
    const std::string midiPath   = g_current_midi_path;

    stopMidiThread();
    {
        std::lock_guard<std::mutex> lk(g_audio.mutex);
        for (auto& v : g_audio.voices) v.stop();
        g_sf2_engine.silence();
    }
    g_midiout.silence_all();
    g_midiout.close();

    if (g_midiout.open(device_id))
        g_instr_model = InstrModel::MIDI_OUT;

    if (wasPlaying && !midiPath.empty()) {
        playMidiFile(midiPath);
        if (resumePos > 0)
            g_seek_target_us.store(resumePos);
        if (wasPaused)
            g_midi_paused.store(true);
    }
}


#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "gui_app.h"

static ID3D11Device*           g_d3d_device  = nullptr;
static ID3D11DeviceContext*    g_d3d_ctx      = nullptr;
static IDXGISwapChain*         g_swap_chain   = nullptr;
static ID3D11RenderTargetView* g_rtv          = nullptr;
static bool                    g_resize_pending = false;
static UINT                    g_resize_w = 0, g_resize_h = 0;

static void CreateRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_d3d_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount          = 2;
    sd.BufferDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags                = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow         = hWnd;
    sd.SampleDesc.Count     = 1;
    sd.Windowed             = TRUE;
    sd.SwapEffect           = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL flOut;
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fl, 2, D3D11_SDK_VERSION, &sd,
        &g_swap_chain, &g_d3d_device, &flOut, &g_d3d_ctx);
    if (res == DXGI_ERROR_UNSUPPORTED)   // Fallback to WARP software renderer
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            fl, 2, D3D11_SDK_VERSION, &sd,
            &g_swap_chain, &g_d3d_device, &flOut, &g_d3d_ctx);
    if (FAILED(res)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_swap_chain) { g_swap_chain->Release(); g_swap_chain = nullptr; }
    if (g_d3d_ctx)    { g_d3d_ctx->Release();    g_d3d_ctx    = nullptr; }
    if (g_d3d_device) { g_d3d_device->Release();  g_d3d_device = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_resize_pending = true;
            g_resize_w = LOWORD(lParam);
            g_resize_h = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // suppress Alt menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Activate 1 ms Windows timer resolution for the lifetime of the process.
    // Default resolution is ~15.6 ms, which causes sleep_for(1ms) to overshoot
    // by up to 15 ms, bunching MIDI events and producing audible clicks.
    timeBeginPeriod(1);

    int32_t     forceDevice = -1;
    std::string forceApiStr;
    bool        listOnly    = false;
    std::string midiPath;
    std::string sf2Path;
    std::string timbreArg;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--list" || a == "-l") {
            listOnly = true;
        } else if ((a == "--device" || a == "-d") && i + 1 < argc) {
            try { forceDevice = std::stoi(argv[++i]); } catch (...) {}
        } else if ((a == "--api" || a == "-a") && i + 1 < argc) {
            forceApiStr = argv[++i];
        } else if ((a == "--midi" || a == "-m") && i + 1 < argc) {
            midiPath = argv[++i];
        } else if ((a == "--sf2" || a == "-s") && i + 1 < argc) {
            sf2Path = argv[++i];
        } else if ((a == "--timbre" || a == "-t") && i + 1 < argc) {
            timbreArg = argv[++i];
        } else if ((a == "--tune" || a == "-T") && i + 1 < argc) {
            try {
                const double val = std::stod(argv[++i]);
                if (val >= 200.0 && val <= 600.0)
                    g_tuning.a4 = val;
                else
                    (void)0; // A4 out of range, ignore
            } catch (...) {
            }
        } else if ((a == "--mode" || a == "-M") && i + 1 < argc) {
            const std::string m(argv[++i]);
            if (m == "equal" || m == "eq" || m == "et")
                g_tuning.mode = TuneMode::EQUAL;
            else if (m == "integer" || m == "int")
                g_tuning.mode = TuneMode::INTEGER;
            else { /* unknown mode, ignore */ }
        } else if (a == "--help" || a == "-h") {
            return 0;
        }
    }

    g_devices = enumerateOutputDevices();
    g_midiout_devices = MidiOutEngine::enumerate();
    auto& devices = g_devices;  // alias for the rest of main
    if (devices.empty()) {
        MessageBoxA(nullptr, "No audio output devices found.", "432 Hz Player",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    if (listOnly) return 0;

    // --- Build priority-ordered candidate list ---
    using Candidate = std::pair<RtAudio::Api, uint32_t>;
    std::vector<Candidate> candidates;

    auto addIfNew = [&](RtAudio::Api api, uint32_t id) {
        for (const auto& c : candidates)
            if (c.first == api && c.second == id) return;
        candidates.push_back({api, id});
    };

    if (forceDevice >= 0) {
        RtAudio::Api wantApi = forceApiStr.empty()
                               ? RtAudio::UNSPECIFIED
                               : apiFromString(forceApiStr);
        for (const auto& d : devices) {
            if (static_cast<int32_t>(d.id) != forceDevice) continue;
            if (wantApi != RtAudio::UNSPECIFIED && d.api != wantApi) continue;
            addIfNew(d.api, d.id);
        }
        if (candidates.empty()) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "Device %d not found.", forceDevice);
            MessageBoxA(nullptr, msg, "432 Hz Player", MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        RtAudio::Api wantApi = forceApiStr.empty()
                               ? RtAudio::UNSPECIFIED
                               : apiFromString(forceApiStr);

        // Prefer the Windows default output device (WASAPI) first.
        if (wantApi == RtAudio::UNSPECIFIED || wantApi == RtAudio::WINDOWS_WASAPI) {
            for (const auto& d : devices) {
                if (d.api == RtAudio::WINDOWS_WASAPI && d.info.isDefaultOutput) {
                    addIfNew(d.api, d.id);
                    break;
                }
            }
        }

        for (auto prefApi : API_PREF) {
            if (wantApi != RtAudio::UNSPECIFIED && prefApi != wantApi) continue;
            // Pass 1: devices that advertise at least one preferred rate
            for (const auto& d : devices) {
                if (d.api != prefApi) continue;
                for (uint32_t rate : PREFERRED_RATES) {
                    if (std::find(d.info.sampleRates.begin(),
                                  d.info.sampleRates.end(), rate)
                        != d.info.sampleRates.end()) {
                        addIfNew(d.api, d.id);
                        break;
                    }
                }
            }
            // Pass 2: any remaining devices for this API
            for (const auto& d : devices)
                if (d.api == prefApi) addIfNew(d.api, d.id);
        }
        // Final fallback: any device in any API
        for (const auto& d : devices) addIfNew(d.api, d.id);
    }

    // --- Try each candidate ---
    uint32_t     negotiatedRate = 0;
    uint32_t     bufferFrames   = FRAMES_PER_BUFFER;
    RtAudio::Api openedApi      = RtAudio::UNSPECIFIED;
    uint32_t     openedDeviceId = 0;
    std::unique_ptr<RtAudio> audio;

    for (const auto& cand : candidates) {
        RtAudio::Api api   = cand.first;
        uint32_t     devId = cand.second;

        // Use all output channels the device advertises
        uint32_t nCh = 2;
        for (const auto& d : devices)
            if (d.api == api && d.id == devId)
                { nCh = std::max(2u, d.info.outputChannels); break; }

        try {
            audio = std::make_unique<RtAudio>(api);
        } catch (...) {
            continue;
        }

        if (tryOpenStream(*audio, devId, api, nCh, negotiatedRate, bufferFrames)) {
            openedApi        = api;
            openedDeviceId   = devId;
            g_audio.channels = nCh;
            break;
        }
        audio.reset();
    }

    if (!audio || negotiatedRate == 0) {
        MessageBoxA(nullptr,
            "Could not open an audio stream on any device/API/rate.\n\n"
            "Possible causes:\n"
            "  - Another app holds exclusive access (DAW, Voicemeeter...)\n"
            "  - The ASIO driver is broken  ->  try installing ASIO4ALL\n"
            "  - Windows audio service needs a restart",
            "432 Hz Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_audio.sample_rate = negotiatedRate;

    // Sync to module-level globals read by GuiApp
    g_opened_api       = openedApi;
    g_opened_device_id = openedDeviceId;
    g_negotiated_rate  = negotiatedRate;
    g_buffer_frames    = bufferFrames;

    std::string openedName;
    for (const auto& d : devices)
        if (d.api == openedApi && d.id == openedDeviceId)
            { openedName = d.info.name; break; }
    g_opened_name = openedName;

    if (audio->startStream()) {
        audio->closeStream();
        return 1;
    }

    // Enable hall reverb by default — transforms any piano timbre
    g_reverb.init(negotiatedRate);
    g_reverb.set_preset("hall");

    // Apply --timbre argument (before --sf2 so sf2 can override when needed)
    if (!timbreArg.empty())
        g_instr_model = instrFromString(timbreArg.c_str());

    // Auto-load SF2 if --sf2 was given on the command line
    if (!sf2Path.empty()) {
        if (g_sf2_engine.load(sf2Path.c_str(), g_audio.sample_rate)) {
            g_sf2_engine.retune(g_tuning.a4);
            g_instr_model = InstrModel::SF2;
            g_reverb.set_preset("hall");
        }
    }

    // Auto-play MIDI file if --midi was given on the command line
    if (!midiPath.empty()) {
        playMidiFile(midiPath);
    }

    // ---- helper: close current stream, open a new one, restart ----
    // Also updates module-level globals so GuiApp can read the current state.
    auto switchToDevice = [&](RtAudio::Api targetApi, uint32_t targetId) -> bool {
        stopMidiThread();
        stopScaleThread();
        {
            std::lock_guard<std::mutex> lk(g_audio.mutex);
            for (auto& v : g_audio.voices) v.stop();
        }
        if (audio) {
            if (audio->isStreamRunning()) audio->stopStream();
            if (audio->isStreamOpen())    audio->closeStream();
            audio.reset();
        }
        std::unique_ptr<RtAudio> newAudio;
        try {
            newAudio = std::make_unique<RtAudio>(targetApi);
        } catch (...) {
            return false;
        }
        uint32_t newRate = 0, newBuf = FRAMES_PER_BUFFER;
        uint32_t nCh = 2;
        for (const auto& d : devices)
            if (d.api == targetApi && d.id == targetId)
                { nCh = std::max(2u, d.info.outputChannels); break; }
        if (!tryOpenStream(*newAudio, targetId, targetApi, nCh, newRate, newBuf)) {
            return false;
        }
        if (newAudio->startStream()) {
            newAudio->closeStream();
            return false;
        }
        audio            = std::move(newAudio);
        openedApi        = targetApi;
        openedDeviceId   = targetId;
        negotiatedRate   = newRate;
        bufferFrames     = newBuf;
        g_audio.sample_rate = newRate;
        g_audio.channels    = nCh;
        {
            std::lock_guard<std::mutex> sfLk(g_audio.mutex);
            g_sf2_engine.update_sr(newRate);
            g_reverb.init(newRate);
        }
        openedName.clear();
        for (const auto& d : devices)
            if (d.api == openedApi && d.id == openedDeviceId)
                { openedName = d.info.name; break; }
        // Sync to module-level globals that GuiApp reads
        g_opened_api       = openedApi;
        g_opened_device_id = openedDeviceId;
        g_opened_name      = openedName;
        g_negotiated_rate  = newRate;
        g_buffer_frames    = newBuf;
        return true;
    };

    // Expose switchToDevice globally so GuiApp can call it
    g_switch_device_fn = switchToDevice;

    // -------------------------------------------------------------------------
    //  Dear ImGui  —  Win32 + DirectX 11 window
    // -------------------------------------------------------------------------
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"432HzSynth";
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(
        0L, wc.lpszClassName,
        L"432 Hz Bit-Perfect Synthesiser",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1340, 800,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxA(nullptr, "Failed to create Direct3D 11 device.",
                    "432 Hz Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    ::UpdateWindow(hwnd);

    // ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Build an absolute path for the ImGui layout INI so it persists
    // regardless of the working directory.
    {
        static char s_ini_path[MAX_PATH] = {};
        PWSTR pApp = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,
                                           KF_FLAG_CREATE, nullptr, &pApp))) {
            char buf[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, pApp, -1, buf, MAX_PATH, nullptr, nullptr);
            CoTaskMemFree(pApp);
            std::string dir = std::string(buf) + "\\432HzPlayer";
            CreateDirectoryA(dir.c_str(), nullptr);
            std::snprintf(s_ini_path, MAX_PATH, "%s\\432hz_layout.ini", dir.c_str());
        } else {
            std::strncpy(s_ini_path, "432hz_player_layout.ini", MAX_PATH);
        }
        io.IniFilename = s_ini_path;
    }

    // Dark theme with custom tweaks
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding    = 6.f;
        style.ChildRounding     = 4.f;
        style.FrameRounding     = 4.f;
        style.PopupRounding     = 4.f;
        style.ScrollbarRounding = 4.f;
        style.GrabRounding      = 4.f;
        style.TabRounding       = 4.f;
        style.FramePadding      = {6.f, 4.f};
        style.ItemSpacing       = {8.f, 5.f};
        style.WindowPadding     = {10.f, 10.f};
        // Slightly warmer dark palette
        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg]  = {0.11f, 0.11f, 0.13f, 0.97f};
        c[ImGuiCol_ChildBg]   = {0.08f, 0.08f, 0.10f, 0.60f};
        c[ImGuiCol_FrameBg]   = {0.18f, 0.18f, 0.22f, 1.00f};
        c[ImGuiCol_Header]    = {0.22f, 0.40f, 0.60f, 0.80f};
        c[ImGuiCol_HeaderHovered] = {0.28f, 0.50f, 0.75f, 1.00f};
        c[ImGuiCol_TitleBgActive] = {0.16f, 0.29f, 0.48f, 1.00f};
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            style.WindowRounding = 0.f;
            c[ImGuiCol_WindowBg].w = 1.f;
        }
    }

    // Try to load a reasonably sharp system font; fall back to default
    {
        const char* fontCandidates[] = {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/calibri.ttf",
        };
        for (const char* fp : fontCandidates) {
            if (std::filesystem::exists(fp)) {
                io.Fonts->AddFontFromFileTTF(fp, 15.0f);
                break;
            }
        }
        // If none found, ImGui uses its built-in Proggy font automatically.
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_d3d_device, g_d3d_ctx);

    // Instantiate the GUI application
    GuiApp app;

    // Restore main window placement from settings and auto-load last SF2
    app.ApplyMainWindowPlacement(hwnd);  // must be before ShowWindow to avoid flicker
    ::ShowWindow(hwnd, app.st.main_maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    ::UpdateWindow(hwnd);

    if (app.st.last_sf2_path[0] != '\0' &&
        std::filesystem::exists(app.st.last_sf2_path))
    {
        loadSf2File(app.st.last_sf2_path);
    }

    // Restore last MIDI out device (by name, so it survives index changes)
    if (app.st.last_midiout_device[0] != '\0') {
        const std::string want(app.st.last_midiout_device);
        for (const auto& dev : g_midiout_devices) {
            if (dev.name == want) {
                if (g_midiout.open(dev.id))
                    g_instr_model = InstrModel::MIDI_OUT;
                break;
            }
        }
    }

    // If CLI --midi / --sf2 args were given, pre-fill the browser folders
    if (!midiPath.empty()) {
        const auto p = std::filesystem::path(midiPath).parent_path().string();
        if (!p.empty()) {
            std::strncpy(app.st.midi_folder, p.c_str(),
                         sizeof(app.st.midi_folder) - 1);
            app.st.midi_folder_changed = true;
        }
    }
    if (!sf2Path.empty()) {
        const auto p = std::filesystem::path(sf2Path).parent_path().string();
        if (!p.empty()) {
            std::strncpy(app.st.sf2_folder, p.c_str(),
                         sizeof(app.st.sf2_folder) - 1);
            app.st.sf2_folder_changed = true;
        }
    }

    // ---- Render loop ----
    static const float kClearColor[4] = {0.08f, 0.08f, 0.10f, 1.0f};
    bool done = false;
    while (!done) {
        // Win32 message pump
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Handle resize
        if (g_resize_pending) {
            CleanupRenderTarget();
            g_swap_chain->ResizeBuffers(0, g_resize_w, g_resize_h,
                                        DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
            g_resize_pending = false;
        }

        // Handle keyboard shortcuts not processed by ImGui
        if (!io.WantCaptureKeyboard) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                stopMidiThread();
                stopScaleThread();
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                for (auto& v : g_audio.voices) v.stop();
                g_sf2_engine.silence();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M))
                app.st.show_midi = true;
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F))
                app.st.show_sf2 = true;
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
                app.st.show_devices = true;
        }

        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render all UI windows
        app.Render(hwnd);

        // Render to D3D11 back-buffer
        ImGui::Render();
        g_d3d_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_d3d_ctx->ClearRenderTargetView(g_rtv, kClearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Multi-viewport support (pop-out windows on other monitors)
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();

            // Make every floating (undocked) viewport window owned by the main
            // HWND.  Win32 guarantees that an owned window always renders above
            // its owner, so this keeps all pop-out panels on top of the main
            // window without any per-frame Z-order juggling.
            {
                ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
                for (int i = 1; i < pio.Viewports.Size; i++) {
                    if (HWND vh = (HWND)pio.Viewports[i]->PlatformHandle)
                        if (::GetWindow(vh, GW_OWNER) != hwnd)
                            ::SetWindowLongPtr(vh, GWLP_HWNDPARENT,
                                               reinterpret_cast<LONG_PTR>(hwnd));
                }
            }

            ImGui::RenderPlatformWindowsDefault();
        }

        g_swap_chain->Present(1, 0); // vsync (1 = lock to refresh rate)
    }

    // ---- Shutdown ----
    app.Shutdown();       // persist window states and placement on exit
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    stopMidiThread();
    stopScaleThread();
    {
        std::lock_guard<std::mutex> lk(g_audio.mutex);
        for (auto& v : g_audio.voices) v.stop();
        g_sf2_engine.silence();
    }
    audio->stopStream();
    audio->closeStream();
    timeEndPeriod(1);
    return 0;
}
