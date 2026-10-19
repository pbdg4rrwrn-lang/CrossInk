#include "ReadingSessionLog.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"

namespace ReadingSessionLog {
namespace {

constexpr char LOG_PATH[] = "/reading_sessions.csv";
constexpr char LOG_HEADER[] = "start,end,active_seconds,elapsed_seconds,title,path\n";
constexpr char ARCHIVE_PATH[] = "/reading_sessions_v1.csv";
constexpr char ARCHIVE_PATH_PATTERN[] = "/reading_sessions_v1_%u.csv";
constexpr unsigned MAX_ARCHIVES = 100;

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

}  // namespace ReadingSessionLog
