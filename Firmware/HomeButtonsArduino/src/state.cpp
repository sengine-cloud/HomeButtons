#include "state.h"
#include "utils.h"
#include "config.h"

void DeviceState::save_user() {
  preferences_.begin("user", false);
  preferences_.putString("device_name", user_preferences_.device_name.c_str());
  for (int i = 0; i < NUM_BUTTONS; i++) {
    preferences_.putString(StaticString<9>("btn%d_txt", i + 1).c_str(),
                           user_preferences_.btn_labels[i].c_str());
  }
  preferences_.putUInt("sen_itv", user_preferences_.heartbeat_interval);
  preferences_.putString("ssid", user_preferences_.network.ssid.c_str());
  preferences_.putString(
      "sta_ip",
      ip_address_to_static_string(user_preferences_.network.static_ip).c_str());
  preferences_.putString(
      "g_way",
      ip_address_to_static_string(user_preferences_.network.gateway).c_str());
  preferences_.putString(
      "s_net",
      ip_address_to_static_string(user_preferences_.network.subnet).c_str());
  preferences_.putString(
      "dns",
      ip_address_to_static_string(user_preferences_.network.dns).c_str());
  preferences_.putString(
      "dns2",
      ip_address_to_static_string(user_preferences_.network.dns2).c_str());
  preferences_.putString("endpoint", user_preferences_.endpoint_url.c_str());
  preferences_.putString("auth_tok", user_preferences_.auth_token.c_str());
  preferences_.end();
}

void DeviceState::load_user() {
  preferences_.begin("user", true);
  _load_to_static_string(
      user_preferences_.device_name, "device_name",
      (DeviceName{DEVICE_NAME_DFLT} + " " + factory_.random_id).c_str());

  // Defaults describe the counter layout: buttons 1/3 increment counters A/B
  // and show the current value, buttons 2/4 decrement, 5/6 are unassigned.
  static const char* kDefaultLabels[NUM_BUTTONS] = {"A 0", "A -1", "B 0",
                                                    "B -1", "",    ""};
  for (int i = 0; i < NUM_BUTTONS; i++) {
    _load_to_static_string(user_preferences_.btn_labels[i],
                           StaticString<9>("btn%d_txt", i + 1).c_str(),
                           kDefaultLabels[i]);
  }

  user_preferences_.heartbeat_interval =
      preferences_.getUInt("sen_itv", HEARTBEAT_INTERVAL_DFLT);

  _load_to_static_string(user_preferences_.network.ssid, "ssid", "");
  _load_to_ip_address(user_preferences_.network.static_ip, "sta_ip", "0.0.0.0");
  _load_to_ip_address(user_preferences_.network.gateway, "g_way", "0.0.0.0");
  _load_to_ip_address(user_preferences_.network.subnet, "s_net", "0.0.0.0");
  _load_to_ip_address(user_preferences_.network.dns, "dns", "0.0.0.0");
  _load_to_ip_address(user_preferences_.network.dns2, "dns2", "0.0.0.0");

  _load_to_static_string(user_preferences_.endpoint_url, "endpoint", "");
  _load_to_static_string(user_preferences_.auth_token, "auth_tok", "");

  preferences_.end();
}

void DeviceState::clear_user() {
  preferences_.begin("user", false);
  preferences_.clear();
  preferences_.end();
}

void DeviceState::clear_static_ip_config() {
  user_preferences_.network.static_ip = IPAddress();
  user_preferences_.network.gateway = IPAddress();
  user_preferences_.network.subnet = IPAddress();
  user_preferences_.network.dns = IPAddress();
  user_preferences_.network.dns2 = IPAddress();
  save_user();
}

void DeviceState::save_persisted() {
  preferences_.begin("persisted", false);
  preferences_.putBool("lb_mode", persisted_.low_batt_mode);
  preferences_.putBool("wifi_done", persisted_.wifi_done);
  preferences_.putBool("setup_done", persisted_.setup_done);
  preferences_.putString("last_sw", persisted_.last_sw_ver);
  preferences_.putBool("u_awake", persisted_.user_awake_mode);
  for (uint8_t i = 0; i < NUM_COUNTERS; i++) {
    preferences_.putInt(StaticString<8>("cnt_%d", i).c_str(),
                        persisted_.counters[i]);
  }
  preferences_.putUInt("seq", persisted_.seq);
  preferences_.putBool("wifi_qc", persisted_.wifi_quick_connect);
  preferences_.putBool("chg_cpt_shwn", persisted_.charge_complete_showing);
  preferences_.putBool("u_msg_shwn", persisted_.user_msg_showing);
  preferences_.putBool("chk_conn", persisted_.check_connection);
  preferences_.putUInt("faild_cons", persisted_.failed_connections);
  preferences_.putBool("rst_to_w_stp", persisted_.restart_to_wifi_setup);
  preferences_.putBool("rst_to_stp", persisted_.restart_to_setup);
  preferences_.putBool("silent_rst", persisted_.silent_restart);
  preferences_.putBool("con_on_r", persisted_.connect_on_restart);
  preferences_.end();
}

void DeviceState::load_persisted() {
  // Read-only: upstream opened this namespace read-write on every boot.
  preferences_.begin("persisted", true);
  persisted_.low_batt_mode = preferences_.getBool("lb_mode", false);
  persisted_.wifi_done = preferences_.getBool("wifi_done", false);
  persisted_.setup_done = preferences_.getBool("setup_done", false);
  persisted_.last_sw_ver = preferences_.getString("last_sw", "");
  persisted_.user_awake_mode = preferences_.getBool("u_awake", false);
  for (uint8_t i = 0; i < NUM_COUNTERS; i++) {
    persisted_.counters[i] =
        preferences_.getInt(StaticString<8>("cnt_%d", i).c_str(), 0);
  }
  persisted_.seq = preferences_.getUInt("seq", 0);
  persisted_.wifi_quick_connect = preferences_.getBool("wifi_qc", false);
  persisted_.charge_complete_showing =
      preferences_.getBool("chg_cpt_shwn", false);
  persisted_.user_msg_showing = preferences_.getBool("u_msg_shwn", false);
  persisted_.check_connection = preferences_.getBool("chk_conn", false);
  persisted_.failed_connections = preferences_.getUInt("faild_cons", 0);
  persisted_.restart_to_wifi_setup =
      preferences_.getBool("rst_to_w_stp", false);
  persisted_.restart_to_setup = preferences_.getBool("rst_to_stp", false);
  persisted_.silent_restart = preferences_.getBool("silent_rst", false);
  persisted_.connect_on_restart = preferences_.getBool("con_on_r", false);
  preferences_.end();
}

void DeviceState::clear_persisted() {
  preferences_.begin("persisted", false);
  preferences_.clear();
  preferences_.end();
}

void DeviceState::clear_persisted_flags() {
  persisted_.wifi_quick_connect = false;
  persisted_.charge_complete_showing = false;
  persisted_.check_connection = false;
  persisted_.failed_connections = 0;
  persisted_.restart_to_wifi_setup = false;
  persisted_.restart_to_setup = false;
  persisted_.silent_restart = false;
  persisted_.connect_on_restart = false;
  save_all();
}

void DeviceState::_load_factory(HardwareDefinition& hw) {
  factory_.serial_number = hw.get_serial_number();
  factory_.random_id = hw.get_random_id();
  factory_.model_name = hw.get_model_name();
  factory_.model_id = hw.get_model_id();
  factory_.hw_version = hw.get_hw_version();
  factory_.unique_id = hw.get_unique_id();
  ap_password_ = APPasswordType("HB-") + factory_.random_id.c_str();
}

void DeviceState::save_all() {
  debug("state save all");
  save_user();
  save_persisted();
}

void DeviceState::load_all(HardwareDefinition& hw) {
  debug("state load all");
  _load_factory(hw);
  load_user();
  load_persisted();
  size_t free_entries = get_free_entries();
  info("nvs free entries: %d", free_entries);
}

void DeviceState::clear_all() {
  debug("state clear all");
  clear_user();
  clear_persisted();
}

size_t DeviceState::get_free_entries() { return preferences_.freeEntries(); }

const ButtonLabel& DeviceState::get_btn_label(uint8_t i) const {
  static ButtonLabel noLabel;
  if (i > 0 && i <= NUM_BUTTONS) {
    return user_preferences_.btn_labels[i - 1];
  } else {
    return noLabel;
  }
}

void DeviceState::set_btn_label(uint8_t i, const char* label) {
  if (i > 0 && i <= NUM_BUTTONS) {
    user_preferences_.btn_labels[i - 1].set(label);
  }
}

void DeviceState::_load_to_ip_address(IPAddress& destination, const char* key,
                                      const char* defaultValue) {
  // Zero-initialised: Preferences::getString does not guarantee NUL
  // termination on every path, and an IPv4 string is exactly at the limit.
  char buffer[16] = {};
  auto ret = preferences_.getString(key, buffer, sizeof(buffer));
  if (ret == 0)
    destination.fromString(defaultValue);
  else
    destination.fromString(buffer);
}
