#ifndef HOMEBUTTONS_TYPES_H
#define HOMEBUTTONS_TYPES_H

#include "static_string.h"
#include "config.h"

using SerialNumber = StaticString<8>;
using RandomID = StaticString<6>;
using ModelName = StaticString<30>;
using ModelID = StaticString<2>;
using HWVersion = StaticString<3>;
using UniqueID = StaticString<21>;

using DeviceName = StaticString<20>;
using ButtonLabel = StaticString<BTN_LABEL_MAXLEN>;
using MDIName = StaticString<48>;
using UserMessage = StaticString<USER_MSG_MAXLEN>;

using EndpointUrlType = StaticString<ENDPOINT_URL_MAXLEN>;
using AuthTokenType = StaticString<AUTH_TOKEN_MAXLEN>;
using ResetSpecType = StaticString<RESET_SPEC_MAXLEN>;
using BuildIdType = StaticString<BUILD_ID_MAXLEN>;
using CountryCodeType = StaticString<WIFI_COUNTRY_MAXLEN>;
using PayloadType = StaticString<HTTP_PAYLOAD_SIZE>;

using SSIDType = StaticString<32>;
using HostnameType = StaticString<32>;

// AP password is "HB-" + the 6-char eFuse random id, so 9 chars — comfortably
// over the 8-char WPA2 minimum.
using APPasswordType = StaticString<16>;

enum class DisplayPage {
  EMPTY,
  MAIN,
  INFO,
  DEVICE_INFO,
  MESSAGE,
  MESSAGE_LARGE,
  ERROR,
  WELCOME,
  SETTINGS,
  AP_CONFIG,
  WEB_CONFIG,
  TEST
};

struct UIState {
  static constexpr size_t MAX_MESSAGE_SIZE = 64;
  using MessageType = StaticString<MAX_MESSAGE_SIZE>;

  DisplayPage page = DisplayPage::EMPTY;
  MessageType message{};
  MDIName mdi_name{};
  uint16_t mdi_size = 0;
  bool disappearing = false;
  uint32_t appear_time = 0;
  uint32_t disappear_timeout = 0;
};

enum class LabelType : uint8_t { None, Text, Icon, Mixed };

#endif  // HOMEBUTTONS_TYPES_H
