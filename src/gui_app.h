#pragma once
// =============================================================================
//  gui_app.h  —  Dear ImGui UI for the 432 Hz Bit-Perfect Synthesiser
//
//  INCLUSION MODEL
//  ---------------
//  This header is a "late include" — it must be included from main.cpp AFTER:
//    • All type definitions  (AudioState, DevRec, InstrModel, ...)
//    • All global variable definitions
//    • All helper-function definitions  (stopMidiThread, etc.)
//  The GuiApp class methods reference those symbols directly (same TU).
//
//  This header must NOT be included from any other translation unit.
//
//  Windows
//  -------
//   Transport   – Now-playing, progress, transport controls, VU meter
//   MIDI        – Folder browser + file list (double-click to play)
//   SF2         – Folder browser + file list (double-click to load)
//   Presets     – Preset list for the loaded SF2
//   Instrument  – Timbre selector + synthesis model info
//   Tuning      – A4 Hz, scale mode, comparison table
//   Reverb      – Named presets + fine-control sliders
//   Devices     – Audio device table + live device switch
//   Keyboard    – On-screen piano keyboard (2 octaves) for interactive notes
//   About       – Version, credits, keyboard shortcuts
// =============================================================================

// --- These headers are already included by main.cpp before us,
//     but we re-include for standalone header hygiene (guarded by #pragma once). ---
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "debug_console.h"   // AppLog*, g_log_entries, LogLevel

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>       // SHGetKnownFolderPath
#include <shobjidl_core.h>

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
namespace gui_detail {

static std::string FormatTime(uint64_t us)
{
    const uint32_t total_s = static_cast<uint32_t>(us / 1'000'000u);
    const uint32_t m = total_s / 60u;
    const uint32_t s = total_s % 60u;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u:%02u", m, s);
    return buf;
}

static float LinearToDb(float linear)
{
    if (linear < 1e-5f) return -100.f;
    return 20.f * std::log10(linear);
}

// Native Win32 folder picker (IFileOpenDialog, Vista+)
static std::string BrowseFolder(HWND hwnd, const wchar_t* title)
{
    std::string result;
    IFileOpenDialog* pfd = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (FAILED(hr)) return result;

    DWORD dwOpts = 0;
    pfd->GetOptions(&dwOpts);
    pfd->SetOptions(dwOpts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (title) pfd->SetTitle(title);

    if (SUCCEEDED(pfd->Show(hwnd))) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR pszPath = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                const int sz = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                                   nullptr, 0, nullptr, nullptr);
                if (sz > 1) {
                    result.resize(static_cast<size_t>(sz) - 1);
                    WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                       result.data(), sz, nullptr, nullptr);
                }
                CoTaskMemFree(pszPath);
            }
            psi->Release();
        }
    }
    pfd->Release();
    return result;
}

// Scan a folder for files with the given extension (case-insensitive).
static void ScanFolder(const std::string& folder,
                       std::initializer_list<const char*> exts,
                       std::vector<std::pair<std::string, std::string>>& out)
{
    out.clear();
    if (folder.empty()) return;
    namespace fs = std::filesystem;
    try {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            std::string extLow = ext;
            for (char& c : extLow) c = static_cast<char>(std::tolower(c));
            for (const char* want : exts) {
                if (extLow == want) {
                    out.push_back({
                        entry.path().filename().string(),
                        entry.path().string()
                    });
                    break;
                }
            }
        }
        std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
    } catch (...) {}
}

// Decay a VU peak value with a given half-life per frame.
static float DecayPeak(float peak, float dt, float halftime_s = 0.4f)
{
    const float k = std::pow(0.5f, dt / halftime_s);
    return peak * k;
}

// Render a horizontal dB bar using ImGui draw list.
static void DrawDbBar(const char* label, float linear,
                      float bar_w = 0.f, float bar_h = 14.f)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float avail = bar_w > 0.f ? bar_w : ImGui::GetContentRegionAvail().x;

    // background
    dl->AddRectFilled(p, {p.x + avail, p.y + bar_h}, IM_COL32(30, 30, 30, 255), 2.f);

    // colour zones: green 0–70 %, yellow 70–90 %, red 90–100 %
    const float frac = linear / 1.4f; // 1.4 = clipping level
    const float clamped = std::min(frac, 1.f);
    const float w_total = avail * clamped;
    const float w_g = avail * std::min(clamped, 0.70f);
    const float w_y = avail * std::min(std::max(clamped - 0.70f, 0.f), 0.20f);
    const float w_r = avail * std::max(clamped - 0.90f, 0.f);

    if (w_g > 0) dl->AddRectFilled(p, {p.x + w_g, p.y + bar_h}, IM_COL32(50, 200, 80, 255), 2.f);
    if (w_y > 0) dl->AddRectFilled({p.x + w_g, p.y}, {p.x + w_g + w_y, p.y + bar_h}, IM_COL32(240, 200, 50, 255));
    if (w_r > 0) dl->AddRectFilled({p.x + w_g + w_y, p.y}, {p.x + w_g + w_y + w_r, p.y + bar_h}, IM_COL32(230, 60, 60, 255));

    // label on the right
    const float db = LinearToDb(linear);
    char dbstr[16];
    if (db <= -100.f) std::snprintf(dbstr, sizeof(dbstr), " -inf");
    else              std::snprintf(dbstr, sizeof(dbstr), "%5.1f dB", static_cast<double>(db));

    dl->AddText({p.x + avail - 64.f, p.y + 1.f}, IM_COL32(220, 220, 220, 255), dbstr);
    ImGui::Dummy({avail, bar_h});

    ImGui::SameLine(0, 4.f);
    ImGui::TextDisabled("%s", label);
}

} // namespace gui_detail

// ===========================================================================
//  GuiApp
// ===========================================================================
class GuiApp {
public:
    // Per-file info stored in browsers
    using FileEntry = std::pair<std::string, std::string>; // {filename, fullpath}

    // -----------------------------------------------------------------------
    //  State  (persisted across frames)
    // -----------------------------------------------------------------------
    struct State {
        // --- MIDI browser --------------------------------------------------
        char   midi_folder[512]{};
        std::vector<FileEntry> midi_files;
        int    midi_selected   = -1;
        char   midi_filter[128]{};
        bool   midi_folder_changed = false;

        // --- SF2 browser ---------------------------------------------------
        char   sf2_folder[512]{};
        std::vector<FileEntry> sf2_files;
        int    sf2_selected    = -1;
        char   sf2_filter[128]{};
        bool   sf2_folder_changed = false;

        // --- SF2 preset browser --------------------------------------------
        std::vector<std::string> sf2_presets;
        int    preset_selected = -1;

        // --- VU meter -------------------------------------------------------
        float  vu_bar_l       = 0.f, vu_bar_r       = 0.f; // decaying bars
        float  vu_hold_l      = 0.f, vu_hold_r      = 0.f; // peak-hold
        float  vu_hold_ttl_l  = 0.f, vu_hold_ttl_r  = 0.f; // hold timer (s)

        // --- On-screen keyboard --------------------------------------------
        bool   kb_active[128]{};
        float  kb_velocity    = 0.8f;
        int    kb_octave      = 4; // centre octave (C4)

        // --- Window visibility ---------------------------------------------
        bool show_transport       = true;
        bool show_midi            = true;
        bool show_sf2             = true;
        bool show_presets         = true;
        bool show_instrument      = true;
        bool show_tuning          = true;
        bool show_reverb          = true;
        bool show_devices         = false;
        bool show_keyboard        = false;
        bool show_about           = false;
        bool show_midi_instruments = false;

        bool show_spatial_mixer    = false;
        bool show_debug_console    = false;
        bool show_midiout_devices  = false;

        // --- Seek slider state ---------------------------------------------
        uint64_t seek_drag_val  = 0;
        bool     seek_dragging  = false;

        // --- Per-channel VU display (decayed copy of g_ch_vu) --------------
        float    mix_vu[16]{};

        // --- Devices panel -------------------------------------------------
        int  dev_selected     = -1;

        // --- Docking layout initialised? -----------------------------------
        bool docking_init     = false;

        // --- Tuning comparison toggle -------------------------------------
        bool tuning_show_table = false;

        // --- Persisted main window placement --------------------------------
        int  main_x         = 100;
        int  main_y         = 100;
        int  main_w         = 1340;
        int  main_h         = 800;
        bool main_maximized = false;

        // --- Persisted SF2 path for auto-load on startup -------------------
        char last_sf2_path[512]{};
        char last_midiout_device[256]{};  // persisted MIDI device name

        // --- Spatial Mixer -------------------------------------------------
        int         selected_channel       = -1;   // channel selected for spatial positioning
        std::string last_loaded_mixer_song;         // song whose mixer was last loaded
    };

    State st;
    HWND  m_hwnd = nullptr;   // set on first Render(); used by SaveSettings

    // -----------------------------------------------------------------------
    GuiApp()
    {
        // Initialize folder defaults to current working directory
        namespace fs = std::filesystem;
        const std::string cwd = fs::current_path().string();
        std::strncpy(st.midi_folder, cwd.c_str(), sizeof(st.midi_folder) - 1);
        std::strncpy(st.sf2_folder,  cwd.c_str(), sizeof(st.sf2_folder)  - 1);

        // Restore last-used folders; fall back to cwd if they no longer exist
        LoadSettings();
        if (!fs::exists(st.midi_folder))
            std::strncpy(st.midi_folder, cwd.c_str(), sizeof(st.midi_folder) - 1);
        if (!fs::exists(st.sf2_folder))
            std::strncpy(st.sf2_folder,  cwd.c_str(), sizeof(st.sf2_folder)  - 1);

        st.midi_folder_changed = true;
        st.sf2_folder_changed  = true;
    }

    // -----------------------------------------------------------------------
    //  Render — top-level frame call
    // -----------------------------------------------------------------------

    // Returns a bitmask of the current window-visibility flags for change detection.
    uint32_t ShowStateBits() const
    {
        uint32_t b = 0;
        if (st.show_transport)          b |= (1u <<  0);
        if (st.show_midi)               b |= (1u <<  1);
        if (st.show_sf2)                b |= (1u <<  2);
        if (st.show_presets)            b |= (1u <<  3);
        if (st.show_instrument)         b |= (1u <<  4);
        if (st.show_tuning)             b |= (1u <<  5);
        if (st.show_reverb)             b |= (1u <<  6);
        if (st.show_devices)            b |= (1u <<  7);
        if (st.show_keyboard)           b |= (1u <<  8);
        if (st.show_about)              b |= (1u <<  9);
        if (st.show_midi_instruments)   b |= (1u << 10);
        if (st.show_spatial_mixer)      b |= (1u << 11);
        if (st.show_debug_console)      b |= (1u << 12);
        if (st.show_midiout_devices)    b |= (1u << 13);
        return b;
    }

    // Apply the main Win32 window placement loaded from settings.
    // Call this once after constructing GuiApp, before entering the message loop.
    void ApplyMainWindowPlacement(HWND hwnd)
    {
        if (st.main_w <= 0 || st.main_h <= 0) return;
        // Use SetWindowPlacement so rcNormalPosition (the restore-rect when
        // the window is later maximized) is set correctly.
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        ::GetWindowPlacement(hwnd, &wp);          // read current flags
        wp.showCmd = SW_HIDE;                     // caller does ShowWindow
        wp.rcNormalPosition.left   = st.main_x;
        wp.rcNormalPosition.top    = st.main_y;
        wp.rcNormalPosition.right  = st.main_x + st.main_w;
        wp.rcNormalPosition.bottom = st.main_y + st.main_h;
        ::SetWindowPlacement(hwnd, &wp);
    }

    void Render(HWND hwnd)
    {
        m_hwnd = hwnd;

        // Cache main window placement into st.* every frame so that
        // SaveSettings() can write the correct values even after the HWND
        // has been destroyed (WM_DESTROY fires before Shutdown() is called).
        {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (::GetWindowPlacement(hwnd, &wp) && wp.showCmd != SW_SHOWMINIMIZED) {
                const RECT& rc = wp.rcNormalPosition;
                st.main_x         = rc.left;
                st.main_y         = rc.top;
                st.main_w         = rc.right  - rc.left;
                st.main_h         = rc.bottom - rc.top;
                st.main_maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
            }
        }

        const float dt = ImGui::GetIO().DeltaTime;
        UpdateVU(dt);
        SyncSf2Presets();

        if (st.midi_folder_changed) { ScanMidi(); SaveSettings(); st.midi_folder_changed = false; }
        if (st.sf2_folder_changed)  { ScanSf2();  SaveSettings(); st.sf2_folder_changed  = false; }

        const uint32_t showBefore = ShowStateBits();

        // Full-screen dockspace
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                     ImGuiDockNodeFlags_PassthruCentralNode);

        // Song-change detection: load saved mixer when a new MIDI file starts
        {
            std::string nowPlaying;
            {
                std::lock_guard<std::mutex> lk(g_now_playing_mutex);
                nowPlaying = g_now_playing_name;
            }
            if (nowPlaying != st.last_loaded_mixer_song) {
                st.last_loaded_mixer_song = nowPlaying;
                LoadMixerForSong(nowPlaying);
            }
        }

        RenderMenuBar(hwnd);

        if (st.show_transport)        RenderTransport();
        if (st.show_midi)             RenderMidiBrowser(hwnd);
        if (st.show_sf2)              RenderSf2Browser(hwnd);
        if (st.show_presets)          RenderPresets();
        if (st.show_instrument)       RenderInstrument();
        if (st.show_tuning)           RenderTuning();
        if (st.show_reverb)           RenderReverb();
        if (st.show_devices)          RenderDevices();
        if (st.show_keyboard)         RenderKeyboard();
        if (st.show_midi_instruments) RenderMidiInstruments();
        if (st.show_spatial_mixer)    RenderSpatialMixer();
        if (st.show_debug_console)    RenderDebugConsole();
        if (st.show_midiout_devices)  RenderMidiOutDevices();
        if (st.show_about)            RenderAbout();

        if (ShowStateBits() != showBefore) SaveSettings();
    }

    // Called by main() at shutdown to write the final settings (incl. window placement).
    void Shutdown() { SaveSettings(); }

private:
    // =======================================================================
    //  UPDATE HELPERS
    // =======================================================================

    void UpdateVU(float dt)
    {
        // Atomic swap: grab peak since last frame
        const float rawL = g_vu_peak_l.exchange(0.f, std::memory_order_relaxed);
        const float rawR = g_vu_peak_r.exchange(0.f, std::memory_order_relaxed);

        // Fast attack, smooth decay for the bar
        st.vu_bar_l = (rawL > st.vu_bar_l) ? rawL : gui_detail::DecayPeak(st.vu_bar_l, dt, 0.25f);
        st.vu_bar_r = (rawR > st.vu_bar_r) ? rawR : gui_detail::DecayPeak(st.vu_bar_r, dt, 0.25f);

        // Peak hold: 1.5 s then slow fallback
        if (rawL >= st.vu_hold_l) { st.vu_hold_l = rawL; st.vu_hold_ttl_l = 1.5f; }
        else if (st.vu_hold_ttl_l > 0.f) { st.vu_hold_ttl_l -= dt; }
        else                              { st.vu_hold_l = gui_detail::DecayPeak(st.vu_hold_l, dt, 1.0f); }

        if (rawR >= st.vu_hold_r) { st.vu_hold_r = rawR; st.vu_hold_ttl_r = 1.5f; }
        else if (st.vu_hold_ttl_r > 0.f) { st.vu_hold_ttl_r -= dt; }
        else                              { st.vu_hold_r = gui_detail::DecayPeak(st.vu_hold_r, dt, 1.0f); }

        // Per-channel VU: fast attack from MIDI thread, 0.4 s decay
        for (int i = 0; i < 16; ++i) {
            const float raw = g_ch_vu[i].exchange(0.f, std::memory_order_relaxed);
            st.mix_vu[i] = (raw > st.mix_vu[i]) ? raw
                         : gui_detail::DecayPeak(st.mix_vu[i], dt, 0.4f);
        }
    }

    void ScanMidi()
    {
        gui_detail::ScanFolder(st.midi_folder, {".mid", ".midi"}, st.midi_files);
        st.midi_selected = -1;
    }

    void ScanSf2()
    {
        gui_detail::ScanFolder(st.sf2_folder, {".sf2", ".sfz"}, st.sf2_files);
        st.sf2_selected = -1;
    }

    // -----------------------------------------------------------------------
    //  Persistent settings  (%APPDATA%\432HzPlayer\settings.ini)
    // -----------------------------------------------------------------------
    static std::string SettingsPath()
    {
        PWSTR pAppData = nullptr;
        std::string dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,
                                            KF_FLAG_CREATE, nullptr, &pAppData))) {
            const int sz = WideCharToMultiByte(CP_UTF8, 0, pAppData, -1,
                                               nullptr, 0, nullptr, nullptr);
            if (sz > 1) {
                dir.resize(static_cast<size_t>(sz) - 1);
                WideCharToMultiByte(CP_UTF8, 0, pAppData, -1,
                                    dir.data(), sz, nullptr, nullptr);
            }
            CoTaskMemFree(pAppData);
        }
        if (!dir.empty()) {
            dir += "\\432HzPlayer";
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir + "\\settings.ini";
        }
        // Fallback: next to the executable
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string p(exePath);
        const auto pos = p.rfind('\\');
        if (pos != std::string::npos) p = p.substr(0, pos);
        return p + "\\432hz_settings.ini";
    }

    void LoadSettings()
    {
        std::ifstream f(SettingsPath());
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const std::string val = line.substr(eq + 1);
            if (val.empty()) continue;

            auto setBool = [&](bool& flag){ flag = (val != "0"); };
            auto setInt  = [&](int&  i)   { try { i = std::stoi(val); } catch (...) {} };

            if      (key == "midi_folder")          std::strncpy(st.midi_folder,   val.c_str(), sizeof(st.midi_folder)-1);
            else if (key == "sf2_folder")            std::strncpy(st.sf2_folder,    val.c_str(), sizeof(st.sf2_folder)-1);
            else if (key == "last_sf2_path")         std::strncpy(st.last_sf2_path,      val.c_str(), sizeof(st.last_sf2_path)-1);
            else if (key == "last_midiout_device")   std::strncpy(st.last_midiout_device, val.c_str(), sizeof(st.last_midiout_device)-1);
            else if (key == "main_x")                setInt(st.main_x);
            else if (key == "main_y")                setInt(st.main_y);
            else if (key == "main_w")                setInt(st.main_w);
            else if (key == "main_h")                setInt(st.main_h);
            else if (key == "main_maximized")        setBool(st.main_maximized);
            else if (key == "show_transport")        setBool(st.show_transport);
            else if (key == "show_midi")             setBool(st.show_midi);
            else if (key == "show_sf2")              setBool(st.show_sf2);
            else if (key == "show_presets")          setBool(st.show_presets);
            else if (key == "show_instrument")       setBool(st.show_instrument);
            else if (key == "show_tuning")           setBool(st.show_tuning);
            else if (key == "show_reverb")           setBool(st.show_reverb);
            else if (key == "show_devices")          setBool(st.show_devices);
            else if (key == "show_keyboard")         setBool(st.show_keyboard);
            else if (key == "show_midi_instruments") setBool(st.show_midi_instruments);
            else if (key == "show_spatial_mixer")    setBool(st.show_spatial_mixer);
            else if (key == "show_debug_console")    setBool(st.show_debug_console);
            else if (key == "show_midiout_devices")  setBool(st.show_midiout_devices);
        }
    }

    void SaveSettings()
    {
        std::ofstream f(SettingsPath());
        if (!f.is_open()) return;

        f << "midi_folder=" << st.midi_folder << "\n";
        f << "sf2_folder="  << st.sf2_folder  << "\n";

        // Persisted SF2 path
        if (g_sf2_engine.loaded && !g_sf2_engine.path.empty())
            f << "last_sf2_path=" << g_sf2_engine.path << "\n";

        // Persisted MIDI out device name
        if (g_midiout.opened &&
            g_midiout.device_id >= 0 &&
            g_midiout.device_id < static_cast<int>(g_midiout_devices.size()))
            f << "last_midiout_device=" << g_midiout_devices[static_cast<size_t>(g_midiout.device_id)].name << "\n";

        // Panel visibility flags
        f << "show_transport="        << (st.show_transport        ? '1' : '0') << "\n";
        f << "show_midi="             << (st.show_midi             ? '1' : '0') << "\n";
        f << "show_sf2="              << (st.show_sf2              ? '1' : '0') << "\n";
        f << "show_presets="          << (st.show_presets          ? '1' : '0') << "\n";
        f << "show_instrument="       << (st.show_instrument       ? '1' : '0') << "\n";
        f << "show_tuning="           << (st.show_tuning           ? '1' : '0') << "\n";
        f << "show_reverb="           << (st.show_reverb           ? '1' : '0') << "\n";
        f << "show_devices="          << (st.show_devices          ? '1' : '0') << "\n";
        f << "show_keyboard="         << (st.show_keyboard         ? '1' : '0') << "\n";
        f << "show_midi_instruments=" << (st.show_midi_instruments ? '1' : '0') << "\n";
        f << "show_spatial_mixer="    << (st.show_spatial_mixer    ? '1' : '0') << "\n";
        f << "show_debug_console="    << (st.show_debug_console    ? '1' : '0') << "\n";
        f << "show_midiout_devices="  << (st.show_midiout_devices  ? '1' : '0') << "\n";

        // Main Win32 window placement (values kept current by Render() every frame)
        f << "main_x="         << st.main_x                              << "\n";
        f << "main_y="         << st.main_y                              << "\n";
        f << "main_w="         << st.main_w                              << "\n";
        f << "main_h="         << st.main_h                              << "\n";
        f << "main_maximized=" << (st.main_maximized ? '1' : '0')        << "\n";
    }

    void SyncSf2Presets()
    {
        // Rebuild preset list only if SF2 was loaded/changed
        if (!g_sf2_engine.loaded) {
            if (!st.sf2_presets.empty()) {
                st.sf2_presets.clear();
                st.preset_selected = -1;
            }
            return;
        }
        const int n = tsf_get_presetcount(g_sf2_engine.sf2);
        if (static_cast<int>(st.sf2_presets.size()) != n) {
            st.sf2_presets.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i)
                st.sf2_presets[static_cast<size_t>(i)] = tsf_get_presetname(g_sf2_engine.sf2, i);
            st.preset_selected = g_sf2_engine.preset;
        }
    }

    // =======================================================================
    //  MENU BAR
    // =======================================================================
    void RenderMenuBar(HWND hwnd)
    {
        if (!ImGui::BeginMainMenuBar()) return;

        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open MIDI Folder...", "Ctrl+M")) {
                const auto f = gui_detail::BrowseFolder(hwnd, L"Select MIDI Folder");
                if (!f.empty()) {
                    std::strncpy(st.midi_folder, f.c_str(), sizeof(st.midi_folder)-1);
                    st.midi_folder_changed = true;
                    st.show_midi = true;
                }
            }
            if (ImGui::MenuItem("Open SF2 Folder...", "Ctrl+F")) {
                const auto f = gui_detail::BrowseFolder(hwnd, L"Select SoundFont Folder");
                if (!f.empty()) {
                    std::strncpy(st.sf2_folder, f.c_str(), sizeof(st.sf2_folder)-1);
                    st.sf2_folder_changed = true;
                    st.show_sf2 = true;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Stop Playback", "Space")) {
                stopMidiThread();
                stopScaleThread();
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                for (auto& v : g_audio.voices) v.stop();
                g_sf2_engine.silence();
                g_midiout.silence_all();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Transport",         nullptr, &st.show_transport);
            ImGui::MenuItem("MIDI Browser",      nullptr, &st.show_midi);
            ImGui::MenuItem("SF2 Browser",       nullptr, &st.show_sf2);
            ImGui::MenuItem("Presets",           nullptr, &st.show_presets);
            ImGui::MenuItem("Instrument",        nullptr, &st.show_instrument);
            ImGui::MenuItem("Tuning",            nullptr, &st.show_tuning);
            ImGui::MenuItem("Reverb",            nullptr, &st.show_reverb);
            ImGui::MenuItem("MIDI Instruments",  nullptr, &st.show_midi_instruments);
            ImGui::MenuItem("Spatial Mixer",     nullptr, &st.show_spatial_mixer);
            ImGui::MenuItem("Keyboard",          nullptr, &st.show_keyboard);
            ImGui::Separator();
            ImGui::MenuItem("MIDI Out Devices",   nullptr, &st.show_midiout_devices);
            ImGui::MenuItem("Audio Devices",     nullptr, &st.show_devices);
            ImGui::MenuItem("Debug Console",      nullptr, &st.show_debug_console);
            ImGui::MenuItem("About",             nullptr, &st.show_about);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Audio")) {
            if (ImGui::MenuItem("Show Audio Devices", "Ctrl+D"))
                st.show_devices = true;
            ImGui::Separator();
            if (ImGui::BeginMenu("Timbre")) {
                const InstrModel models[] = {
                    InstrModel::SINE, InstrModel::PIANO, InstrModel::ORGAN,
                    InstrModel::RHODES, InstrModel::SF2, InstrModel::MIDI_OUT
                };
                const char* labels[] = { "Sine", "Piano (Karplus-Strong)",
                    "Organ (8-rank drawbar)", "Rhodes (FM)", "SoundFont2 (SF2)",
                    "MIDI Out (Windows device)" };
                for (int i = 0; i < 6; ++i) {
                    const bool sel = (g_instr_model == models[i]);
                    if (ImGui::MenuItem(labels[i], nullptr, sel))
                        g_instr_model = models[i];
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About 432 Hz Synthesiser"))
                st.show_about = true;
            ImGui::EndMenu();
        }

        // Right-aligned status in menu bar
        {
            const auto& io = ImGui::GetIO();
            char fps[32];
            std::snprintf(fps, sizeof(fps), "%.0f fps  ", static_cast<double>(io.Framerate));
            const float fps_w = ImGui::CalcTextSize(fps).x;
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - fps_w);
            ImGui::TextDisabled("%s", fps);
        }

        ImGui::EndMainMenuBar();
    }

    // =======================================================================
    //  TRANSPORT WINDOW
    // =======================================================================
    void RenderTransport()
    {
        ImGui::SetNextWindowSize({520, 210}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({10, 30}, ImGuiCond_FirstUseEver);

        ImGui::Begin("Transport##win", &st.show_transport,
                     ImGuiWindowFlags_NoCollapse);

        const bool playing = g_midi_playing.load();
        const bool paused  = g_midi_paused.load();
        const uint64_t elapsed = g_midi_elapsed_us.load();
        const uint64_t total   = g_midi_total_us.load();

        // ---- Now playing --------------------------------------------------
        {
            std::lock_guard<std::mutex> lk(g_now_playing_mutex);
            if (g_now_playing_name.empty()) {
                ImGui::TextDisabled("  No file loaded");
            } else {
                ImGui::TextColored({0.4f, 0.9f, 0.5f, 1.f}, "  \xE2\x96\xB6 ");
                ImGui::SameLine();
                ImGui::TextUnformatted(g_now_playing_name.c_str());
            }
        }

        // ---- Seek slider -------------------------------------------------
        {
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%s / %s",
                gui_detail::FormatTime(elapsed).c_str(),
                gui_detail::FormatTime(total).c_str());

            static const uint64_t kZero = 0;
            const uint64_t maxVal = (total > 0) ? total : uint64_t{1};

            // Keep display in sync with playback when not dragging
            if (!st.seek_dragging)
                st.seek_drag_val = elapsed;

            ImGui::PushStyleColor(ImGuiCol_SliderGrab,       {0.2f, 0.7f, 0.9f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {0.4f, 0.9f, 1.0f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_FrameBg,          {0.1f, 0.2f, 0.3f, 1.f});
            ImGui::SetNextItemWidth(-1.f);
            if (total == 0) ImGui::BeginDisabled();
            if (ImGui::SliderScalar("##seek", ImGuiDataType_U64,
                                    &st.seek_drag_val, &kZero, &maxVal, overlay))
                g_seek_target_us.store(st.seek_drag_val);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drag to seek  \xE2\x80\x94  %s / %s",
                    gui_detail::FormatTime(elapsed).c_str(),
                    gui_detail::FormatTime(total).c_str());
            st.seek_dragging = ImGui::IsItemActive();
            if (total == 0) ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
        }

        // ---- Transport buttons -------------------------------------------
        ImGui::Spacing();
        ImGui::Indent(8.f);

        const ImVec2 btnSz = {80.f, 30.f};

        // Find index of currently playing file in the MIDI list
        int curIdx = -1;
        {
            std::lock_guard<std::mutex> lk(g_now_playing_mutex);
            for (int i = 0; i < (int)st.midi_files.size(); ++i)
                if (st.midi_files[i].first == g_now_playing_name) { curIdx = i; break; }
        }
        const bool hasPrev = curIdx > 0;
        const bool hasNext = curIdx >= 0 && curIdx < (int)st.midi_files.size() - 1;

        // Restart
        if (ImGui::Button("  |<   ", btnSz)) {
            // Re-play the current file from the beginning
            std::string name;
            {
                std::lock_guard<std::mutex> lk(g_now_playing_mutex);
                name = g_now_playing_name;
            }
            if (!name.empty()) {
                // Find full path in midi_files list
                for (const auto& e : st.midi_files) {
                    if (e.first == name) { playMidiFile(e.second); break; }
                }
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restart \xe2\x80\x94 play from the beginning");
        ImGui::SameLine(0, 6);

        // Stop
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.6f, 0.1f, 0.1f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.8f, 0.2f, 0.2f, 1.f});
        if (ImGui::Button(" Stop ", btnSz)) {
            stopMidiThread();
            stopScaleThread();
            std::lock_guard<std::mutex> lk(g_audio.mutex);
            for (auto& v : g_audio.voices) v.stop();
            g_sf2_engine.silence();
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop playback and silence all voices  (Space)");
        ImGui::SameLine(0, 6);

        // Play / Pause toggle
        if (!paused) {
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.1f, 0.5f, 0.2f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.2f, 0.7f, 0.3f, 1.f});
            if (ImGui::Button(" Play ", btnSz)) {
                // If nothing playing, replay last file
                if (!playing) {
                    std::string name;
                    {
                        std::lock_guard<std::mutex> lk(g_now_playing_mutex);
                        name = g_now_playing_name;
                    }
                    if (!name.empty()) {
                        for (const auto& e : st.midi_files) {
                            if (e.first == name) { playMidiFile(e.second); break; }
                        }
                    }
                }
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play (or resume from start if stopped)");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.5f, 0.4f, 0.1f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.7f, 0.6f, 0.2f, 1.f});
            if (ImGui::Button("Resume", btnSz))
                g_midi_paused.store(false);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Resume playback from where it was paused");
        }
        ImGui::SameLine(0, 6);

        // Pause
        {
            const bool canPause = playing && !paused;
            if (!canPause) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.45f, 0.35f, 0.1f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.65f, 0.55f, 0.2f, 1.f});
            if (ImGui::Button("Pause ", btnSz)) {
                if (playing && !paused) {
                    g_midi_paused.store(true);
                    std::lock_guard<std::mutex> lk(g_audio.mutex);
                    for (auto& v : g_audio.voices) v.stop();
                    g_sf2_engine.silence();
                }
            }
            ImGui::PopStyleColor(2);
            if (!canPause) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(canPause ? "Pause playback  \xe2\x80\x94  audio is silenced while paused"
                                           : "Nothing is playing");
        }
        ImGui::SameLine(0, 6);

        // Prev
        {
            if (!hasPrev) ImGui::BeginDisabled();
            if (ImGui::Button(" |<< ", btnSz))
                playMidiFile(st.midi_files[curIdx - 1].second);
            if (!hasPrev) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(hasPrev ? "Previous: %s" : "No previous track",
                                  hasPrev ? st.midi_files[curIdx - 1].first.c_str() : "");
        }
        ImGui::SameLine(0, 6);

        // Next
        {
            if (!hasNext) ImGui::BeginDisabled();
            if (ImGui::Button(" >>| ", btnSz))
                playMidiFile(st.midi_files[curIdx + 1].second);
            if (!hasNext) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(hasNext ? "Next: %s" : "No next track",
                                  hasNext ? st.midi_files[curIdx + 1].first.c_str() : "");
        }

        ImGui::Unindent(8.f);
        ImGui::Spacing();

        // ---- Master Volume + VU ------------------------------------------
        ImGui::Separator();

        float mv = g_master_volume.load();
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::SliderFloat("Volume", &mv, 0.f, 2.f, "%.2f"))
            g_master_volume.store(mv);
        if (ImGui::IsItemHovered()) {
            const float db = mv > 0.001f ? 20.f * std::log10(mv) : -60.f;
            ImGui::SetTooltip(mv < 0.001f
                ? "Master volume: -\xe2\x88\x9e dB\nDoubling = +6 dB  |  0.5 = -6 dB"
                : "Master volume: %.1f dB\n0.0 = silence  \xe2\x80\xa2  1.0 = unity  \xe2\x80\xa2  2.0 = +6 dB",
                static_cast<double>(db));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f%%)", static_cast<double>(mv * 100.f));

        // VU Bars
        ImGui::Spacing();
        const float vu_w = ImGui::GetContentRegionAvail().x - 30.f;

        ImGui::Text("L");
        ImGui::SameLine(20.f);
        gui_detail::DrawDbBar("", st.vu_bar_l, vu_w, 12.f);

        ImGui::Text("R");
        ImGui::SameLine(20.f);
        gui_detail::DrawDbBar("", st.vu_bar_r, vu_w, 12.f);

        ImGui::End();
    }

    // =======================================================================
    //  MIDI INSTRUMENTS WINDOW
    // =======================================================================
    void RenderMidiInstruments()
    {
        ImGui::SetNextWindowSize({320, 360}, ImGuiCond_FirstUseEver);
        ImGui::Begin("MIDI Instruments##win", &st.show_midi_instruments,
                     ImGuiWindowFlags_NoCollapse);

        // Copy state under lock so the render loop is not blocked long
        int  prog[16];
        bool used[16];
        {
            std::lock_guard<std::mutex> lk(g_midi_info_mutex);
            for (int i = 0; i < 16; ++i) {
                prog[i] = g_midi_info_programs[i];
                used[i] = g_midi_info_used[i];
            }
        }

        bool anyUsed = false;
        for (int i = 0; i < 16; ++i) if (used[i]) { anyUsed = true; break; }

        if (!anyUsed) {
            ImGui::TextDisabled("Load a MIDI file to see its channels here.");
        } else if (ImGui::BeginTable("##instbl", 3,
                       ImGuiTableFlags_Borders      |
                       ImGuiTableFlags_RowBg         |
                       ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Ch",         ImGuiTableColumnFlags_WidthFixed,   28.f);
            ImGui::TableSetupColumn("Instrument", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("GM #",       ImGuiTableColumnFlags_WidthFixed,   42.f);
            ImGui::TableHeadersRow();

            for (int ch = 0; ch < 16; ++ch) {
                if (!used[ch]) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", ch + 1);
                ImGui::TableSetColumnIndex(1);
                if (ch == 9)
                    ImGui::TextDisabled("Percussion");
                else
                    ImGui::TextUnformatted(gmInstrumentName(prog[ch]));
                ImGui::TableSetColumnIndex(2);
                if (ch == 9)
                    ImGui::TextDisabled("—");
                else
                    ImGui::Text("%d", prog[ch] + 1);
            }
            ImGui::EndTable();
        }

        ImGui::End();
    }

    // =======================================================================
    //  MIXER INI  â€” 432hz_player_mixer.ini  (next to the executable)
    // =======================================================================
    static std::string MixerPath()
    {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string p(exePath);
        const auto pos = p.rfind('\\');
        return (pos != std::string::npos
                ? p.substr(0, pos + 1) : std::string(".\\"))
               + "432hz_player_mixer.ini";
    }

    // Parse the whole mixer INI into a map: songName -> list of "key=value" lines.
    static std::map<std::string, std::vector<std::string>> ReadMixerIni()
    {
        std::map<std::string, std::vector<std::string>> sections;
        std::ifstream f(MixerPath());
        if (!f.is_open()) return sections;
        std::string line, cur;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']')
                cur = line.substr(1, line.size() - 2);
            else if (!cur.empty())
                sections[cur].push_back(line);
        }
        return sections;
    }

    static void WriteMixerIni(const std::map<std::string, std::vector<std::string>>& sections)
    {
        std::ofstream f(MixerPath());
        if (!f.is_open()) return;
        for (const auto& [name, lines] : sections) {
            f << '[' << name << "]\n";
            for (const auto& ln : lines) f << ln << '\n';
        }
    }

    void SaveMixerForSong(const std::string& songName)
    {
        if (songName.empty()) return;
        auto sections = ReadMixerIni();
        std::vector<std::string> lines;
        char buf[128];
        for (int ch = 0; ch < 16; ++ch) {
            std::snprintf(buf, sizeof(buf), "ch%d_vol=%f",  ch, static_cast<double>(g_mix[ch].vol));
            lines.push_back(buf);
            std::snprintf(buf, sizeof(buf), "ch%d_pan=%f",  ch, static_cast<double>(g_mix[ch].pan));
            lines.push_back(buf);
            std::snprintf(buf, sizeof(buf), "ch%d_send=%f", ch, static_cast<double>(g_mix[ch].send));
            lines.push_back(buf);
        }
        sections[songName] = std::move(lines);
        WriteMixerIni(sections);
    }

    void LoadMixerForSong(const std::string& songName)
    {
        for (int ch = 0; ch < 16; ++ch) g_mix[ch] = MixChannel{};
        if (songName.empty()) { applyMixerToSF2(); return; }
        auto sections = ReadMixerIni();
        auto it = sections.find(songName);
        if (it == sections.end()) { applyMixerToSF2(); return; }
        for (const auto& line : it->second) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const std::string val = line.substr(eq + 1);
            if (key.size() < 5 || key[0] != 'c' || key[1] != 'h') continue;
            try {
                const std::size_t us = key.find('_');
                if (us == std::string::npos) continue;
                const int ch = std::stoi(key.substr(2, us - 2));
                if (ch < 0 || ch >= 16) continue;
                const std::string field = key.substr(us + 1);
                const float v = static_cast<float>(std::stod(val));
                if      (field == "vol")  g_mix[ch].vol  = std::clamp(v, 0.f, 1.4f);
                else if (field == "pan")  g_mix[ch].pan  = std::clamp(v, -1.f, 1.f);
                else if (field == "send") g_mix[ch].send = std::clamp(v, -1.f, 1.f);
            } catch (...) {}
        }
        applyMixerToSF2();
    }

    void DeleteMixerForSong(const std::string& songName)
    {
        if (songName.empty()) return;
        auto sections = ReadMixerIni();
        sections.erase(songName);
        WriteMixerIni(sections);
    }

    // =======================================================================
    //  SPATIAL MIXER WINDOW  (redesigned)
    // =======================================================================
    void RenderSpatialMixer()
    {
        ImGui::SetNextWindowSize({540, 370}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Spatial Mixer##win", &st.show_spatial_mixer,
                     ImGuiWindowFlags_NoCollapse);

        // Current song (for save/delete)
        std::string curSong;
        {
            std::lock_guard<std::mutex> lk(g_now_playing_mutex);
            curSong = g_now_playing_name;
        }

        // Read MIDI channel info
        int  prog[16]; bool used[16];
        {
            std::lock_guard<std::mutex> lk(g_midi_info_mutex);
            for (int i = 0; i < 16; ++i) {
                prog[i] = g_midi_info_programs[i];
                used[i] = g_midi_info_used[i];
            }
        }

        // Validate selected channel
        if (st.selected_channel >= 0 && !used[st.selected_channel])
            st.selected_channel = -1;

        // Collect active channels in order
        int activeChs[16]; int nActive = 0;
        for (int i = 0; i < 16; ++i) if (used[i]) activeChs[nActive++] = i;

        // Channel strip dimensions
        constexpr float STRIP_W  = 34.f;
        constexpr float HEADER_H = 22.f;
        constexpr float VU_W     =  8.f;
        constexpr float FAD_W    = 16.f;
        constexpr float VU_H     = 100.f;
        constexpr int   COLS     = 8;

        const float PANEL_H  = ImGui::GetContentRegionAvail().y;
        const float STRIPS_W = static_cast<float>(COLS) * (STRIP_W + 3.f) + 8.f;

        // Channel colours (16 distinct hues)
        static const ImU32 kChColor[16] = {
            IM_COL32( 80,180,255,230), IM_COL32( 80,230,130,230),
            IM_COL32(255,210, 50,230), IM_COL32(255, 90, 90,230),
            IM_COL32(200, 80,255,230), IM_COL32( 80,230,230,230),
            IM_COL32(255,160, 30,230), IM_COL32(130,255, 80,230),
            IM_COL32(255, 80,180,230), IM_COL32(200,200, 50,230),
            IM_COL32( 50,130,255,230), IM_COL32(255,140,140,230),
            IM_COL32(100,230,160,230), IM_COL32(230,100,230,230),
            IM_COL32(160,230, 50,230), IM_COL32(230,160, 80,230),
        };

        bool anyChanged = false;

        // ====================================================================
        //  LEFT: channel strips â€” up to 2 rows of 8
        // ====================================================================
        ImGui::BeginChild("##strips", {STRIPS_W, PANEL_H}, false);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.f, 2.f});

        if (!nActive) {
            ImGui::TextDisabled("Load a MIDI file.");
        } else {
            const int nRows = (nActive > COLS) ? 2 : 1;
            for (int row = 0; row < nRows; ++row) {
                const int startIdx = row * COLS;
                const int endIdx   = std::min(startIdx + COLS, nActive);

                for (int ci = startIdx; ci < endIdx; ++ci) {
                    const int ch = activeChs[ci];
                    ImGui::PushID(ch);
                    ImGui::BeginGroup();

                    // --- Header: channel number, colored, clickable ---
                    {
                        const bool  isSel = (st.selected_channel == ch);
                        const float r  = ((kChColor[ch] >>  0) & 0xFF) / 255.f;
                        const float g2 = ((kChColor[ch] >>  8) & 0xFF) / 255.f;
                        const float b2 = ((kChColor[ch] >> 16) & 0xFF) / 255.f;
                        const float br = isSel ? 0.75f : 0.38f;
                        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                            {r * br, g2 * br, b2 * br, 1.f});
                        ImGui::BeginChild("##hdr", {STRIP_W, HEADER_H}, false);
                        char hdr[6];
                        std::snprintf(hdr, sizeof(hdr), "%d", ch + 1);
                        const float tx = (STRIP_W - ImGui::CalcTextSize(hdr).x) * 0.5f;
                        ImGui::SetCursorPos({tx, 3.f});
                        if (isSel)
                            ImGui::TextColored({1.f, 1.f, 1.f, 1.f}, "%s", hdr);
                        else
                            ImGui::TextDisabled("%s", hdr);
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                        // Click to select / deselect
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                            st.selected_channel = isSel ? -1 : ch;
                        if (ImGui::IsItemHovered()) {
                            const char* iname = (ch == 9) ? "Percussion"
                                : (prog[ch] >= 0 ? gmInstrumentName(prog[ch]) : "\xe2\x80\x94");
                            ImGui::SetTooltip(
                                "Ch %d: %s\nClick to select for spatial positioning",
                                ch + 1, iname);
                        }
                    }

                    // --- Mute / Solo buttons (below header) ---
                    {
                        bool& mu = g_mix[ch].mute;
                        bool& so = g_mix[ch].solo;
                        const float bw = (STRIP_W - 2.f) * 0.5f;

                        // Reduce frame padding so the label fits at a smaller font scale.
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {1.f, 1.f});
                        ImGui::SetWindowFontScale(0.78f);

                        if (mu) ImGui::PushStyleColor(ImGuiCol_Button, {0.75f, 0.10f, 0.10f, 1.f});
                        else    ImGui::PushStyleColor(ImGuiCol_Button, {0.22f, 0.22f, 0.25f, 1.f});
                        if (ImGui::Button("M##m", {bw, 17.f})) { mu = !mu; anyChanged = true; }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(mu ? "Ch %d: MUTED \xe2\x80\x94 click to unmute"
                                               : "Mute channel %d", ch + 1);
                        ImGui::PopStyleColor();

                        ImGui::SameLine(0, 2.f);

                        if (so) ImGui::PushStyleColor(ImGuiCol_Button, {0.60f, 0.50f, 0.08f, 1.f});
                        else    ImGui::PushStyleColor(ImGuiCol_Button, {0.22f, 0.22f, 0.25f, 1.f});
                        if (ImGui::Button("S##s", {bw, 17.f})) { so = !so; anyChanged = true; }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(so ? "Ch %d: SOLO \xe2\x80\x94 click to unsolo"
                                               : "Solo channel %d", ch + 1);
                        ImGui::PopStyleColor();

                        ImGui::SetWindowFontScale(1.0f);
                        ImGui::PopStyleVar(); // FramePadding
                    }

                    // --- VU bar + Volume fader ---
                    {
                        ImDrawList* dl  = ImGui::GetWindowDrawList();
                        const ImVec2 vp = ImGui::GetCursorScreenPos();

                        // VU background
                        dl->AddRectFilled(vp, {vp.x + VU_W, vp.y + VU_H},
                                          IM_COL32(25, 25, 30, 255), 2.f);
                        const float frac = std::min(st.mix_vu[ch], 1.f);
                        if (frac > 0.f) {
                            const float filled = VU_H * frac;
                            const ImU32 col =
                                frac > 0.85f ? IM_COL32(230,  55,  55, 255)
                              : frac > 0.65f ? IM_COL32(240, 195,  45, 255)
                                             : IM_COL32( 50, 195,  75, 255);
                            dl->AddRectFilled(
                                {vp.x, vp.y + VU_H - filled},
                                {vp.x + VU_W, vp.y + VU_H}, col, 2.f);
                        }
                        ImGui::Dummy({VU_W, VU_H});
                        ImGui::SameLine(0, 2.f);

                        // Volume fader (vertical)
                        ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.1f, 0.1f, 0.16f, 1.f});
                        ImGui::PushStyleColor(ImGuiCol_SliderGrab, {
                            ((kChColor[ch] >> 0) & 0xFF) / 255.f * 0.7f + 0.1f,
                            ((kChColor[ch] >> 8) & 0xFF) / 255.f * 0.7f + 0.1f,
                            ((kChColor[ch] >>16) & 0xFF) / 255.f * 0.7f + 0.1f, 1.f});
                        float& vol = g_mix[ch].vol;
                        const float db  = vol > 0.001f ? 20.f * std::log10(vol) : -60.f;
                        char vlab[8];
                        std::snprintf(vlab, sizeof(vlab),
                            vol < 0.001f ? "-inf" : "%.0f", static_cast<double>(db));
                        if (ImGui::VSliderFloat("##vol", {FAD_W, VU_H}, &vol, 0.f, 1.4f, "")) {
                            vol = std::clamp(vol, 0.f, 1.4f);
                            anyChanged = true;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Ch %d: %s dB", ch + 1, vlab);
                        ImGui::PopStyleColor(2);
                    }

                    ImGui::EndGroup();

                    // Vertical separator between channels (not after the last)
                    if (ci < endIdx - 1) {
                        ImGui::SameLine(0, 1.f);
                        {
                            const ImVec2 sp = ImGui::GetCursorScreenPos();
                            const float  sh = HEADER_H + VU_H + 14.f + 4.f;
                            ImGui::GetWindowDrawList()->AddLine(
                                {sp.x, sp.y}, {sp.x, sp.y + sh},
                                IM_COL32(80, 80, 90, 120), 1.f);
                        }
                        ImGui::SameLine(0, 2.f);
                    }
                    ImGui::PopID();
                } // channels in row

                // Row separator between row 1 and row 2
                if (row < nRows - 1) {
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                }
            } // rows
        }

        ImGui::PopStyleVar();
        ImGui::EndChild(); // strips

        if (anyChanged) {
            applyMixerToSF2();
            if (!curSong.empty()) SaveMixerForSong(curSong);
        }

        ImGui::SameLine();

        // ====================================================================
        //  RIGHT: 2D Spatial scene  (click-to-position the selected channel)
        // ====================================================================
        ImGui::BeginChild("##spatial", {0.f, PANEL_H}, false);

        // Title / selection hint
        if (st.selected_channel >= 0 && used[st.selected_channel]) {
            const char* iname = (st.selected_channel == 9) ? "Percussion"
                : (prog[st.selected_channel] >= 0
                   ? gmInstrumentName(prog[st.selected_channel]) : "?");
            ImGui::TextDisabled("Ch %d: %s \xe2\x80\x94 click to place",
                st.selected_channel + 1, iname);
        } else {
            ImGui::TextDisabled("Spatial Scene \xe2\x80\x94 select a channel");
        }

        // Reserve bottom space for label (18) + spacing (6) + button (24) + spacing (6)
        const float avW = ImGui::GetContentRegionAvail().x - 4.f;
        const float CS  = std::min(avW, PANEL_H - 18.f - 54.f);
        const ImVec2 cp0 = ImGui::GetCursorScreenPos();

        // Single invisible button over the whole canvas â€” captures click/drag
        ImGui::InvisibleButton("##canvas_bg", {CS, CS});
        const bool canvasActive  = ImGui::IsItemActive();
        const bool canvasHovered = ImGui::IsItemHovered();

        ImDrawList* sdl = ImGui::GetWindowDrawList();
        const ImVec2 cc = {cp0.x + CS * 0.5f, cp0.y + CS * 0.5f};
        const float  R  = CS * 0.5f - 6.f;

        // Click / drag on canvas -> position the selected channel
        if (canvasActive && st.selected_channel >= 0 && used[st.selected_channel]) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            g_mix[st.selected_channel].pan  =
                std::clamp((mp.x - cc.x) / R, -1.f, 1.f);
            // send range extended to [-1,+1] so channels can be placed in the
            // near/bottom half; applyMixerToSF2 clamps CC91 to [0,127] so
            // negative values simply mean zero reverb (fully dry / near).
            g_mix[st.selected_channel].send =
                std::clamp((cc.y - mp.y) / R, -1.f, 1.f);
            const float dist = std::hypot(g_mix[st.selected_channel].pan,
                                          g_mix[st.selected_channel].send);
            g_mix[st.selected_channel].vol  =
                std::clamp(1.15f - dist * 0.55f, 0.1f, 1.4f);
            applyMixerToSF2();
            if (!curSong.empty()) SaveMixerForSong(curSong);
        }
        if (canvasHovered && st.selected_channel >= 0 && used[st.selected_channel])
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        // Draw room
        sdl->AddRectFilled(cp0, {cp0.x + CS, cp0.y + CS}, IM_COL32(14, 20, 32, 255), 6.f);
        sdl->AddCircleFilled(cc, R,        IM_COL32( 18,  27,  45, 255), 64);
        sdl->AddCircle      (cc, R,        IM_COL32( 55,  80, 115, 200), 64, 1.5f);
        for (float fr = 0.33f; fr < 1.f; fr += 0.33f)
            sdl->AddCircle(cc, R * fr, IM_COL32(35, 52, 80, 130), 32, 1.f);
        sdl->AddLine({cc.x - R, cc.y}, {cc.x + R, cc.y}, IM_COL32(40, 60, 90, 100));
        sdl->AddLine({cc.x, cc.y - R}, {cc.x, cc.y + R}, IM_COL32(40, 60, 90, 100));
        sdl->AddText({cp0.x + 4.f,      cc.y - 7.f},    IM_COL32(110,110,130,200), "L");
        sdl->AddText({cp0.x + CS - 14.f,cc.y - 7.f},    IM_COL32(110,110,130,200), "R");
        sdl->AddText({cc.x - 9.f,       cp0.y + 3.f},   IM_COL32(110,110,130,200), "front");
        sdl->AddText({cc.x - 8.f,       cp0.y+CS-14.f}, IM_COL32(110,110,130,200), "rear");
        // Listener dot at centre
        sdl->AddCircleFilled(cc, 6.f, IM_COL32(200, 225, 255, 255));
        sdl->AddCircle      (cc, 9.f, IM_COL32(140, 175, 220, 190), 16, 1.5f);

        // Channel dots
        for (int ch = 0; ch < 16; ++ch) {
            if (!used[ch]) continue;
            const float dx = cc.x + g_mix[ch].pan  * R;
            const float dy = cc.y - g_mix[ch].send * R;
            const float dr = 8.f + st.mix_vu[ch] * 4.f;
            const bool  isSel = (st.selected_channel == ch);
            if (isSel)
                sdl->AddCircle({dx, dy}, dr + 5.f, IM_COL32(255, 255, 255, 220), 32, 2.f);
            sdl->AddCircleFilled({dx, dy}, dr, kChColor[ch]);
            sdl->AddCircle      ({dx, dy}, dr + 1.2f, IM_COL32(255,255,255,80));
            char lbl[4];
            std::snprintf(lbl, sizeof(lbl), "%d", ch + 1);
            sdl->AddText({dx - 4.5f, dy - 5.5f}, IM_COL32(0, 0, 0, 220), lbl);
        }

        // Axis label
        ImGui::SetCursorScreenPos({cp0.x, cp0.y + CS + 3.f});
        ImGui::TextDisabled("X=Pan    Y=Front(top)/Rear(bot) -> surround routing");

        // Delete mix button
        ImGui::Spacing();
        if (curSong.empty()) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.50f, 0.10f, 0.10f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.72f, 0.16f, 0.16f, 1.f});
        if (ImGui::Button("Del Mix", {CS, 24.f})) {
            DeleteMixerForSong(curSong);
            for (int ch = 0; ch < 16; ++ch) g_mix[ch] = MixChannel{};
            applyMixerToSF2();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(curSong.empty()
                ? "No song loaded"
                : "Delete saved mixer for:\n%s", curSong.c_str());
        ImGui::PopStyleColor(2);
        if (curSong.empty()) ImGui::EndDisabled();

        ImGui::EndChild(); // spatial
        ImGui::End();
    }

    // =======================================================================
    //  MIDI BROWSER
    // =======================================================================
    void RenderMidiBrowserWindow(HWND hwnd) { RenderMidiBrowser(hwnd); }
    void RenderMidiBrowser(HWND hwnd)
    {
        ImGui::SetNextWindowSize({300, 450}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({540, 30},   ImGuiCond_FirstUseEver);

        ImGui::Begin("MIDI Files##win", &st.show_midi, ImGuiWindowFlags_NoCollapse);

        // ---- Folder row ---------------------------------------------------
        // Right side: 4 + Browse(62) + 4 + R(20) = 90 px total
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 90.f);
        if (ImGui::InputText("##midifolder", st.midi_folder,
                             sizeof(st.midi_folder),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            st.midi_folder_changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Path to the MIDI file folder.\nPress Enter or click R to refresh.");
        ImGui::PopItemWidth();

        ImGui::SameLine(0, 4.f);
        if (ImGui::Button("Browse##midi", {62.f, 0.f})) {
            const auto f = gui_detail::BrowseFolder(hwnd, L"Select MIDI Folder");
            if (!f.empty()) {
                std::strncpy(st.midi_folder, f.c_str(), sizeof(st.midi_folder)-1);
                st.midi_folder_changed = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse for a folder containing MIDI files");

        // ---- Refresh button & count --------------------------------------
        ImGui::SameLine(0, 4.f);
        if (ImGui::Button("R##r1", {20.f, 0.f})) st.midi_folder_changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh folder listing");
        ImGui::SameLine(0, 8.f);
        ImGui::TextDisabled("%zu files", st.midi_files.size());

        // ---- Search filter -----------------------------------------------
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##midifilter", "Filter...",
                                 st.midi_filter, sizeof(st.midi_filter));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type to filter MIDI files by name");

        // ---- File list ----------------------------------------------------
        const float list_h = ImGui::GetContentRegionAvail().y - 6.f;
        if (ImGui::BeginChild("##midilist", {0.f, list_h}, true,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            const std::string filter(st.midi_filter);
            for (int i = 0; i < static_cast<int>(st.midi_files.size()); ++i) {
                const auto& e = st.midi_files[static_cast<size_t>(i)];
                if (!filter.empty()) {
                    std::string nameLow = e.first;
                    for (char& c : nameLow) c = static_cast<char>(std::tolower(c));
                    std::string fLow = filter;
                    for (char& c : fLow) c = static_cast<char>(std::tolower(c));
                    if (nameLow.find(fLow) == std::string::npos) continue;
                }
                const bool selected = (st.midi_selected == i);
                if (ImGui::Selectable(e.first.c_str(), selected,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                {
                    st.midi_selected = i;
                    if (ImGui::IsMouseDoubleClicked(0))
                        playMidiFile(e.second);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Double-click to play\n%s", e.second.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::End();
    }

    // =======================================================================
    //  SF2 BROWSER
    // =======================================================================
    void RenderSf2BrowserWindow(HWND hwnd) { RenderSf2Browser(hwnd); }
    void RenderSf2Browser(HWND hwnd)
    {
        ImGui::SetNextWindowSize({300, 380}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({850, 30},   ImGuiCond_FirstUseEver);

        ImGui::Begin("Synthesizer##win", &st.show_sf2, ImGuiWindowFlags_NoCollapse);

        // ---- Active status -----------------------------------------------
        if (g_instr_model == InstrModel::MIDI_OUT && g_midiout.opened) {
            const int di = g_midiout.device_id;
            const std::string devName =
                (di >= 0 && di < static_cast<int>(g_midiout_devices.size()))
                    ? g_midiout_devices[static_cast<size_t>(di)].name : "Unknown";
            ImGui::TextColored({0.4f, 0.9f, 0.5f, 1.f}, "Active:");
            ImGui::SameLine(0, 4.f);
            ImGui::TextUnformatted(devName.c_str());
        } else if (g_sf2_engine.loaded) {
            ImGui::TextColored({0.4f, 0.9f, 0.5f, 1.f}, "Loaded:");
            ImGui::SameLine(0, 4.f);
            ImGui::TextUnformatted(
                std::filesystem::path(g_sf2_engine.path).filename().string().c_str());
        } else {
            ImGui::TextDisabled("No synthesizer active");
        }

        // ---- SF2 folder row ---------------------------------------------
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 90.f);
        if (ImGui::InputText("##sf2folder", st.sf2_folder, sizeof(st.sf2_folder),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            st.sf2_folder_changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Path to the SoundFont folder.\nPress Enter or click R to refresh.");
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 4.f);
        if (ImGui::Button("Browse##sf2", {62.f, 0.f})) {
            const auto f = gui_detail::BrowseFolder(hwnd, L"Select SoundFont Folder");
            if (!f.empty()) {
                std::strncpy(st.sf2_folder, f.c_str(), sizeof(st.sf2_folder)-1);
                st.sf2_folder_changed = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse for a folder containing SF2 SoundFont files");
        ImGui::SameLine(0, 4.f);
        if (ImGui::Button("R##r2", {20.f, 0.f})) st.sf2_folder_changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh SF2 folder listing");

        // ---- SF2 filter -------------------------------------------------
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##sf2filter", "Filter SF2...",
                                 st.sf2_filter, sizeof(st.sf2_filter));

        // ---- Unified list -----------------------------------------------
        const float bottom_h = g_midiout.opened ? 28.f : 6.f;
        const float list_h   = ImGui::GetContentRegionAvail().y - bottom_h;
        if (ImGui::BeginChild("##synthlist", {0.f, list_h}, true,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            // --- Windows MIDI section ---
            ImGui::TextDisabled("Windows MIDI");
            ImGui::SameLine();
            if (ImGui::SmallButton("Refresh##midi"))
                g_midiout_devices = MidiOutEngine::enumerate();
            ImGui::Separator();
            if (g_midiout_devices.empty()) {
                ImGui::TextDisabled("  (no devices found)");
            } else {
                for (size_t i = 0; i < g_midiout_devices.size(); ++i) {
                    const auto& dev = g_midiout_devices[i];
                    const bool active = g_instr_model == InstrModel::MIDI_OUT &&
                                        g_midiout.opened &&
                                        g_midiout.device_id == static_cast<int>(i);
                    if (active)
                        ImGui::PushStyleColor(ImGuiCol_Text, {0.4f, 0.9f, 0.5f, 1.f});
                    const bool sel = ImGui::Selectable(dev.name.c_str(), active,
                                                       ImGuiSelectableFlags_AllowDoubleClick);
                    if (active)
                        ImGui::PopStyleColor();
                    if (sel && ImGui::IsMouseDoubleClicked(0)) {
                        switchToMidiOut(static_cast<int>(i));
                        SaveSettings();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Double-click to activate: %s", dev.name.c_str());
                }
            }

            ImGui::Spacing();

            // --- SoundFont (SF2) section ---
            ImGui::TextDisabled("SoundFont (SF2)");
            ImGui::Separator();
            const std::string filter(st.sf2_filter);
            for (int i = 0; i < static_cast<int>(st.sf2_files.size()); ++i) {
                const auto& e = st.sf2_files[static_cast<size_t>(i)];
                if (!filter.empty()) {
                    std::string nameLow = e.first;
                    for (char& c : nameLow) c = static_cast<char>(std::tolower(c));
                    std::string fLow = filter;
                    for (char& c : fLow) c = static_cast<char>(std::tolower(c));
                    if (nameLow.find(fLow) == std::string::npos) continue;
                }
                const bool active = g_instr_model == InstrModel::SF2 &&
                    g_sf2_engine.loaded &&
                    std::filesystem::path(g_sf2_engine.path).filename().string() == e.first;
                const bool selected = (st.sf2_selected == i);
                if (active) ImGui::PushStyleColor(ImGuiCol_Text, {0.4f, 0.9f, 0.5f, 1.f});
                if (ImGui::Selectable(e.first.c_str(), selected || active,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                {
                    st.sf2_selected = i;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        loadSf2File(e.second);
                        std::strncpy(st.last_sf2_path, e.second.c_str(),
                                     sizeof(st.last_sf2_path) - 1);
                        SaveSettings();
                        st.sf2_presets.clear();
                    }
                }
                if (active) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Double-click to load\n%s", e.second.c_str());
            }
        }
        ImGui::EndChild();

        if (g_midiout.opened) {
            if (ImGui::Button("Close Device")) {
                stopMidiThread();
                g_midiout.close();
                SaveSettings();
            }
        }

        ImGui::End();
    }

    // =======================================================================
    //  SF2 PRESETS
    // =======================================================================
    void RenderPresets()
    {
        ImGui::SetNextWindowSize({260, 350}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({850, 300}, ImGuiCond_FirstUseEver);

        ImGui::Begin("SF2 Presets##win", &st.show_presets, ImGuiWindowFlags_NoCollapse);

        if (g_instr_model == InstrModel::MIDI_OUT) {
            ImGui::TextDisabled("Windows MIDI device active.");
            ImGui::TextDisabled("No presets available.");
            ImGui::End();
            return;
        }
        if (!g_sf2_engine.loaded) {
            ImGui::TextDisabled("Load a SoundFont first.");
            ImGui::End();
            return;
        }

        ImGui::TextDisabled("%zu presets", st.sf2_presets.size());

        const float list_h = ImGui::GetContentRegionAvail().y - 6.f;
        if (ImGui::BeginChild("##presetlist", {0.f, list_h}, true)) {
            for (int i = 0; i < static_cast<int>(st.sf2_presets.size()); ++i) {
                const bool active   = (g_sf2_engine.preset == i);
                const bool selected = (st.preset_selected == i);
                if (active) ImGui::PushStyleColor(ImGuiCol_Text, {0.4f, 0.9f, 0.5f, 1.f});
                char label[128];
                std::snprintf(label, sizeof(label), "[%d] %s", i,
                              st.sf2_presets[static_cast<size_t>(i)].c_str());
                if (ImGui::Selectable(label, selected || active,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                {
                    st.preset_selected = i;
                    // Single or double click — apply preset immediately
                    std::lock_guard<std::mutex> lk(g_audio.mutex);
                    g_sf2_engine.select_preset(i);
                    g_sf2_engine.silence();
                }
                if (active) ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();

        ImGui::End();
    }

    // =======================================================================
    //  INSTRUMENT / TIMBRE
    // =======================================================================
    void RenderInstrument()
    {
        ImGui::SetNextWindowSize({280, 220}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({10, 250}, ImGuiCond_FirstUseEver);

        ImGui::Begin("Instrument##win", &st.show_instrument, ImGuiWindowFlags_NoCollapse);

        const int model_count = INSTR_MODEL_COUNT;
        const char* model_names[] = {
            "Sine  (LUT pure sine)",
            "Piano (Karplus-Strong string)",
            "Organ (8-rank drawbar additive)",
            "Rhodes (2-op FM electric piano)",
            "SF2   (SoundFont2 sample playback)",
            "MIDI Out (Windows MIDI device)"
        };
        const InstrModel models[] = {
            InstrModel::SINE, InstrModel::PIANO, InstrModel::ORGAN,
            InstrModel::RHODES, InstrModel::SF2, InstrModel::MIDI_OUT
        };

        ImGui::TextUnformatted("Active Timbre:");
        ImGui::Spacing();

        for (int i = 0; i < model_count; ++i) {
            const bool sel = (g_instr_model == models[i]);
            bool b = sel;
            if (ImGui::RadioButton(model_names[i], b))
                g_instr_model = models[i];
            static const char* kDesc[] = {
                "Pure sine wave via look-up table \xe2\x80\x94 no overtones, mathematically clean",
                "Karplus-Strong struck-string model \xe2\x80\x94 realistic piano-like decay",
                "8-rank additive drawbar organ \xe2\x80\x94 constant amplitude, Hammond-style",
                "2-operator FM electric piano \xe2\x80\x94 bright attack, warm DX7-style sustain",
                "SoundFont2 sample playback via TinySoundFont \xe2\x80\x94 load an .sf2 file in the SF2 browser",
                "Windows WinMM MIDI output device \xe2\x80\x94 Microsoft GS Wavetable Synth or external hardware",
            };
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kDesc[i]);
        }

        ImGui::Separator();

        // Timbre-specific info
        switch (g_instr_model) {
        case InstrModel::PIANO:
            ImGui::TextWrapped("Extended Karplus-Strong struck-string model.\n"
                               "Delay line length = sr/Hz (exact integer -> perfectly in tune).");
            break;
        case InstrModel::ORGAN:
            ImGui::TextWrapped("8-rank drawbar organ. Harmonics 1-8 via LUT. Constant amplitude.");
            break;
        case InstrModel::RHODES:
            ImGui::TextWrapped("2-operator FM electric piano. Modulation index decays:\n"
                               "bright attack -> warm sustain (DX7-style).");
            break;
        case InstrModel::SF2:
            if (g_sf2_engine.loaded) {
                const std::string base = std::filesystem::path(g_sf2_engine.path).filename().string();
                ImGui::TextColored({0.4f, 0.9f, 0.5f, 1.f}, "Loaded: %s", base.c_str());
                ImGui::TextDisabled("Preset [%d] %s", g_sf2_engine.preset,
                                    tsf_get_presetname(g_sf2_engine.sf2, g_sf2_engine.preset));
            } else {
                ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f},
                                   "No SF2 loaded. Use the SF2 Browser.");
            }
            break;
        case InstrModel::MIDI_OUT:
            if (g_midiout.opened) {
                ImGui::TextColored({0.4f, 0.9f, 0.5f, 1.f}, "Device: %s",
                                   g_midiout_devices[static_cast<size_t>(g_midiout.device_id)].name.c_str());
                ImGui::TextDisabled("432 Hz via pitch-bend offset (RPN 0)");
            } else {
                ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f}, "No device open.");
                if (ImGui::Button("Select MIDI Out Device"))
                    st.show_midiout_devices = true;
            }
            break;
        default:
            ImGui::TextWrapped("Pure sine wave, LUT-based, zero distortion.");
            break;
        }

        ImGui::End();
    }

    // =======================================================================
    //  TUNING
    // =======================================================================
    void RenderTuning()
    {
        ImGui::SetNextWindowSize({320, 270}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({300, 250}, ImGuiCond_FirstUseEver);

        ImGui::Begin("Tuning##win", &st.show_tuning, ImGuiWindowFlags_NoCollapse);

        // ---- A4 slider ---------------------------------------------------
        double a4 = g_tuning.a4;
        static const double kA4SliderMin = 200.0, kA4SliderMax = 600.0;
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderScalar("##a4slider", ImGuiDataType_Double,
                                &a4, &kA4SliderMin, &kA4SliderMax, "A4 = %.4f Hz"))
        {
            g_tuning.a4 = a4;
            std::lock_guard<std::mutex> lk(g_audio.mutex);
            g_sf2_engine.retune(a4);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reference pitch for A4.\n432 Hz = healing tuning  \xe2\x80\xa2  440 Hz = concert standard\nDrag or use the input field below.");

        // Fine-tune with input field
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::InputDouble("A4 (Hz)", &a4, 0.01, 1.0, "%.4f",
                               ImGuiInputTextFlags_EnterReturnsTrue))
        {
            a4 = std::max(200.0, std::min(600.0, a4));
            g_tuning.a4 = a4;
            std::lock_guard<std::mutex> lk(g_audio.mutex);
            g_sf2_engine.retune(a4);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Type the exact A4 frequency and press Enter.\n+/- step: 0.01 Hz (fine) or 1 Hz (coarse).");

        // ---- Quick presets -----------------------------------------------
        ImGui::Spacing();
        struct TunePreset { const char* label; double hz; TuneMode mode; };
        static const TunePreset kPresets[] = {
            {"432 Integer", 432.0, TuneMode::INTEGER},
            {"432 Equal",   432.0, TuneMode::EQUAL},
            {"440 Equal",   440.0, TuneMode::EQUAL},
            {"444 Equal",   444.0, TuneMode::EQUAL},
        };
        for (const auto& p : kPresets) {
            const bool active = (std::abs(g_tuning.a4 - p.hz) < 0.01 &&
                                  g_tuning.mode == p.mode);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, {0.2f, 0.5f, 0.8f, 1.f});
            if (ImGui::Button(p.label)) {
                g_tuning.a4   = p.hz;
                g_tuning.mode = p.mode;
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                g_sf2_engine.retune(p.hz);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Set A4 = %.0f Hz  %s temperament%s",
                    p.hz,
                    p.mode == TuneMode::INTEGER ? "Integer (pure rational)" : "Equal (12-TET)",
                    active ? "  \xe2\x80\x94  currently active" : "");
            }
            if (active) ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::NewLine();

        // ---- Mode ---------------------------------------------------------
        ImGui::Spacing();
        ImGui::TextUnformatted("Scale division:");
        {
            bool intMode = (g_tuning.mode == TuneMode::INTEGER);
            if (ImGui::RadioButton("Integer (pure rational ratios)", intMode))
                g_tuning.mode = TuneMode::INTEGER;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pythagorean / integer-ratio tuning.\nOvertones align perfectly \xe2\x80\x94 pure fifths and thirds.\nSlightly different from equal temperament.");
            bool eqMode = (g_tuning.mode == TuneMode::EQUAL);
            if (ImGui::RadioButton("Equal temperament (12-TET)", eqMode))
                g_tuning.mode = TuneMode::EQUAL;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Standard 12-tone equal temperament.\nEvery semitone = 2^(1/12) ratio.\nUsed by most modern instruments.");
        }

        // ---- SF2 detune info --------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        const double det = g_tuning.sf2Detune();
        ImGui::TextDisabled("SF2 channel detune: %+.4f semitones from 440 Hz", det);

        // ---- Comparison table toggle ------------------------------------
        ImGui::Checkbox("Show scale comparison table", &st.tuning_show_table);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Compare note frequencies across Integer-432, Equal-432, and Equal-440 tunings.");
        if (st.tuning_show_table) {
            ImGui::Spacing();
            if (ImGui::BeginTable("##cmp", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY, {0.f, 160.f}))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Note");
                ImGui::TableSetupColumn("Int-432");
                ImGui::TableSetupColumn("Eq-432");
                ImGui::TableSetupColumn("Eq-440");
                ImGui::TableHeadersRow();

                static const char* kNotes[] = {
                    "C4","C#4","D4","D#4","E4","F4","F#4","G4","G#4","A4","A#4","B4","C5"
                };
                static const int MIDI_C4 = 60;
                TuningConfig int432; int432.a4 = 432.0; int432.mode = TuneMode::INTEGER;
                TuningConfig eq432;  eq432.a4  = 432.0; eq432.mode  = TuneMode::EQUAL;
                TuningConfig eq440;  eq440.a4  = 440.0; eq440.mode  = TuneMode::EQUAL;

                for (int i = 0; i <= 12; ++i) {
                    const int midi = MIDI_C4 + i;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(kNotes[i]);
                    ImGui::TableNextColumn(); ImGui::Text("%.4f", int432.noteFreq(midi));
                    ImGui::TableNextColumn(); ImGui::Text("%.4f", eq432.noteFreq(midi));
                    ImGui::TableNextColumn(); ImGui::Text("%.4f", eq440.noteFreq(midi));
                }
                ImGui::EndTable();
            }
        }

        ImGui::End();
    }

    // =======================================================================
    //  REVERB
    // =======================================================================
    void RenderReverb()
    {
        ImGui::SetNextWindowSize({310, 260}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({630, 250}, ImGuiCond_FirstUseEver);

        ImGui::Begin("Reverb##win", &st.show_reverb, ImGuiWindowFlags_NoCollapse);

        // ---- On/Off toggle -----------------------------------------------
        {
            bool en = g_reverb.enabled;
            if (ImGui::Checkbox("Enable Reverb", &en)) {
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                g_reverb.enabled = en;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toggle Freeverb convolution reverb.\nAdjusting any slider below will auto-enable reverb.");
        }

        ImGui::Spacing();

        // ---- Named presets ------------------------------------------------
        struct ReverbPreset { const char* name; const char* key; };
        static const ReverbPreset kPresets[] = {
            {"Hall",      "hall"},
            {"Stage",     "stage"},
            {"Studio",    "studio"},
            {"Plate",     "plate"},
            {"Cathedral", "cathedral"},
            {"Off",       "off"},
        };
        ImGui::TextUnformatted("Presets:");
        ImGui::SameLine();
        for (const auto& p : kPresets) {
            if (ImGui::Button(p.name)) {
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                g_reverb.set_preset(p.key);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Apply \"%-10s\" reverb preset", p.name);
            ImGui::SameLine(0, 4.f);
        }
        ImGui::NewLine();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Fine-tune:");

        // ---- Sliders ------------------------------------------------------
        auto Slider = [](const char* label, float& val,
                          float lo, float hi, const char* fmt, const char* tip = nullptr)
        {
            float v = val;
            if (ImGui::SliderFloat(label, &v, lo, hi, fmt)) {
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                val = v;
                g_reverb.apply_params();
                g_reverb.enabled = true;
            }
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };

        Slider("Room Size##rvb", g_reverb.roomSize, 0.f, 1.f, "%.2f",
               "Room size (Schroeder coefficient).\nHigher = larger, longer reflections.");
        Slider("Damping##rvb",   g_reverb.damp,     0.f, 1.f, "%.2f",
               "High-frequency damping.\nHigher = warmer, softer reflections.");
        Slider("Wet##rvb",       g_reverb.wet,      0.f, 1.f, "%.2f",
               "Reverb wet signal level.\nAmount of processed (reverberant) signal mixed in.");
        Slider("Dry##rvb",       g_reverb.dry,      0.f, 1.f, "%.2f",
               "Dry (direct) signal level.\nReduce for a more distant, immersive sound.");
        Slider("Width##rvb",     g_reverb.width,    0.f, 1.f, "%.2f",
               "Stereo width of the reverb tail.\n0 = mono reverb  \xe2\x80\xa2  1 = full stereo.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("room=%.2f  damp=%.2f  wet=%.2f  dry=%.2f  w=%.2f",
                            g_reverb.roomSize, g_reverb.damp,
                            g_reverb.wet, g_reverb.dry, g_reverb.width);

        ImGui::End();
    }

    // =======================================================================
    //  AUDIO DEVICES
    // =======================================================================
    void RenderDevices()
    {
        ImGui::SetNextWindowSize({700, 320}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({60, 60}, ImGuiCond_FirstUseEver);

        ImGui::Begin("Audio Devices##win", &st.show_devices, ImGuiWindowFlags_NoCollapse);

        // ---- Status bar ---------------------------------------------------
        ImGui::TextColored({0.4f, 0.9f, 0.5f, 1.f}, "Active:");
        ImGui::SameLine();
        ImGui::Text("API=%s  Device=%u  \"%s\"  %u Hz",
                    apiLabel(g_opened_api), g_opened_device_id,
                    g_opened_name.c_str(), g_negotiated_rate);

        ImGui::Spacing();

        // ---- Device table -------------------------------------------------
        if (ImGui::BeginTable("##devtable", 6,
            ImGuiTableFlags_Borders    | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY    | ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_Resizable,
            {0.f, ImGui::GetContentRegionAvail().y - 40.f}))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("ID",   ImGuiTableColumnFlags_WidthFixed, 36.f);
            ImGui::TableSetupColumn("API",  ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Out Ch", ImGuiTableColumnFlags_WidthFixed, 48.f);
            ImGui::TableSetupColumn("In Ch",  ImGuiTableColumnFlags_WidthFixed, 42.f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Rates (Hz)", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(g_devices.size()); ++i) {
                const auto& d = g_devices[static_cast<size_t>(i)];
                const bool active = (d.api == g_opened_api &&
                                      d.id == g_opened_device_id);
                const bool sel = (st.dev_selected == i);

                ImGui::TableNextRow();
                if (active) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                                   IM_COL32(30, 80, 30, 120));

                ImGui::TableNextColumn();
                char selLabel[16];
                std::snprintf(selLabel, sizeof(selLabel), "%u##dev%d", d.id, i);
                if (ImGui::Selectable(selLabel, sel || active,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                      {0.f, 0.f}))
                    st.dev_selected = i;

                ImGui::TableNextColumn(); ImGui::TextUnformatted(apiLabel(d.api));
                ImGui::TableNextColumn(); ImGui::Text("%u", d.info.outputChannels);
                ImGui::TableNextColumn();
                if (d.info.inputChannels > 0)
                    ImGui::Text("%u", d.info.inputChannels);
                else
                    ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.info.name.c_str());
                ImGui::TableNextColumn();
                {
                    std::string rates;
                    for (auto r : d.info.sampleRates) {
                        if (!rates.empty()) rates += ' ';
                        rates += std::to_string(r);
                    }
                    ImGui::TextUnformatted(rates.c_str());
                }
            }
            ImGui::EndTable();
        }

        // ---- Switch button -----------------------------------------------
        const bool canSwitch = (st.dev_selected >= 0 &&
            st.dev_selected < static_cast<int>(g_devices.size()));
        if (!canSwitch) ImGui::BeginDisabled();
        if (ImGui::Button("Switch to Selected Device")) {
            if (canSwitch) {
                const auto& d = g_devices[static_cast<size_t>(st.dev_selected)];
                switchToDeviceGlobal(d.api, d.id);
            }
        }
        if (!canSwitch) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(canSwitch
                ? "Switch the audio output to the highlighted device"
                : "Select a device in the table above");
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            // Re-enumerate is not live here, but we can at least tell the user
            ImGui::SetTooltip("Restart the application to re-enumerate devices.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Device list is captured at startup.\nRestart the application to re-enumerate.");

        ImGui::End();
    }

    // =======================================================================
    //  ON-SCREEN PIANO KEYBOARD
    // =======================================================================
    void RenderKeyboard()
    {
        ImGui::SetNextWindowSize({780, 200}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({10, 500}, ImGuiCond_FirstUseEver);

        ImGui::Begin("Piano Keyboard##win", &st.show_keyboard,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

        // Octave selector
        ImGui::Text("Octave:");
        ImGui::SameLine();
        if (ImGui::Button(" < ##oct")) st.kb_octave = std::max(0, st.kb_octave - 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shift keyboard one octave down");
        ImGui::SameLine();
        ImGui::Text(" C%d ", st.kb_octave);
        ImGui::SameLine();
        if (ImGui::Button(" > ##oct")) st.kb_octave = std::min(9, st.kb_octave + 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shift keyboard one octave up");

        // Velocity slider
        ImGui::SameLine(0, 16.f);
        ImGui::SetNextItemWidth(100.f);
        ImGui::SliderFloat("Velocity##kb", &st.kb_velocity, 0.01f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Note-on velocity for clicks on the piano keyboard.\n0.01 = very soft  \xe2\x80\xa2  1.0 = full velocity");

        ImGui::Spacing();
        DrawPianoKeys();

        ImGui::End();
    }

    void DrawPianoKeys()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  avail  = ImGui::GetContentRegionAvail().x;

        // 2 octaves, 14 white keys
        const int octaves = 2;
        const int n_white = 7 * octaves;
        const float wk_w  = std::floor(avail / n_white); // white key width
        const float wk_h  = 100.f;
        const float bk_w  = wk_w * 0.55f;
        const float bk_h  = wk_h * 0.62f;

        // Chromatic pattern within an octave: which positions are black keys
        // relative to white-key positions 0-6: B after 0(C), 1(D), 3(F), 4(G), 5(A)
        static const bool isBlack[12] = {0,1,0,1,0,0,1,0,1,0,1,0};
        // White-key index → semitone offset within octave
        static const int  whiteToSemi[7]  = {0, 2, 4, 5, 7, 9, 11};
        // Black-key positions: between white keys 0-1, 1-2, 3-4, 4-5, 5-6
        static const bool blackAfter[7] = {true, true, false, true, true, true, false};

        const int base_midi = 12 * st.kb_octave; // C of current octave

        // Invisible wide button to capture clicks
        ImGui::PushID("kbhitbox");
        const ImVec2 hitSz = {wk_w * n_white, wk_h};
        ImGui::InvisibleButton("##kbhit", hitSz);
        const bool clicking = ImGui::IsItemActive();
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        ImGui::PopID();

        int clicked_midi = -1;
        if (clicking) {
            const float rx = mousePos.x - origin.x;
            const float ry = mousePos.y - origin.y;
            if (rx >= 0 && ry >= 0 && ry < wk_h) {
                // Check black keys first (they overlap white keys)
                int wk = 0;
                for (int oct = 0; oct < octaves; ++oct) {
                    for (int w = 0; w < 7; ++w, ++wk) {
                        if (blackAfter[w] && w < 6) {
                            const float bx = origin.x + (wk + 1) * wk_w - bk_w * 0.5f;
                            if (rx >= bx - origin.x && rx < bx - origin.x + bk_w &&
                                ry < bk_h)
                            {
                                // semitone = whiteToSemi[w]+1
                                clicked_midi = base_midi + oct*12 + whiteToSemi[w] + 1;
                                goto found;
                            }
                        }
                    }
                }
                // White key
                wk = static_cast<int>(rx / wk_w);
                if (wk < n_white) {
                    const int o = wk / 7;
                    const int w = wk % 7;
                    clicked_midi = base_midi + o*12 + whiteToSemi[w];
                }
            }
        }
        found:;

        // Handle note events
        static int last_midi = -1;
        if (clicking && clicked_midi >= 0 && clicked_midi != last_midi) {
            // Note off previous
            if (last_midi >= 0) {
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded)
                    g_sf2_engine.note_off(last_midi);
                else
                    for (auto& v : g_audio.voices)
                        if (v.is_active) v.stop();
            }
            // Note on
            {
                const double hz = g_tuning.noteFreq(clicked_midi);
                std::lock_guard<std::mutex> lk(g_audio.mutex);
                if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded) {
                    g_sf2_engine.note_on(clicked_midi, st.kb_velocity);
                } else {
                    const uint32_t sr = g_audio.sample_rate;
                    if (hz > 0.0 && hz < sr / 2.0)
                        g_audio.voices[0].start(g_instr_model, hz, sr,
                                                static_cast<double>(st.kb_velocity));
                }
            }
            last_midi = clicked_midi;
        } else if (!clicking && last_midi >= 0) {
            std::lock_guard<std::mutex> lk(g_audio.mutex);
            if (g_instr_model == InstrModel::SF2 && g_sf2_engine.loaded)
                g_sf2_engine.note_off(last_midi);
            else
                for (auto& v : g_audio.voices) v.stop();
            last_midi = -1;
        }

        // ---- Draw white keys ------------------------------------------
        {
            int wk = 0;
            for (int oct = 0; oct < octaves; ++oct) {
                for (int w = 0; w < 7; ++w, ++wk) {
                    const int midi = base_midi + oct*12 + whiteToSemi[w];
                    const float x0 = origin.x + wk * wk_w;
                    const float y0 = origin.y;
                    const bool pressed = (clicking && midi == clicked_midi);
                    const ImU32 col = pressed
                        ? IM_COL32(160, 200, 255, 255)
                        : IM_COL32(240, 240, 240, 255);
                    dl->AddRectFilled({x0+1, y0}, {x0+wk_w-1, y0+wk_h}, col, 3.f);
                    dl->AddRect({x0, y0}, {x0+wk_w, y0+wk_h}, IM_COL32(80,80,80,200), 3.f);

                    // Note name on C keys
                    if (w == 0) {
                        char name[8];
                        std::snprintf(name, sizeof(name), "C%d", st.kb_octave + oct);
                        dl->AddText({x0+2.f, y0+wk_h-16.f}, IM_COL32(80,80,80,200), name);
                    }
                }
            }
        }

        // ---- Draw black keys ------------------------------------------
        {
            int wk = 0;
            for (int oct = 0; oct < octaves; ++oct) {
                for (int w = 0; w < 7; ++w, ++wk) {
                    if (!blackAfter[w] || w >= 6) continue;
                    // semitone = whiteToSemi[w]+1, always a black key
                    const int semi = whiteToSemi[w] + 1;
                    const int midi = base_midi + oct*12 + semi;
                    const float cx = origin.x + (wk+1) * wk_w;
                    const float x0 = cx - bk_w * 0.5f;
                    const float y0 = origin.y;
                    const bool pressed = (clicking && midi == clicked_midi);
                    const ImU32 col = pressed
                        ? IM_COL32(60, 120, 200, 255)
                        : IM_COL32(30, 30, 30, 255);
                    dl->AddRectFilled({x0, y0}, {x0+bk_w, y0+bk_h}, col, 2.f);
                    dl->AddRect({x0, y0}, {x0+bk_w, y0+bk_h}, IM_COL32(0,0,0,255), 2.f);
                }
            }
        }

        // Advance cursor past the keyboard
        ImGui::Dummy({wk_w * n_white, wk_h});
    }

    // =======================================================================
    //  DEBUG CONSOLE WINDOW
    // =======================================================================
    void RenderDebugConsole()
    {
        ImGui::SetNextWindowSize({720, 400}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({80, 80},    ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Debug Console##win", &st.show_debug_console,
                          ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        if (ImGui::BeginTabBar("##debug_tabs"))
        {
            // -----------------------------------------------------------------
            //  LOG TAB
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Log"))
            {
                static int    s_min_level      = 0;   // 0=Info, 1=Warn, 2=Error
                static int    s_min_level_prev = -1;
                static size_t s_log_size_cached = 0;
                static std::string s_log_buf;          // plain-text buffer for InputTextMultiline

                // Toolbar row
                if (ImGui::SmallButton("Clear")) {
                    std::lock_guard<std::mutex> lk(g_log_mutex);
                    g_log_entries.clear();
                    s_log_buf.clear();
                    s_log_size_cached = 0;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy All"))
                    ImGui::SetClipboardText(s_log_buf.c_str());
                ImGui::SameLine();

                // Level filter
                ImGui::SetNextItemWidth(120.f);
                ImGui::Combo("Level", &s_min_level, "Info\0Warning\0Error\0");
                ImGui::SameLine();

                // Entry count badge
                {
                    std::lock_guard<std::mutex> lk(g_log_mutex);
                    ImGui::TextDisabled("%zu entries", g_log_entries.size());
                }

                ImGui::Separator();

                // Rebuild plain-text buffer when entries change or filter changes
                {
                    std::lock_guard<std::mutex> lk(g_log_mutex);
                    if (g_log_entries.size() != s_log_size_cached ||
                        s_min_level != s_min_level_prev)
                    {
                        s_log_size_cached = g_log_entries.size();
                        s_min_level_prev  = s_min_level;
                        s_log_buf.clear();
                        for (const auto& e : g_log_entries) {
                            if (static_cast<int>(e.level) < s_min_level) continue;
                            const char* badge =
                                (e.level == LogLevel::Error) ? "[ERR] " :
                                (e.level == LogLevel::Warn)  ? "[WRN] " : "[INF] ";
                            s_log_buf += e.timestamp + "  " + badge + e.text + "\n";
                        }
                    }
                }

                // Scroll-to-bottom: move cursor to end via callback so
                // InputTextMultiline scrolls to keep the cursor visible.
                struct ScrollCB {
                    static int fn(ImGuiInputTextCallbackData* data) {
                        if (g_log_scroll_to_bottom) {
                            data->CursorPos        = data->BufTextLen;
                            g_log_scroll_to_bottom = false;
                        }
                        return 0;
                    }
                };

                const float footer_h = ImGui::GetStyle().ItemSpacing.y
                                     + ImGui::GetFrameHeightWithSpacing();

                // Monospace font makes columns align; use default if none loaded.
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0.08f, 0.08f, 0.10f, 1.f});
                ImGui::InputTextMultiline(
                    "##log_out",
                    s_log_buf.empty() ? const_cast<char*>("") : s_log_buf.data(),
                    s_log_buf.size() + 1,
                    {-1.f, -footer_h},
                    ImGuiInputTextFlags_ReadOnly |
                    ImGuiInputTextFlags_CallbackAlways,
                    ScrollCB::fn);
                ImGui::PopStyleColor();

                // Status line
                {
                    std::lock_guard<std::mutex> lk(g_log_mutex);
                    if (!g_log_entries.empty()) {
                        const auto& last = g_log_entries.back();
                        ImVec4 col = (last.level == LogLevel::Error) ? ImVec4{1.f,0.35f,0.35f,1.f}
                                   : (last.level == LogLevel::Warn)  ? ImVec4{1.f,0.85f,0.20f,1.f}
                                   :                                    ImVec4{0.6f,0.6f,0.6f,1.f};
                        ImGui::TextColored(col, "Last: [%s] %s",
                                           last.timestamp.c_str(), last.text.c_str());
                    }
                }

                ImGui::EndTabItem();
            }

            // -----------------------------------------------------------------
            //  DIAGNOSTICS TAB
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Diagnostics"))
            {
                if (ImGui::BeginTable("##diag", 2,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Property",  ImGuiTableColumnFlags_WidthFixed, 200.f);
                    ImGui::TableSetupColumn("Value");
                    ImGui::TableHeadersRow();

                    auto Row = [](const char* k, const char* fmt, ...) {
                        char vbuf[256];
                        va_list ap;
                        va_start(ap, fmt);
                        std::vsnprintf(vbuf, sizeof(vbuf), fmt, ap);
                        va_end(ap);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(vbuf);
                    };

                    // --- Audio ---
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored({0.4f,0.8f,1.f,1.f}, "AUDIO");
                    ImGui::TableNextColumn();

                    Row("Sample Rate",    "%u Hz", g_audio.sample_rate);
                    Row("Channels",       "%u",    g_audio.channels);
                    Row("Master Volume",  "%.0f %%", static_cast<double>(g_master_volume.load()) * 100.0);

                    // --- MIDI ---
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored({0.4f,1.f,0.6f,1.f}, "MIDI");
                    ImGui::TableNextColumn();

                    {
                        const bool playing = g_midi_playing.load();
                        const bool paused  = g_midi_paused.load();
                        Row("State",
                            playing ? (paused ? "Paused" : "Playing") : "Stopped");
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_now_playing_mutex);
                        Row("File", "%s",
                            g_now_playing_name.empty() ? "(none)" : g_now_playing_name.c_str());
                    }
                    {
                        const uint64_t elUs  = g_midi_elapsed_us.load();
                        const uint64_t totUs = g_midi_total_us.load();
                        const uint32_t elS   = static_cast<uint32_t>(elUs  / 1'000'000u);
                        const uint32_t totS  = static_cast<uint32_t>(totUs / 1'000'000u);
                        Row("Position", "%u:%02u / %u:%02u",
                            elS/60, elS%60, totS/60, totS%60);
                    }

                    // --- SF2 ---
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored({1.f,0.8f,0.4f,1.f}, "SF2");
                    ImGui::TableNextColumn();

                    Row("Loaded", "%s", g_sf2_engine.loaded ? "Yes" : "No");
                    if (g_sf2_engine.loaded) {
                        const std::string sfname = std::filesystem::path(
                            g_sf2_engine.path).filename().string();
                        Row("SF2 File",  "%s", sfname.c_str());
                        Row("Presets",      "%d",
                            tsf_get_presetcount(g_sf2_engine.sf2));
                    }

                    // --- Voices ---
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored({1.f,0.5f,0.8f,1.f}, "VOICES");
                    ImGui::TableNextColumn();

                    {
                        int active = 0;
                        // Read without lock (best-effort display)
                        for (const auto& v : g_audio.voices)
                            if (v.active()) ++active;
                        Row("Active voices", "%d / %u", active, MAX_VOICES);
                    }

                    // --- Log ---
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored({0.7f,0.7f,0.7f,1.f}, "LOG");
                    ImGui::TableNextColumn();

                    {
                        std::lock_guard<std::mutex> lk(g_log_mutex);
                        Row("Entries", "%zu / %zu",
                            g_log_entries.size(), LOG_MAX_ENTRIES);
                        // Count by level
                        int ni=0, nw=0, ne=0;
                        for (const auto& e : g_log_entries) {
                            if      (e.level == LogLevel::Error) ++ne;
                            else if (e.level == LogLevel::Warn)  ++nw;
                            else                                  ++ni;
                        }
                        Row("Info / Warning / Error", "%d / %d / %d", ni, nw, ne);
                    }

                    // --- FPS / Frame ---
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored({0.7f,0.7f,0.7f,1.f}, "UI");
                    ImGui::TableNextColumn();

                    Row("FPS",          "%.1f",  static_cast<double>(ImGui::GetIO().Framerate));
                    Row("Delta Time",   "%.3f ms",
                        static_cast<double>(ImGui::GetIO().DeltaTime * 1000.f));

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    // =======================================================================
    //  MIDI OUT DEVICE SELECTOR
    // =======================================================================
    void RenderMidiOutDevices()
    {
        ImGui::SetNextWindowSize({500, 300}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({200, 150}, ImGuiCond_FirstUseEver);

        ImGui::Begin("MIDI Out Devices##win", &st.show_midiout_devices,
                     ImGuiWindowFlags_NoCollapse);

        ImGui::TextUnformatted("Select a Windows MIDI output device:");
        ImGui::TextDisabled("432 Hz tuning is applied via RPN 0 pitch-bend range = 1 semitone.");
        ImGui::Separator();

        if (g_midiout_devices.empty()) {
            ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f}, "No MIDI output devices found.");
        } else {
            for (size_t i = 0; i < g_midiout_devices.size(); ++i) {
                const auto& dev = g_midiout_devices[i];
                const bool active = g_midiout.opened &&
                                    g_midiout.device_id == static_cast<int>(i);
                if (active)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.5f, 1.f));

                const bool sel = ImGui::Selectable(dev.name.c_str(), active,
                                                   ImGuiSelectableFlags_AllowDoubleClick);
                if (active)
                    ImGui::PopStyleColor();

                if (sel && ImGui::IsMouseDoubleClicked(0)) {
                    stopMidiThread();
                    g_midiout.close();
                    if (g_midiout.open(static_cast<int>(i)))
                        g_instr_model = InstrModel::MIDI_OUT;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Double-click to open device %d: %s",
                                      static_cast<int>(i), dev.name.c_str());
            }
        }

        ImGui::Separator();
        if (g_midiout.opened) {
            if (ImGui::Button("Close Device")) {
                stopMidiThread();
                g_midiout.close();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            g_midiout_devices = MidiOutEngine::enumerate();
        }

        ImGui::End();
    }

    // =======================================================================
    //  ABOUT
    // =======================================================================
    void RenderAbout()
    {
        ImGui::SetNextWindowSize({420, 320}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({200, 150}, ImGuiCond_FirstUseEver);

        if (ImGui::Begin("About##win", &st.show_about,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, {0.7f, 1.0f, 0.7f, 1.f});
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextUnformatted("432 Hz Bit-Perfect Synthesiser");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::TextWrapped(
                "Integer-scale polyphonic synthesiser and MIDI player.\n"
                "A4 = 432 Hz (tunable 200-600 Hz) with pure rational intervals.\n"
                "Audio via RtAudio (ASIO / WASAPI exclusive-mode bit-perfect output).\n"
                "Synthesis via TinySoundFont (SF2) + custom Karplus-Strong / FM / Organ.\n"
                "Reverb via Freeverb (Jezar at Dreampoint).\n"
                "UI via Dear ImGui."
            );

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextUnformatted("Keyboard shortcuts:");
            if (ImGui::BeginTable("##kshort", 2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                auto Row = [](const char* k, const char* v) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
                };
                Row("Space",      "Stop playback");
                Row("Ctrl+M",     "Open MIDI folder");
                Row("Ctrl+F",     "Open SF2 folder");
                Row("Ctrl+D",     "Show Audio Devices");
                Row("Alt+F4",     "Exit");
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("MIDI browser: double-click to play");
            ImGui::TextDisabled("SF2 browser:  double-click to load");
            ImGui::TextDisabled("Preset list:  click to switch instantly");
        }
        ImGui::End();
    }

};

