#pragma once
// =============================================================================
//  midi_out_engine.h  —  Windows MIDI output (WinMM) synthesiser path
//
//  Streams MIDI events directly to any WinMM MIDI output port, e.g.:
//    • Microsoft GS Wavetable Synth  (built-in Windows software synth)
//    • Roland/Yamaha hardware MIDI interfaces
//    • Virtual ports (LoopMIDI, VirtualMIDISynth, etc.)
//
//  432 Hz tuning
//  -------------
//  Windows MIDI devices have no per-channel frequency-offset API, so 432 Hz
//  is approximated using Pitch Bend + RPN 0 (Pitch Bend Sensitivity):
//
//   1. On open(), every channel's Pitch Bend Sensitivity (RPN 0) is set to
//      2 semitones — the GM default, made explicit here.
//   2. The resting pitch-bend value is shifted by
//        offset_steps = round(8192 * tuning_semitones / 2)
//      = round(8192 * -0.31769 / 2) = -1302 steps
//      which encodes -31.77 cents  (= 12*log2(432/440) × 100).
//   3. When a MIDI file sends a pitch-bend event it is summed with the
//      offset before transmission so expressive bends still work correctly.
//   4. If the MIDI file changes PB range via RPN 0 the offset is
//      recalculated for that channel automatically.
//
//  Limitations vs SF2 path
//  -----------------------
//    • Audio routes through the MIDI device driver, NOT through RtAudio,
//      so Reverb, Spatial Mixer, and Master Volume have no effect.
//    • Pan/volume (CC10/CC7) from the MIDI file pass through unmodified;
//      per-channel mute/solo does scale CC7.
//
//  Thread safety
//  -------------
//  send_short() / note_on() / note_off() / pitch_bend() / control_change() /
//  program_change() / aftertouch() / silence_all() are safe to call from any
//  single thread (the MIDI playback thread).
//  open() / close() must NOT be called from the playback thread while it runs.
// =============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Device descriptor
// ---------------------------------------------------------------------------
struct MidiOutDevice {
    int         id;
    std::string name;
};

// =============================================================================
//  MidiOutEngine
// =============================================================================
struct MidiOutEngine {
    HMIDIOUT handle    = nullptr;
    bool     opened    = false;
    int      device_id = -1;

    // Per-channel state
    float tuning_semitones = -0.31769f;  // 12*log2(432/440); updated via retune()
    int   pb_range[16];   // current PB sensitivity in semitones (default 2)
    int   pb_offset[16];  // pre-computed PB-unit offset for 432 Hz per channel
    int   rpn_msb[16];    // last RPN MSB received per channel (PB-range tracking)
    int   rpn_lsb[16];    // last RPN LSB received per channel

    MidiOutEngine()
    {
        for (int i = 0; i < 16; ++i) {
            pb_range[i]  = 2;
            pb_offset[i] = 0;
            rpn_msb[i]   = 127;
            rpn_lsb[i]   = 127;
        }
        recalc_offsets();
    }

    ~MidiOutEngine() { close(); }

    // -----------------------------------------------------------------------
    //  Enumerate available WinMM MIDI output ports.
    //  Uses the wide-char API so Unicode device names are not dropped.
    // -----------------------------------------------------------------------
    static std::vector<MidiOutDevice> enumerate()
    {
        std::vector<MidiOutDevice> devs;

        // Helper: wide MIDIOUTCAPSW.szPname → UTF-8 std::string
        auto toUtf8 = [](const wchar_t* w) -> std::string {
            const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                                nullptr, 0, nullptr, nullptr);
            if (len <= 0) return {};
            std::string s(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                s.data(), len, nullptr, nullptr);
            return s;
        };

        const int n = static_cast<int>(midiOutGetNumDevs());
        for (int i = 0; i < n; ++i) {
            MIDIOUTCAPSW caps{};
            if (midiOutGetDevCapsW(static_cast<UINT>(i), &caps,
                                   sizeof(caps)) == MMSYSERR_NOERROR)
                devs.push_back({i, toUtf8(caps.szPname)});
        }
        return devs;
    }

    // -----------------------------------------------------------------------
    //  open — open a WinMM MIDI out port by device index.
    //  Returns true on success.  Must NOT be called from the playback thread.
    // -----------------------------------------------------------------------
    bool open(int id)
    {
        close();
        const MMRESULT r = midiOutOpen(&handle,
                                       static_cast<UINT>(id),
                                       0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) { handle = nullptr; return false; }
        device_id = id;
        opened    = true;
        init_all_channels();
        return true;
    }

    // -----------------------------------------------------------------------
    //  close — silence all notes and close the port.
    //  Must NOT be called from the playback thread while it runs.
    // -----------------------------------------------------------------------
    void close()
    {
        if (!opened) return;
        silence_all();
        midiOutClose(handle);
        handle    = nullptr;
        opened    = false;
        device_id = -1;
    }

    // -----------------------------------------------------------------------
    //  retune — update target A4 Hz, recalculate PB offsets, reapply to all
    //  channels.  Safe to call from the GUI thread while the device is open.
    // -----------------------------------------------------------------------
    void retune(double a4_hz)
    {
        tuning_semitones = static_cast<float>(12.0 * std::log2(a4_hz / 440.0));
        recalc_offsets();
        if (opened)
            for (int ch = 0; ch < 16; ++ch)
                apply_tuning_bend(ch);
    }

    // -----------------------------------------------------------------------
    //  init_channel — reset channel state, set PB range, apply 432 Hz offset,
    //  and send the initial program change.
    //  Called at song start, after seek, and after pause-resume.
    // -----------------------------------------------------------------------
    void init_channel(int ch, int program)
    {
        if (!opened) return;
        pb_range[ch] = 2;
        rpn_msb[ch]  = 127;
        rpn_lsb[ch]  = 127;
        recalc_offset(ch);

        // RPN 0 = Pitch Bend Sensitivity = 2 semitones (explicit GM default)
        send_short(0xB0 | ch, 101, 0);    // RPN MSB = 0
        send_short(0xB0 | ch, 100, 0);    // RPN LSB = 0
        send_short(0xB0 | ch,   6, 2);    // Data Entry MSB = 2 semitones
        send_short(0xB0 | ch,  38, 0);    // Data Entry LSB = 0
        send_short(0xB0 | ch, 101, 127);  // RPN null - deactivate
        send_short(0xB0 | ch, 100, 127);

        // Reset expression, mod-wheel, sustain pedal
        send_short(0xB0 | ch,  64,   0);  // sustain off
        send_short(0xB0 | ch,   1,   0);  // modulation wheel = 0
        send_short(0xB0 | ch,  11, 127);  // expression = full

        // Apply the 432 Hz tuning as a constant pitch-bend shift
        apply_tuning_bend(ch);

        // Program change — skip ch9 (drums: device keeps bank 128 by default)
        if (ch != 9)
            send_short(0xC0 | ch, program & 0x7F);
    }

    // -----------------------------------------------------------------------
    //  silence_all — all-notes-off + sustain off + restore tuning bends.
    //  Safe to call from the playback thread or the GUI thread.
    // -----------------------------------------------------------------------
    void silence_all()
    {
        if (!opened) return;
        for (int ch = 0; ch < 16; ++ch) {
            send_short(0xB0 | ch,  64,   0);  // CC64 sustain off
            send_short(0xB0 | ch, 123,   0);  // CC123 all notes off
            send_short(0xB0 | ch, 121,   0);  // CC121 reset all controllers
            // CC121 resets pitch bend to centre (8192).
            // Re-apply the 432 Hz offset so the device stays in tune.
            apply_tuning_bend(ch);
        }
    }

    // -----------------------------------------------------------------------
    //  Event senders (called from playMidiThread)
    // -----------------------------------------------------------------------

    void note_on(int ch, int note, int vel)
    {
        send_short(0x90 | (ch & 0xF),
                   static_cast<uint8_t>(note & 0x7F),
                   static_cast<uint8_t>(vel  & 0x7F));
    }

    void note_off(int ch, int note)
    {
        send_short(0x80 | (ch & 0xF),
                   static_cast<uint8_t>(note & 0x7F), 0);
    }

    void program_change(int ch, int prog)
    {
        if (ch == 9) return;  // drums: preserve bank 128 assignment
        send_short(0xC0 | (ch & 0xF), static_cast<uint8_t>(prog & 0x7F));
    }

    // CC — passes through all CCs; tracks RPN 0 to keep PB offset accurate
    void control_change(int ch, int cc, int val)
    {
        if (!opened || ch < 0 || ch >= 16) return;
        send_short(0xB0 | (ch & 0xF),
                   static_cast<uint8_t>(cc  & 0x7F),
                   static_cast<uint8_t>(val & 0x7F));

        // Track RPN 0 (Pitch Bend Sensitivity) to keep the offset accurate
        if      (cc == 101) rpn_msb[ch] = val;
        else if (cc == 100) rpn_lsb[ch] = val;
        else if (cc == 6 && rpn_msb[ch] == 0 && rpn_lsb[ch] == 0 && val > 0) {
            // Data Entry for RPN 0: new PB range in semitones
            pb_range[ch] = val;
            recalc_offset(ch);
            // Re-send the tuning bend with the new range
            apply_tuning_bend(ch);
        }
    }

    // Pitch bend: combine file value with the 432 Hz offset.
    // file_pb = raw 14-bit value (0-16383, centre = 8192).
    void pitch_bend(int ch, int file_pb)
    {
        if (!opened) return;
        // The MIDI file's bend is relative to centre (8192).
        // pb_offset encodes the -31.77-cent shift in PB units for the
        // current PB range of this channel.  Simply add — if the result
        // clips to [0,16383] the artist's bend was near the physical limit.
        const int combined = std::clamp(file_pb + pb_offset[ch], 0, 16383);
        send_pb_raw(ch, combined);
    }

    void aftertouch(int ch, int pressure)
    {
        if (!opened) return;
        send_short(0xD0 | (ch & 0xF), static_cast<uint8_t>(pressure & 0x7F));
    }

    // -----------------------------------------------------------------------
    //  send_short — pack and dispatch a 3-byte WinMM short MIDI message.
    // -----------------------------------------------------------------------
    void send_short(int status, int d1 = 0, int d2 = 0) const
    {
        if (!opened) return;
        const DWORD msg = static_cast<DWORD>(status & 0xFF)
                        | (static_cast<DWORD>(d1 & 0x7F) << 8)
                        | (static_cast<DWORD>(d2 & 0x7F) << 16);
        midiOutShortMsg(handle, msg);
    }

private:
    // Recalculate the PB-unit offset for one channel:
    //   offset = 8192 * tuning_semitones / pb_range[ch]
    void recalc_offset(int ch)
    {
        const float R = static_cast<float>(pb_range[ch] > 0 ? pb_range[ch] : 2);
        pb_offset[ch] = static_cast<int>(std::round(8192.f * tuning_semitones / R));
    }

    void recalc_offsets()
    {
        for (int ch = 0; ch < 16; ++ch)
            recalc_offset(ch);
    }

    // Send pitch bend that encodes only the 432 Hz offset (no file contribution)
    void apply_tuning_bend(int ch)
    {
        const int val = std::clamp(8192 + pb_offset[ch], 0, 16383);
        send_pb_raw(ch, val);
    }

    void send_pb_raw(int ch, int val) const
    {
        if (!opened) return;
        const uint8_t lsb = static_cast<uint8_t>( val        & 0x7F);
        const uint8_t msb = static_cast<uint8_t>((val >> 7)  & 0x7F);
        // Cast to int so send_short's masking works correctly
        send_short(0xE0 | (ch & 0xF), lsb, msb);
    }

    // Send GM System On SysEx then initialise all 16 channels.
    void init_all_channels()
    {
        // GM System On: F0 7E 7F 09 01 F7
        // Most devices ignore it harmlessly; the Microsoft GS Wavetable Synth
        // responds correctly, resetting all program assignments.
        static const uint8_t gmReset[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
        MIDIHDR hdr{};
        hdr.lpData         = reinterpret_cast<LPSTR>(const_cast<uint8_t*>(gmReset));
        hdr.dwBufferLength = sizeof(gmReset);
        if (midiOutPrepareHeader(handle, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) {
            midiOutLongMsg(handle, &hdr, sizeof(hdr));
            while (!(hdr.dwFlags & MHDR_DONE)) Sleep(1);
            midiOutUnprepareHeader(handle, &hdr, sizeof(hdr));
        }
        // Init all channels; program 0 will be overridden by the MIDI file
        for (int ch = 0; ch < 16; ++ch)
            init_channel(ch, 0);
    }
};
