#ifndef HOMEBUTTONS_NETWORK_H
#define HOMEBUTTONS_NETWORK_H

#include <Arduino.h>
#include <WiFi.h>

#include "state_machine.h"
#include "logger.h"
#include "state.h"

class DeviceState;
class Network;

namespace NetworkSMStates {
class IdleState : public State<Network> {
 public:
  using State<Network>::State;

  void loop() override;

  const char *get_name() override { return "IdleState"; }
};

class QuickConnectState : public State<Network> {
 public:
  using State<Network>::State;

  void entry() override;
  void loop() override;

  const char *get_name() override { return "QuickConnectState"; }

 private:
  uint32_t start_time_ = 0;
};

class NormalConnectState : public State<Network> {
 public:
  using State<Network>::State;

  void entry() override;
  void loop() override;

  const char *get_name() override { return "NormalConnectState"; }

 private:
  uint32_t start_time_ = 0;
  bool await_confirm_quick_wifi_settings_ = false;
};

class WifiConnectedState : public State<Network> {
 public:
  using State<Network>::State;

  void entry() override;
  void loop() override;

  const char *get_name() override { return "WifiConnectedState"; }
};

class DisconnectState : public State<Network> {
 public:
  using State<Network>::State;

  void entry() override;
  void loop() override;

  const char *get_name() override { return "DisconnectState"; }
};

// Terminal state once the station is associated. Upstream had a further
// MQTTConnectState/FullyConnectedState pair; with the webhook transport
// there is no session to establish beyond Wi-Fi, so association is the
// whole story.
class ConnectedState : public State<Network> {
 public:
  using State<Network>::State;

  void entry() override;
  void loop() override;

  const char *get_name() override { return "ConnectedState"; }

 private:
  uint32_t last_conn_check_time_ = 0;
};
}  // namespace NetworkSMStates

using NetworkStateMachine = StateMachine<
    Network, NetworkSMStates::IdleState, NetworkSMStates::QuickConnectState,
    NetworkSMStates::NormalConnectState, NetworkSMStates::WifiConnectedState,
    NetworkSMStates::DisconnectState, NetworkSMStates::ConnectedState>;

class Network : public NetworkStateMachine, public Logger {
 public:
  enum class State {
    DISCONNECTED,
    W_CONNECTED,
  };

  enum class Command { NONE, CONNECT, DISCONNECT };

  explicit Network(DeviceState &device_state);
  Network(const Network &) = delete;
  ~Network() = default;

  void connect();
  void disconnect(bool erase = false);
  void update();
  void setup();  // Warning: must be called from same task (thread) as update()

  State get_state();

  IPAddress get_ip() { return WiFi.localIP(); }

  int32_t get_rssi() { return WiFi.RSSI(); }

  void set_on_connect(std::function<void()> on_connect);

 private:
  State state_ = State::DISCONNECTED;
  Command command_ = Command::NONE;
  uint32_t cmd_connect_time_ = 0;
  bool erase_ = false;

  DeviceState &device_state_;
  TaskHandle_t network_task_handle_ = nullptr;

  std::function<void()> on_connect_callback_;

  void _pre_wifi_connect();

  friend class NetworkSMStates::IdleState;
  friend class NetworkSMStates::QuickConnectState;
  friend class NetworkSMStates::NormalConnectState;
  friend class NetworkSMStates::WifiConnectedState;
  friend class NetworkSMStates::DisconnectState;
  friend class NetworkSMStates::ConnectedState;
};

StaticIPConfig validate_static_ip_config(StaticIPConfig config);

// Pins the Wi-Fi regulatory domain, with 802.11d disabled so the configured
// range is used always rather than being taken from the AP and reverted on
// disconnect. Must be called after WiFi.mode() - the driver has to be
// initialised - and before any scan or connect.
//
// Both the setup portal's scan and the normal connect path need this: a
// router on channel 12 or 13 is invisible to a scan that does not know
// those channels are permitted, which looks exactly like the network having
// disappeared.
void apply_wifi_country(const char *country_code, const Logger &log);

#endif  // HOMEBUTTONS_NETWORK_H
