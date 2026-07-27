// Host-side tests for the counter reset schedule.
//
// This is the one part of the firmware that is pure calendar arithmetic with
// no hardware behind it, and the one part whose bugs would surface as "the
// counter cleared on the wrong day" weeks after a flash. Run with:
//
//     pio test -e native
//
// All times below are "local seconds" - a UTC epoch already shifted by the
// offset the receiver reported - which is exactly what the firmware passes
// in, so the tests exercise the same units as production.

#include <unity.h>

#include <string.h>
#include <time.h>

#include "reset_schedule.h"

using reset_schedule::Mode;
using reset_schedule::Spec;

namespace {

// Builds a local-seconds value from a civil date/time, without depending on
// the host timezone: timegm-equivalent via a fixed reference.
time_t at(int year, int month, int day, int hour = 0, int minute = 0,
          int second = 0) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  return timegm(&t);
}

Spec spec_from(const char* text, bool expect_ok = true) {
  bool ok = false;
  Spec s = reset_schedule::parse(text, &ok);
  TEST_ASSERT_EQUAL_MESSAGE(expect_ok, ok, text);
  return s;
}

}  // namespace

// --- parsing ---------------------------------------------------------------

void test_parse_daily() {
  Spec s = spec_from("daily 03:00");
  TEST_ASSERT_TRUE(s.mode == Mode::kDaily);
  TEST_ASSERT_EQUAL_UINT16(180, s.minute_of_day);
}

void test_parse_off() {
  Spec s = spec_from("off");
  TEST_ASSERT_TRUE(s.mode == Mode::kOff);
}

void test_parse_weekly() {
  Spec s = spec_from("weekly mon 04:30");
  TEST_ASSERT_TRUE(s.mode == Mode::kWeekly);
  TEST_ASSERT_EQUAL_UINT8(1, s.weekday);
  TEST_ASSERT_EQUAL_UINT16(4 * 60 + 30, s.minute_of_day);
}

void test_parse_monthly() {
  Spec s = spec_from("monthly 5 06:15");
  TEST_ASSERT_TRUE(s.mode == Mode::kMonthly);
  TEST_ASSERT_EQUAL_UINT8(5, s.day_of_month);
  TEST_ASSERT_EQUAL_UINT16(6 * 60 + 15, s.minute_of_day);
}

void test_parse_is_case_insensitive() {
  Spec s = spec_from("WEEKLY Fri 23:59");
  TEST_ASSERT_TRUE(s.mode == Mode::kWeekly);
  TEST_ASSERT_EQUAL_UINT8(5, s.weekday);
}

// A typo must not silently disable the reset - it falls back to the default
// and reports that it did not understand.
void test_parse_garbage_falls_back_to_daily() {
  Spec s = spec_from("every other tuesday", /*expect_ok=*/false);
  TEST_ASSERT_TRUE(s.mode == Mode::kDaily);
  TEST_ASSERT_EQUAL_UINT16(180, s.minute_of_day);
}

void test_parse_rejects_out_of_range_time() {
  Spec s = spec_from("daily 25:00", /*expect_ok=*/false);
  TEST_ASSERT_EQUAL_UINT16(180, s.minute_of_day);
}

void test_parse_caps_monthly_day_at_28() {
  Spec s = spec_from("monthly 31 03:00", /*expect_ok=*/false);
  TEST_ASSERT_EQUAL_UINT8(1, s.day_of_month);
}

void test_parse_empty_is_default_and_ok() {
  Spec s = spec_from("");
  TEST_ASSERT_TRUE(s.mode == Mode::kDaily);
}

void test_format_roundtrips() {
  const char* cases[] = {"off", "daily 03:00", "weekly mon 04:30",
                         "monthly 5 06:15"};
  for (const char* c : cases) {
    char out[reset_schedule::kSpecMaxLen + 1] = {};
    reset_schedule::format(spec_from(c), out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(c, out);
  }
}

// --- period boundaries -----------------------------------------------------

void test_daily_period_changes_at_boundary() {
  Spec s = spec_from("daily 03:00");
  // One second before 03:00 is still the previous period.
  TEST_ASSERT_EQUAL_INT32(period_of(s, at(2026, 7, 27, 2, 59, 59)),
                          period_of(s, at(2026, 7, 26, 12, 0, 0)));
  // 03:00 exactly starts the new one.
  TEST_ASSERT_NOT_EQUAL(period_of(s, at(2026, 7, 27, 2, 59, 59)),
                        period_of(s, at(2026, 7, 27, 3, 0, 0)));
}

// Midnight must not be a boundary when the reset is at 03:00 - the whole
// point of shifting by minute_of_day.
void test_daily_midnight_is_not_a_boundary() {
  Spec s = spec_from("daily 03:00");
  TEST_ASSERT_EQUAL_INT32(period_of(s, at(2026, 7, 26, 23, 59, 0)),
                          period_of(s, at(2026, 7, 27, 0, 1, 0)));
}

void test_weekly_period_changes_on_configured_day() {
  Spec s = spec_from("weekly mon 03:00");
  // 2026-07-27 is a Monday.
  const int32_t before = period_of(s, at(2026, 7, 27, 2, 0, 0));
  const int32_t after = period_of(s, at(2026, 7, 27, 4, 0, 0));
  TEST_ASSERT_NOT_EQUAL(before, after);
  // Tuesday through Sunday all sit in the same period as Monday afternoon.
  TEST_ASSERT_EQUAL_INT32(after, period_of(s, at(2026, 7, 28, 12, 0, 0)));
  TEST_ASSERT_EQUAL_INT32(after, period_of(s, at(2026, 8, 2, 23, 0, 0)));
  // The following Monday starts a new one.
  TEST_ASSERT_NOT_EQUAL(after, period_of(s, at(2026, 8, 3, 4, 0, 0)));
}

void test_monthly_period_changes_on_configured_day() {
  Spec s = spec_from("monthly 5 03:00");
  const int32_t before = period_of(s, at(2026, 7, 5, 2, 0, 0));
  const int32_t after = period_of(s, at(2026, 7, 5, 4, 0, 0));
  TEST_ASSERT_NOT_EQUAL(before, after);
  TEST_ASSERT_EQUAL_INT32(after, period_of(s, at(2026, 7, 31, 12, 0, 0)));
  TEST_ASSERT_EQUAL_INT32(after, period_of(s, at(2026, 8, 4, 12, 0, 0)));
  TEST_ASSERT_NOT_EQUAL(after, period_of(s, at(2026, 8, 5, 4, 0, 0)));
}

// 0 is the "never established" sentinel the firmware relies on to avoid
// clearing a count on first setup, so a real period must never be 0.
void test_period_is_never_zero_for_real_dates() {
  const char* specs[] = {"daily 03:00", "weekly mon 03:00", "monthly 1 03:00"};
  for (const char* c : specs) {
    Spec s = spec_from(c);
    TEST_ASSERT_NOT_EQUAL(0, period_of(s, at(2026, 7, 27, 12, 0, 0)));
    TEST_ASSERT_NOT_EQUAL(0, period_of(s, at(2035, 1, 1, 0, 0, 0)));
  }
}

void test_off_never_resets() {
  Spec s = spec_from("off");
  TEST_ASSERT_EQUAL_INT32(0, period_of(s, at(2026, 7, 27, 12, 0, 0)));
  TEST_ASSERT_EQUAL_UINT32(0, seconds_until_next(s, at(2026, 7, 27, 12, 0, 0)));
}

// --- next-wake -------------------------------------------------------------

void test_seconds_until_next_daily() {
  Spec s = spec_from("daily 03:00");
  // 23:00 -> 03:00 is four hours.
  TEST_ASSERT_EQUAL_UINT32(4 * 3600,
                           seconds_until_next(s, at(2026, 7, 26, 23, 0, 0)));
  // Just after the boundary, nearly a full day.
  TEST_ASSERT_EQUAL_UINT32(24 * 3600 - 60,
                           seconds_until_next(s, at(2026, 7, 27, 3, 1, 0)));
}

// The wake it computes must actually land in the next period, for every
// mode - this is the property that makes the schedule correct.
void test_next_wake_lands_in_the_next_period() {
  const char* specs[] = {"daily 03:00", "weekly wed 07:45", "monthly 12 21:30"};
  const time_t probes[] = {at(2026, 1, 1, 0, 0, 0), at(2026, 2, 28, 23, 59, 0),
                           at(2026, 7, 27, 3, 0, 0), at(2026, 12, 31, 22, 0, 0),
                           at(2027, 3, 1, 12, 0, 0)};
  for (const char* c : specs) {
    Spec s = spec_from(c);
    for (time_t p : probes) {
      const uint32_t secs = seconds_until_next(s, p);
      // Clamped at 24h, so weekly/monthly wake daily and re-evaluate; only
      // assert progress in that case.
      if (secs >= reset_schedule::kWakeMaxSeconds) continue;
      TEST_ASSERT_NOT_EQUAL_MESSAGE(period_of(s, p),
                                    period_of(s, p + (time_t)secs), c);
      // And not a second earlier.
      TEST_ASSERT_EQUAL_INT32(period_of(s, p),
                              period_of(s, p + (time_t)secs - 1));
    }
  }
}

void test_next_wake_is_clamped() {
  Spec s = spec_from("monthly 1 03:00");
  const uint32_t secs = seconds_until_next(s, at(2026, 7, 2, 3, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(reset_schedule::kWakeMaxSeconds, secs);
  TEST_ASSERT_TRUE(secs >= reset_schedule::kWakeMinSeconds);
}

// --- decide() ------------------------------------------------------------
// Every case below is a defect that shipped, or the invariant that defect
// broke. The rule used to live inside App::_check_reset(), where none of it
// could be reached without hardware.

void test_decide_same_period_does_nothing() {
  TEST_ASSERT_TRUE(reset_schedule::decide(20661, 20661) ==
                   reset_schedule::Action::kNone);
}

void test_decide_adopts_when_nothing_stored() {
  // First boot with a known date, and the state left by a schedule change.
  // Adopting rather than clearing keeps a count the device was just given.
  TEST_ASSERT_TRUE(reset_schedule::decide(0, 20661) ==
                   reset_schedule::Action::kAdopt);
}

void test_decide_clears_on_forward_crossing() {
  TEST_ASSERT_TRUE(reset_schedule::decide(20661, 20662) ==
                   reset_schedule::Action::kClear);
}

void test_decide_holds_when_time_moves_back() {
  // Autumn DST, or a clock correction. Must not clear.
  TEST_ASSERT_TRUE(reset_schedule::decide(20662, 20661) ==
                   reset_schedule::Action::kHold);
}

void test_decide_never_clears_twice_for_one_boundary() {
  // The defect: holding is worthless if the earlier period gets adopted,
  // because the boundary is then re-armed and fires again on the way
  // forward. Walk the DST sequence and count the clears.
  int32_t stored = 20661;
  int clears = 0;
  const int32_t walk[] = {20662, 20661, 20662};  // 03:00, back to 02:00, 03:00
  for (int32_t period : walk) {
    switch (reset_schedule::decide(stored, period)) {
      case reset_schedule::Action::kClear:
        clears++;
        stored = period;
        break;
      case reset_schedule::Action::kAdopt:
        stored = period;
        break;
      default:
        break;
    }
  }
  TEST_ASSERT_EQUAL_INT(1, clears);
  TEST_ASSERT_EQUAL_INT32(20662, stored);
}

void test_decide_off_never_clears() {
  // period_of() returns 0 for a disabled schedule, and 0 must not read as
  // a boundary crossing however large the stored period is.
  TEST_ASSERT_TRUE(reset_schedule::decide(20661, 0) ==
                   reset_schedule::Action::kNone);
  TEST_ASSERT_TRUE(reset_schedule::decide(0, 0) ==
                   reset_schedule::Action::kNone);
}

void test_decide_handles_a_change_of_schedule_unit() {
  // daily counts days (~20661), monthly counts months (~678). Compared
  // directly, the new period looks like time running backwards and resets
  // would be suppressed indefinitely - so set_reset_spec() zeroes the
  // stored period, and this is what decide() must then see.
  TEST_ASSERT_TRUE(reset_schedule::decide(20661, 678) ==
                   reset_schedule::Action::kHold);
  TEST_ASSERT_TRUE(reset_schedule::decide(0, 678) ==
                   reset_schedule::Action::kAdopt);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_daily);
  RUN_TEST(test_parse_off);
  RUN_TEST(test_parse_weekly);
  RUN_TEST(test_parse_monthly);
  RUN_TEST(test_parse_is_case_insensitive);
  RUN_TEST(test_parse_garbage_falls_back_to_daily);
  RUN_TEST(test_parse_rejects_out_of_range_time);
  RUN_TEST(test_parse_caps_monthly_day_at_28);
  RUN_TEST(test_parse_empty_is_default_and_ok);
  RUN_TEST(test_format_roundtrips);
  RUN_TEST(test_daily_period_changes_at_boundary);
  RUN_TEST(test_daily_midnight_is_not_a_boundary);
  RUN_TEST(test_weekly_period_changes_on_configured_day);
  RUN_TEST(test_monthly_period_changes_on_configured_day);
  RUN_TEST(test_period_is_never_zero_for_real_dates);
  RUN_TEST(test_off_never_resets);
  RUN_TEST(test_seconds_until_next_daily);
  RUN_TEST(test_next_wake_lands_in_the_next_period);
  RUN_TEST(test_next_wake_is_clamped);
  RUN_TEST(test_decide_same_period_does_nothing);
  RUN_TEST(test_decide_adopts_when_nothing_stored);
  RUN_TEST(test_decide_clears_on_forward_crossing);
  RUN_TEST(test_decide_holds_when_time_moves_back);
  RUN_TEST(test_decide_never_clears_twice_for_one_boundary);
  RUN_TEST(test_decide_off_never_clears);
  RUN_TEST(test_decide_handles_a_change_of_schedule_unit);
  return UNITY_END();
}
