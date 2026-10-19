#include "ReadingSessionLog.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>

#include "CrossPointSettings.h"

namespace ReadingSessionLog {
namespace {

constexpr char LOG_PATH[] = "/reading_sessions.csv";
constexpr char LOG_HEADER[] = "start,end,active_seconds,title,path\n";

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

}  // namespace

void append(const ReadingStatsDateTime* sessionStart, const uint32_t activeSeconds, const std::string& title,
            const std::string& path) {
  if (activeSeconds < MIN_LOGGED_SECONDS) return;

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
