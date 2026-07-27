#include "console.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "config.h"
#include "reset_schedule.h"

const Console::Command Console::kCommands[] = {
    {"help", "", "this list", &Console::_cmd_help},
    {"status", "", "everything at a glance", &Console::_cmd_status},
    {"press", "<1-6>", "inject a button press", &Console::_cmd_press},
    {"counter", "[a|b [n]]", "show or set a counter", &Console::_cmd_counter},
    {"sched", "[spec]", "show or set the reset schedule", &Console::_cmd_sched},
    {"reset", "", "run the reset check now", &Console::_cmd_reset},
    {"time", "[set <epoch> [off]]", "show or override the clock",
     &Console::_cmd_time},
    {"sync", "", "post a time sync", &Console::_cmd_sync},
    {"post", "", "post a heartbeat", &Console::_cmd_post},
    {"endpoint", "[url]", "show or set the webhook URL",
     &Console::_cmd_endpoint},
    {"token", "[tok]", "show or set the auth token", &Console::_cmd_token},
    {"wifi", "", "link detail", &Console::_cmd_wifi},
    {"awake", "[0|1]", "show or set awake mode", &Console::_cmd_awake},
    {"save", "", "persist NVS now", &Console::_cmd_save},
    {"sleep", "", "sleep immediately", &Console::_cmd_sleep},
    {"restart", "", "reboot", &Console::_cmd_restart},
    {"setup", "", "reboot into the full setup portal", &Console::_cmd_setup},
    {"wifisetup", "", "reboot into the Wi-Fi portal", &Console::_cmd_wifisetup},
};
const size_t Console::kNumCommands =
    sizeof(Console::kCommands) / sizeof(Console::kCommands[0]);

void Console::begin() {
  usb_.stream = &Serial;
  uart_.stream = &Serial0;
  Serial.begin(SERIAL_BAUD_RATE);
  Serial0.begin(SERIAL_BAUD_RATE);

  line_queue_ = xQueueCreate(LINE_QUEUE_SIZE, sizeof(Line));
  if (line_queue_ == nullptr) {
    error("failed to create the line queue, console unavailable");
    return;
  }

  xTaskCreate(_reader_task, "console", READER_STACK, this, 1,
              &reader_task_h_);
  info("console ready on USB CDC and UART0 - type 'help'");
}

void Console::_reader_task(void* self) {
  static_cast<Console*>(self)->_reader();
}

void Console::_reader() {
  while (true) {
    // Bounded per pass so one port cannot monopolise the loop, and so a
    // stuck stream cannot spin here without yielding.
    for (Port* port : {&usb_, &uart_}) {
      if (port->stream == nullptr) continue;
      for (int i = 0; i < 64 && port->stream->available() > 0; ++i) {
        _feed(*port, static_cast<char>(port->stream->read()));
      }
    }
    delay(20);
  }
}

void Console::_feed(Port& port, char c) {
  if (c == '\r') return;  // CRLF terminals send both; act on the LF

  if (c == '\b' || c == 0x7F) {
    if (port.len > 0) {
      port.len--;
      port.stream->write("\b \b");  // erase on the sender's terminal only
    }
    return;
  }

  if (c != '\n') {
    if (port.len + 1 < LINE_MAXLEN) {
      port.buf[port.len++] = c;
      port.stream->write(c);  // echo, so typing into a terminal is bearable
    }
    return;
  }

  port.stream->write("\r\n");
  port.buf[port.len] = '\0';
  const size_t len = port.len;
  port.len = 0;
  if (len == 0) return;

  Line line{};
  memcpy(line.text, port.buf, len + 1);
  if (line_queue_ == nullptr ||
      xQueueSend(line_queue_, &line, (TickType_t)0) != pdTRUE) {
    // Only reachable by pasting a block of commands while a POST is in
    // flight. Saying so beats silently dropping one.
    _out("busy, command dropped\n");
  }
}

void Console::service() {
  if (line_queue_ == nullptr) return;
  Line line;
  while (xQueueReceive(line_queue_, &line, 0) == pdTRUE) {
    _execute(line.text);
  }
}

void Console::_execute(char* line) {
  char* argv[MAX_ARGS] = {};
  int argc = 0;
  for (char* tok = strtok(line, " \t"); tok != nullptr && argc < MAX_ARGS;
       tok = strtok(nullptr, " \t")) {
    argv[argc++] = tok;
  }
  if (argc == 0) return;

  for (size_t i = 0; i < kNumCommands; ++i) {
    if (strcasecmp(argv[0], kCommands[i].name) == 0) {
      (this->*kCommands[i].fn)(argc, argv);
      return;
    }
  }
  _out("unknown command '%s' - try 'help'\n", argv[0]);
}

void Console::_out(const char* fmt, ...) const {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n <= 0) return;
  Serial.write(buf);
  Serial0.write(buf);
}

bool Console::_parse_counter_idx(const char* text, uint8_t& idx) {
  for (uint8_t i = 0; i < NUM_COUNTERS; ++i) {
    if (strcasecmp(text, COUNTER_NAMES[i]) == 0) {
      idx = i;
      return true;
    }
  }
  _out("no counter '%s'\n", text);
  return false;
}

// --- commands -------------------------------------------------------------

void Console::_cmd_help(int, char**) {
  _out("commands:\n");
  for (size_t i = 0; i < kNumCommands; ++i) {
    _out("  %-9s %-19s %s\n", kCommands[i].name, kCommands[i].args,
         kCommands[i].help);
  }
}

void Console::_cmd_status(int, char**) {
  App::StateLock lock(app_.state_mutex_);
  DeviceState& st = app_.device_state_;

  _out("build     fw %s / spiffs %s / %s\n", BUILD_ID,
       app_.spiffs_build_.empty() ? "?" : app_.spiffs_build_.c_str(),
       SW_VERSION);
  _out("device    %s\n", st.factory().unique_id.c_str());
  _out("uptime    %lu s, heap %u free / %u min\n",
       static_cast<unsigned long>(millis() / 1000), ESP.getFreeHeap(),
       ESP.getMinFreeHeap());
  _out("state     app %s / net %s\n", app_.current_state_name(),
       app_.network_.get_state() == Network::State::W_CONNECTED
           ? "connected"
           : "disconnected");
  _out("boot      cause %d, wake btn %u, awake mode %d\n",
       static_cast<int>(app_.boot_cause_), app_.wakeup_btn_id_,
       st.flags().awake_mode);
  _out("wifi      %s, ip %s, rssi %d\n",
       WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "-", st.ip(),
       static_cast<int>(WiFi.RSSI()));
  _out("battery   %u%%, %.2f V, present %d, dc %d\n", st.sensors().battery_pct,
       st.sensors().battery_voltage, st.sensors().battery_present,
       st.sensors().dc_connected);

  for (uint8_t i = 0; i < NUM_COUNTERS; ++i) {
    _out("counter %s %ld\n", COUNTER_NAMES[i],
         static_cast<long>(st.counter(i)));
  }
  _out("seq       %u\n", st.seq());
  _out("endpoint  %s\n",
       st.endpoint_url().empty() ? "(unset)" : st.endpoint_url().c_str());
  _out("token     %s\n", st.auth_token().empty() ? "(unset)" : "(set)");
  _out("queued    %u press(es)\n",
       app_.press_queue_ == nullptr
           ? 0
           : uxQueueMessagesWaiting(app_.press_queue_));

  _cmd_time(0, nullptr);
  _cmd_sched(0, nullptr);
}

void Console::_cmd_press(int argc, char** argv) {
  if (argc < 2) {
    _out("usage: press <1-%u>\n", NUM_BUTTONS);
    return;
  }
  const long btn = strtol(argv[1], nullptr, 10);
  if (btn < 1 || btn > NUM_BUTTONS) {
    _out("button out of range (1-%u)\n", NUM_BUTTONS);
    return;
  }
  // The same entry point a real press takes, so what this exercises is the
  // production path and not a parallel one. Runs on the main task rather
  // than the UI task; the counters are behind state_mutex_ and the LED
  // calls match what _flush_pending() already does from here.
  _out("press %ld\n", btn);
  app_._console_press(static_cast<uint8_t>(btn));
}

void Console::_cmd_counter(int argc, char** argv) {
  App::StateLock lock(app_.state_mutex_);
  DeviceState& st = app_.device_state_;

  if (argc < 2) {
    for (uint8_t i = 0; i < NUM_COUNTERS; ++i) {
      _out("counter %s %ld\n", COUNTER_NAMES[i],
           static_cast<long>(st.counter(i)));
    }
    return;
  }
  uint8_t idx = 0;
  if (!_parse_counter_idx(argv[1], idx)) return;

  if (argc < 3) {
    _out("counter %s %ld\n", COUNTER_NAMES[idx],
         static_cast<long>(st.counter(idx)));
    return;
  }
  // Goes through adjust_counter() so the clamp applies here exactly as it
  // does to a press.
  const long target = strtol(argv[2], nullptr, 10);
  const int32_t now =
      st.adjust_counter(idx, static_cast<int32_t>(target) - st.counter(idx));
  app_._refresh_counter_labels();
  st.flags().display_redraw = true;
  _out("counter %s %ld\n", COUNTER_NAMES[idx], static_cast<long>(now));
}

void Console::_cmd_sched(int argc, char** argv) {
  App::StateLock lock(app_.state_mutex_);
  DeviceState& st = app_.device_state_;

  if (argc >= 2) {
    // Rejoin the tokens: a spec is "daily 03:00", two words.
    ResetSpecType spec;
    for (int i = 1; i < argc; ++i) {
      if (i > 1) spec += " ";
      spec += argv[i];
    }
    bool ok = false;
    reset_schedule::parse(spec.c_str(), &ok);
    if (!ok) {
      _out("not understood: '%s'\n", spec.c_str());
      _out("try: off | daily 03:00 | weekly mon 03:00 | monthly 1 03:00\n");
      return;
    }
    st.set_reset_spec(spec);
    st.save_all();
    // The stored period belongs to the old schedule; keeping it would make
    // the next check compare across two different definitions of "period"
    // and clear the counters for no reason. Re-adopt instead.
    st.set_last_reset_period(0);
    app_._check_reset();
    app_._schedule_next_wake();
  }

  const reset_schedule::Spec spec = app_._reset_spec();
  char canonical[reset_schedule::kSpecMaxLen] = {};
  reset_schedule::format(spec, canonical, sizeof(canonical));
  _out("sched     %s (stored '%s')\n", canonical, st.reset_spec().c_str());
  _out("period    last %ld\n", static_cast<long>(st.last_reset_period()));
  _out("nextwake  %u s\n", st.flags().schedule_wakeup_time);
}

void Console::_cmd_reset(int, char**) {
  app_._check_reset();
  app_._schedule_next_wake();
  _cmd_counter(0, nullptr);
}

void Console::_cmd_time(int argc, char** argv) {
  DeviceState& st = app_.device_state_;

  if (argc >= 3 && strcasecmp(argv[1], "set") == 0) {
    const uint32_t epoch = strtoul(argv[2], nullptr, 10);
    const int32_t offset = (argc >= 4)
                               ? static_cast<int32_t>(strtol(argv[3], nullptr, 10))
                               : st.tz_offset();
    if (epoch < 1700000000UL) {
      _out("implausible epoch %u\n", epoch);
      return;
    }
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(epoch);
    settimeofday(&tv, nullptr);
    st.set_clock_synced(epoch, offset);
    st.save_all();
    _out("clock set: ts=%u offset=%d\n", epoch, offset);
    // A jump may well have crossed a boundary; test it rather than waiting
    // for the next press to notice.
    app_._check_reset();
    app_._schedule_next_wake();
  }

  const time_t now = time(nullptr);
  const time_t local = now + static_cast<time_t>(st.tz_offset());
  struct tm tm_utc = {};
  struct tm tm_loc = {};
  gmtime_r(&now, &tm_utc);
  gmtime_r(&local, &tm_loc);
  _out("utc       %04d-%02d-%02d %02d:%02d:%02d\n", tm_utc.tm_year + 1900,
       tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min,
       tm_utc.tm_sec);
  _out("local     %04d-%02d-%02d %02d:%02d:%02d (offset %+ld s)\n",
       tm_loc.tm_year + 1900, tm_loc.tm_mon + 1, tm_loc.tm_mday,
       tm_loc.tm_hour, tm_loc.tm_min, tm_loc.tm_sec,
       static_cast<long>(st.tz_offset()));
  _out("clock     valid %d, fresh %d, synced %lu s ago\n", st.clock_valid(),
       app_._clock_fresh(),
       st.clock_valid()
           ? static_cast<unsigned long>(now - static_cast<time_t>(
                                                  st.last_time_sync()))
           : 0UL);
}

void Console::_cmd_sync(int, char**) {
  if (app_.network_.get_state() != Network::State::W_CONNECTED) {
    _out("not connected\n");
    return;
  }
  _out("sync %s\n", app_.webhook_.sync_time() ? "ok" : "failed");
  // The clock may only just have become valid. _service_webhook() does the
  // same after its own sync; without it here the period stays unadopted
  // until the next connect.
  app_._check_reset();
  app_._schedule_next_wake();
  _cmd_time(0, nullptr);
}

void Console::_cmd_post(int, char**) {
  if (app_.network_.get_state() != Network::State::W_CONNECTED) {
    _out("not connected\n");
    return;
  }
  _out("heartbeat %s\n", app_.webhook_.send_heartbeat() ? "ok" : "failed");
}

void Console::_cmd_endpoint(int argc, char** argv) {
  DeviceState& st = app_.device_state_;
  if (argc >= 2) {
    st.set_endpoint_url(EndpointUrlType(argv[1]));
    st.save_all();
    // The HTTPClient holds the old host; force a rebuild on the next send.
    app_.webhook_.begin();
  }
  _out("endpoint  %s\n",
       st.endpoint_url().empty() ? "(unset)" : st.endpoint_url().c_str());
}

void Console::_cmd_token(int argc, char** argv) {
  DeviceState& st = app_.device_state_;
  if (argc >= 2) {
    st.set_auth_token(AuthTokenType(argv[1]));
    st.save_all();
  }
  // Never echoed back. Length is enough to tell "saved" from "truncated",
  // which is the only thing worth knowing here.
  _out("token     %s (%u chars)\n", st.auth_token().empty() ? "(unset)" : "(set)",
       static_cast<unsigned>(st.auth_token().length()));
}

void Console::_cmd_wifi(int, char**) {
  char country[4] = {};
  esp_wifi_get_country_code(country);
  _out("ssid      %s\n",
       WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "(not connected)");
  _out("bssid     %s\n", WiFi.BSSIDstr().c_str());
  _out("channel   %d, rssi %d\n", WiFi.channel(),
       static_cast<int>(WiFi.RSSI()));
  _out("ip        %s\n", app_.device_state_.ip());
  _out("country   applied '%s', configured '%s'\n", country,
       app_.device_state_.wifi_country().c_str());
}

void Console::_cmd_awake(int argc, char** argv) {
  App::StateLock lock(app_.state_mutex_);
  DeviceState& st = app_.device_state_;
  if (argc >= 2) {
    st.persisted().user_awake_mode = strtol(argv[1], nullptr, 10) != 0;
    st.save_all();
  }
  // Setting this to 0 lets the device deep sleep, which takes the USB CDC
  // down with it. UART0 on the debug header survives, and the console is
  // up early enough in boot to catch a command sent during a wake.
  _out("awake     user %d, effective %d\n", st.persisted().user_awake_mode,
       st.flags().awake_mode);
}

void Console::_cmd_save(int, char**) {
  App::StateLock lock(app_.state_mutex_);
  app_.device_state_.save_all();
  _out("saved\n");
}

void Console::_cmd_sleep(int, char**) {
  _out("sleeping\n");
  app_.transition_to<AppSMStates::CmdShutdownState>();
}

void Console::_cmd_restart(int, char**) {
  _out("restarting\n");
  {
    App::StateLock lock(app_.state_mutex_);
    app_.device_state_.save_all();
  }
  delay(100);
  ESP.restart();
}

void Console::_cmd_setup(int, char**) {
  App::StateLock lock(app_.state_mutex_);
  app_.device_state_.persisted().restart_to_setup = true;
  app_.device_state_.save_all();
  _out("restarting into setup\n");
  delay(100);
  ESP.restart();
}

void Console::_cmd_wifisetup(int, char**) {
  App::StateLock lock(app_.state_mutex_);
  app_.device_state_.persisted().restart_to_wifi_setup = true;
  app_.device_state_.save_all();
  _out("restarting into Wi-Fi setup\n");
  delay(100);
  ESP.restart();
}
