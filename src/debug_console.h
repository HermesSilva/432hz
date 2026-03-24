#pragma once
// =============================================================================
//  debug_console.h  —  Thread-safe application logger / debug console
//
//  Usage (from any thread):
//      AppLogInfo ("MIDI: loading " + path);
//      AppLogWarn ("MIDI: no events found in " + name);
//      AppLogError("MIDI: " + mf.error);
//
//  The ImGui window is rendered by GuiApp::RenderDebugConsole() in gui_app.h.
//  Include this header from main.cpp BEFORE gui_app.h.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
//  Log entry
// ---------------------------------------------------------------------------
enum class LogLevel : uint8_t { Info = 0, Warn = 1, Error = 2 };

struct LogEntry {
    LogLevel    level;
    std::string timestamp;  // HH:MM:SS
    std::string text;
};

// ---------------------------------------------------------------------------
//  Global state (inline = single definition across TUs, C++17)
// ---------------------------------------------------------------------------
inline std::mutex           g_log_mutex;
inline std::deque<LogEntry> g_log_entries;
inline bool                 g_log_scroll_to_bottom = false;

static constexpr size_t LOG_MAX_ENTRIES = 2000;

// ---------------------------------------------------------------------------
//  Append one entry
// ---------------------------------------------------------------------------
inline void AppLog(LogLevel level, const std::string& msg)
{
    // Build HH:MM:SS timestamp
    const auto now  = std::chrono::system_clock::to_time_t(
                          std::chrono::system_clock::now());
    struct tm tm_info{};
#if defined(_WIN32)
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif
    char ts[12];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d",
                  tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (g_log_entries.size() >= LOG_MAX_ENTRIES)
        g_log_entries.pop_front();
    g_log_entries.push_back({ level, ts, msg });
    g_log_scroll_to_bottom = true;
}

inline void AppLogInfo (const std::string& msg) { AppLog(LogLevel::Info,  msg); }
inline void AppLogWarn (const std::string& msg) { AppLog(LogLevel::Warn,  msg); }
inline void AppLogError(const std::string& msg) { AppLog(LogLevel::Error, msg); }
