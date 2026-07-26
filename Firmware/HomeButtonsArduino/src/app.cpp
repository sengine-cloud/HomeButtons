#include "app.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "esp_ota_ops.h"

#include "config.h"
#include "hardware.h"

extern "C" bool verifyRollbackLater() { return true; };

App::App()
    : AppStateMachine("AppSM", *this),
      Logger("APP"),
      b1_("B1", 1, false, true, hw_),
      b2_("B2", 2, false, true, hw_),
      b3_("B3", 3, false, true, hw_),
      b4_("B4", 4, false, true, hw_),
      b5_("B5", 5, false, true, hw_),
      b6_("B6", 6, false, true, hw_),
      bsl_input_("BSLInput",
                 std::array<std::reference_wrapper<BtnSwLED>, NUM_BUTTONS>{
                     b1_, b2_, b3_, b4_, b5_, b6_}),
      display_(device_state_),
      network_(device_state_),
      webhook_(device_state_),
      setup_(*this) {
  press_queue_ = xQueueCreate(PRESS_QUEUE_SIZE, sizeof(PressQueueElement));
  if (press_queue_ == nullptr) error("failed to create press queue");
}

void App::setup() {
  info("starting...");
  xTaskCreate(_main_task_helper,  // Function that should be called
              "MAIN",             // Name of the task (for debugging)
              20000,              // Stack size (bytes)
              this,               // Parameter to pass
              tskIDLE_PRIORITY,   // Task priority
              &main_task_h_       // Task handle
  );
  debug("main task started.");

  vTaskDelete(NULL);
}

void App::_sleep_or_restart() {
  delay(3000);
  error("Going to sleep...");
  _start_esp_sleep();
}

void App::_start_esp_sleep() {
  esp_sleep_enable_ext1_wakeup(hw_.WAKE_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
  if (device_state_.persisted().wifi_done &&
      device_state_.persisted().setup_done &&
      !device_state_.persisted().low_batt_mode &&
      !device_state_.persisted().check_connection) {
    if (device_state_.flags().schedule_wakeup_time > 0) {
      esp_sleep_enable_timer_wakeup(device_state_.flags().schedule_wakeup_time *
                                    1000000ULL);
    } else {
      esp_sleep_enable_timer_wakeup(
          static_cast<uint64_t>(device_state_.heartbeat_interval()) *
          60000000ULL);
    }
  }
  info("deep sleep... z z z");
  esp_deep_sleep_start();
}

void App::_go_to_sleep() {
  _schedule_next_wake();
  device_state_.save_all();
  hw_.set_all_leds(0);
  _start_esp_sleep();
}

std::pair<BootCause, int16_t> App::_determine_boot_cause() {
  BootCause boot_cause = BootCause::RESET;
  uint8_t wakeup_btn_id = 0;
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      uint64_t gpio_reason = esp_sleep_get_ext1_wakeup_status();
      if (gpio_reason == 0) {
        debug("EXT1 wakeup with empty status");
        break;
      }
      // Lowest set bit is the pin. Upstream used log(x)/log(2) on floats,
      // which yields a nonsense pin when two buttons are held at wake and
      // is undefined for a zero mask.
      int wakeup_pin = __builtin_ctzll(gpio_reason);
      debug("wakeup cause: PIN %d (mask 0x%llx)", wakeup_pin, gpio_reason);
      wakeup_btn_id = bsl_input_.IdFromPin(wakeup_pin);
      if (wakeup_btn_id > 0) {
        boot_cause = BootCause::BUTTON;
      }
    } break;
    case ESP_SLEEP_WAKEUP_TIMER:
      boot_cause = BootCause::TIMER;
      break;
    default:
      break;
  }
  return std::make_pair(boot_cause, wakeup_btn_id);
}

void App::_log_task_stats() {
  const UBaseType_t maxTasks = 20;
  TaskStatus_t statusArray[maxTasks];
  uint32_t totalRunTime;

  UBaseType_t numTasks =
      uxTaskGetSystemState(statusArray, maxTasks, &totalRunTime);

  debug("#### Task Stats ####");
  for (UBaseType_t i = 0; i < numTasks; i++) {
    TaskStatus_t* taskStatus = &statusArray[i];

    float runTimePercentage = 0.0;
    if (totalRunTime > 0) {
      runTimePercentage =
          (taskStatus->ulRunTimeCounter / (float)totalRunTime) * 100;
    }

    // Task name is an argument, never the format string: upstream passed a
    // formatted buffer straight to debug(), so a '%' in a task name would
    // have been interpreted as a conversion.
    debug("%-15s%10d%10d%10d%10d%10.2f", taskStatus->pcTaskName,
          taskStatus->eCurrentState, taskStatus->uxCurrentPriority,
          taskStatus->usStackHighWaterMark, (int)taskStatus->xTaskNumber,
          runTimePercentage);
  }
  debug("Free heap: ESP %d, ESP MIN %d, RTOS %d", ESP.getFreeHeap(),
        ESP.getMinFreeHeap(), xPortGetFreeHeapSize());
}

void App::_ui_task(void* param) {
  App* app = static_cast<App*>(param);
  while (true) {
    app->bsl_input_.Loop();
    delay(5);
  }
}

void App::_start_ui_task() {
  if (ui_task_h_ != nullptr) return;
  debug("UI task started.");
  xTaskCreate(_ui_task,    // Function that should be called
              "UI",        // Name of the task (for debugging)
              10000,       // Stack size (bytes)
              this,        // Parameter to pass
              23,          // Task priority, same as the wifi driver
              &ui_task_h_  // Task handle
  );
}

void App::_display_task(void* param) {
  App* app = static_cast<App*>(param);
  while (true) {
    app->display_.update();
    delay(50);
  }
}

void App::_start_display_task() {
  if (display_task_h_ != nullptr) return;
  debug("display task started.");
  xTaskCreate(_display_task,    // Function that should be called
              "DISPLAY",        // Name of the task (for debugging)
              5000,             // Stack size (bytes)
              this,             // Parameter to pass
              1,                // Task priority
              &display_task_h_  // Task handle
  );
}

void App::_network_task(void* param) {
  App* app = static_cast<App*>(param);
  app->network_.setup();
  while (true) {
    app->network_.update();
    delay(10);
  }
}

void App::_start_network_task() {
  if (network_task_h_ != nullptr) return;
  debug("network task started.");
  xTaskCreate(_network_task,    // Function that should be called
              "NETWORK",        // Name of the task (for debugging)
              15000,            // Stack size (bytes)
              this,             // Parameter to pass
              1,                // Task priority
              &network_task_h_  // Task handle
  );
}

void App::_begin_hw() {
  // must be before ledAttachPin (reserves GPIO37 = SPIDQS).
  // Display::begin() also mounts SPIFFS, which is where the pre-flashed
  // icons live.
  display_.begin(hw_);
  hw_.begin();
  bsl_input_.Init();
  bsl_input_.LEDSetDefaultBrightnessAll(LED_DFLT_BRIGHT);
}

void App::_start_tasks() {
  _start_ui_task();
  _start_network_task();
  _start_display_task();
  bsl_input_.Start();
}

// ---------------------------------------------------------------------------
// Counter plumbing
// ---------------------------------------------------------------------------

bool App::_btn_to_counter(uint8_t btn_id, uint8_t& idx, int32_t& delta) {
  for (uint8_t i = 0; i < NUM_COUNTERS; i++) {
    if (btn_id == BTN_COUNTER_INC[i]) {
      idx = i;
      delta = 1;
      return true;
    }
    if (btn_id == BTN_COUNTER_DEC[i]) {
      idx = i;
      delta = -1;
      return true;
    }
  }
  return false;  // title buttons and anything unmapped
}

void App::_refresh_counter_labels() {
  // Middle row only. The title above says what the counter is and the minus
  // below is a fixed glyph, so both stay exactly as configured in the
  // portal - only the number is owned by the firmware.
  for (uint8_t i = 0; i < NUM_COUNTERS; i++) {
    device_state_.set_btn_label(
        BTN_COUNTER_INC[i],
        ButtonLabel("%ld", static_cast<long>(device_state_.counter(i)))
            .c_str());
  }
}

reset_schedule::Spec App::_reset_spec() {
  return reset_schedule::parse(device_state_.reset_spec().c_str(), *this);
}

bool App::_clock_fresh() const {
  if (!device_state_.clock_valid()) return false;
  const time_t now = time(nullptr);
  const time_t synced = static_cast<time_t>(device_state_.last_time_sync());
  // Drift on the internal RC oscillator is fine for hours and meaningless
  // after days, so refuse to act on a clock that old.
  return now >= synced &&
         (now - synced) <= static_cast<time_t>(CLOCK_STALE_SECONDS);
}

void App::_check_reset() {
  if (!_clock_fresh()) return;

  const reset_schedule::Spec spec = _reset_spec();
  if (spec.mode == reset_schedule::Mode::kOff) return;

  const time_t local =
      time(nullptr) + static_cast<time_t>(device_state_.tz_offset());
  const int32_t period = reset_schedule::period_of(spec, local);
  const int32_t last = device_state_.last_reset_period();

  if (last == 0) {
    // First time the date has ever been known. Adopt the period without
    // clearing, so setting up a device does not wipe a count it was just
    // given.
    device_state_.set_last_reset_period(period);
    return;
  }
  if (period == last) return;

  const bool had_counts = device_state_.clear_counters();
  device_state_.set_last_reset_period(period);
  _refresh_counter_labels();
  device_state_.flags().display_redraw = true;
  reset_to_report_ = true;
  info("reset boundary crossed (%s, period %d -> %d), counters cleared%s",
       reset_schedule::mode_name(spec.mode), last, period,
       had_counts ? "" : " (already zero)");
}

void App::_schedule_next_wake() {
  device_state_.flags().schedule_wakeup_time = 0;
  if (!device_state_.clock_valid()) return;

  const reset_schedule::Spec spec = _reset_spec();
  if (spec.mode == reset_schedule::Mode::kOff) return;

  const time_t local =
      time(nullptr) + static_cast<time_t>(device_state_.tz_offset());
  const uint32_t secs = reset_schedule::seconds_until_next(spec, local);
  device_state_.flags().schedule_wakeup_time = secs;
  info("next reset wake in %u s (%s)", secs,
       reset_schedule::mode_name(spec.mode));
}

// Runs on the UI task. RAM only: no NVS write, no HTTP.
void App::_handle_counter_press(uint8_t btn_id) {
  // Before anything else: a press just after a boundary belongs to the new
  // period, not the one that ended. The clock survives deep sleep, so this
  // is knowable without the network.
  _check_reset();

  uint8_t idx = 0;
  int32_t delta = 0;
  if (!_btn_to_counter(btn_id, idx, delta)) {
    debug("button %u is not assigned to a counter", btn_id);
    // Two quick blinks: registered, but nothing is bound to this button.
    bsl_input_.LEDBlink(btn_id, 2, 0, 0, 0, false);
    return;
  }

  int32_t count = device_state_.adjust_counter(idx, delta);
  info("counter %s %+ld -> %ld", COUNTER_NAMES[idx], static_cast<long>(delta),
       static_cast<long>(count));

  _refresh_counter_labels();
  device_state_.flags().display_redraw = true;

  // Solid while the press is in flight. _flush_pending() clears it once
  // every press on this button has been delivered, so the LED reports
  // delivery rather than merely "the device is awake".
  if (inflight_[btn_id - 1].fetch_add(1) == 0) {
    send_failed_[btn_id - 1] = false;  // start of a fresh burst
  }
  bsl_input_.LEDOn(btn_id);

  PressQueueElement element{};
  element.event.counter_idx = idx;
  element.event.button_id = btn_id;
  element.event.delta = delta;
  element.event.count = count;
  element.event.seq = device_state_.next_seq();
  element.queued_at = millis();

  if (press_queue_ == nullptr ||
      xQueueSend(press_queue_, &element, (TickType_t)0) != pdTRUE) {
    // The counter and the display already moved; only the notification is
    // lost. The next delivered press carries the corrected absolute count.
    error("press queue full, event for counter %s not sent",
          COUNTER_NAMES[idx]);
    send_failed_[btn_id - 1] = true;
    if (inflight_[btn_id - 1].fetch_sub(1) == 1) {
      bsl_input_.LEDBlink(btn_id, 3, 0, 0, 0, false);
    }
  }
}

// Runs on the main task. Owns the NVS write and the POST.
void App::_flush_pending() {
  if (press_queue_ == nullptr) return;
  if (uxQueueMessagesWaiting(press_queue_) == 0) return;
  if (network_.get_state() != Network::State::W_CONNECTED) return;

  // One NVS write covers however many presses are waiting.
  device_state_.save_all();

  PressQueueElement element;
  while (xQueueReceive(press_queue_, &element, 0) == pdTRUE) {
    element.event.age_ms = millis() - element.queued_at;
    const uint8_t btn = element.event.button_id;
    if (!webhook_.send_press(element.event)) {
      warning("failed to deliver press seq %u", element.event.seq);
      if (btn >= 1 && btn <= NUM_BUTTONS) send_failed_[btn - 1] = true;
    }
    // Only release the LED once nothing is left in flight for this button:
    // a press that lands while a later one is still queued must not take
    // the light out from under it.
    if (btn >= 1 && btn <= NUM_BUTTONS &&
        inflight_[btn - 1].fetch_sub(1) == 1) {
      if (send_failed_[btn - 1]) {
        // Three fast blinks, then dark. Distinguishable from the solid
        // in-flight state and from the two-blink unassigned pattern.
        bsl_input_.LEDBlink(btn, 3, 0, 0, 0, false);
      } else {
        bsl_input_.LEDOff(btn);
      }
    }
    esp_task_wdt_reset();
  }
}

void App::_net_on_connect() {
  webhook_.begin();

  const bool have_presses =
      press_queue_ != nullptr && uxQueueMessagesWaiting(press_queue_) > 0;
  const time_t now = time(nullptr);
  const bool clock_old =
      !device_state_.clock_valid() ||
      (now - static_cast<time_t>(device_state_.last_time_sync())) >
          static_cast<time_t>(CLOCK_RESYNC_SECONDS);

  if (boot_cause_ == BootCause::TIMER) {
    webhook_.send_heartbeat();
  } else if (!have_presses && clock_old) {
    // Every response carries the clock, so a bare sync is only worth
    // sending when nothing else is going out anyway.
    webhook_.sync_time();
  }

  // The clock may only just have become valid, so re-test the boundary now
  // that it has.
  _check_reset();
  if (reset_to_report_) {
    if (webhook_.send_reset(reset_schedule::mode_name(_reset_spec().mode))) {
      reset_to_report_ = false;
    }
  }
}

void App::_handle_ui_event_global(UserInput::Event event) {
  device_state_.flags().last_user_input_time = millis();
}

void App::_service_display() {
  if (!device_state_.flags().display_redraw) return;
  // Coalesce a burst so a run of presses does not queue up a full e-paper
  // refresh each. The timestamp only moves when something is actually
  // drawn, so the first press after an idle spell redraws straight away
  // rather than waiting out an interval that has been ticking in the
  // background.
  if (millis() - last_m_display_redraw_ < AWAKE_REDRAW_INTERVAL) return;
  device_state_.flags().display_redraw = false;
  last_m_display_redraw_ = millis();
  display_.disp_main();
}

void App::_main_task() {
  info("woke up.");
  info("cpu freq: %d MHz", getCpuFrequencyMhz());
  info("SW version: %s", SW_VERSION);

  // ------ init hardware ------
  bool hw_init_ok = hw_.init();

  // verify OTA if this is first boot after OTA
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      if (hw_init_ok) {
        esp_ota_mark_app_valid_cancel_rollback();
      } else {
        error("OTA verification failed! Rolling back...");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP.restart();  // if above line fails
      }
    }
  }

  if (!hw_init_ok) {
    error("HW init failed!");
    _sleep_or_restart();
  }

  device_state_.load_all(hw_);

  _begin_hw();

  // ------ after update handler ------
  if (device_state_.persisted().last_sw_ver != SW_VERSION) {
    if (device_state_.persisted().last_sw_ver.length() > 0) {
      info("firmware updated from %s to %s",
           device_state_.persisted().last_sw_ver.c_str(), SW_VERSION);
      device_state_.persisted().last_sw_ver = SW_VERSION;
      device_state_.save_all();
      display_.disp_message(
          (UIState::MessageType("Firmware\nupdated to\n") + SW_VERSION)
              .c_str());
      display_.update();
      ESP.restart();
    } else {  // first boot after factory flash
      device_state_.persisted().last_sw_ver = SW_VERSION;
    }
  }

  // ------ determine power mode ------
  device_state_.sensors().battery_present = hw_.is_battery_present();
  device_state_.sensors().dc_connected = hw_.is_dc_connected();
  info("batt present: %d, DC connected: %d",
       device_state_.sensors().battery_present,
       device_state_.sensors().dc_connected);

  if (device_state_.sensors().battery_present) {
    float batt_voltage = hw_.read_battery_voltage();
    info("batt volts: %f", batt_voltage);
    if (device_state_.sensors().dc_connected) {
      // charging
      if (batt_voltage < hw_.CHARGE_HYSTERESIS_VOLT) {
        device_state_.sensors().charging = true;
        hw_.enable_charger(true);
        device_state_.flags().awake_mode = true;
      } else {
        device_state_.flags().awake_mode =
            device_state_.persisted().user_awake_mode;
      }
      device_state_.persisted().low_batt_mode = false;
    } else {  // dc_connected == false
      if (device_state_.persisted().low_batt_mode) {
        if (batt_voltage >= hw_.BATT_HYSTERESIS_VOLT) {
          device_state_.persisted().low_batt_mode = false;
          device_state_.save_all();
          info("low batt mode disabled");
          ESP.restart();  // to handle display update
        } else {
          info("in low batt mode...");
          _go_to_sleep();
        }
      } else {  // low_batt_mode == false
        if (batt_voltage < hw_.MIN_BATT_VOLT) {
          // check again
          delay(1000);
          batt_voltage = hw_.read_battery_voltage();
          if (batt_voltage < hw_.MIN_BATT_VOLT) {
            device_state_.persisted().low_batt_mode = true;
            warning("batt voltage too low, low bat mode enabled");
            display_.disp_message_large(
                "Turned\nOFF\n\nPlease\nrecharge\nbattery!");
            display_.update();
            _go_to_sleep();
          }
        } else if (batt_voltage <= hw_.WARN_BATT_VOLT) {
          device_state_.sensors().battery_low = true;
        }
        device_state_.flags().awake_mode = false;
      }
    }
  } else {  // battery_present == false
    if (device_state_.sensors().dc_connected) {
      device_state_.persisted().low_batt_mode = false;
      device_state_.flags().awake_mode =
          device_state_.persisted().user_awake_mode;
    } else {
      // should never happen
      _go_to_sleep();
    }
  }
  info("usr awake mode: %d, awake mode: %d",
       device_state_.persisted().user_awake_mode,
       device_state_.flags().awake_mode);

  device_state_.sensors().battery_pct = hw_.read_battery_percent();
  device_state_.sensors().battery_voltage = hw_.read_battery_voltage();

  // The clock survives deep sleep, so the boundary can be tested before the
  // network is up - which matters when the reset wake is what woke us.
  _check_reset();

  // Labels carry the running totals, so make them match the counters
  // restored from NVS before anything is drawn.
  _refresh_counter_labels();

  // ------ start tasks ------
  _start_tasks();

  // ------ boot cause ------
  std::tie(boot_cause_, wakeup_btn_id_) = _determine_boot_cause();
  info("boot cause: %d, wakeup btn id: %d", static_cast<int>(boot_cause_),
       wakeup_btn_id_);

  // ------ handle boot cause ------
  switch (boot_cause_) {
    case BootCause::RESET: {
      if (!device_state_.persisted().silent_restart) {
        bsl_input_.LEDOnAll();
        delay(1000);
        display_.disp_message("RESTART...", 0);
        delay(3000);
      }

      // check if restart to setup or Wi-Fi setup is needed
      if (device_state_.persisted().restart_to_wifi_setup) {
        device_state_.clear_persisted_flags();
        info("staring Wi-Fi setup...");
        setup_.start_wifi_setup();  // resets ESP when done
      } else if (device_state_.persisted().restart_to_setup) {
        device_state_.clear_persisted_flags();
        info("staring setup...");
        setup_.start_setup();  // resets ESP when done
      }

      device_state_.clear_persisted_flags();

      if (!device_state_.persisted().wifi_done ||
          !device_state_.persisted().setup_done) {
        display_.disp_welcome();
        delay(3000);
        display_.end();
        delay(3000);
        _go_to_sleep();
      } else {
        display_.disp_main();
        delay(3000);
      }
      device_state_.save_all();
      if (device_state_.flags().awake_mode) {
        bsl_input_.LEDOffAll();
      } else {
        display_.end();
        delay(3000);
        _go_to_sleep();
      }
      break;
    }
    case BootCause::BUTTON: {
      if (!device_state_.flags().awake_mode) {
        if (device_state_.persisted().charge_complete_showing) {
          device_state_.persisted().charge_complete_showing = false;
          display_.disp_main();
          delay(3000);
          display_.end();
          delay(3000);
          _go_to_sleep();
        } else if (device_state_.persisted().user_msg_showing) {
          device_state_.persisted().user_msg_showing = false;
          display_.disp_main();
          delay(3000);
          display_.end();
          delay(3000);
          _go_to_sleep();
        } else if (device_state_.persisted().check_connection) {
          device_state_.persisted().check_connection = false;
          display_.disp_main();
          delay(3000);
          display_.end();
          delay(3000);
          _go_to_sleep();
        } else {
          // proceed
        }
      }
      break;
    }
    case BootCause::TIMER: {
      if (!device_state_.flags().awake_mode) {
        if (hw_.is_charger_in_standby()) {
          if (!device_state_.persisted().charge_complete_showing) {
            device_state_.persisted().charge_complete_showing = true;
            display_.disp_message_large("Fully\ncharged!");
          }
        }
        // proceed with the heartbeat post
      }
      break;
    }
    default:
      break;
  }

  display_.init_ui_state(UIState{.page = DisplayPage::MAIN});
  network_.set_on_connect(std::bind(&App::_net_on_connect, this));

  debug("Starting main state machine loop");
  while (true) {
    loop();
    _service_display();
    esp_task_wdt_reset();
    delay(10);
  }
}

// ---------------------------------------------------------------------------
// States
// ---------------------------------------------------------------------------

void AppSMStates::InitState::entry() {
  sm().network_.connect();
  sm().bsl_input_.InitPress(sm().wakeup_btn_id_);

  // open settings menu if setup not done
  if (sm().device_state_.flags().awake_mode) {
    if (!sm().device_state_.persisted().wifi_done) {
      sm().info("Wi-Fi setup not done, opening settings menu...");
      return transition_to<SettingsMenuState>();
    } else if (!sm().device_state_.persisted().setup_done) {
      sm().info("setup not done, opening settings menu...");
      return transition_to<SettingsMenuState>();
    }
  }

  if (!sm().device_state_.flags().awake_mode) {
    esp_task_wdt_init(WDT_TIMEOUT_SLEEP, true);
    esp_task_wdt_add(NULL);
    if (sm().boot_cause_ == BootCause::TIMER ||
        sm().boot_cause_ == BootCause::RESET) {
      return transition_to<NetConnectingState>();
    } else {  // button
      return transition_to<SleepModeHandleInput>();
    }
  } else {
    sm().device_state_.persisted().check_connection = false;
    sm().device_state_.persisted().charge_complete_showing = false;
    esp_task_wdt_init(WDT_TIMEOUT_AWAKE, true);
    esp_task_wdt_add(NULL);
    sm().display_.disp_main();
    return transition_to<AwakeModeIdleState>();
  }
}

void AppSMStates::AwakeModeIdleState::entry() {
  sm().display_.disp_main();
  sm().bsl_input_.SetEventCallback(std::bind(
      &AwakeModeIdleState::handle_ui_event, this, std::placeholders::_1));
}

void AppSMStates::AwakeModeIdleState::exit() {
  sm().bsl_input_.ClearEventCallback();
}

void AppSMStates::AwakeModeIdleState::loop() {
  sm()._flush_pending();

  if (!sm().hw_.is_dc_connected()) {
    sm().device_state_.sensors().charging = false;
    return transition_to<CmdShutdownState>();
  }
  if (sm().device_state_.sensors().charging) {
    if (sm().hw_.is_charger_in_standby()) {
      sm().device_state_.sensors().charging = false;
      sm().display_.disp_main();
      if (!sm().device_state_.persisted().user_awake_mode) {
        return transition_to<CmdShutdownState>();
      }
    }
  } else {
    if (!sm().device_state_.persisted().user_awake_mode) {
      return transition_to<CmdShutdownState>();
    }
  }
}

void AppSMStates::AwakeModeIdleState::handle_ui_event(UserInput::Event event) {
  sm().device_state_.flags().last_user_input_time = millis();
  if (event.final) {
    switch (event.type) {
      case UserInput::EventType::kClickSingle:
        sm()._handle_counter_press(event.btn_id);
        break;
      default:
        break;
    }
  } else {
    switch (event.type) {
      case UserInput::EventType::kHoldLong2s:
        if (sm().hw_.num_buttons_pressed() == 1) {
          return transition_to<InfoScreenState>();
        }
        break;
      case UserInput::EventType::kHoldLong5s:
        if (sm().hw_.num_buttons_pressed() == 2) {
          return transition_to<SettingsMenuState>();
        }
        break;
      default:
        break;
    }
  }
}

void AppSMStates::SleepModeHandleInput::entry() {
  sm().input_start_time_ = millis();
  sm().bsl_input_.SetEventCallback(std::bind(
      &SleepModeHandleInput::handle_ui_event, this, std::placeholders::_1));
}

void AppSMStates::SleepModeHandleInput::exit() {
  sm().bsl_input_.ClearEventCallback();
}

void AppSMStates::SleepModeHandleInput::loop() {
  if (millis() - sm().input_start_time_ > SLEEP_MODE_INPUT_TIMEOUT) {
    return transition_to<CmdShutdownState>();
  }
}

void AppSMStates::SleepModeHandleInput::handle_ui_event(
    UserInput::Event event) {
  sm().device_state_.flags().last_user_input_time = millis();
  if (event.final) {
    switch (event.type) {
      case UserInput::EventType::kClickSingle: {
        if (!sm().device_state_.persisted().wifi_done) {
          sm().device_state_.persisted().restart_to_wifi_setup = true;
          sm().device_state_.persisted().silent_restart = true;
          sm().device_state_.save_all();
          sm().info("restarting to Wi-Fi setup...");
          ESP.restart();
        } else if (!sm().device_state_.persisted().setup_done) {
          sm().device_state_.persisted().restart_to_setup = true;
          sm().device_state_.persisted().silent_restart = true;
          sm().device_state_.save_all();
          sm().info("restarting to setup...");
          ESP.restart();
        }
        sm()._handle_counter_press(event.btn_id);
        if (sm().device_state_.sensors().battery_low) {
          sm().display_.disp_message_large(BATT_EMPTY_MSG, 3000);
        }
        sm().user_event_ = event;
        return transition_to<NetConnectingState>();
      }
      default:
        break;
    }
  } else {  // non final event
    switch (event.type) {
      case UserInput::EventType::kHoldLong2s:
        if (sm().hw_.num_buttons_pressed() == 1) {
          return transition_to<InfoScreenState>();
        }
        break;
      case UserInput::EventType::kHoldLong5s:
        if (sm().hw_.num_buttons_pressed() == 2) {
          return transition_to<SettingsMenuState>();
        }
        break;
      default:
        break;
    }
  }
}

void AppSMStates::NetConnectingState::entry() {
  start_time_ = millis();
  sm().bsl_input_.SetEventCallback(std::bind(
      &NetConnectingState::handle_ui_event, this, std::placeholders::_1));
}

void AppSMStates::NetConnectingState::exit() {
  sm().bsl_input_.ClearEventCallback();
}

void AppSMStates::NetConnectingState::loop() {
  if (sm().network_.get_state() == Network::State::W_CONNECTED) {
    sm().device_state_.persisted().failed_connections = 0;
    sm()._flush_pending();
    if (sm().boot_cause_ == BootCause::BUTTON) {
      return transition_to<SessionState>();
    }
    return transition_to<CmdShutdownState>();
    // Upstream compared millis() against the timeout directly, which only
    // happened to work because the device had just booted.
  } else if (millis() - start_time_ >= NET_CONNECT_TIMEOUT) {
    sm().warning("network connect timeout.");
    if (sm().boot_cause_ == BootCause::BUTTON) {
      sm().display_.disp_error("Network\nconnection\nnot\nsuccessful", 3000);
    } else if (sm().boot_cause_ == BootCause::TIMER) {
      sm().device_state_.persisted().failed_connections++;
      if (sm().device_state_.persisted().failed_connections >=
          MAX_FAILED_CONNECTIONS) {
        sm().device_state_.persisted().failed_connections = 0;
        sm().device_state_.persisted().check_connection = true;
        sm().display_.disp_error("Check\nconnection!");
      }
    }
    return transition_to<CmdShutdownState>();
  }
}

void AppSMStates::NetConnectingState::handle_ui_event(UserInput::Event event) {
  sm().device_state_.flags().last_user_input_time = millis();
  if (event.final && event.type == UserInput::EventType::kClickSingle) {
    // Queued now, delivered as soon as the link comes up.
    sm()._handle_counter_press(event.btn_id);
  }
}

void AppSMStates::SessionState::entry() {
  sm().session_last_input_time_ = millis();
  sm().info("session open, sleeping after %u ms idle", SESSION_IDLE_TIMEOUT);
  sm().display_.disp_main();
  sm().bsl_input_.SetEventCallback(
      std::bind(&SessionState::handle_ui_event, this, std::placeholders::_1));
}

void AppSMStates::SessionState::exit() { sm().bsl_input_.ClearEventCallback(); }

void AppSMStates::SessionState::loop() {
  sm()._flush_pending();

  if (millis() - sm().session_last_input_time_ > SESSION_IDLE_TIMEOUT) {
    sm().info("session idle, shutting down");
    return transition_to<CmdShutdownState>();
  }
}

void AppSMStates::SessionState::handle_ui_event(UserInput::Event event) {
  sm().device_state_.flags().last_user_input_time = millis();
  sm().session_last_input_time_ = millis();
  if (event.final) {
    if (event.type == UserInput::EventType::kClickSingle) {
      sm()._handle_counter_press(event.btn_id);
    }
  } else {
    switch (event.type) {
      case UserInput::EventType::kHoldLong2s:
        if (sm().hw_.num_buttons_pressed() == 1) {
          return transition_to<InfoScreenState>();
        }
        break;
      case UserInput::EventType::kHoldLong5s:
        if (sm().hw_.num_buttons_pressed() == 2) {
          return transition_to<SettingsMenuState>();
        }
        break;
      default:
        break;
    }
  }
}

void AppSMStates::InfoScreenState::entry() {
  sm().info_screen_start_time_ = millis();
  sm().display_.disp_info();
  sm().bsl_input_.SetEventCallback(std::bind(&InfoScreenState::handle_ui_event,
                                             this, std::placeholders::_1));
}

void AppSMStates::InfoScreenState::exit() {
  sm().bsl_input_.ClearEventCallback();
}

void AppSMStates::InfoScreenState::loop() {
  if (millis() - sm().info_screen_start_time_ > INFO_SCREEN_DISP_TIME) {
    if (sm().device_state_.flags().awake_mode) {
      return transition_to<AwakeModeIdleState>();
    } else {
      return transition_to<CmdShutdownState>();
    }
  }
}

void AppSMStates::InfoScreenState::handle_ui_event(UserInput::Event event) {
  sm().device_state_.flags().last_user_input_time = millis();
  if (event.final && event.type == UserInput::EventType::kClickSingle) {
    if (sm().device_state_.flags().awake_mode) {
      return transition_to<AwakeModeIdleState>();
    } else {
      return transition_to<CmdShutdownState>();
    }
  } else if (!event.final && event.type == UserInput::EventType::kHoldLong5s) {
    if (sm().hw_.num_buttons_pressed() == 2) {
      return transition_to<SettingsMenuState>();
    }
  }
}

void AppSMStates::SettingsMenuState::entry() {
  sm().settings_menu_start_time_ = millis();
  sm().display_.disp_settings();
  sm().bsl_input_.SetEventCallback(std::bind(
      &SettingsMenuState::handle_ui_event, this, std::placeholders::_1));
}

void AppSMStates::SettingsMenuState::exit() {
  sm().bsl_input_.ClearEventCallback();
}

void AppSMStates::SettingsMenuState::loop() {
  if (millis() - sm().settings_menu_start_time_ > SETTINGS_MENU_TIMEOUT) {
    sm().debug("settings menu timeout");
    if (sm().device_state_.flags().awake_mode) {
      return transition_to<AwakeModeIdleState>();
    } else {
      return transition_to<CmdShutdownState>();
    }
  }
}

void AppSMStates::SettingsMenuState::handle_ui_event(UserInput::Event event) {
  sm().device_state_.flags().last_user_input_time = millis();
  if (event.final) {
    if (event.type == UserInput::EventType::kClickSingle) {
      switch (event.btn_id) {
        case 1:
          // setup
          sm().device_state_.persisted().restart_to_setup = true;
          sm().device_state_.persisted().silent_restart = true;
          sm().device_state_.save_all();
          sm().info("restarting to setup...");
          ESP.restart();
          break;
        case 2:
          // Wi-Fi setup
          sm().device_state_.persisted().restart_to_wifi_setup = true;
          sm().device_state_.persisted().silent_restart = true;
          sm().device_state_.save_all();
          sm().info("restarting to Wi-Fi setup...");
          ESP.restart();
          break;
        case 3:
          // restart
          sm().info("restarting...");
          ESP.restart();
          break;
        case 4:
          // cancel
          if (sm().device_state_.flags().awake_mode) {
            return transition_to<AwakeModeIdleState>();
          } else {
            return transition_to<CmdShutdownState>();
          }
          break;
        default:
          break;
      }
    }
  } else {  // non final
    switch (event.type) {
      case UserInput::EventType::kHoldLong10s:
        if (event.btn_id == 3) {
          // factory reset
          return transition_to<FactoryResetState>();
        }
        break;
      case UserInput::EventType::kHoldLong2s:
        if (event.btn_id == 1) {
          // device info screen
          return transition_to<DeviceInfoState>();
        }
        break;
      default:
        break;
    }
  }
}

void AppSMStates::DeviceInfoState::entry() {
  sm().device_info_start_time_ = millis();
  sm().display_.disp_device_info();
  sm().bsl_input_.SetEventCallback(std::bind(&DeviceInfoState::handle_ui_event,
                                             this, std::placeholders::_1));
}

void AppSMStates::DeviceInfoState::exit() {
  sm().bsl_input_.ClearEventCallback();
}

void AppSMStates::DeviceInfoState::loop() {
  if (millis() - sm().device_info_start_time_ > DEVICE_INFO_TIMEOUT) {
    if (sm().device_state_.flags().awake_mode) {
      return transition_to<AwakeModeIdleState>();
    } else {
      return transition_to<CmdShutdownState>();
    }
  }
}

void AppSMStates::DeviceInfoState::handle_ui_event(UserInput::Event event) {
  sm().device_state_.flags().last_user_input_time = millis();
  if (event.final && event.type == UserInput::EventType::kClickSingle) {
    if (sm().device_state_.flags().awake_mode) {
      return transition_to<AwakeModeIdleState>();
    } else {
      return transition_to<CmdShutdownState>();
    }
  }
}

void AppSMStates::CmdShutdownState::entry() {
  sm().shutdown_cmd_time_ = millis();
  // Anything still queued has to go out before the link drops.
  sm()._flush_pending();
  sm().device_state_.save_all();
  sm().network_.disconnect();
  sm().bsl_input_.Stop();
}

void AppSMStates::CmdShutdownState::loop() {
  if (millis() - sm().shutdown_cmd_time_ > SHUTDOWN_DELAY) {
    return transition_to<NetDisconnectingState>();
  }
}

void AppSMStates::NetDisconnectingState::loop() {
  if (sm().network_.get_state() == Network::State::DISCONNECTED) {
    return transition_to<ShuttingDownState>();
  }
}

void AppSMStates::ShuttingDownState::loop() {
  if (sm().device_state_.flags().display_redraw) {
    sm().device_state_.flags().display_redraw = false;
    sm().display_.disp_main();
    sm().display_.update();
  }
  sm().display_.end();
  delay(100);
  sm()._go_to_sleep();
}

void AppSMStates::FactoryResetState::entry() {
  sm().info("factory reset...");
  sm().display_.disp_message("Factory\nRESET", 0);
  sm().device_state_.clear_all();
  sm().network_.disconnect(true);
}

void AppSMStates::FactoryResetState::loop() {
  if (sm().network_.get_state() == Network::State::DISCONNECTED) {
    sm().display_.disp_message("Factory\nRESET\ncomplete", 3000);
    sm().display_.update();
    delay(3000);
    sm().display_.end();
    delay(1000);
    ESP.restart();
  }
}
