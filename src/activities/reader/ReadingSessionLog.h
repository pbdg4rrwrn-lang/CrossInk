#pragma once
#include <cstdint>
#include <string>

#include "ReadingStatsUtils.h"

// Appends one row per finished reading session to /reading_sessions.csv on the
// SD card, so sessions can be imported into a time tracker later. CrossInk's own
// stats keep only totals; this keeps each session.
//
// Columns: start,end,active_seconds,elapsed_seconds,title,path
//   start / end     local time with UTC offset (ISO 8601), e.g. 2026-10-19T14:52-05:00;
//                   empty when the clock is not set
//   active_seconds  time spent on pages (CrossInk's measure: page-turn redraws not
//                   counted, each page rounded down to whole seconds)
//   elapsed_seconds clock time from start to stop; empty when unknown (XTC books)
//
// A file with an older header is renamed to reading_sessions_v1.csv (rows kept)
// and a fresh file is started, so one file never mixes two column layouts.
//
// Sessions shorter than MIN_LOGGED_SECONDS are skipped. 10 s matches the point
// where CrossInk starts adding time to its own totals.
namespace ReadingSessionLog {

constexpr uint32_t MIN_LOGGED_SECONDS = 10;

// Pass as elapsedSeconds when clock time isn't tracked; the column stays empty.
constexpr uint32_t ELAPSED_UNKNOWN = UINT32_MAX;

// CrossInk normally drops a page's time entirely when it stays on screen longer
// than the idle threshold (Settings, default 5 min). This fork turns that off so
// slow pages still count; the auto-sleep timeout ends a session instead.
// Set to true to restore CrossInk's behavior. Affects CrossInk's own totals too.
// Reading-pace estimates (time left) keep their own threshold either way.
constexpr bool APPLY_IDLE_THRESHOLD_TO_SESSIONS = false;

void append(const ReadingStatsDateTime* sessionStart, uint32_t activeSeconds, uint32_t elapsedSeconds,
            const std::string& title, const std::string& path);

}  // namespace ReadingSessionLog
