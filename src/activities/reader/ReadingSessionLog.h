#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

// One logged session as shown in the "Recent Sessions" list.
struct RecentEntry {
  std::string line;    // the exact CSV row, used to find it again when deleting
  std::string prefix;  // time and duration, e.g. "10/19 4:01p | 35m | "
  std::string title;   // full book title; the reader shortens it to fit the screen
  std::string label() const { return prefix + title; }
};

// Up to maxCount most recent sessions, newest first.
std::vector<RecentEntry> loadRecent(size_t maxCount);

// Removes one row equal to `line` from the log and appends it to
// /reading_sessions_deleted.csv, so a mistaken delete can be undone by hand.
bool deleteEntry(const std::string& line);

// The manual session that's running right now (not in the file yet), so stats can
// include it. startMs is millis() when it started; start is its local start time
// (nullptr when the clock isn't set).
void setLiveSession(const std::string& title, unsigned long startMs, const ReadingStatsDateTime* start);
void clearLiveSession();

// One logged session, as the stats screens use it.
struct SessionRecord {
  std::string title;
  ReadingStatsDateTime start;  // local time; only meaningful when hasStart
  bool hasStart = false;
  uint32_t seconds = 0;  // elapsed time, or active time when elapsed is empty
};

// Calls onSession for every logged session (oldest first), then for the live
// session if one is running. Reads the file line by line.
void forEachSession(const std::function<void(const SessionRecord&)>& onSession);

// Per-book numbers for the stats screen, all from the log. Each session's time is
// its elapsed time (active time when elapsed is empty). The live session, if it's
// for this book, counts as one more session.
struct BookTotals {
  uint32_t sessions = 0;
  uint32_t totalSeconds = 0;
  uint32_t todaySeconds = 0;  // sessions that started on `today`; 0 when today is unknown
};

// One pass over the log. `today` may be invalid (clock not set).
BookTotals totalsForTitle(const std::string& title, const ReadingStatsDate& today);

}  // namespace ReadingSessionLog
