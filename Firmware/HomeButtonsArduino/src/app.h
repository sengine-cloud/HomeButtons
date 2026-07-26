#ifndef HOMEBUTTONS_APP_H
#define HOMEBUTTONS_APP_H

#include <array>
#include <atomic>
#include "freertos/FreeRTOS.h"  // must precede queue.h
#include "freertos/queue.h"
#include "state.h"
#include "network.h"
#include "webhook.h"
#include "logger.h"
#include "hardware.h"
#include "setup.h"
#include "display/display.h"
#include "button_ui/btn_sw_led.h"

class App;

enum class BootCause { RESET, TIMER, BUTTON };

namespace AppSMStates {

class InitState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;

  const char* get_name() override { return "InitState"; }
};

class AwakeModeIdleState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void exit() override;
  void loop() override;
  void handle_ui_event(UserInput::Event event);

  const char* get_name() override { return "AwakeModeIdleState"; }
};

class SleepModeHandleInput : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void exit() override;
  void loop() override;
  void handle_ui_event(UserInput::Event event);

  const char* get_name() override { return "SleepModeHandleInput"; }
};

class NetConnectingState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void exit() override;
  void loop() override;
  void handle_ui_event(UserInput::Event event);

  const char* get_name() override { return "NetConnectingState"; }

 private:
  uint32_t start_time_ = 0;
};

// Battery-mode burst window. Entered once the first press has been
// delivered; keeps Wi-Fi and the TLS session open so a run of presses
// shares one association and one handshake. Exits to shutdown after
// SESSION_IDLE_TIMEOUT with no user input.
class SessionState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void exit() override;
  void loop() override;
  void handle_ui_event(UserInput::Event event);

  const char* get_name() override { return "SessionState"; }
};

class InfoScreenState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void exit() override;
  void loop() override;
  void handle_ui_event(UserInput::Event event);

  const char* get_name() override { return "InfoScreenState"; }
};

class SettingsMenuState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void exit() override;
  void loop() override;
  void handle_ui_event(UserInput::Event event);

  const char* get_name() override { return "SettingsMenuState"; }
};

class DeviceInfoState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void exit() override;
  void loop() override;
  void handle_ui_event(UserInput::Event event);

  const char* get_name() override { return "DeviceInfoState"; }
};

class CmdShutdownState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void loop() override;

  const char* get_name() override { return "CmdShutdownState"; }
};

class NetDisconnectingState : public State<App> {
 public:
  using State<App>::State;

  void loop() override;

  const char* get_name() override { return "NetDisconnectingState"; }
};

class ShuttingDownState : public State<App> {
 public:
  using State<App>::State;

  void loop() override;

  const char* get_name() override { return "ShuttingDownState"; }
};

class FactoryResetState : public State<App> {
 public:
  using State<App>::State;

  void entry() override;
  void loop() override;

  const char* get_name() override { return "FactoryResetState"; }
};

}  // namespace AppSMStates

using AppStateMachine = StateMachine<
    App, AppSMStates::InitState, AppSMStates::AwakeModeIdleState,
    AppSMStates::SleepModeHandleInput, AppSMStates::NetConnectingState,
    AppSMStates::SessionState, AppSMStates::InfoScreenState,
    AppSMStates::SettingsMenuState, AppSMStates::DeviceInfoState,
    AppSMStates::CmdShutdownState, AppSMStates::NetDisconnectingState,
    AppSMStates::ShuttingDownState, AppSMStates::FactoryResetState>;

class App : public AppStateMachine, public Logger {
 public:
  App();
  App(const App&) = delete;
  void setup();

 private:
  void _start_esp_sleep();
  void _go_to_sleep();
  void _sleep_or_restart();
  std::pair<BootCause, int16_t> _determine_boot_cause();
  void _log_task_stats();

  static void _ui_task(void* app);
  void _start_ui_task();

  static void _display_task(void* app);
  void _start_display_task();

  static void _network_task(void* app);
  void _start_network_task();

  void _main_task();
  static void _main_task_helper(void* app) {
    static_cast<App*>(app)->_main_task();
  }

  void _begin_hw();
  void _start_tasks();

  void _handle_ui_event_global(UserInput::Event event);
  void _net_on_connect();

  // Counter plumbing ---------------------------------------------------
  // Returns true and fills idx/delta when btn_id is one of the four
  // counter buttons; false for the two unassigned buttons.
  static bool _btn_to_counter(uint8_t btn_id, uint8_t& idx, int32_t& delta);
  // Applies the press locally (counter, label, redraw) and either sends it
  // straight away or queues it until the network is up.
  void _handle_counter_press(uint8_t btn_id);
  void _refresh_counter_labels();
  void _flush_pending();

  DeviceState device_state_;
  TaskHandle_t ui_task_h_ = nullptr;
  TaskHandle_t display_task_h_ = nullptr;
  TaskHandle_t network_task_h_ = nullptr;
  TaskHandle_t main_task_h_ = nullptr;

  BtnSwLED b1_;
  BtnSwLED b2_;
  BtnSwLED b3_;
  BtnSwLED b4_;
  BtnSwLED b5_;
  BtnSwLED b6_;
  BtnSwLEDInput<NUM_BUTTONS> bsl_input_;

  UserInput::Event user_event_ = {};

  Display display_;
  Network network_;
  Webhook webhook_;
  HardwareDefinition hw_;
  HBSetup setup_;

  // Button callbacks run on the UI task, so a press may not block on HTTP or
  // NVS there. handle_ui_event() only touches RAM and pushes onto this queue;
  // the main task drains it in _flush_pending(), which owns the NVS write and
  // the POST.
  static constexpr uint8_t PRESS_QUEUE_SIZE = 8;
  struct PressQueueElement {
    Webhook::Event event;
    uint32_t queued_at;
  };
  QueueHandle_t press_queue_ = nullptr;

  // Presses queued but not yet confirmed delivered, per button. Incremented
  // on the UI task and decremented on the main task, hence atomic.
  //
  // The LED is only released when a button's count returns to zero. Without
  // this, pressing the same button while its first press is still in flight
  // would light the LED, then have the first press's 200 immediately clear
  // it again - LED dark while a press was still pending.
  std::array<std::atomic<uint8_t>, NUM_BUTTONS> inflight_{};
  // Set if any press in the current burst failed, so the button can end on
  // the error pattern rather than simply going dark.
  std::array<std::atomic<bool>, NUM_BUTTONS> send_failed_{};

  BootCause boot_cause_;
  uint8_t wakeup_btn_id_ = 0;

  uint32_t last_m_display_redraw_ = 0;
  uint32_t input_start_time_ = 0;
  uint32_t session_last_input_time_ = 0;
  uint32_t info_screen_start_time_ = 0;
  uint32_t settings_menu_start_time_ = 0;
  uint32_t device_info_start_time_ = 0;
  uint32_t shutdown_cmd_time_ = 0;

  friend class HBSetup;

  friend class AppSMStates::InitState;
  friend class AppSMStates::AwakeModeIdleState;
  friend class AppSMStates::SleepModeHandleInput;
  friend class AppSMStates::NetConnectingState;
  friend class AppSMStates::SessionState;
  friend class AppSMStates::InfoScreenState;
  friend class AppSMStates::SettingsMenuState;
  friend class AppSMStates::DeviceInfoState;
  friend class AppSMStates::CmdShutdownState;
  friend class AppSMStates::NetDisconnectingState;
  friend class AppSMStates::ShuttingDownState;
  friend class AppSMStates::FactoryResetState;
};

#endif  // HOMEBUTTONS_APP_H
