#include "reset_schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace reset_schedule {
namespace {

constexpr int64_t kSecondsPerDay = 24LL * 60LL * 60LL;

const char* const kWeekdayNames[7] = {"sun", "mon", "tue", "wed",
                                      "thu", "fri", "sat"};

// Local time here is a UTC epoch already shifted by the offset the receiver
// reported, so plain UTC calendar functions are the correct ones. TZ is
// never set on this device and tzset() is never called.
struct tm to_tm(int64_t local_time) {
  time_t t = static_cast<time_t>(local_time);
  struct tm out = {};
  gmtime_r(&t, &out);
  return out;
}

// Floor division: C truncates toward zero, which would put times before the
// boundary on the wrong day for negative values.
int64_t floordiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
// Used to build a boundary date; extraction goes through gmtime_r.
int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

bool parse_hhmm(const char* text, uint16_t& minute_of_day) {
  if (text == nullptr) return false;
  char* end = nullptr;
  long hour = strtol(text, &end, 10);
  if (end == text || *end != ':') return false;
  const char* min_start = end + 1;
  long minute = strtol(min_start, &end, 10);
  if (end == min_start) return false;
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
  minute_of_day = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

int weekday_from_name(const char* text) {
  for (int i = 0; i < 7; i++) {
    if (strncasecmp(text, kWeekdayNames[i], 3) == 0) return i;
  }
  return -1;
}

// Start of the period containing local_time, in days since the epoch,
// measured in boundary-shifted space.
int64_t period_day(const Spec& spec, int64_t shifted) {
  return floordiv(shifted, kSecondsPerDay);
}

}  // namespace

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::kOff:
      return "off";
    case Mode::kDaily:
      return "daily";
    case Mode::kWeekly:
      return "weekly";
    case Mode::kMonthly:
      return "monthly";
    default:
      return "unknown";
  }
}

Spec parse(const char* text, bool* ok) {
  Spec spec;  // defaults to daily 03:00
  if (ok != nullptr) *ok = true;
  auto fail = [&]() {
    if (ok != nullptr) *ok = false;
  };
  if (text == nullptr || *text == '\0') return spec;

  char buf[kSpecMaxLen + 1] = {};
  snprintf(buf, sizeof(buf), "%s", text);

  char* save = nullptr;
  const char* mode_tok = strtok_r(buf, " \t", &save);
  if (mode_tok == nullptr) {
    fail();
    return spec;
  }

  if (strcasecmp(mode_tok, "off") == 0) {
    spec.mode = Mode::kOff;
    return spec;
  }

  if (strcasecmp(mode_tok, "daily") == 0) {
    spec.mode = Mode::kDaily;
    if (!parse_hhmm(strtok_r(nullptr, " \t", &save), spec.minute_of_day)) {
      fail();
    }
    return spec;
  }

  if (strcasecmp(mode_tok, "weekly") == 0) {
    spec.mode = Mode::kWeekly;
    const char* day_tok = strtok_r(nullptr, " \t", &save);
    int weekday = (day_tok != nullptr) ? weekday_from_name(day_tok) : -1;
    if (weekday < 0) {
      fail();
      weekday = 1;
    }
    spec.weekday = static_cast<uint8_t>(weekday);
    if (!parse_hhmm(strtok_r(nullptr, " \t", &save), spec.minute_of_day)) {
      fail();
    }
    return spec;
  }

  if (strcasecmp(mode_tok, "monthly") == 0) {
    spec.mode = Mode::kMonthly;
    const char* day_tok = strtok_r(nullptr, " \t", &save);
    long day = (day_tok != nullptr) ? strtol(day_tok, nullptr, 10) : 0;
    // Capped at 28 deliberately: 29-31 would skip short months, which is a
    // surprising way for a reset to quietly not happen.
    if (day < 1 || day > 28) {
      fail();
      day = 1;
    }
    spec.day_of_month = static_cast<uint8_t>(day);
    if (!parse_hhmm(strtok_r(nullptr, " \t", &save), spec.minute_of_day)) {
      fail();
    }
    return spec;
  }

  fail();
  return Spec{};
}

void format(const Spec& spec, char* out, size_t out_size) {
  if (out == nullptr || out_size == 0) return;
  const int hour = spec.minute_of_day / 60;
  const int minute = spec.minute_of_day % 60;
  switch (spec.mode) {
    case Mode::kOff:
      snprintf(out, out_size, "off");
      break;
    case Mode::kWeekly:
      snprintf(out, out_size, "weekly %s %02d:%02d",
               kWeekdayNames[spec.weekday % 7], hour, minute);
      break;
    case Mode::kMonthly:
      snprintf(out, out_size, "monthly %u %02d:%02d", spec.day_of_month, hour,
               minute);
      break;
    case Mode::kDaily:
    default:
      snprintf(out, out_size, "daily %02d:%02d", hour, minute);
      break;
  }
}

int32_t period_of(const Spec& spec, time_t local_time) {
  if (spec.mode == Mode::kOff) return 0;

  // Shift back by the boundary time so a period starts at the reset moment
  // rather than at local midnight; everything below is then whole days.
  const int64_t shifted =
      static_cast<int64_t>(local_time) - static_cast<int64_t>(spec.minute_of_day) * 60;
  const int64_t day_number = period_day(spec, shifted);

  switch (spec.mode) {
    case Mode::kDaily:
      // +1 throughout so a valid period is never 0, which means "unset".
      return static_cast<int32_t>(day_number + 1);

    case Mode::kWeekly: {
      // 1970-01-01 was a Thursday (weekday 4); align periods to the
      // configured weekday.
      const int64_t offset = (4 - static_cast<int64_t>(spec.weekday) + 7) % 7;
      return static_cast<int32_t>(floordiv(day_number + offset, 7) + 1);
    }

    case Mode::kMonthly: {
      struct tm t = to_tm(shifted);
      int32_t months = (t.tm_year + 1900) * 12 + t.tm_mon;
      if (t.tm_mday < static_cast<int>(spec.day_of_month)) months -= 1;
      return months + 1;
    }

    default:
      return 0;
  }
}

uint32_t seconds_until_next(const Spec& spec, time_t local_time) {
  if (spec.mode == Mode::kOff) return 0;

  const int64_t boundary_offset = static_cast<int64_t>(spec.minute_of_day) * 60;
  const int64_t shifted = static_cast<int64_t>(local_time) - boundary_offset;
  const int64_t day_number = period_day(spec, shifted);

  int64_t boundary_day = 0;
  switch (spec.mode) {
    case Mode::kDaily:
      boundary_day = day_number + 1;
      break;

    case Mode::kWeekly: {
      const int64_t offset = (4 - static_cast<int64_t>(spec.weekday) + 7) % 7;
      boundary_day = (floordiv(day_number + offset, 7) + 1) * 7 - offset;
      break;
    }

    case Mode::kMonthly: {
      struct tm t = to_tm(shifted);
      int year = t.tm_year + 1900;
      int month = t.tm_mon;  // 0-based
      if (t.tm_mday >= static_cast<int>(spec.day_of_month)) {
        month++;
        if (month > 11) {
          month = 0;
          year++;
        }
      }
      boundary_day =
          days_from_civil(year, static_cast<unsigned>(month + 1),
                          static_cast<unsigned>(spec.day_of_month));
      break;
    }

    default:
      return 0;
  }

  const int64_t boundary =
      boundary_day * kSecondsPerDay + boundary_offset;
  int64_t delta = boundary - static_cast<int64_t>(local_time);

  // Clamped into what the deep sleep timer accepts. kWakeMaxSeconds is
  // 24h, so weekly and monthly simply wake once a day and re-evaluate -
  // which also keeps the clock resynced rather than drifting for a month.
  if (delta < static_cast<int64_t>(kWakeMinSeconds)) {
    delta = kWakeMinSeconds;
  }
  if (delta > static_cast<int64_t>(kWakeMaxSeconds)) {
    delta = kWakeMaxSeconds;
  }
  return static_cast<uint32_t>(delta);
}

Action decide(int32_t stored_period, int32_t current_period) {
  // Schedule off: period_of() yields 0, which must never match a stored
  // period and must never clear.
  if (current_period == 0) return Action::kNone;

  // Nothing usable stored. Either the date has never been known, or the
  // schedule was just changed and the stored value counts a different unit
  // - days for daily, weeks for weekly, months for monthly. Adopt rather
  // than clear, so setting a device up does not wipe a count it was just
  // given.
  if (stored_period == 0) return Action::kAdopt;

  if (current_period == stored_period) return Action::kNone;

  // Local time moved backwards: a clock correction, or the autumn DST step
  // handing back an hour. Hold the stored period rather than adopting the
  // earlier one. Adopting would re-arm a boundary that has already fired,
  // so crossing it again on the way forward would clear a second time and
  // post a second report for one scheduled reset, losing every count taken
  // in between.
  if (current_period < stored_period) return Action::kHold;

  return Action::kClear;
}

}  // namespace reset_schedule
