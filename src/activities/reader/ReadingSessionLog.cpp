#include "ReadingSessionLog.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

#include "CrossPointSettings.h"

namespace ReadingSessionLog {
namespace {

constexpr char LOG_PATH[] = "/reading_sessions.csv";
constexpr char LOG_HEADER[] = "start,end,active_seconds,elapsed_seconds,title,path\n";
constexpr char ARCHIVE_PATH[] = "/reading_sessions_v1.csv";
constexpr char ARCHIVE_PATH_PATTERN[] = "/reading_sessions_v1_%u.csv";
constexpr unsigned MAX_ARCHIVES = 100;
constexpr char DELETED_PATH[] = "/reading_sessions_deleted.csv";
constexpr char TMP_PATH[] = "/reading_sessions.csv.tmp";
constexpr char BAK_PATH[] = "/reading_sessions.csv.bak";

// Runaway guard so a bug can never fill the card. A row is ~150 bytes, so this
// holds roughly 13,000 sessions (years of reading).
constexpr size_t MAX_LOG_BYTES = 2u * 1024u * 1024u;

// "2026-10-19T14:52-05:00" plus terminator.
constexpr size_t TIMESTAMP_LEN = 24;

// Formats a local time with the device's UTC offset, or an empty string when
// there is no valid time.
void formatTimestamp(char* buf, const size_t len, const ReadingStatsDateTime* time) {
  buf[0] = '\0';
  if (!time || !time->isValid()) return;

  const int offsetMinutes = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  const char sign = offsetMinutes < 0 ? '-' : '+';
  const int absOffset = std::abs(offsetMinutes);

  snprintf(buf, len, "%04u-%02u-%02uT%02u:%02u%c%02d:%02d", static_cast<unsigned>(time->date.year),
           static_cast<unsigned>(time->date.month), static_cast<unsigned>(time->date.day),
           static_cast<unsigned>(time->hour), static_cast<unsigned>(time->minute), sign, absOffset / 60,
           absOffset % 60);
}

// Wraps a value in quotes and doubles any quotes inside it, so commas or quotes
// in a book title can't break the CSV columns.
std::string csvField(const std::string& value) {
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"') out += '"';
    if (c == '\n' || c == '\r') {
      out += ' ';
      continue;
    }
    out += c;
  }
  out += '"';
  return out;
}

// True when the file is missing, empty, or already uses LOG_HEADER. Otherwise
// moves the old file aside so the new layout starts in a clean file.
bool prepareLogSchema() {
  if (!Storage.exists(LOG_PATH)) return true;

  HalFile file = Storage.open(LOG_PATH, O_RDONLY);
  if (!file) {
    LOG_ERR("SESSION", "Failed to inspect %s", LOG_PATH);
    return false;
  }
  constexpr size_t headerLen = sizeof(LOG_HEADER) - 1;
  char header[sizeof(LOG_HEADER)];
  const bool empty = file.size() == 0;
  const int read = empty ? 0 : file.read(header, headerLen);
  file.close();
  if (empty) return true;
  if (read == static_cast<int>(headerLen) && memcmp(header, LOG_HEADER, headerLen) == 0) return true;

  char archivePath[40];
  for (unsigned index = 0; index < MAX_ARCHIVES; ++index) {
    if (index == 0) {
      snprintf(archivePath, sizeof(archivePath), "%s", ARCHIVE_PATH);
    } else {
      snprintf(archivePath, sizeof(archivePath), ARCHIVE_PATH_PATTERN, index);
    }
    if (Storage.exists(archivePath)) continue;
    if (!Storage.rename(LOG_PATH, archivePath)) {
      LOG_ERR("SESSION", "Failed to move old log to %s", archivePath);
      return false;
    }
    LOG_INF("SESSION", "Old-format log kept as %s", archivePath);
    return true;
  }
  LOG_ERR("SESSION", "No free name to keep the old log; not appending");
  return false;
}

// Calls onLine(line) for every line of the file at `path` (without the newline),
// reading in small chunks so a large log never needs much memory.
template <typename Fn>
bool forEachLine(const char* path, Fn onLine) {
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  char buf[256];
  std::string current;
  int n = 0;
  while ((n = file.read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        onLine(current);
        current.clear();
      } else if (buf[i] != '\r') {
        current += buf[i];
      }
    }
  }
  if (!current.empty()) onLine(current);
  file.close();
  return true;
}

// Splits one CSV row into fields, honouring "quoted, fields" and "" escapes.
std::vector<std::string> splitCsv(const std::string& line) {
  std::vector<std::string> fields(1);
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        fields.back() += '"';
        ++i;
      } else if (c == '"') {
        quoted = false;
      } else {
        fields.back() += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      fields.emplace_back();
    } else {
      fields.back() += c;
    }
  }
  return fields;
}

// "2026-10-19T16:01-05:00" -> "10/19 4:01 PM"; empty or malformed -> "No time".
std::string formatListTime(const std::string& iso) {
  unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0;
  if (sscanf(iso.c_str(), "%4u-%2u-%2uT%2u:%2u", &year, &month, &day, &hour, &minute) != 5) return "No time";
  const unsigned hour12 = hour % 12 == 0 ? 12 : hour % 12;
  char buf[24];
  snprintf(buf, sizeof(buf), "%u/%u %u:%02u%s", month, day, hour12, minute, hour < 12 ? "a" : "p");
  return buf;
}

std::string formatListDuration(const uint32_t seconds) {
  char buf[24];
  if (seconds < 60) {
    snprintf(buf, sizeof(buf), "%us", static_cast<unsigned>(seconds));
  } else if (seconds < 3600) {
    snprintf(buf, sizeof(buf), "%um", static_cast<unsigned>((seconds + 30) / 60));
  } else {
    const unsigned minutes = static_cast<unsigned>((seconds + 30) / 60);
    snprintf(buf, sizeof(buf), "%uh %um", minutes / 60, minutes % 60);
  }
  return buf;
}

RecentEntry entryForRow(const std::string& line) {
  const std::vector<std::string> f = splitCsv(line);
  if (f.size() < 6) return {line, "", line};
  // Prefer clock time (elapsed); fall back to active time when it's empty.
  const std::string& durationText = !f[3].empty() ? f[3] : f[2];
  const uint32_t seconds = static_cast<uint32_t>(strtoul(durationText.c_str(), nullptr, 10));
  return {line, formatListTime(f[0]) + " | " + formatListDuration(seconds) + " | ", f[4]};
}

bool appendToDeletedLog(const std::string& line) {
  HalFile file = Storage.open(DELETED_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return false;
  bool ok = true;
  if (file.size() == 0) {
    constexpr size_t headerLen = sizeof(LOG_HEADER) - 1;
    ok = file.write(LOG_HEADER, headerLen) == headerLen;
  }
  const std::string row = line + "\n";
  if (ok) ok = file.write(row.c_str(), row.size()) == row.size();
  file.close();
  return ok;
}

}  // namespace

void append(const ReadingStatsDateTime* sessionStart, const uint32_t activeSeconds, const uint32_t elapsedSeconds,
            const std::string& title, const std::string& path) {
  if (activeSeconds < MIN_LOGGED_SECONDS) return;
  if (!prepareLogSchema()) return;

  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("SESSION", "Failed to open %s for append", LOG_PATH);
    return;
  }

  const size_t existingBytes = file.size();
  if (existingBytes >= MAX_LOG_BYTES) {
    file.close();
    LOG_ERR("SESSION", "%s hit the size cap; not appending", LOG_PATH);
    return;
  }

  bool ok = true;
  if (existingBytes == 0) {
    constexpr size_t headerLen = sizeof(LOG_HEADER) - 1;
    ok = file.write(LOG_HEADER, headerLen) == headerLen;
  }

  ReadingStatsDateTime now;
  const bool hasNow = getCurrentLocalReadingStatsDateTime(now);

  char start[TIMESTAMP_LEN];
  char end[TIMESTAMP_LEN];
  formatTimestamp(start, sizeof(start), sessionStart);
  formatTimestamp(end, sizeof(end), hasNow ? &now : nullptr);

  std::string row = start;
  row += ',';
  row += end;
  row += ',';
  row += std::to_string(activeSeconds);
  row += ',';
  if (elapsedSeconds != ELAPSED_UNKNOWN) row += std::to_string(elapsedSeconds);
  row += ',';
  row += csvField(title);
  row += ',';
  row += csvField(path);
  row += '\n';

  if (ok) {
    ok = file.write(row.c_str(), row.size()) == row.size();
  }
  file.close();

  if (ok) {
    LOG_INF("SESSION", "Logged %u s session: %s", static_cast<unsigned>(activeSeconds), title.c_str());
  } else {
    LOG_ERR("SESSION", "Failed to write session row to %s", LOG_PATH);
  }
}

std::vector<RecentEntry> loadRecent(const size_t maxCount) {
  std::vector<RecentEntry> result;
  if (maxCount == 0 || !prepareLogSchema() || !Storage.exists(LOG_PATH)) return result;

  std::deque<std::string> recent;
  bool header = true;
  forEachLine(LOG_PATH, [&](const std::string& line) {
    if (header) {
      header = false;
      return;
    }
    if (line.empty()) return;
    recent.push_back(line);
    if (recent.size() > maxCount) recent.pop_front();
  });

  for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
    result.push_back(entryForRow(*it));
  }
  return result;
}

bool deleteEntry(const std::string& line) {
  if (line.empty() || !Storage.exists(LOG_PATH)) return false;
  if (Storage.exists(TMP_PATH)) Storage.remove(TMP_PATH);

  HalFile out = Storage.open(TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!out) {
    LOG_ERR("SESSION", "Failed to open %s", TMP_PATH);
    return false;
  }
  // Copy every line except the first exact match. Identical rows are the same
  // session, so removing any one of them is equivalent.
  bool removed = false;
  bool writeOk = true;
  const bool readOk = forEachLine(LOG_PATH, [&](const std::string& current) {
    if (!removed && current == line) {
      removed = true;
      return;
    }
    const std::string row = current + "\n";
    if (out.write(row.c_str(), row.size()) != row.size()) writeOk = false;
  });
  out.close();

  const bool ok = readOk && writeOk;
  if (!ok || !removed) {
    Storage.remove(TMP_PATH);
    LOG_ERR("SESSION", removed ? "Failed to rewrite session log" : "Session to delete not found");
    return false;
  }

  // Swap files with a backup step, so a power cut can't leave no log at all.
  if (Storage.exists(BAK_PATH)) Storage.remove(BAK_PATH);
  if (!Storage.rename(LOG_PATH, BAK_PATH)) {
    Storage.remove(TMP_PATH);
    LOG_ERR("SESSION", "Failed to back up session log before delete");
    return false;
  }
  if (!Storage.rename(TMP_PATH, LOG_PATH)) {
    Storage.rename(BAK_PATH, LOG_PATH);
    LOG_ERR("SESSION", "Failed to replace session log; restored backup");
    return false;
  }
  Storage.remove(BAK_PATH);

  if (!appendToDeletedLog(line)) LOG_ERR("SESSION", "Deleted, but failed to copy row to %s", DELETED_PATH);
  LOG_INF("SESSION", "Deleted session row");
  return true;
}

namespace {
bool liveActive = false;
std::string liveTitle;
unsigned long liveStartMs = 0;
}  // namespace

void setLiveSession(const std::string& title, const unsigned long startMs) {
  liveActive = true;
  liveTitle = title;
  liveStartMs = startMs;
}

void clearLiveSession() {
  liveActive = false;
  liveTitle.clear();
}

BookTotals totalsForTitle(const std::string& title, const ReadingStatsDate& today) {
  BookTotals totals;
  if (Storage.exists(LOG_PATH)) {
    bool header = true;
    forEachLine(LOG_PATH, [&](const std::string& line) {
      if (header) {
        header = false;
        return;
      }
      const std::vector<std::string> f = splitCsv(line);
      if (f.size() < 6 || f[4] != title) return;
      const std::string& durationText = !f[3].empty() ? f[3] : f[2];
      const uint32_t seconds = static_cast<uint32_t>(strtoul(durationText.c_str(), nullptr, 10));
      totals.sessions++;
      totals.totalSeconds += seconds;
      unsigned year = 0, month = 0, day = 0;
      if (today.isValid() && sscanf(f[0].c_str(), "%4u-%2u-%2u", &year, &month, &day) == 3 && year == today.year &&
          month == today.month && day == today.day) {
        totals.todaySeconds += seconds;
      }
    });
  }
  if (liveActive && liveTitle == title) {
    const uint32_t liveSeconds = static_cast<uint32_t>((millis() - liveStartMs + 500UL) / 1000UL);
    totals.sessions++;
    totals.totalSeconds += liveSeconds;
    if (today.isValid()) totals.todaySeconds += liveSeconds;
  }
  return totals;
}

}  // namespace ReadingSessionLog
