#ifndef HOMEBUTTONS_HARDWARE_H
#define HOMEBUTTONS_HARDWARE_H

#include <Arduino.h>

#include <semver.hpp>
#include "freertos/semphr.h"

#include "types.h"
#include "config.h"
#include "logger.h"

struct HardwareDefinition : public Logger {
 public:
  HardwareDefinition() : Logger("HW") {}
  HardwareDefinition(const HardwareDefinition &) = delete;
  semver::version version;

  // ------ PIN definitions ------
  uint8_t BTN1_PIN = 0;
  uint8_t BTN2_PIN = 0;
  uint8_t BTN3_PIN = 0;
  uint8_t BTN4_PIN = 0;
  uint8_t BTN5_PIN = 0;
  uint8_t BTN6_PIN = 0;

  bool BTN1_ACTIVE_HIGH = false;
  bool BTN2_ACTIVE_HIGH = false;
  bool BTN3_ACTIVE_HIGH = false;
  bool BTN4_ACTIVE_HIGH = false;
  bool BTN5_ACTIVE_HIGH = false;
  bool BTN6_ACTIVE_HIGH = false;

  uint8_t LED1_PIN = 0;
  uint8_t LED2_PIN = 0;
  uint8_t LED3_PIN = 0;
  uint8_t LED4_PIN = 0;
  uint8_t LED5_PIN = 0;
  uint8_t LED6_PIN = 0;

  // I2C pins. No I2C peripheral is populated on this build (the temperature
  // & humidity sensor has been removed); kept as board documentation.
  uint8_t SDA = 0;
  uint8_t SCL = 0;

  uint8_t VBAT_ADC = 0;
  uint8_t CHARGER_STDBY = 0;
  uint8_t BOOST_EN = 0;
  uint8_t DC_IN_DETECT = 0;
  uint8_t CHG_ENABLE = 0;

  uint8_t EINK_CS = 0;
  uint8_t EINK_DC = 0;
  uint8_t EINK_RST = 0;
  uint8_t EINK_BUSY = 0;

  // ------ LED analog parameters ------
  uint8_t LED1_CH = 0;
  uint8_t LED2_CH = 0;
  uint8_t LED3_CH = 0;
  uint8_t LED4_CH = 0;
  uint8_t LED5_CH = 0;
  uint8_t LED6_CH = 0;

  uint8_t LED_RES = 0;
  uint16_t LED_FREQ = 0;
  uint16_t LED_MAX_PWM = 0;

  // ------ battery reading ------
  float BATT_DIVIDER = 0;
  float BATT_ADC_REF_VOLT = 0;
  float MIN_BATT_VOLT = 0;
  float BATT_HYSTERESIS_VOLT = 0;
  float WARN_BATT_VOLT = 0;
  float BATT_FULL_VOLT = 0;
  float BATT_EMPTY_VOLT = 0;
  float BATT_PRESENT_VOLT = 0;
  float DC_DETECT_VOLT = 0;
  float CHARGE_HYSTERESIS_VOLT = 0;

  // battery SoC linear approximation coefficients (used for lithium cells)
  float BATT_SOC_EST_K = 0;
  float BATT_SOC_EST_N = 0;

  // ------ wakeup ------
  uint64_t WAKE_BITMASK = 0;

  // ------ functions ------
  bool init();

  void begin();

#if defined(HAS_BUTTON_UI)
  uint8_t map_button_num_sw_to_hw(uint8_t hw_num);
  uint8_t button_pin(uint8_t num);
  bool button_pressed(uint8_t num);
  uint8_t num_buttons_pressed();
  bool any_button_pressed();

  void set_led(uint8_t ch, uint16_t brightness,
               uint16_t fade_time = LED_DEFAULT_FADE_TIME);
  void set_led_num(uint8_t num, uint16_t brightness,
                   uint16_t fade_time = LED_DEFAULT_FADE_TIME);
  void set_led_pct_num(uint8_t num, uint8_t brightness_pct,
                       uint16_t fade_time = LED_DEFAULT_FADE_TIME);
  void set_all_leds(uint16_t brightness,
                    uint16_t fade_time = LED_DEFAULT_FADE_TIME);
  void set_all_leds_pct(uint8_t brightness_pct,
                        uint16_t fade_time = LED_DEFAULT_FADE_TIME);

#endif

#if defined(HAS_BATTERY)
  float read_battery_voltage();
  uint8_t read_battery_percent();
  bool is_battery_present();
#endif

#if defined(HAS_CHARGER)
  bool is_charger_in_standby();
  bool is_dc_connected();
  void enable_charger(bool enable);
#endif

  const char *get_serial_number() { return factory_params_.serial_number; }
  const char *get_random_id() { return factory_params_.random_id; }
  const char *get_model_id() { return factory_params_.model_id; }
  const char *get_hw_version() { return factory_params_.hw_version; }
  const char *get_model_name() const { return model_name_; }
  const char *get_unique_id() const { return unique_id_; }

  bool factory_params_ok();

  void set_serial_number(const char *serial_number) {
    memcpy(factory_params_.serial_number, serial_number, 8);
  }
  void set_random_id(const char *random_id) {
    memcpy(factory_params_.random_id, random_id, 6);
  }
  void set_model_id(const char *model_id) {
    memcpy(factory_params_.model_id, model_id, 2);
  }
  void set_hw_version(const char *hw_version) {
    memcpy(factory_params_.hw_version, hw_version, 3);
  }

  void write_factory_params() { _write_efuse(); }

  void load_hw_rev_1_0();
  void load_hw_rev_2_0();
  void load_hw_rev_2_2();
  void load_hw_rev_2_3();
  void load_hw_rev_2_4();
  void load_hw_rev_2_5();

 private:
  struct {
    // members have length +1 for null terminator
    char serial_number[9];
    char random_id[7];
    char model_id[3];
    char hw_version[4];
  } factory_params_{};

  char model_name_[30] = "";
  char unique_id_[22] = "";

  bool _efuse_burned();
  void _read_efuse();
  void _write_efuse();
  void _nvs_2_efuse();
};

#endif  // HOMEBUTTONS_HARDWARE_H
