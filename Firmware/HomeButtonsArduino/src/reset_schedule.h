#ifndef HOMEBUTTONS_RESET_SCHEDULE_H
#define HOMEBUTTONS_RESET_SCHEDULE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

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

// Deliberately free of Arduino, Logger and config.h so the whole of this
// unit can be compiled and unit-tested on the host. Every calendar bug in
// here is the kind that only shows up months later on a device, which is
// exactly the code that should not need hardware to exercise.
static constexpr size_t kSpecMaxLen = 24;
static constexpr uint32_t kWakeMinSeconds = 5;
static constexpr uint32_t kWakeMaxSeconds = 24UL * 60UL * 60UL;

enum class Mode : uint8_t { kOff, kDaily, kWeekly, kMonthly };

struct Spec {
  Mode mode = Mode::kDaily;
  uint16_t minute_of_day = 3 * 60;  // 03:00 local
  uint8_t weekday = 1;              // 0 = Sunday, matches struct tm
  uint8_t day_of_month = 1;         // capped at 28, see parse()
};

const char* mode_name(Mode mode);

// Never fails: an unrecognised spec yields the default. `ok` reports
// whether the input was understood exactly, so the caller can log without
// this unit needing a logger.
Spec parse(const char* text, bool* ok = nullptr);

// Renders a spec back to its canonical text, for the portal field.
// Writes at most out_size bytes including the terminator.
void format(const Spec& spec, char* out, size_t out_size);

// The reset period a given local time falls in, as a plain integer that
// only changes when a boundary is crossed. Comparing this against the
// stored value is the whole reset test - no date arithmetic at the call
// site, and it works identically for all three modes.
//
// Returns 0 for Mode::kOff, which never matches a stored non-zero period.
int32_t period_of(const Spec& spec, time_t local_time);

// What a period comparison means. Pulled out of App::_check_reset() so the
// rule can be exercised on the host: the three cases below are subtle
// enough that two of them shipped wrong, and neither was reachable from a
// unit test while the decision lived inside a FreeRTOS task.
enum class Action : uint8_t {
  kNone,   // same period, nothing to do
  kAdopt,  // no usable stored period - take this one without clearing
  kHold,   // local time moved backwards - keep the stored period
  kClear,  // boundary genuinely crossed
};

// stored_period is what the device last acted on, 0 if none (which is also
// what a change of schedule leaves behind - see DeviceState::set_reset_spec).
// current_period comes from period_of(); 0 means the schedule is off.
Action decide(int32_t stored_period, int32_t current_period);

// Seconds from local_time until the next boundary, clamped into the range
// the deep sleep timer accepts. Returns 0 when the mode is off, meaning
// "no scheduled wake, fall back to the heartbeat interval".
uint32_t seconds_until_next(const Spec& spec, time_t local_time);

}  // namespace reset_schedule

#endif  // HOMEBUTTONS_RESET_SCHEDULE_H
