#pragma once
// =============================================================================
//  midi_loader.h  —  Minimal Standard MIDI File (SMF) parser
//
//  Supports SMF formats 0, 1, and 2.
//  All timing is converted to absolute microseconds.
//  No external dependencies — pure C++ / STL.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  MidiNote: one note-on or note-off event at an absolute time
//  (kept for backward compatibility; also embedded in MidiEvent)
// ---------------------------------------------------------------------------
struct MidiNote {
    uint64_t absUs;     // absolute playback time in microseconds (from start)
    uint8_t  note;      // MIDI note number 0-127
    uint8_t  channel;   // MIDI channel 0-15
    uint8_t  velocity;  // 0-127  (on: 1-127 / off: 0)
    bool     on;        // true = note-on, false = note-off
};

// ---------------------------------------------------------------------------
//  MidiProgramChange: a GM program-change event at an absolute time
// ---------------------------------------------------------------------------
struct MidiProgramChange {
    uint64_t absUs;   // absolute playback time in microseconds
    uint8_t  channel; // MIDI channel 0-15
    uint8_t  program; // GM program number 0-127
};

// ---------------------------------------------------------------------------
//  MidiEvent — unified timestamped MIDI channel event
//
//  type   MIDI status nibble (high)   data1      data2
//  ----   --------------------------  --------   --------
//  0x8    Note Off                    note       velocity
//  0x9    Note On                     note       velocity
//  0xA    Polyphonic Key Pressure     note       pressure
//  0xB    Control Change              cc#        value
//  0xC    Program Change              program    -
//  0xD    Channel Pressure            pressure   -
//  0xE    Pitch Bend                  lsb        msb (14-bit combined = data1|(data2<<7))
// ---------------------------------------------------------------------------
struct MidiEvent {
    uint64_t absUs;   // absolute time in microseconds
    uint8_t  channel; // 0-15
    uint8_t  type;    // status nibble 0x8..0xE
    uint8_t  data1;
    uint8_t  data2;
};

// ---------------------------------------------------------------------------
//  GM1 instrument names (programs 0-127, channel 9 = Percussion)
// ---------------------------------------------------------------------------
static inline const char* gmInstrumentName(int program)
{
    static const char* const GM[128] = {
        "Acoustic Grand Piano","Bright Acoustic Piano","Electric Grand Piano","Honky-tonk Piano",
        "Electric Piano 1","Electric Piano 2","Harpsichord","Clavinet",
        "Celesta","Glockenspiel","Music Box","Vibraphone",
        "Marimba","Xylophone","Tubular Bells","Dulcimer",
        "Drawbar Organ","Percussive Organ","Rock Organ","Church Organ",
        "Reed Organ","Accordion","Harmonica","Tango Accordion",
        "Acoustic Guitar (nylon)","Acoustic Guitar (steel)","Electric Guitar (jazz)","Electric Guitar (clean)",
        "Electric Guitar (muted)","Overdriven Guitar","Distortion Guitar","Guitar Harmonics",
        "Acoustic Bass","Electric Bass (finger)","Electric Bass (pick)","Fretless Bass",
        "Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2",
        "Violin","Viola","Cello","Contrabass",
        "Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani",
        "String Ensemble 1","String Ensemble 2","Synth Strings 1","Synth Strings 2",
        "Choir Aahs","Voice Oohs","Synth Choir","Orchestra Hit",
        "Trumpet","Trombone","Tuba","Muted Trumpet",
        "French Horn","Brass Section","Synth Brass 1","Synth Brass 2",
        "Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax",
        "Oboe","English Horn","Bassoon","Clarinet",
        "Piccolo","Flute","Recorder","Pan Flute",
        "Blown Bottle","Shakuhachi","Whistle","Ocarina",
        "Lead 1 (square)","Lead 2 (sawtooth)","Lead 3 (calliope)","Lead 4 (chiff)",
        "Lead 5 (charang)","Lead 6 (voice)","Lead 7 (fifths)","Lead 8 (bass+lead)",
        "Pad 1 (new age)","Pad 2 (warm)","Pad 3 (polysynth)","Pad 4 (choir)",
        "Pad 5 (bowed)","Pad 6 (metallic)","Pad 7 (halo)","Pad 8 (sweep)",
        "FX 1 (rain)","FX 2 (soundtrack)","FX 3 (crystal)","FX 4 (atmosphere)",
        "FX 5 (brightness)","FX 6 (goblins)","FX 7 (echoes)","FX 8 (sci-fi)",
        "Sitar","Banjo","Shamisen","Koto",
        "Kalimba","Bag pipe","Fiddle","Shanai",
        "Tinkle Bell","Agogo","Steel Drums","Woodblock",
        "Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal",
        "Guitar Fret Noise","Breath Noise","Seashore","Bird Tweet",
        "Telephone Ring","Helicopter","Applause","Gunshot"
    };
    if (program < 0 || program > 127) return "Unknown";
    return GM[program];
}

// ---------------------------------------------------------------------------
//  MidiFile: result returned by loadMidiFile()
// ---------------------------------------------------------------------------
struct MidiFile {
    bool                  valid   = false;
    uint16_t              ppq     = 480;    // ticks per quarter note
    uint32_t              durationUs = 0;   // total duration in microseconds
    std::vector<MidiNote> notes;            // sorted ascending by absUs
    std::string           error;

    // Per-channel first/last program (snapshot for display and initial setup)
    int8_t  channelProgram[16];  // GM program number 0-127 per channel
    bool    channelUsed[16];     // true if any note was seen on this channel

    // All program-change events with precise timing (sorted by absUs).
    // Use these to apply mid-song instrument switches during playback.
    std::vector<MidiProgramChange> programChanges;

    // All channel events (note on/off, CC, pitch bend, aftertouch, program
    // change) sorted by absUs.  The playback thread iterates this single list.
    std::vector<MidiEvent> events;

    // Diagnostic messages populated during parsing (informational & warnings).
    // Always filled; use as a post-mortem when events/notes come up empty.
    std::vector<std::string> diagnostics;

    // Detected MIDI standard, e.g. "General MIDI", "Roland GS", "Yamaha XG",
    // "Roland MT-32", "General MIDI 2", or "Unknown" when no SysEx reset found.
    std::string standard;

    MidiFile() {
        for (int i = 0; i < 16; ++i) { channelProgram[i] = 0; channelUsed[i] = false; }
    }
};

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------
namespace midi_detail {

static inline uint16_t u16be(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
static inline uint32_t u32be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

// Decode variable-length quantity and advance p.
static inline uint32_t vlq(const uint8_t*& p, const uint8_t* end) {
    uint32_t val = 0;
    for (int i = 0; i < 4 && p < end; ++i) {
        uint8_t b = *p++;
        val = (val << 7) | (b & 0x7Fu);
        if (!(b & 0x80u)) break;
    }
    return val;
}

using TempoMap = std::vector<std::pair<uint64_t, uint32_t>>; // (absTick, us/beat)

// Parse one MTrk chunk.
// tempoOnly=true  → extract only tempo meta-events  (used for format-1 track 0)
// tempoOnly=false → extract all channel events
// sysexOut        → if non-null, all raw SysEx payloads are appended (F0 byte included)
// metaTextOut     → if non-null, meta types 0x01-0x09 text strings are appended
static void parseTrack(const uint8_t* data, uint32_t len,
                       TempoMap&              tempoMap,
                       std::vector<MidiNote>& noteOut,   // kept for channelUsed scan
                       bool                   tempoOnly,
                       int8_t                 channelProgram[16],
                       std::vector<MidiProgramChange>* pcOut = nullptr,
                       std::vector<MidiEvent>*         evOut = nullptr,
                       std::vector<std::vector<uint8_t>>* sysexOut  = nullptr,
                       std::vector<std::string>*          metaTextOut = nullptr)
{
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    uint64_t absTick       = 0;
    uint8_t  runningStatus = 0;

    while (p < end) {
        // delta time
        uint32_t delta = vlq(p, end);
        if (p >= end) break;
        absTick += delta;

        uint8_t b = *p;

        // --- SysEx ---
        if (b == 0xF0u || b == 0xF7u) {
            const bool isF0 = (b == 0xF0u);
            ++p;
            uint32_t slen = vlq(p, end);
            if (p + slen > end) break;
            if (sysexOut && isF0) {
                // Store F0 + payload (without the trailing F7 if present)
                std::vector<uint8_t> sx;
                sx.reserve(slen + 1);
                sx.push_back(0xF0u);
                sx.insert(sx.end(), p, p + slen);
                sysexOut->push_back(std::move(sx));
            }
            p += slen;
            runningStatus = 0;
            continue;
        }

        // --- Meta event ---
        if (b == 0xFFu) {
            ++p;
            if (p + 1 > end) break;
            uint8_t metaType = *p++;
            uint32_t mlen = vlq(p, end);
            if (p + mlen > end) break;
            const uint8_t* md = p;
            p += mlen;

            if (metaType == 0x51u && mlen >= 3) {   // Set Tempo
                uint32_t us = (uint32_t(md[0]) << 16)
                            | (uint32_t(md[1]) <<  8)
                            |  uint32_t(md[2]);
                if (us == 0) us = 500000;
                tempoMap.push_back({absTick, us});
            }
            // Meta text events: Text(01), CopyrightNotice(02), TrackName(03),
            // InstrumentName(04), Lyric(05), Marker(06), CuePoint(07),
            // ProgramName(08), DeviceName(09)
            if (metaTextOut && mlen > 0 &&
                metaType >= 0x01u && metaType <= 0x09u) {
                std::string txt(reinterpret_cast<const char*>(md), mlen);
                // Strip non-printable bytes
                for (auto& c : txt) if ((unsigned char)c < 0x20u) c = ' ';
                metaTextOut->push_back(txt);
            }
            runningStatus = 0;
            continue;
        }

        // --- Channel event ---
        uint8_t status;
        if (b & 0x80u) {
            status        = b;
            runningStatus = b;
            ++p;
        } else {
            status = runningStatus;   // running status
        }
        if (!status) { if (p < end) ++p; continue; }

        uint8_t type    = (status >> 4) & 0x0Fu;
        uint8_t channel = status & 0x0Fu;

        // Data byte counts for types 0x8..0xE
        static const int SZ[7] = {2, 2, 2, 2, 1, 1, 2};
        if (type < 8 || type > 14) continue;
        int nb = SZ[type - 8];
        if (p + nb > end) break;

        if (tempoOnly) { p += nb; continue; }

        // Emit a unified MidiEvent for every channel message
        if (evOut) {
            MidiEvent ev;
            ev.absUs   = absTick;   // tick; converted to µs after parsing
            ev.channel = channel;
            ev.type    = type;
            ev.data1   = (nb >= 1) ? p[0] : 0;
            ev.data2   = (nb >= 2) ? p[1] : 0;
            evOut->push_back(ev);
        }

        if (type == 0x8u || type == 0x9u) {   // Note Off / Note On
            uint8_t note = p[0];
            uint8_t vel  = p[1];
            MidiNote mn;
            mn.absUs    = absTick;    // tick value — converted to µs later
            mn.note     = note;
            mn.channel  = channel;
            mn.velocity = vel;
            mn.on       = (type == 0x9u && vel > 0);
            noteOut.push_back(mn);
        } else if (type == 0xCu) {   // Program Change
            uint8_t prog = p[0] & 0x7Fu;
            if (channelProgram) channelProgram[channel] = static_cast<int8_t>(prog);
            if (pcOut) pcOut->push_back({absTick, channel, prog});
        }
        p += nb;
    }
}

} // namespace midi_detail

// ---------------------------------------------------------------------------
//  loadMidiFile — public API
// ---------------------------------------------------------------------------
inline MidiFile loadMidiFile(const std::string& path)
{
    MidiFile result;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        result.error = "Cannot open: " + path;
        return result;
    }
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    if (!f.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(sz))) {
        result.error = "Read error";
        return result;
    }

    const uint8_t* p   = buf.data();
    const uint8_t* end = p + sz;

    // MThd header
    if (sz < 14 || std::memcmp(p, "MThd", 4) != 0) {
        result.error = "Not a MIDI file (no MThd)";
        return result;
    }
    uint32_t hdrLen  = midi_detail::u32be(p + 4);
    uint16_t fmt     = midi_detail::u16be(p + 8);
    uint16_t nTracks = midi_detail::u16be(p + 10);
    uint16_t timing  = midi_detail::u16be(p + 12);

    if (timing & 0x8000u) {
        // SMPTE: upper byte = -fps (negative), lower byte = subdivisions
        const uint8_t fps_raw = static_cast<uint8_t>((timing >> 8) & 0x7Fu);
        const uint8_t subdiv  = static_cast<uint8_t>(timing & 0xFFu);
        int fps = 0;
        switch (fps_raw) {
            case 24: fps = 24; break;
            case 25: fps = 25; break;
            case 29: fps = 30; break;  // 29.97 drop-frame
            case 30: fps = 30; break;
            default: fps = fps_raw;
        }
        char smptebuf[128];
        std::snprintf(smptebuf, sizeof(smptebuf),
            "SMPTE timing not supported: %d fps, %d subframes/frame", fps, subdiv);
        result.error = smptebuf;
        result.diagnostics.push_back(std::string("Timing: SMPTE ") + smptebuf);
        result.diagnostics.push_back(
            "Fix: this file uses SMPTE time-code timing (frame-based) instead of"
            " PPQ (tick-based). The player only supports PPQ timing.");
        return result;
    }
    result.ppq = timing ? timing : 480;

    // Record header info as diagnostics
    {
        const char* fmtName = (fmt == 0) ? "0 (single-track)" :
                              (fmt == 1) ? "1 (multi-track sync)" :
                              (fmt == 2) ? "2 (multi-track async)" : "unknown";
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "Header: format %s, %u tracks declared, %u PPQ",
            fmtName, nTracks, result.ppq);
        result.diagnostics.push_back(buf);
    }

    p += 8 + hdrLen;

    // Collect MTrk spans — scan entire file; silently skip unknown chunk types.
    // Many real-world MIDI files embed non-MTrk chunks (DISP, LIST, XMID, CASM,
    // RIFF sub-chunks, etc.).  The old code broke on the first unknown chunk,
    // leaving all following note tracks unread → silent playback, no error.
    struct TrackSpan { const uint8_t* data; uint32_t len; };
    std::vector<TrackSpan> tracks;
    std::vector<std::string> unknownChunks;
    tracks.reserve(nTracks);
    while (p + 8 <= end) {
        const uint32_t chunkLen = midi_detail::u32be(p + 4);
        if (p + 8 + chunkLen > end) break;   // chunk overflows file
        if (std::memcmp(p, "MTrk", 4) == 0) {
            tracks.push_back({p + 8, chunkLen});
        } else {
            // Record unrecognised chunk IDs for diagnostics
            char id[5] = {};
            for (int i = 0; i < 4; ++i)
                id[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '?';
            unknownChunks.push_back(id);
        }
        p += 8 + chunkLen;
    }
    if (tracks.empty()) {
        result.error = "No valid MTrk chunks found";
        if (!unknownChunks.empty()) {
            std::string ids;
            for (const auto& s : unknownChunks) ids += "'" + s + "' ";
            result.diagnostics.push_back("Non-MTrk chunks found (skipped): " + ids);
            result.diagnostics.push_back(
                "File may use a proprietary container (XMID, RMID, etc.) "
                "that is not a standard SMF.");
        }
        return result;
    }

    // Log chunk summary
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "Chunks: %zu MTrk found (header declared %u)",
            tracks.size(), nTracks);
        result.diagnostics.push_back(buf);
        if (!unknownChunks.empty()) {
            std::string ids;
            for (const auto& s : unknownChunks) ids += "'" + s + "' ";
            result.diagnostics.push_back("Non-MTrk chunks skipped: " + ids);
        }
        if ((size_t)nTracks != tracks.size()) {
            char wb[128];
            std::snprintf(wb, sizeof(wb),
                "WARNING: header says %u tracks but %zu MTrk chunks were found",
                nTracks, tracks.size());
            result.diagnostics.push_back(wb);
        }
    }

    // Build tempo map + raw note list (absUs field holds raw tick at this stage)
    midi_detail::TempoMap tempoMap;
    tempoMap.push_back({0u, 500000u});   // default: 120 BPM

    std::vector<MidiNote> raw;

    std::vector<std::vector<uint8_t>> sysexCollector;
    std::vector<std::string>          metaTextCollector;

    if (fmt == 0) {
        midi_detail::parseTrack(tracks[0].data, tracks[0].len,
                                tempoMap, raw, false, result.channelProgram,
                                &result.programChanges, &result.events,
                                &sysexCollector, &metaTextCollector);
    } else if (fmt == 1) {
        // Track 0 = tempo map only; tracks 1+ = note + program data
        midi_detail::parseTrack(tracks[0].data, tracks[0].len,
                                tempoMap, raw, true, result.channelProgram,
                                nullptr, nullptr,
                                &sysexCollector, &metaTextCollector);
        if (tracks.size() == 1) {
            // Only the tempo track exists — no note tracks at all.
            // Some tools save Format 1 with a single combined track (should be
            // Format 0). Try re-parsing track 0 as a full event track.
            result.diagnostics.push_back(
                "WARNING: Format 1 with only 1 MTrk chunk found. "
                "Track 0 is normally tempo-only in Format 1; there are no note tracks.");
            result.diagnostics.push_back(
                "Attempting fallback: re-parsing track 0 as a full event track (like Format 0).");
            result.events.clear();
            result.programChanges.clear();
            raw.clear();
            midi_detail::parseTrack(tracks[0].data, tracks[0].len,
                                    tempoMap, raw, false, result.channelProgram,
                                    &result.programChanges, &result.events,
                                    &sysexCollector, &metaTextCollector);
            if (!result.events.empty())
                result.diagnostics.push_back(
                    "Fallback succeeded: events recovered from track 0.");
            else
                result.diagnostics.push_back(
                    "Fallback failed: track 0 contains no channel events either.");
        } else {
            for (size_t t = 1; t < tracks.size(); ++t)
                midi_detail::parseTrack(tracks[t].data, tracks[t].len,
                                        tempoMap, raw, false, result.channelProgram,
                                        &result.programChanges, &result.events,
                                        &sysexCollector, &metaTextCollector);
        }
    } else if (fmt == 2) {
        // Format 2: each track is independent — merge all
        for (auto& tr : tracks)
            midi_detail::parseTrack(tr.data, tr.len, tempoMap, raw, false,
                                    result.channelProgram, &result.programChanges,
                                    &result.events, &sysexCollector, &metaTextCollector);
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "WARNING: Unknown MIDI format %u — attempting to parse all tracks as Format 0.",
            fmt);
        result.diagnostics.push_back(buf);
        for (auto& tr : tracks)
            midi_detail::parseTrack(tr.data, tr.len, tempoMap, raw, false,
                                    result.channelProgram, &result.programChanges,
                                    &result.events, &sysexCollector, &metaTextCollector);
    }

    // ------------------------------------------------------------------
    // Detect MIDI standard from SysEx reset messages
    // ------------------------------------------------------------------
    result.standard = "Unknown";
    for (const auto& sx : sysexCollector) {
        const size_t n = sx.size();
        if (n < 5) continue;
        // GM2 Reset:  F0 7E ?? 09 03
        if (sx[0]==0xF0u && sx[1]==0x7Eu && sx[3]==0x09u && sx[4]==0x03u)
            { result.standard = "General MIDI 2"; break; }
        // GM Reset:   F0 7E ?? 09 01
        if (sx[0]==0xF0u && sx[1]==0x7Eu && sx[3]==0x09u && sx[4]==0x01u)
            { result.standard = "General MIDI"; continue; }  // keep scanning for GS/XG
        // Roland MT-32: F0 41 ?? 16 12
        if (n >= 5 && sx[0]==0xF0u && sx[1]==0x41u && sx[3]==0x16u && sx[4]==0x12u)
            { result.standard = "Roland MT-32"; break; }
        // Roland GS:  F0 41 ?? 42 12 40 00 7F
        if (n >= 8 && sx[0]==0xF0u && sx[1]==0x41u && sx[3]==0x42u && sx[4]==0x12u
                   && sx[5]==0x40u && sx[6]==0x00u && sx[7]==0x7Fu)
            { result.standard = "Roland GS"; break; }
        // Yamaha XG:  F0 43 ?? 4C 00 00 7E 00
        if (n >= 8 && sx[0]==0xF0u && sx[1]==0x43u && sx[3]==0x4Cu && sx[4]==0x00u
                   && sx[5]==0x00u && sx[6]==0x7Eu && sx[7]==0x00u)
            { result.standard = "Yamaha XG"; break; }
    }
    // Fallback: scan meta text for hints if SysEx gave nothing
    if (result.standard == "Unknown") {
        for (const auto& txt : metaTextCollector) {
            std::string lo = txt;
            for (auto& c : lo) c = (char)std::tolower((unsigned char)c);
            if (lo.find("roland") != std::string::npos || lo.find(" gs") != std::string::npos)
                { result.standard = "Roland GS (text hint)"; break; }
            if (lo.find("yamaha") != std::string::npos || lo.find(" xg") != std::string::npos)
                { result.standard = "Yamaha XG (text hint)"; break; }
            if (lo.find("mt-32") != std::string::npos || lo.find("mt32") != std::string::npos)
                { result.standard = "Roland MT-32 (text hint)"; break; }
            if (lo.find("gm2") != std::string::npos || lo.find("general midi 2") != std::string::npos)
                { result.standard = "General MIDI 2 (text hint)"; break; }
            if (lo.find("general midi") != std::string::npos)
                { result.standard = "General MIDI (text hint)"; break; }
        }
    }
    result.diagnostics.push_back("MIDI Standard: " + result.standard);

    // Sort tempo map by tick (stable to preserve first-defined order)
    std::stable_sort(tempoMap.begin(), tempoMap.end(),
                     [](const auto& a, const auto& b){ return a.first < b.first; });

    // Convert raw tick → absolute microseconds
    auto ticksToUs = [&](uint64_t absTick) -> uint64_t {
        uint64_t us        = 0;
        uint64_t prevTk    = 0;
        uint32_t prevTempo = 500000;
        for (const auto& te : tempoMap) {
            if (te.first >= absTick) break;
            us       += (te.first - prevTk) * prevTempo / result.ppq;
            prevTk    = te.first;
            prevTempo = te.second;
        }
        us += (absTick - prevTk) * prevTempo / result.ppq;
        return us;
    };

    for (auto& mn : raw)
        mn.absUs = ticksToUs(mn.absUs);

    for (auto& pc : result.programChanges)
        pc.absUs = ticksToUs(pc.absUs);

    for (auto& ev : result.events)
        ev.absUs = ticksToUs(ev.absUs);

    // Sort by absolute microseconds
    std::stable_sort(raw.begin(), raw.end(),
                     [](const MidiNote& a, const MidiNote& b){
                         return a.absUs < b.absUs;
                     });

    std::stable_sort(result.programChanges.begin(), result.programChanges.end(),
                     [](const MidiProgramChange& a, const MidiProgramChange& b){
                         return a.absUs < b.absUs;
                     });

    std::stable_sort(result.events.begin(), result.events.end(),
                     [](const MidiEvent& a, const MidiEvent& b){
                         return a.absUs < b.absUs;
                     });

    result.notes = std::move(raw);
    // Duration in microseconds (+ 1 s tail so progress bar reaches 100 %).
    // Use the last event time; it covers CCs/pitch-bends that outlast the last note.
    // Bug was: divided by 1 000 000 before storing → stored seconds, not µs.
    if (!result.events.empty())
        result.durationUs = static_cast<uint32_t>(
            std::min<uint64_t>(result.events.back().absUs + 1'000'000u, 0xFFFFFFFFu));
    else if (!result.notes.empty())
        result.durationUs = static_cast<uint32_t>(
            std::min<uint64_t>(result.notes.back().absUs + 1'000'000u, 0xFFFFFFFFu));

    // Mark which channels have actual notes
    for (const auto& mn : result.notes)
        result.channelUsed[mn.channel] = true;

    // Final parse summary diagnostics
    {
        // Count event types
        int nNoteOn=0, nNoteOff=0, nCC=0, nPC=0, nPB=0, nAT=0, nOther=0;
        for (const auto& ev : result.events) {
            switch (ev.type) {
            case 0x9: if (ev.data2 > 0) ++nNoteOn;  else ++nNoteOff; break;
            case 0x8: ++nNoteOff; break;
            case 0xB: ++nCC;  break;
            case 0xC: ++nPC;  break;
            case 0xE: ++nPB;  break;
            case 0xA: case 0xD: ++nAT; break;
            default:  ++nOther; break;
            }
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "Events: %zu total  (NoteOn:%d NoteOff:%d CC:%d ProgChg:%d PitchBend:%d Aftertouch:%d Other:%d)",
            result.events.size(), nNoteOn, nNoteOff, nCC, nPC, nPB, nAT, nOther);
        result.diagnostics.push_back(buf);

        // Channels used
        std::string chStr;
        for (int ch = 0; ch < 16; ++ch) {
            if (result.channelUsed[ch]) {
                char cbuf[32];
                std::snprintf(cbuf, sizeof(cbuf), "ch%d(prg%d) ", ch + 1,
                              (int)(uint8_t)result.channelProgram[ch]);
                chStr += cbuf;
            }
        }
        if (!chStr.empty())
            result.diagnostics.push_back("Channels with notes: " + chStr);

        // Tempo map summary
        std::snprintf(buf, sizeof(buf),
            "Tempo map: %zu change(s)", tempoMap.size());
        result.diagnostics.push_back(buf);

        if (result.events.empty()) {
            result.diagnostics.push_back(
                "PROBLEM: 0 channel events parsed. Possible causes:");
            result.diagnostics.push_back(
                "  1) Format 1 + single MTrk: only a tempo track, no note tracks.");
            result.diagnostics.push_back(
                "  2) All events are SysEx or Meta (text, markers) — no Note On/Off.");
            result.diagnostics.push_back(
                "  3) Running status broken: status byte > 0x7F missing in data stream.");
            result.diagnostics.push_back(
                "  4) File uses non-standard chunk layout (RMID/XMF wrapper).");
        }
    }

    result.valid = true;
    return result;
}
