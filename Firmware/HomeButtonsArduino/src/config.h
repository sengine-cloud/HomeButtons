#ifndef HOMEBUTTONS_CONFIG_H
#define HOMEBUTTONS_CONFIG_H

// This fork targets Home Buttons Original (model A1) only, running as a
// two-channel tally counter that reports presses to an HTTPS webhook.
// The mini, pro and industrial variants have been removed, as have the
// MQTT client, Home Assistant discovery, the temperature/humidity sensor
// and the runtime MDI icon downloader.

#if !defined(HOME_BUTTONS_ORIGINAL)
#error "This fork builds the Original (A1) variant only."
#endif

#define HAS_BUTTON_UI
#define HAS_DISPLAY
#define HAS_BATTERY
#define HAS_CHARGER
#define HAS_AWAKE_MODE
#define HAS_SLEEP_MODE

#include <WString.h>
#include <IPAddress.h>

// ------ device ------
static constexpr char MANUFACTURER[] = "PLab";
static constexpr char SW_VERSION[] = "v3.0.0-counter.1";
static constexpr char SW_MODEL_ID[] = "A1";  // must match the burnt eFuse

// ------ URLs ------
static constexpr char DOCS_LINK[] =
    "https://github.com/sengine-cloud/HomeButtons";

// ------ wifi AP ------
// The setup AP password is derived per-device from the eFuse random ID, so
// there is no shared default. See DeviceState::get_ap_password().

// ------ buttons ------
static constexpr uint8_t NUM_BUTTONS = 6;
static constexpr uint8_t BTN_LABEL_MAXLEN = 56;
static constexpr uint8_t USER_MSG_MAXLEN = 64;

// ------ counters ------
// Two independent counters, each with an increment and a decrement button.
// Buttons 5 and 6 are unassigned.
static constexpr uint8_t NUM_COUNTERS = 2;
static constexpr uint8_t BTN_COUNTER_A_INC = 1;
static constexpr uint8_t BTN_COUNTER_A_DEC = 2;
static constexpr uint8_t BTN_COUNTER_B_INC = 3;
static constexpr uint8_t BTN_COUNTER_B_DEC = 4;
static constexpr int32_t COUNTER_MIN = 0;
static constexpr int32_t COUNTER_MAX = 999999;
static constexpr char COUNTER_NAMES[NUM_COUNTERS][2] = {"a", "b"};

// ------ webhook ------
static constexpr size_t ENDPOINT_URL_MAXLEN = 128;
static constexpr size_t AUTH_TOKEN_MAXLEN = 128;
static constexpr uint32_t HTTP_TIMEOUT = 10000L;    // ms
static constexpr uint8_t HTTP_MAX_ATTEMPTS = 3;     // per press, within session
static constexpr uint16_t HTTP_PAYLOAD_SIZE = 512;  // bytes

// ------ defaults ------
static constexpr char DEVICE_NAME_DFLT[] = "Home Buttons";
static constexpr char BNT_LABEL_DFLT_PREFIX[] = "B";

// ------ heartbeat ------
// Timer wake used purely to report battery level when nobody presses a
// button. Stored under the legacy "sen_itv" NVS key to avoid a migration.
static constexpr uint16_t HEARTBEAT_INTERVAL_DFLT = 720;  // min (12 h)
static constexpr uint16_t HEARTBEAT_INTERVAL_MIN = 15;    // min
static constexpr uint16_t HEARTBEAT_INTERVAL_MAX = 1440;  // min (24 h)

// ----- timing ------
static constexpr uint32_t SETUP_TIMEOUT = 600;                // s
static constexpr uint32_t INFO_SCREEN_DISP_TIME = 15000L;     // ms
static constexpr uint32_t WDT_TIMEOUT_AWAKE = 60;             // s
static constexpr uint32_t WDT_TIMEOUT_SLEEP = 60;             // s
static constexpr uint32_t AWAKE_REDRAW_INTERVAL = 1000L;      // ms
static constexpr uint32_t SETTINGS_MENU_TIMEOUT = 30000L;     // ms
static constexpr uint32_t DEVICE_INFO_TIMEOUT = 30000L;       // ms
static constexpr uint32_t SHUTDOWN_DELAY = 500L;              // ms
static constexpr uint32_t SLEEP_MODE_INPUT_TIMEOUT = 10000L;  // ms

// How long the device stays awake with the connection open after a press,
// so a burst of presses shares one Wi-Fi association and TLS handshake.
// Reset on every button event.
static constexpr uint32_t SESSION_IDLE_TIMEOUT = 30000L;  // ms

// ------ network ------
static constexpr uint32_t QUICK_WIFI_TIMEOUT = 5000L;
static constexpr uint32_t WIFI_TIMEOUT = 20000L;
static constexpr uint32_t NET_CONN_CHECK_INTERVAL = 1000L;
static constexpr uint32_t NET_CONNECT_TIMEOUT = 30000L;
static constexpr uint8_t MAX_FAILED_CONNECTIONS = 5;
static const IPAddress DEFAULT_DNS2 = IPAddress(1, 1, 1, 1);

// ------ other ------
static constexpr uint32_t MIN_FREE_HEAP = 10000UL;
static constexpr uint32_t SCHEDULE_WAKEUP_MIN = 5;  // s
static constexpr uint32_t SCHEDULE_WAKEUP_MAX =
    static_cast<uint32_t>(HEARTBEAT_INTERVAL_MAX) * 60;  // s
static constexpr uint16_t LED_DEFAULT_FADE_TIME = 50;    // ms

// ------ UI ------
static constexpr char BATT_EMPTY_MSG[] =
    "Battery\nLOW\n\nPlease\nrecharge\nsoon!";

// ------ LEDs ------
static constexpr uint8_t LED_DFLT_BRIGHT = 100;    // pct
static constexpr uint8_t LED_MIN_BRIGHT = 10;      // pct
static constexpr uint8_t LED_MAX_AMB_BRIGHT = 20;  // pct
static constexpr float LED_GAMMA = 2.2;

// ------ BUTTONS ------
static constexpr uint32_t kBtnDebounceTimeout = 50L;
static constexpr uint32_t kBtnPressTimeout = 500L;
static constexpr uint32_t kBtnTriggerInterval = 250L;

#endif  // HOMEBUTTONS_CONFIG_H
