#ifndef HOMEBUTTONS_RESET_SCHEDULE_H
#define HOMEBUTTONS_RESET_SCHEDULE_H

#include <time.h>

#include "config.h"
#include "logger.h"
#include "types.h"

// When the counters clear themselves.
//
// The device has no clock of its own worth trusting and deliberately knows
// nothing about timezones: every webhook response carries a UTC epoch and
// the current offset in seconds, and the offset is whatever the receiver
// says it is, DST included. Everything here works in local seconds derived
// from those two numbers.
//
// The spec is a single free-text portal field so one string covers every
// mode without four more inputs to fill in:
//
//     off
//     daily 03:00
//     weekly mon 03:00
//     monthly 1 03:00
//
// An unparseable spec falls back to the default rather than disabling the
// reset silently.
namespace reset_schedule {

enum class Mode : uint8_t { kOff, kDaily, kWeekly, kMonthly };

struct Spec {
  Mode mode = Mode::kDaily;
  uint16_t minute_of_day = 3 * 60;  // 03:00 local
  uint8_t weekday = 1;              // 0 = Sunday, matches struct tm
  uint8_t day_of_month = 1;         // capped at 28, see parse()
};

const char* mode_name(Mode mode);

// Never fails: an unrecognised spec logs and yields the default.
Spec parse(const char* text, const Logger& log);

// Renders a spec back to its canonical text, for the portal field.
StaticString<RESET_SPEC_MAXLEN> format(const Spec& spec);

// The reset period a given local time falls in, as a plain integer that
// only changes when a boundary is crossed. Comparing this against the
// stored value is the whole reset test - no date arithmetic at the call
// site, and it works identically for all three modes.
//
// Returns 0 for Mode::kOff, which never matches a stored non-zero period.
int32_t period_of(const Spec& spec, time_t local_time);

// Seconds from local_time until the next boundary, clamped into the range
// the deep sleep timer accepts. Returns 0 when the mode is off, meaning
// "no scheduled wake, fall back to the heartbeat interval".
uint32_t seconds_until_next(const Spec& spec, time_t local_time);

}  // namespace reset_schedule

#endif  // HOMEBUTTONS_RESET_SCHEDULE_H
