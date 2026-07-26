#include "setup.h"
#include "app.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>

#include "utils.h"

static WiFiManager wifi_manager;

// Wi-Fi association retries while bringing the config portal up.
static constexpr int MAX_WIFI_RETRIES_DURING_SETUP = 3;

static WiFiManagerParameter device_name_param("device_name", "Device Name", "",
                                              20);
static WiFiManagerParameter endpoint_url_param("endpoint", "Webhook URL", "",
                                               ENDPOINT_URL_MAXLEN);
// The 5th arg is custom HTML: render the token as a password field so the
// portal page never shows the stored secret in cleartext.
static WiFiManagerParameter auth_token_param("auth_token", "Auth Token", "",
                                             AUTH_TOKEN_MAXLEN,
                                             "type=\"password\"");
// Keeps the device from deep sleeping. Needed to hold a USB CDC console
// open while debugging, since sleep tears the USB device down. Upstream set
// this over MQTT; with MQTT gone the portal is the only way to reach it.
// Drains the battery quickly - leave it off for normal use.
static WiFiManagerParameter awake_mode_param(
    "awake_mode", "Awake Mode (debug, drains battery) - 1 or 0", "", 1);
// off | daily 03:00 | weekly mon 03:00 | monthly 1 03:00
static WiFiManagerParameter reset_spec_param(
    "reset_spec", "Counter Reset (e.g. daily 03:00, weekly mon 03:00, off)",
    "", RESET_SPEC_MAXLEN);
static WiFiManagerParameter static_ip_param("static_ip", "Static IP", "", 15);
static WiFiManagerParameter gateway_param("gateway", "Gateway", "", 15);
static WiFiManagerParameter subnet_param("subnet", "Subnet Mask", "", 15);
static WiFiManagerParameter dns_param("dns", "Primary DNS Server", "", 15);
static WiFiManagerParameter dns2_param("dns2", "Secondary DNS Server", "", 15);

#if defined(HAS_DISPLAY)
static char* button_ids[NUM_BUTTONS];
static char* button_labels[NUM_BUTTONS];
static WiFiManagerParameter* btn_label_params[NUM_BUTTONS];

void allocate_btn_label_params() {
  // The allocations below are never freed (the params live for the lifetime
  // of the portal), so guard against a second call leaking them.
  static bool allocated = false;
  if (allocated) return;

  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    button_ids[i] = new char[11];
    button_labels[i] = new char[17];
    snprintf(button_ids[i], 11, "btn%d_lbl", i + 1);
    snprintf(button_labels[i], 17, "Button %d Label", i + 1);

    btn_label_params[i] = new WiFiManagerParameter(
        button_ids[i], button_labels[i], "", BTN_LABEL_MAXLEN);
  }
  allocated = true;
}

void set_btn_label_params_from_device_state(DeviceState& device_state_) {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    btn_label_params[i]->setValue(device_state_.get_btn_label(i + 1).c_str(),
                                  BTN_LABEL_MAXLEN);
  }
}

void set_device_state_from_btn_label_params(DeviceState& device_state_) {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    device_state_.set_btn_label(i + 1, btn_label_params[i]->getValue());
  }
}
#endif

void HBSetup::start_wifi_setup() {
  info("Wi-Fi setup");
#if defined(HAS_DISPLAY)
  app_.display_.disp_ap_config();
#else
  app_.bsl_input_.LEDPulse(2, LED_DFLT_BRIGHT, 2000);
#endif

#if defined(HOME_BUTTONS_DEBUG)
  Serial.begin(115200);
  wifi_manager.setDebugOutput(true, WM_DEBUG_DEV);
#endif

  WiFi.mode(WIFI_STA);
  wifi_manager.setTitle(app_.device_state_.get_model_name_w_rand_id().c_str());
  wifi_manager.setBreakAfterConfig(true);
  wifi_manager.setDarkMode(true);
  wifi_manager.setShowInfoUpdate(false);
  wifi_manager.setConfigPortalBlocking(false);

  uint32_t setup_start_time = millis();
  wifi_manager.startConfigPortal(app_.device_state_.get_ap_ssid().c_str(),
                                 app_.device_state_.get_ap_password());
  bool user_stopped = false;
  while (true) {
    bool connected = wifi_manager.process();
    if (millis() - setup_start_time > SETUP_TIMEOUT * 1000L) {
      debug("Wi-Fi config portal stopped, timeout");
      break;
    }
    if (connected) {
      debug("Wi-Fi config portal stopped, connected");
      break;
    }
    if (app_.hw_.any_button_pressed()) {
      user_stopped = true;
      debug("Wi-Fi config portal stopped by user");
      break;
    }
    delay(250);
  }

  if (user_stopped) {
    info("Wi-Fi config portal stopped by user");
#if defined(HAS_DISPLAY)
    app_.display_.disp_error("Wi-Fi\nsetup\ncancelled");
    delay(5000);
#else
    app_.bsl_input_.LEDBlink(2, 5, LED_DFLT_BRIGHT, 200, 160, false);
    delay(3000);
#endif
    ESP.restart();
  }

  info("Wi-Fi config portal stopped, trying to connect to Wi-Fi...");

  bool wifi_connected = false;
  WiFi.mode(WIFI_STA);
  uint32_t wifi_start_time = millis();
  WiFi.begin();
  while (true) {
    delay(100);
    if (WiFi.status() == WL_CONNECTED) {
      wifi_connected = true;
      break;
    } else if (millis() - wifi_start_time >= WIFI_TIMEOUT) {
      wifi_connected = false;
      break;
    }
  }

  if (wifi_connected) {
    SSIDType ssid(WiFi.SSID());
    if (!(ssid == app_.device_state_.user_preferences().network.ssid)) {
      app_.device_state_.clear_static_ip_config();
    }
    app_.device_state_.persisted().wifi_done = true;
    app_.device_state_.persisted().restart_to_setup = true;
    app_.device_state_.persisted().silent_restart = true;
    app_.device_state_.save_all();
    info("Wi-Fi connected :)");
#if defined(HAS_DISPLAY)
    app_.display_.disp_message_large("Wi-Fi\nconnected\n:)");
    delay(3000);
#else
    app_.bsl_input_.LEDBlink(2, 2, LED_DFLT_BRIGHT, 500, 400, false);
    delay(3000);
#endif
    ESP.restart();
  } else {
    app_.device_state_.persisted().wifi_done = false;
    app_.device_state_.persisted().silent_restart = true;
    app_.device_state_.save_all();
    warning("Wi-Fi error :(");
#if defined(HAS_DISPLAY)
    app_.display_.disp_error("Wi-Fi\nconnection\nerror");
    delay(5000);
#else
    app_.bsl_input_.LEDBlink(2, 5, LED_DFLT_BRIGHT, 200, 160, false);
    delay(3000);
#endif
    ESP.restart();
  }
}

void HBSetup::save_params_callback() {
  app_.device_state_.set_device_name(DeviceName{device_name_param.getValue()});
  app_.device_state_.set_endpoint_url(
      EndpointUrlType{endpoint_url_param.getValue()});
  app_.device_state_.set_auth_token(AuthTokenType{auth_token_param.getValue()});
  {
    // Normalised through the parser so whatever lands in NVS is canonical
    // and a typo cannot silently disable the reset.
    const ResetSpecType entered{reset_spec_param.getValue()};
    bool ok = false;
    const auto spec = reset_schedule::parse(
        entered.empty() ? RESET_SPEC_DFLT : entered.c_str(), &ok);
    if (!ok) {
      app_.warning("reset spec '%s' not understood, storing default",
                   entered.c_str());
    }
    char canonical[reset_schedule::kSpecMaxLen + 1] = {};
    reset_schedule::format(spec, canonical, sizeof(canonical));
    app_.device_state_.set_reset_spec(ResetSpecType{canonical});
  }
  {
    const char* v = awake_mode_param.getValue();
    app_.device_state_.persisted().user_awake_mode =
        (v != nullptr && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y'));
  }

#if defined(HAS_DISPLAY)
  set_device_state_from_btn_label_params(app_.device_state_);
#endif

  SSIDType ssid(WiFi.SSID());
  IPAddress static_ip, gateway, subnet, dns, dns2;
  static_ip.fromString(static_ip_param.getValue());
  gateway.fromString(gateway_param.getValue());
  subnet.fromString(subnet_param.getValue());
  dns.fromString(dns_param.getValue());
  dns2.fromString(dns2_param.getValue());
  app_.device_state_.set_static_ip_config(ssid, static_ip, gateway, subnet, dns,
                                          dns2);
  web_portal_saved_ = true;
}

void HBSetup::start_setup() {
  info("Setup");
  // config
  wifi_manager.setTitle(app_.device_state_.get_model_name_w_rand_id().c_str());
  wifi_manager.setSaveParamsCallback(
      std::bind(&HBSetup::save_params_callback, this));
  wifi_manager.setBreakAfterConfig(true);
  wifi_manager.setParamsPage(true);
  wifi_manager.setDarkMode(true);
  wifi_manager.setShowInfoUpdate(true);

  // hostname
  wifi_manager.setHostname(app_.device_state_.get_hostname().c_str());

  // parameters
  device_name_param.setValue(app_.device_state_.device_name().c_str(), 20);
  endpoint_url_param.setValue(app_.device_state_.endpoint_url().c_str(),
                              ENDPOINT_URL_MAXLEN);
  auth_token_param.setValue(app_.device_state_.auth_token().c_str(),
                            AUTH_TOKEN_MAXLEN);
  awake_mode_param.setValue(
      app_.device_state_.persisted().user_awake_mode ? "1" : "0", 1);
  reset_spec_param.setValue(app_.device_state_.reset_spec().c_str(),
                            RESET_SPEC_MAXLEN);
  static_ip_param.setValue(app_.device_state_.user_preferences()
                               .network.static_ip.toString()
                               .c_str(),
                           15);
  gateway_param.setValue(
      app_.device_state_.user_preferences().network.gateway.toString().c_str(),
      15);
  subnet_param.setValue(
      app_.device_state_.user_preferences().network.subnet.toString().c_str(),
      15);
  dns_param.setValue(
      app_.device_state_.user_preferences().network.dns.toString().c_str(), 15);
  dns2_param.setValue(
      app_.device_state_.user_preferences().network.dns2.toString().c_str(),
      15);

#if defined(HAS_DISPLAY)
  allocate_btn_label_params();
  set_btn_label_params_from_device_state(app_.device_state_);
#endif

  wifi_manager.addParameter(&device_name_param);
  wifi_manager.addParameter(&endpoint_url_param);
  wifi_manager.addParameter(&auth_token_param);
  wifi_manager.addParameter(&awake_mode_param);
  wifi_manager.addParameter(&reset_spec_param);
  wifi_manager.addParameter(&static_ip_param);
  wifi_manager.addParameter(&gateway_param);
  wifi_manager.addParameter(&subnet_param);
  wifi_manager.addParameter(&dns_param);
  wifi_manager.addParameter(&dns2_param);

#if defined(HAS_DISPLAY)
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    wifi_manager.addParameter(btn_label_params[i]);
  }
#endif

#if defined(HAS_DISPLAY)
  app_.display_.disp_message("Entering\nSETUP...");
#else
  app_.bsl_input_.LEDPulse(1, LED_DFLT_BRIGHT, 500);
  delay(2000);
#endif

  // set static IP if configured
  StaticIPConfig static_ip_config =
      validate_static_ip_config(app_.device_state_.user_preferences().network);
  if (static_ip_config.valid) {
    WiFi.config(static_ip_config.static_ip, static_ip_config.gateway,
                static_ip_config.subnet, static_ip_config.dns,
                static_ip_config.dns2);
    info("Using static IP %s, Gateway %s, Subnet %s, DNS1 %s, DNS2 %s",
         ip_address_to_static_string(static_ip_config.static_ip).c_str(),
         ip_address_to_static_string(static_ip_config.gateway).c_str(),
         ip_address_to_static_string(static_ip_config.subnet).c_str(),
         ip_address_to_static_string(static_ip_config.dns).c_str(),
         ip_address_to_static_string(static_ip_config.dns2).c_str());
  } else {
    info("Using DHCP. Static IP not set or not valid.");
  }

  // connect Wi-Fi
  WiFi.mode(WIFI_STA);
  int remaining_tries = MAX_WIFI_RETRIES_DURING_SETUP;

  while (true) {
    uint32_t wifi_start_time = millis();
    WiFi.begin();
    while (WiFi.status() != WL_CONNECTED) {
      delay(100);
      if (millis() - wifi_start_time >= WIFI_TIMEOUT) {
        break;
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      break;
    } else if (remaining_tries > 0) {
      remaining_tries--;
      warning("Wi-Fi error, retrying (remaining tries: %d)", remaining_tries);
      WiFi.disconnect();
      delay(1000);
    } else {
      app_.device_state_.persisted().silent_restart = true;
      app_.device_state_.save_all();
      warning("Wi-Fi error.");
#if defined(HAS_DISPLAY)
      app_.display_.disp_error("Wi-Fi\nerror");
      delay(3000);
#else
      app_.bsl_input_.LEDBlink(1, 5, LED_DFLT_BRIGHT, 200, 160, false);
      delay(3000);
#endif
      ESP.restart();
    }
  }

  // mDNS
  MDNS.begin(app_.device_state_.get_hostname().c_str());

  app_.device_state_.set_ip(WiFi.localIP());
  info("Connect to http://%s or http://%s.local",
       ip_address_to_static_string(WiFi.localIP()).c_str(),
       app_.device_state_.get_hostname().c_str());
#if defined(HAS_DISPLAY)
  app_.display_.disp_web_config();
#else
  app_.bsl_input_.LEDPulse(1, LED_DFLT_BRIGHT, 2000);
#endif
  uint32_t setup_start_time = millis();

  web_portal_saved_ = false;
  wifi_manager.startWebPortal();
  while (millis() - setup_start_time < SETUP_TIMEOUT * 1000L) {
    wifi_manager.process();
    if (app_.hw_.any_button_pressed() || web_portal_saved_) {
      break;
    }
    delay(10);
  }
  wifi_manager.stopWebPortal();

  if (!web_portal_saved_) {
    debug("User triggered exit setup");
    app_.device_state_.persisted().silent_restart = true;
    app_.device_state_.save_all();
    ESP.restart();
  }

  // A device with no webhook URL has nowhere to report presses, so treat an
  // empty URL as a failed setup instead of silently completing.
  if (app_.device_state_.endpoint_url().length() == 0) {
    app_.device_state_.persisted().setup_done = false;
    app_.device_state_.persisted().silent_restart = true;
    app_.device_state_.save_all();
    warning("Webhook URL not set.");
#if defined(HAS_DISPLAY)
    app_.display_.disp_error("Webhook\nURL\nmissing");
    delay(3000);
#else
    app_.bsl_input_.LEDBlink(1, 5, LED_DFLT_BRIGHT, 200, 160, false);
    delay(3000);
#endif
    ESP.restart();
  }

  WiFi.disconnect(true);
  app_.device_state_.persisted().setup_done = true;
  app_.device_state_.persisted().silent_restart = true;
  app_.device_state_.persisted().connect_on_restart = true;
  app_.device_state_.save_all();

  info("setup successful");
#if defined(HAS_DISPLAY)
  app_.display_.disp_message_large("Setup\ncomplete\n:)");
  delay(3000);
#else
  app_.bsl_input_.LEDBlink(1, 2, LED_DFLT_BRIGHT, 500, 400, false);
  delay(3000);
#endif
  ESP.restart();
}
