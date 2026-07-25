#include "network.h"
#include <esp_wifi.h>
#include "config.h"
#include "state.h"
#include "utils.h"

String mac2String(uint8_t ar[]) {
  String s;
  for (uint8_t i = 0; i < 6; ++i) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", ar[i]);
    s += buf;
    if (i < 5) s += ':';
  }
  return s;
}

void NetworkSMStates::IdleState::loop() {
  if (sm().command_ == Network::Command::CONNECT) {
    if (sm().device_state_.persisted().wifi_quick_connect) {
      return transition_to<QuickConnectState>();
    } else {
      return transition_to<NormalConnectState>();
    }
  }
}

void NetworkSMStates::QuickConnectState::entry() {
  sm()._pre_wifi_connect();
  sm().info("connecting Wi-Fi (quick mode)...");
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  start_time_ = millis();
  WiFi.begin();
}

void NetworkSMStates::QuickConnectState::loop() {
  if (sm().command_ == Network::Command::DISCONNECT) {
    return transition_to<DisconnectState>();
  } else if (WiFi.status() == WL_CONNECTED) {
    sm().info("Wi-Fi connected (quick mode) in %lu ms.",
              millis() - start_time_);
    return transition_to<WifiConnectedState>();
  } else if (millis() - start_time_ > QUICK_WIFI_TIMEOUT) {
    // try again with normal mode
    sm().info(
        "Wi-Fi connect failed (quick mode). Retrying with normal "
        "mode...");
    sm().device_state_.persisted().wifi_quick_connect = false;
    return transition_to<DisconnectState>();
  }
}

void NetworkSMStates::NormalConnectState::entry() {
  sm()._pre_wifi_connect();
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);

  // get ssid from esp32 saved config
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf)) {
    sm().error("failed to get esp wifi config");
  }
  const char *ssid = reinterpret_cast<const char *>(conf.sta.ssid);
  const char *psk = reinterpret_cast<const char *>(conf.sta.password);
  sm().info("connecting Wi-Fi (normal mode): SSID: %s", ssid);
  WiFi.begin(ssid, psk);

  start_time_ = millis();
  await_confirm_quick_wifi_settings_ = false;
}

void NetworkSMStates::NormalConnectState::loop() {
  if (sm().command_ == Network::Command::DISCONNECT) {
    return transition_to<DisconnectState>();
  } else if (WiFi.status() == WL_CONNECTED) {
    if (await_confirm_quick_wifi_settings_) {
      sm().info("Wi-Fi connected, quick mode settings saved.");
      sm().device_state_.persisted().wifi_quick_connect = true;
      sm().device_state_.save_persisted();
      return transition_to<WifiConnectedState>();
    } else {
      // get bssid and ch, and save it directly to ESP
      sm().info(
          "Wi-Fi connected (normal mode) in %lu ms. Saving settings for quick "
          "mode...",
          millis() - start_time_);

      String ssid = WiFi.SSID();
      String psk = WiFi.psk();
      uint8_t *bssid = WiFi.BSSID();
      int32_t ch = WiFi.channel();
      sm().info("SSID: %s, BSSID: %s, CH: %d", ssid.c_str(),
                mac2String(bssid).c_str(), ch);

      // WiFi.disconnect(); not required, already done in WiFi.begin()
      WiFi.begin(ssid.c_str(), psk.c_str(), ch, bssid, true);
      start_time_ = millis();
      await_confirm_quick_wifi_settings_ = true;
    }
  } else if (millis() - start_time_ >= WIFI_TIMEOUT) {
    sm().warning("Wi-Fi connect failed (normal mode). Retrying...");
    return transition_to<DisconnectState>();
  }
}

void NetworkSMStates::WifiConnectedState::entry() {
  sm().state_ = Network::State::W_CONNECTED;
  sm().device_state_.set_ip(WiFi.localIP());
  sm().info("Wi-Fi connected in %lu ms.", millis() - sm().cmd_connect_time_);
  sm().info("IP: %s", ip_address_to_static_string(WiFi.localIP()).c_str());
  String ssid = WiFi.SSID();
  sm().device_state_.save_all();
  uint8_t *bssid = WiFi.BSSID();
  int32_t ch = WiFi.channel();
  sm().info("SSID: %s, BSSID: %s, CH: %d", ssid.c_str(),
            mac2String(bssid).c_str(), ch);
}

void NetworkSMStates::WifiConnectedState::loop() {
  return transition_to<ConnectedState>();
}

void NetworkSMStates::DisconnectState::entry() {
  sm().info("disconnecting...");
  WiFi.disconnect(true, sm().erase_);
  WiFi.mode(WIFI_OFF);
  sm().state_ = Network::State::DISCONNECTED;
  sm().info("disconnected.");
}

void NetworkSMStates::DisconnectState::loop() {
  return transition_to<IdleState>();
}

void NetworkSMStates::ConnectedState::entry() {
  last_conn_check_time_ = millis();
  if (sm().on_connect_callback_) {
    sm().on_connect_callback_();
  }
}

void NetworkSMStates::ConnectedState::loop() {
  if (sm().command_ == Network::Command::DISCONNECT) {
    return transition_to<DisconnectState>();
  } else if (millis() - last_conn_check_time_ > NET_CONN_CHECK_INTERVAL) {
    if (WiFi.status() != WL_CONNECTED) {
      sm().warning("Wi-Fi connection interrupted. Reconnecting...");
      return transition_to<DisconnectState>();
    }
    last_conn_check_time_ = millis();
  }
}

Network::Network(DeviceState &device_state)
    : NetworkStateMachine("NetworkSM", *this),
      Logger("NET"),
      device_state_(device_state) {}

void Network::connect() {
  command_ = Command::CONNECT;
  cmd_connect_time_ = millis();
  this->erase_ = false;
  debug("cmd connect");
}

void Network::disconnect(bool erase) {
  command_ = Command::DISCONNECT;
  this->erase_ = erase;
  debug("cmd disconnect");
}

void Network::update() { loop(); }

void Network::setup() { network_task_handle_ = xTaskGetCurrentTaskHandle(); }

Network::State Network::get_state() { return state_; }

void Network::set_on_connect(std::function<void()> on_connect) {
  this->on_connect_callback_ = on_connect;
}

void Network::_pre_wifi_connect() {
  WiFi.useStaticBuffers(true);

  StaticIPConfig static_ip_config =
      validate_static_ip_config(device_state_.user_preferences().network);

  if (static_ip_config.valid) {
    info("Using static IP %s, Gateway %s, Subnet %s, DNS1 %s, DNS2 %s",
         ip_address_to_static_string(static_ip_config.static_ip).c_str(),
         ip_address_to_static_string(static_ip_config.gateway).c_str(),
         ip_address_to_static_string(static_ip_config.subnet).c_str(),
         ip_address_to_static_string(static_ip_config.dns).c_str(),
         ip_address_to_static_string(static_ip_config.dns2).c_str());
    WiFi.config(static_ip_config.static_ip, static_ip_config.gateway,
                static_ip_config.subnet, static_ip_config.dns,
                static_ip_config.dns2);
  } else {
    info("Using DHCP. Static IP not set or not valid.");
  }
}

StaticIPConfig validate_static_ip_config(StaticIPConfig config) {
  bool ip_ok = config.static_ip != IPAddress(0, 0, 0, 0);
  bool gw_ok = config.gateway != IPAddress(0, 0, 0, 0);
  bool sn_ok = config.subnet != IPAddress(0, 0, 0, 0);

  if (ip_ok && gw_ok && sn_ok) {
    config.valid = true;
  } else {
    config.valid = false;
    return config;
  }

  if (config.dns == IPAddress(0, 0, 0, 0)) {
    config.dns = config.gateway;
  }
  if (config.dns2 == IPAddress(0, 0, 0, 0)) {
    config.dns2 = DEFAULT_DNS2;
  }
  return config;
}
