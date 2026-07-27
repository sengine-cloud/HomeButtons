#ifndef HOMEBUTTONS_STATE_H
#define HOMEBUTTONS_STATE_H

#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"
#include "types.h"
#include "logger.h"
#include "hardware.h"
#include <IPAddress.h>

struct StaticIPConfig {
  bool valid;
  SSIDType ssid;
  IPAddress static_ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns;
  IPAddress dns2;
};

class DeviceState : public Logger {
 private:
  struct Factory {
    SerialNumber serial_number;  // len = 8
    RandomID random_id;          // len = 6
    ModelName model_name;        // 1 <= len <= 20
    ModelID model_id;            // len = 2
    HWVersion hw_version;        // len = 3
    UniqueID unique_id;          // len = 21
  } factory_;

  struct UserPreferences {
    DeviceName device_name;
    ButtonLabel btn_labels[NUM_BUTTONS];
    // Timer wake interval, used only to report battery level when nobody
    // presses a button. Stored under the legacy "sen_itv" NVS key.
    uint16_t heartbeat_interval = 0;  // minutes

    StaticIPConfig network;

    EndpointUrlType endpoint_url;
    AuthTokenType auth_token;
    // When the counters clear themselves. See reset_schedule.h.
    ResetSpecType reset_spec;
    // ISO country code for the Wi-Fi regulatory domain. See config.h.
    CountryCodeType wifi_country;
  } user_preferences_;

  struct Persisted {
    // Vars
    bool low_batt_mode = false;
    bool wifi_done = false;
    bool setup_done = false;
    String last_sw_ver = "";
    bool user_awake_mode = false;

    // Counters. Device is authoritative; the webhook receives absolute values.
    int32_t counters[NUM_COUNTERS] = {0};
    // Monotonic per-device sequence number, used by the receiver to dedupe
    // retries so a replayed press does not notify twice.
    uint32_t seq = 0;

    // Which reset period the current counts belong to, as produced by
    // reset_schedule::period_of(). 0 means "not yet established", which
    // suppresses the first reset so a fresh device does not clear counts
    // it has only just been told about.
    int32_t last_reset_period = 0;
    // Seconds to add to UTC for local time, as reported by the webhook.
    // Persisted so local time is known on wake, before any request.
    int32_t tz_offset = 0;
    // UTC epoch of the last successful clock sync; 0 means never.
    uint32_t last_time_sync = 0;

    // Flags
    bool wifi_quick_connect = false;
    bool charge_complete_showing = false;
    bool user_msg_showing = false;
    bool check_connection = false;
    uint8_t failed_connections = 0;
    bool restart_to_wifi_setup = false;
    bool restart_to_setup = false;
    bool silent_restart = false;
    bool connect_on_restart = false;
  } persisted_;

  struct Flags {
    bool display_redraw = false;
    bool awake_mode = false;
    uint32_t schedule_wakeup_time = 0;
    uint32_t last_user_input_time = 0;
  } flags_;

  struct Sensors {
    uint8_t battery_pct = 0;
    float battery_voltage = 0;
    bool charging = false;
    bool dc_connected = false;
    bool battery_present = false;
    bool battery_low = false;
  } sensors_;

 public:
  DeviceState() : Logger("State") {
    // One Preferences handle serves every caller, and it is written from
    // both the network task (on connect) and the main task (after a press
    // or before sleep). Interleaved begin()/put()/end() on a single handle
    // corrupts it, and the Wi-Fi quick-connect settings live in that
    // namespace - so the failure shows up as Wi-Fi that works only
    // sometimes. Guard every access at the source rather than expecting
    // each call site to remember.
    nvs_mutex_ = xSemaphoreCreateRecursiveMutex();
  }

  // Scoped hold of the NVS handle.
  class NvsLock {
   public:
    explicit NvsLock(SemaphoreHandle_t m) : m_(m) {
      if (m_ != nullptr) xSemaphoreTakeRecursive(m_, portMAX_DELAY);
    }
    ~NvsLock() {
      if (m_ != nullptr) xSemaphoreGiveRecursive(m_);
    }
    NvsLock(const NvsLock&) = delete;

   private:
    SemaphoreHandle_t m_;
  };
  DeviceState(const DeviceState&) = delete;

  // Factory
  const Factory& factory() const { return factory_; }

  // User preferences
  const UserPreferences& user_preferences() const { return user_preferences_; }

  void set_static_ip_config(SSIDType ssid, const IPAddress& static_ip,
                            const IPAddress& gateway, const IPAddress& subnet,
                            const IPAddress& dns = IPAddress(),
                            const IPAddress& dns2 = IPAddress()) {
    user_preferences_.network.valid = false;
    user_preferences_.network.ssid = ssid;
    user_preferences_.network.static_ip = static_ip;
    user_preferences_.network.gateway = gateway;
    user_preferences_.network.subnet = subnet;
    user_preferences_.network.dns = dns;
    user_preferences_.network.dns2 = dns2;
  }

  const StaticIPConfig get_static_ip_config() const {
    return user_preferences_.network;
  }

  void clear_static_ip_config();

  const DeviceName& device_name() const {
    return user_preferences_.device_name;
  }
  void set_device_name(const DeviceName& device_name) {
    user_preferences_.device_name = device_name;
  }

  uint16_t heartbeat_interval() const {
    return user_preferences_.heartbeat_interval;
  }
  void set_heartbeat_interval(uint16_t interval_min) {
    user_preferences_.heartbeat_interval = interval_min;
  }

  const ButtonLabel& get_btn_label(uint8_t i) const;
  void set_btn_label(uint8_t i, const char* label);

  const EndpointUrlType& endpoint_url() const {
    return user_preferences_.endpoint_url;
  }
  void set_endpoint_url(const EndpointUrlType& url) {
    user_preferences_.endpoint_url = url;
  }

  const AuthTokenType& auth_token() const {
    return user_preferences_.auth_token;
  }
  void set_auth_token(const AuthTokenType& token) {
    user_preferences_.auth_token = token;
  }

  const CountryCodeType& wifi_country() const {
    return user_preferences_.wifi_country;
  }
  void set_wifi_country(const CountryCodeType& cc) {
    user_preferences_.wifi_country = cc;
  }

  const ResetSpecType& reset_spec() const {
    return user_preferences_.reset_spec;
  }
  void set_reset_spec(const ResetSpecType& spec) {
    if (user_preferences_.reset_spec == spec) return;
    user_preferences_.reset_spec = spec;
    // A stored period only means anything under the schedule that produced
    // it: period_of() counts days for daily but weeks for weekly and months
    // for monthly, so the same instant maps to ~20600, ~2943 or ~678. Left
    // alone across a mode change, the comparison in _check_reset() is
    // between two different units. Zeroing here re-adopts on the next check
    // rather than leaving it to each caller to remember.
    persisted_.last_reset_period = 0;
  }

  // Clock -------------------------------------------------------------
  int32_t tz_offset() const { return persisted_.tz_offset; }
  uint32_t last_time_sync() const { return persisted_.last_time_sync; }
  bool clock_valid() const { return persisted_.last_time_sync > 0; }

  void set_clock_synced(uint32_t utc_epoch, int32_t tz_offset) {
    persisted_.last_time_sync = utc_epoch;
    persisted_.tz_offset = tz_offset;
  }

  int32_t last_reset_period() const { return persisted_.last_reset_period; }
  void set_last_reset_period(int32_t period) {
    persisted_.last_reset_period = period;
  }

  // Zeroes every counter. Returns true if anything actually changed.
  bool clear_counters() {
    bool changed = false;
    for (uint8_t i = 0; i < NUM_COUNTERS; i++) {
      if (persisted_.counters[i] != 0) changed = true;
      persisted_.counters[i] = 0;
    }
    return changed;
  }

  // Counters -----------------------------------------------------------
  // idx is 0-based. Returns 0 for an out-of-range index rather than
  // reading past the array.
  int32_t counter(uint8_t idx) const {
    if (idx >= NUM_COUNTERS) return 0;
    return persisted_.counters[idx];
  }

  // Applies delta, clamps to [COUNTER_MIN, COUNTER_MAX], returns the new
  // value. A clamped decrement below zero is a no-op rather than an error;
  // the minus button is for corrections and should never go negative.
  int32_t adjust_counter(uint8_t idx, int32_t delta) {
    if (idx >= NUM_COUNTERS) return 0;
    int64_t next = static_cast<int64_t>(persisted_.counters[idx]) + delta;
    if (next < COUNTER_MIN) next = COUNTER_MIN;
    if (next > COUNTER_MAX) next = COUNTER_MAX;
    persisted_.counters[idx] = static_cast<int32_t>(next);
    return persisted_.counters[idx];
  }

  uint32_t seq() const { return persisted_.seq; }
  uint32_t next_seq() { return ++persisted_.seq; }

  void save_user();
  void load_user();
  void clear_user();

  // Others
  const Persisted& persisted() const { return persisted_; }
  Persisted& persisted() { return persisted_; }
  const Flags& flags() const { return flags_; }
  Flags& flags() { return flags_; }
  const Sensors& sensors() const { return sensors_; }
  Sensors& sensors() { return sensors_; }

  void save_persisted();
  void load_persisted();
  void clear_persisted();
  void clear_persisted_flags();

  void save_all();
  void load_all(HardwareDefinition& hw);
  void clear_all();

  size_t get_free_entries();

  SSIDType get_ap_ssid() const {
    return SSIDType("HB-") + factory_.random_id.c_str();
  }

  // Per-device, derived from the eFuse random id which is printed on the
  // case. Upstream shipped a single hardcoded password on every unit.
  const char* get_ap_password() const { return ap_password_.c_str(); }

  void set_ip(const IPAddress& ip_address) {
    ip_address_.set("%u.%u.%u.%u", ip_address[0], ip_address[1], ip_address[2],
                    ip_address[3]);
  }

  const char* ip() const { return ip_address_.c_str(); }

  HostnameType get_hostname() const {
    return HostnameType("HB-") + factory_.random_id.c_str();
  }

  StaticString<50> get_model_name_w_rand_id() const {
    return StaticString<50>("%s %s", factory_.model_name.c_str(),
                            factory_.random_id.c_str());
  }

 private:
  void _load_factory(HardwareDefinition& hw);

  template <std::size_t MAX_SIZE>
  void _load_to_static_string(StaticString<MAX_SIZE>& destination,
                              const char* key, const char* defaultValue) {
    char buffer[MAX_SIZE + 1] = {};  // +1 for '\0' at the end
    auto ret = preferences_.getString(key, buffer, MAX_SIZE + 1);
    if (ret == 0)
      destination.set(defaultValue);
    else
      destination.set(buffer);
  }

  void _load_to_ip_address(IPAddress& destination, const char* key,
                           const char* defaultValue);

  Preferences preferences_;
  SemaphoreHandle_t nvs_mutex_ = nullptr;
  StaticString<15> ip_address_;
  APPasswordType ap_password_;
};

#endif  // HOMEBUTTONS_STATE_H
