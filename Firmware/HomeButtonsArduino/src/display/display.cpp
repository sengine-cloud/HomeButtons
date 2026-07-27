#include "display.h"

#include <GxEPD2_BW.h>
#include <SPIFFS.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <qrcode.h>

#include "bitmaps.h"
#include "config.h"
#include "hardware.h"

static constexpr uint16_t ROTATION = 0;
static constexpr uint16_t WIDTH = 128;
static constexpr uint16_t HEIGHT = 296;

// Pre-loaded icons live at /mdi/<size>/<name>.bmp in SPIFFS. Longest path is
// "/mdi/" + 3-digit size + "/" + a 48-char MDIName + ".bmp" = 61 chars.
using MDIPath = StaticString<80>;

uint16_t read16(File &f) {
  // BMP data is stored little-endian, same as Arduino.
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read();  // LSB
  ((uint8_t *)&result)[1] = f.read();  // MSB

  return result;
}

uint32_t read32(File &f) {
  // BMP data is stored little-endian, same as Arduino.
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read();  // LSB
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read();  // MSB
  return result;
}

LabelType Display::get_label_type(ButtonLabel label) {
  if (label.substring(0, 4) == "mdi:") {
    if (label.index_of(' ') > 0) {
      return LabelType::Mixed;
    } else {
      return LabelType::Icon;
    }
  } else {
    return LabelType::Text;
  }
}

MDIName Display::get_mdi_name(ButtonLabel label) {
  if (get_label_type(label) == LabelType::Icon) {
    return MDIName{label.substring(4)};
  } else if (get_label_type(label) == LabelType::Mixed) {
    return MDIName{label.substring(4, label.index_of(' '))};
  } else {
    return MDIName{};
  }
}

ButtonLabel Display::get_text(ButtonLabel label) {
  if (get_label_type(label) == LabelType::Text) {
    return label;
  } else if (get_label_type(label) == LabelType::Mixed) {
    return label.substring(label.index_of(' ') + 1);
  } else {
    return ButtonLabel{};
  }
}

// True when a label is nothing but a number, optionally negative - which in
// practice means a counter total. Those get a much larger font and are never
// trimmed, so a count reads at a glance from across the room.
static bool is_numeric_label(const ButtonLabel& label) {
  size_t i = (label[0] == '-') ? 1 : 0;
  if (i >= label.length()) return false;  // empty, or "-" on its own
  for (; i < label.length(); i++) {
    if (label[i] < '0' || label[i] > '9') return false;
  }
  return true;
}

// Shrinks label one character at a time, keeping a trailing ".", until it is
// narrower than max_width. The length() > 1 bound is load bearing: without it
// a max_width smaller than a single glyph underflows length() - 2 to SIZE_MAX,
// substring() then returns the whole string unchanged and the loop spins
// forever. A label that still does not fit at one character is returned as is.
ButtonLabel Display::trim_text(ButtonLabel label, uint16_t max_width) {
  while (u8g2.getUTF8Width(label.c_str()) >= max_width && label.length() > 1) {
    label = label.substring(0, label.length() - 2) + ".";
  }
  return label;
}

void Display::begin(HardwareDefinition &HW) {
  if (state != State::IDLE) return;
  // Icons are flashed into SPIFFS, so the mount is permanent for the lifetime
  // of the process - it must not be torn down between draws.
  if (!spiffs_mounted_) {
    spiffs_mounted_ = SPIFFS.begin(true);
    if (!spiffs_mounted_) {
      error("SPIFFS mount failed, icons will not be available");
    }
  }
  disp = new GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS,
                                  MAX_HEIGHT(GxEPD2_DRIVER_CLASS)>(
      GxEPD2_DRIVER_CLASS(/*CS=*/HW.EINK_CS, /*DC=*/HW.EINK_DC,
                          /*RST=*/HW.EINK_RST, /*BUSY=*/HW.EINK_BUSY));
  disp->init(0, false);
  u8g2.begin(*disp);
  current_ui_state = {};
  cmd_ui_state = {};
  draw_ui_state = {};
  pre_disappear_ui_state = {};
  state = State::ACTIVE;
  info("begin");
}

void Display::end() {
  if (state != State::ACTIVE) return;
  state = State::CMD_END;
  debug("cmd end");
}

void Display::update() {
  if (state == State::IDLE) return;

  if (state == State::CMD_END && !new_ui_cmd) {
    state = State::ENDING;
    if (current_ui_state.disappearing) {
      draw_ui_state = pre_disappear_ui_state;
    } else {
      disp->hibernate();
      state = State::IDLE;
      info("ended.");
      return;
    }
  } else if (current_ui_state.disappearing) {
    if (millis() - current_ui_state.appear_time >=
        current_ui_state.disappear_timeout) {
      if (new_ui_cmd) {
        draw_ui_state = cmd_ui_state;
        cmd_ui_state = {};
        new_ui_cmd = false;
      } else {
        draw_ui_state = pre_disappear_ui_state;
        pre_disappear_ui_state = {};
      }
    } else {
      return;
    }
  } else if (new_ui_cmd) {
    if (cmd_ui_state.disappearing) {
      pre_disappear_ui_state = current_ui_state;
    }
    draw_ui_state = cmd_ui_state;
    cmd_ui_state = {};
    new_ui_cmd = false;
  } else {
    return;
  }

  debug("update: page: %d; disappearing: %d, msg: %s",
        static_cast<int>(draw_ui_state.page), draw_ui_state.disappearing,
        draw_ui_state.message.c_str());

  redraw_in_progress = true;
  switch (draw_ui_state.page) {
    case DisplayPage::EMPTY:
      draw_white();
      break;
    case DisplayPage::MAIN:
      draw_main();
      break;
    case DisplayPage::INFO:
      draw_info();
      break;
    case DisplayPage::DEVICE_INFO:
      draw_device_info();
      break;
    case DisplayPage::MESSAGE:
      draw_message(draw_ui_state.message);
      break;
    case DisplayPage::MESSAGE_LARGE:
      draw_message(draw_ui_state.message, false, true);
      break;
    case DisplayPage::ERROR:
      draw_message(draw_ui_state.message, true, false);
      break;
    case DisplayPage::WELCOME:
      draw_welcome();
      break;
    case DisplayPage::SETTINGS:
      draw_settings();
      break;
    case DisplayPage::AP_CONFIG:
      draw_ap_config();
      break;
    case DisplayPage::WEB_CONFIG:
      draw_web_config();
      break;
    case DisplayPage::TEST:
      draw_test(draw_ui_state.message.c_str(), draw_ui_state.mdi_name.c_str(),
                draw_ui_state.mdi_size);
      break;
  }
  current_ui_state = draw_ui_state;
  current_ui_state.appear_time = millis();
  draw_ui_state = {};
  redraw_in_progress = false;

  if (state == State::ENDING) {
    disp->hibernate();
    state = State::IDLE;
    info("ended.");
  }
}

void Display::disp_message(const char *message, uint32_t duration) {
  UIState new_cmd_state{DisplayPage::MESSAGE, UIState::MessageType{message}};
  if (duration > 0) {
    new_cmd_state.disappearing = true;
    new_cmd_state.disappear_timeout = duration;
  }
  set_cmd_state(new_cmd_state);
}

void Display::disp_message_large(const char *message, uint32_t duration) {
  UIState new_cmd_state{DisplayPage::MESSAGE_LARGE,
                        UIState::MessageType{message}};
  if (duration > 0) {
    new_cmd_state.disappearing = true;
    new_cmd_state.disappear_timeout = duration;
  }
  set_cmd_state(new_cmd_state);
}
void Display::disp_error(const char *message, uint32_t duration) {
  UIState new_cmd_state{DisplayPage::ERROR, UIState::MessageType{message}};
  if (duration > 0) {
    new_cmd_state.disappearing = true;
    new_cmd_state.disappear_timeout = duration;
  }
  set_cmd_state(new_cmd_state);
}

void Display::disp_main() {
  UIState new_cmd_state;
  if (device_state_.persisted().setup_done &&
      device_state_.persisted().wifi_done) {
    new_cmd_state = {DisplayPage::MAIN};
  } else {
    new_cmd_state = {DisplayPage::WELCOME};
  }
  set_cmd_state(new_cmd_state);
}

void Display::disp_info() {
  UIState new_cmd_state{DisplayPage::INFO};
  set_cmd_state(new_cmd_state);
}

void Display::disp_device_info() {
  UIState new_cmd_state{DisplayPage::DEVICE_INFO};
  set_cmd_state(new_cmd_state);
}

void Display::disp_welcome() {
  UIState new_cmd_state{DisplayPage::WELCOME};
  set_cmd_state(new_cmd_state);
}

void Display::disp_settings() {
  UIState new_cmd_state{DisplayPage::SETTINGS};
  set_cmd_state(new_cmd_state);
}
void Display::disp_ap_config() {
  UIState new_cmd_state{DisplayPage::AP_CONFIG};
  set_cmd_state(new_cmd_state);
}

void Display::disp_web_config() {
  UIState new_cmd_state{DisplayPage::WEB_CONFIG};
  set_cmd_state(new_cmd_state);
}

void Display::disp_test(const char *text, const char *mdi_name,
                        uint16_t mdi_size) {
  UIState new_cmd_state{};
  new_cmd_state.page = DisplayPage::TEST;
  new_cmd_state.message = UIState::MessageType{text};
  new_cmd_state.mdi_name = MDIName{mdi_name};
  new_cmd_state.mdi_size = mdi_size;
  set_cmd_state(new_cmd_state);
}

UIState Display::get_ui_state() { return current_ui_state; }

void Display::init_ui_state(UIState ui_state) { current_ui_state = ui_state; }

Display::State Display::get_state() { return state; }

void Display::set_cmd_state(UIState cmd) {
  cmd_ui_state = cmd;
  new_ui_cmd = true;
}

void Display::draw_message(const UIState::MessageType &message, bool error,
                           bool large) {
  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(text_color);
  u8g2.setBackgroundColor(bg_color);

  disp->fillScreen(bg_color);

  if (!error) {
    if (!large) {
      u8g2.setFont(u8g2_font_courR12_tr);
      u8g2.setCursor(0, 20);
    } else {
      u8g2.setFont(u8g2_font_helvB18_te);
      u8g2.setCursor(0, 30);
    }
    u8g2.print(message.c_str());
  } else {
    u8g2.setFont(u8g2_font_helvB12_tr);
    const char *text = "ERROR";
    uint16_t w = u8g2.getUTF8Width(text);
    u8g2.setCursor(WIDTH / 2 - w / 2, 20);
    u8g2.print(text);
    u8g2.setFont(u8g2_font_courR12_tr);
    u8g2.setCursor(0, 60);
    u8g2.print(message.c_str());
  }

  disp->display();
}

void Display::draw_main() {
  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(text_color);
  u8g2.setBackgroundColor(bg_color);

  disp->fillScreen(bg_color);

  const uint16_t min_btn_clearance = 14;
  const uint16_t h_padding = 5;

  // charging line
  if (device_state_.sensors().charging) {
    disp->fillRect(12, HEIGHT - 3, WIDTH - 24, 3, text_color);
  }

  LabelType label_type[NUM_BUTTONS] = {};
  for (uint16_t i = 0; i < NUM_BUTTONS; i++) {
    ButtonLabel label = device_state_.get_btn_label(i + 1);
    label_type[i] = get_label_type(label);
  }

  // Loop through buttons
  for (uint16_t i = 0; i < NUM_BUTTONS; i++) {
    ButtonLabel label = device_state_.get_btn_label(i + 1);

    if (label_type[i] == LabelType::Icon) {
      MDIName icon = get_mdi_name(label);

      // make smaller if opposite is text or mixed
      uint16_t size;
      bool small;
      if (i % 2 == 0) {
        if (label_type[i + 1] == LabelType::Text ||
            label_type[i + 1] == LabelType::Mixed) {
          size = MDI_SIZE_SMALL;
          small = true;
        } else {
          size = MDI_SIZE_LARGE;
          small = false;
        }
      } else {
        if (label_type[i - 1] == LabelType::Text ||
            label_type[i - 1] == LabelType::Mixed) {
          size = MDI_SIZE_SMALL;
          small = true;
        } else {
          size = MDI_SIZE_LARGE;
          small = false;
        }
      }

      // calculate icon position on display
      uint16_t x = i % 2 == 0 ? 0 : WIDTH - size;
      // A shrunken icon centres on its own button, a full-size one sits at
      // the shared row top so both columns line up. Upstream spelled this
      // out as three identical if/else pairs on i.
      uint16_t y;
      if (small) {
        y = static_cast<uint16_t>(round(HEIGHT / 12. + i * HEIGHT / 6.)) -
            size / 2;
      } else {
        y = MDI_ROW_TOP_Y[i / 2];
      }
      draw_mdi(icon.c_str(), size, x, y);
    } else if (label_type[i] == LabelType::Mixed) {
      MDIName icon = get_mdi_name(label);
      ButtonLabel text = get_text(label);
      uint16_t icon_size = MDI_SIZE_SMALL;
      uint16_t x = i % 2 == 0 ? 0 : WIDTH - icon_size;
      uint16_t y =
          static_cast<uint16_t>(round(HEIGHT / 12. + i * HEIGHT / 6.)) -
          icon_size / 2;

      draw_mdi(icon.c_str(), icon_size, x, y);
      // draw text
      uint16_t max_text_width = WIDTH - icon_size - h_padding;
      if (text.index_of('_') == 0) {
        // force small font
        text = text.substring(1);
        u8g2.setFont(u8g2_font_helvB18_te);
      } else {
        u8g2.setFont(u8g2_font_helvB24_te);
      }
      uint16_t w, h;
      w = u8g2.getUTF8Width(text.c_str());
      if (w >= max_text_width) {
        u8g2.setFont(u8g2_font_helvB18_te);
        // trim_text() is bounded, so this always terminates
        text = trim_text(text, max_text_width);
        w = u8g2.getUTF8Width(text.c_str());
      }
      h = u8g2.getFontAscent();
      x = i % 2 == 0 ? icon_size + h_padding
                     : WIDTH - icon_size - w - h_padding;
      y = static_cast<uint16_t>(round(HEIGHT / 12. + i * HEIGHT / 6.)) + h / 2;
      u8g2.setCursor(x, y);
      u8g2.print(text.c_str());
    } else {
      uint16_t max_label_width = WIDTH - min_btn_clearance;
      const bool numeric = is_numeric_label(label);
      if (numeric) {
        // A counter total shares its row with the other counter, so budget
        // half the display. helvB24 leaves a two-digit count adrift in a
        // cell nearly 100px tall, so step down a ladder of large numeric
        // fonts instead and take the first that fits.
        max_label_width = WIDTH / 2 - h_padding * 2;
        static const uint8_t* const kNumericFonts[] = {
            u8g2_font_logisoso42_tn, u8g2_font_logisoso32_tn,
            u8g2_font_helvB24_te, u8g2_font_helvB18_te};
        for (const uint8_t* font : kNumericFonts) {
          u8g2.setFont(font);
          if (u8g2.getUTF8Width(label.c_str()) < max_label_width) break;
        }
      } else if (label.index_of('_') == 0) {
        // force small font
        label = label.substring(1);
        u8g2.setFont(u8g2_font_helvB18_te);
      } else {
        u8g2.setFont(u8g2_font_helvB24_te);
      }
      uint16_t w, h;
      w = u8g2.getUTF8Width(label.c_str());
      // Numbers are never trimmed: "12..." is a wrong value, not a
      // shortened word. An implausibly long count just runs small.
      if (!numeric && w >= max_label_width) {
        u8g2.setFont(u8g2_font_helvB18_te);
        // trim_text() is bounded, so this always terminates
        label = trim_text(label, max_label_width);
        w = u8g2.getUTF8Width(label.c_str());
      }
      h = u8g2.getFontAscent();
      int16_t x, y;
      if (numeric) {
        // Placed exactly where a full-size icon would sit: one vertical
        // centre per row shared by both columns, and centred within the
        // column. Text's own placement is per button index, which puts the
        // left column high and the right column low - fine for a caption,
        // wrong for two counters meant to read as a pair.
        const uint16_t cy = MDI_ROW_TOP_Y[i / 2] + MDI_SIZE_LARGE / 2;
        const uint16_t cx =
            (i % 2 == 0) ? MDI_SIZE_LARGE / 2 : WIDTH - MDI_SIZE_LARGE / 2;
        x = static_cast<int16_t>(cx) - w / 2;
        y = static_cast<int16_t>(cy) + h / 2;
      } else if (i % 2 == 0) {
        x = h_padding;
        y = static_cast<uint16_t>(round(HEIGHT / 12. + i * HEIGHT / 6.)) +
            h / 2;
      } else {
        x = WIDTH - w - h_padding;
        y = static_cast<uint16_t>(round(HEIGHT / 12. + i * HEIGHT / 6.)) +
            h / 2;
      }
      u8g2.setCursor(x, y);
      u8g2.print(label.c_str());
    }
  }

  disp->display();
}

void Display::draw_info() {
  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(text_color);
  u8g2.setBackgroundColor(bg_color);

  disp->fillScreen(bg_color);

  UIState::MessageType text;
  uint16_t w;

  // Battery is the only sensor left, so it gets the whole page.
  text = "- Battery -";
  u8g2.setFont(u8g2_font_courR12_tr);
  w = u8g2.getUTF8Width(text.c_str());
  u8g2.setCursor(WIDTH / 2 - w / 2, 60);
  u8g2.print(text.c_str());

  disp->drawXBitmap(WIDTH / 2 - 64 / 2, 80, battery_64x64, 64, 64, text_color);

  if (device_state_.sensors().battery_present) {
    text = UIState::MessageType("%d %%", device_state_.sensors().battery_pct);
  } else {
    text = "-";
  }
  u8g2.setFont(u8g2_font_helvB24_te);
  w = u8g2.getUTF8Width(text.c_str());
  u8g2.setCursor(WIDTH / 2 - w / 2 - 2, 190);
  u8g2.print(text.c_str());

  if (device_state_.sensors().battery_present) {
    text = UIState::MessageType("%.2f V",
                                device_state_.sensors().battery_voltage);
    u8g2.setFont(u8g2_font_courR12_tr);
    w = u8g2.getUTF8Width(text.c_str());
    u8g2.setCursor(WIDTH / 2 - w / 2, 220);
    u8g2.print(text.c_str());
  }

  const char *status = nullptr;
  if (device_state_.sensors().charging) {
    status = "Charging";
  } else if (device_state_.sensors().dc_connected) {
    status = "DC power";
  } else if (device_state_.sensors().battery_low) {
    status = "LOW - recharge";
  }
  if (status != nullptr) {
    u8g2.setFont(u8g2_font_courR12_tr);
    w = u8g2.getUTF8Width(status);
    u8g2.setCursor(WIDTH / 2 - w / 2, 250);
    u8g2.print(status);
  }

  disp->display();
}

void Display::draw_device_info() {
  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(text_color);
  u8g2.setBackgroundColor(bg_color);

  disp->fillScreen(bg_color);

  disp->drawXBitmap(40, 0, hb_logo_48x48, 48, 48, text_color);

  u8g2.setFont(u8g2_font_profont12_tr);

  u8g2.setCursor(0, 70);
  u8g2.print(device_state_.device_name().c_str());

  UIState::MessageType sw_ver = UIState::MessageType("SW: ") + SW_VERSION;
  u8g2.setCursor(0, 82);
  u8g2.print(sw_ver.c_str());

  UIState::MessageType model_info = UIState::MessageType("Model: ") +
                                    device_state_.factory().model_id.c_str() +
                                    " rev " +
                                    device_state_.factory().hw_version.c_str();
  u8g2.setCursor(0, 94);
  u8g2.print(model_info.c_str());

  u8g2.setCursor(0, 106);
  u8g2.print(device_state_.factory().unique_id.c_str());

  // firmware build / filesystem build. They are flashed separately, so
  // showing both makes a stale uploadfs obvious at a glance.
  u8g2.setCursor(0, 118);
  u8g2.print(StaticString<48>("fw   %s", BUILD_ID).c_str());
  u8g2.setCursor(0, 130);
  u8g2.print(StaticString<48>(
                 "fs   %s",
                 spiffs_build_.empty() ? "missing" : spiffs_build_.c_str())
                 .c_str());

  UIState::MessageType ip_info =
      UIState::MessageType("IP: %s", device_state_.ip());
  u8g2.setCursor(0, 140);
  u8g2.print(ip_info.c_str());

  UIState::MessageType batt_volt;
  if (device_state_.sensors().battery_present) {
    batt_volt = UIState::MessageType("Battery: %.2f V",
                                     device_state_.sensors().battery_voltage);
  } else {
    batt_volt = UIState::MessageType("Battery: -");
  }
  u8g2.setCursor(0, 152);
  u8g2.print(batt_volt.c_str());

  disp->display();
}

void Display::draw_welcome() {
  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(text_color);
  u8g2.setBackgroundColor(bg_color);

  disp->fillScreen(bg_color);

  uint16_t w;
  const char *text = "Home Buttons";
  u8g2.setFont(u8g2_font_helvB12_tr);
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 40);
  u8g2.print(text);

  disp->drawXBitmap(52, 52, hb_logo_24x24, 24, 24, GxEPD_BLACK);

  text = "------------------------";
  u8g2.setFont(u8g2_font_helvB12_tr);
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 102);
  u8g2.print(text);

  uint8_t version = 6;  // 41x41px
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(version)];
  qrcode_initText(&qrcode, qrcodeData, version, ECC_HIGH, DOCS_LINK);

  text = "Setup guide:";
  u8g2.setFont(u8g2_font_courR12_tr);
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 145);
  u8g2.print(text);

  uint16_t qr_x = 23;
  uint16_t qr_y = 165;
  for (uint8_t y2 = 0; y2 < qrcode.size; y2++) {
    // Each horizontal module
    for (uint8_t x2 = 0; x2 < qrcode.size; x2++) {
      // Display each module
      if (qrcode_getModule(&qrcode, x2, y2)) {
        disp->drawRect(qr_x + x2 * 2, qr_y + y2 * 2, 2, 2, GxEPD_BLACK);
      }
    }
  }

  u8g2.setFont(u8g2_font_profont12_tr);
  UIState::MessageType sw_ver = UIState::MessageType("SW: ") + SW_VERSION;
  u8g2.setCursor(0, 272);
  u8g2.print(sw_ver.c_str());

  UIState::MessageType model_info = UIState::MessageType("Model: ") +
                                    device_state_.factory().model_id.c_str() +
                                    " rev " +
                                    device_state_.factory().hw_version.c_str();
  u8g2.setCursor(0, 283);
  u8g2.print(model_info.c_str());

  u8g2.setCursor(0, 294);
  u8g2.print(device_state_.factory().unique_id.c_str());

  disp->display();
}

void Display::draw_settings() {
  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(text_color);
  u8g2.setBackgroundColor(bg_color);

  disp->fillScreen(bg_color);

  disp->drawXBitmap(0, 17, account_cog_64x64, 64, 64, text_color);
  disp->drawXBitmap(WIDTH / 2, 17, wifi_cog_64x64, 64, 64, text_color);
  disp->drawXBitmap(0, 116, restore_64x64, 64, 64, text_color);
  disp->drawXBitmap(WIDTH / 2, 116, close_64x64, 64, 64, text_color);

  disp->drawXBitmap(40, 200, hb_logo_48x48, 48, 48, text_color);

  u8g2.setFont(u8g2_font_profont12_tr);

  u8g2.setCursor(0, 261);
  u8g2.print(device_state_.device_name().c_str());

  UIState::MessageType sw_ver = UIState::MessageType("SW: ") + SW_VERSION;
  u8g2.setCursor(0, 272);
  u8g2.print(sw_ver.c_str());

  UIState::MessageType model_info = UIState::MessageType("Model: ") +
                                    device_state_.factory().model_id.c_str() +
                                    " rev " +
                                    device_state_.factory().hw_version.c_str();
  u8g2.setCursor(0, 283);
  u8g2.print(model_info.c_str());

  u8g2.setCursor(0, 294);
  u8g2.print(device_state_.factory().unique_id.c_str());

  disp->display();
}

void Display::draw_ap_config() {
  UIState::MessageType contents = UIState::MessageType("WIFI:T:WPA;S:") +
                                  device_state_.get_ap_ssid().c_str() +
                                  ";P:" + device_state_.get_ap_password() +
                                  ";;";

  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(text_color);
  u8g2.setBackgroundColor(bg_color);

  disp->fillScreen(bg_color);

  uint8_t version = 6;  // 41x41px
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(version)];
  qrcode_initText(&qrcode, qrcodeData, version, ECC_HIGH, contents.c_str());

  u8g2.setFont(u8g2_font_courR12_tr);

  uint16_t w;
  const char *text = "Scan:";
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 20);
  u8g2.print(text);

  uint16_t qr_x = 23;
  uint16_t qr_y = 35;
  for (uint8_t y2 = 0; y2 < qrcode.size; y2++) {
    // Each horizontal module
    for (uint8_t x2 = 0; x2 < qrcode.size; x2++) {
      // Display each module
      if (qrcode_getModule(&qrcode, x2, y2)) {
        disp->drawRect(qr_x + x2 * 2, qr_y + y2 * 2, 2, 2, GxEPD_BLACK);
      }
    }
  }
  text = "--------- or ---------";
  u8g2.setFont(u8g2_font_helvB12_tr);
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 153);
  u8g2.print(text);

  text = "Connect to:";
  u8g2.setFont(u8g2_font_courR12_tr);
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 190);
  u8g2.print(text);

  u8g2.setCursor(0, 220);
  u8g2.print("Wi-Fi:");
  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.setCursor(0, 235);
  u8g2.print(device_state_.get_ap_ssid().c_str());

  u8g2.setFont(u8g2_font_courR12_tr);
  u8g2.setCursor(0, 260);
  u8g2.print("Password:");
  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.setCursor(0, 275);
  u8g2.print(device_state_.get_ap_password());

  disp->display();
}

void Display::draw_web_config() {
  UIState::MessageType contents =
      UIState::MessageType("http://") + device_state_.ip();

  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(text_color);
  u8g2.setBackgroundColor(bg_color);

  disp->fillScreen(bg_color);

  uint8_t version = 6;  // 41x41px
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(version)];
  qrcode_initText(&qrcode, qrcodeData, version, ECC_HIGH, contents.c_str());

  u8g2.setFont(u8g2_font_courR12_tr);

  uint16_t w;
  const char *text = "Scan:";
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 20);
  u8g2.print(text);

  uint16_t qr_x = 23;
  uint16_t qr_y = 35;

  for (uint8_t y2 = 0; y2 < qrcode.size; y2++) {
    // Each horizontal module
    for (uint8_t x2 = 0; x2 < qrcode.size; x2++) {
      // Display each module
      if (qrcode_getModule(&qrcode, x2, y2)) {
        disp->drawRect(qr_x + x2 * 2, qr_y + y2 * 2, 2, 2, GxEPD_BLACK);
      }
    }
  }
  text = "--------- or ---------";
  u8g2.setFont(u8g2_font_helvB12_tr);
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 153);
  u8g2.print(text);

  text = "Go to:";
  u8g2.setFont(u8g2_font_courR12_tr);
  w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 200);
  u8g2.print(text);

  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.setCursor(0, 240);
  u8g2.print("http://");
  u8g2.setCursor(0, 260);
  u8g2.print(device_state_.ip());

  disp->display();
}

void Display::draw_test(const char *text, const char *mdi_name,
                        uint16_t mdi_size) {
  uint16_t fg, bg;
  fg = GxEPD_BLACK;
  bg = GxEPD_WHITE;

  disp->setRotation(ROTATION);
  disp->setFullWindow();

  u8g2.setFontMode(1);
  u8g2.setForegroundColor(fg);
  u8g2.setBackgroundColor(bg);

  disp->fillScreen(bg);

  draw_mdi(mdi_name, mdi_size, WIDTH / 2 - mdi_size / 2, 50);

  u8g2.setFont(u8g2_font_helvB24_te);
  uint16_t w = u8g2.getUTF8Width(text);
  u8g2.setCursor(WIDTH / 2 - w / 2, 250);
  u8g2.print(text);

  disp->display();
}

void Display::draw_white() {
  disp->setFullWindow();
  disp->fillScreen(GxEPD_WHITE);
  disp->display();
}

void Display::draw_black() {
  disp->setFullWindow();
  disp->fillScreen(GxEPD_BLACK);
  disp->display();
}

// based on GxEPD2_Spiffs_Example.ino - drawBitmapFromSpiffs_Buffered()
// Warning - SPIFFS.begin() must be called before this function
bool Display::draw_bmp(File &file, int16_t x, int16_t y) {
  uint32_t startTime = millis();
  if (!file) {
    error("error opening file");
    return false;
  }
  bool valid = false;  // valid format to be handled
  bool flip = true;    // bitmap is stored bottom-to-top
  if ((x >= disp->width()) || (y >= disp->height())) return false;

  // Parse BMP header
  if (read16(file) == 0x4D42) {
    debug("BMP signature detected");
    uint32_t fileSize = read32(file);
    uint32_t creatorBytes = read32(file);
    (void)creatorBytes;                   // unused
    uint32_t imageOffset = read32(file);  // Start of image data
    uint32_t headerSize = read32(file);
    uint32_t width = read32(file);
    int32_t height = (int32_t)read32(file);
    uint16_t planes = read16(file);
    uint16_t depth = read16(file);  // bits per pixel
    uint32_t format = read32(file);
    if ((planes == 1) && ((format == 0) || (format == 3))) {
      debug("BMP Image Offset: %d", imageOffset);
      debug("BMP Header size: %d", headerSize);
      debug("BMP File size: %d", fileSize);
      debug("BMP Bit Depth: %d", depth);
      debug("BMP Image size: %d x %d", width, height);
      // BMP rows are padded (if needed) to 4-byte boundary
      uint32_t rowSize = (width * depth / 8 + 3) & ~3;
      if (depth < 8) rowSize = ((width * depth + 8 - depth) / 8 + 3) & ~3;
      if (height < 0) {
        height = -height;
        flip = false;
      }
      uint16_t w = width;
      uint16_t h = height;
      if ((x + w - 1) >= disp->width()) w = disp->width() - x;
      if ((y + h - 1) >= disp->height()) h = disp->height() - y;
      valid = true;
      uint8_t bitmask = 0xFF;
      uint8_t bitshift = 8 - depth;
      uint16_t red, green, blue;
      bool whitish = false;
      bool colored = false;
      if (depth <= 8) {
        if (depth < 8) bitmask >>= depth;
        file.seek(imageOffset - (4 << depth));
        for (uint16_t pn = 0; pn < (1 << depth); pn++) {
          blue = file.read();
          green = file.read();
          red = file.read();
          file.read();
          whitish = (red + green + blue) > 3 * 0x80;
          // reddish or yellowish?
          colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0));
          if (0 == pn % 8) mono_palette_buffer[pn / 8] = 0;
          mono_palette_buffer[pn / 8] |= whitish << pn % 8;
          if (0 == pn % 8) color_palette_buffer[pn / 8] = 0;
          color_palette_buffer[pn / 8] |= colored << pn % 8;
          rgb_palette_buffer[pn] = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) |
                                   ((blue & 0xF8) >> 3);
        }
      }
      uint32_t rowPosition =
          flip ? imageOffset + (height - h) * rowSize : imageOffset;
      for (uint16_t row = 0; row < h;
           row++, rowPosition += rowSize)  // for each line
      {
        uint32_t in_remain = rowSize;
        uint32_t in_idx = 0;
        uint32_t in_bytes = 0;
        uint8_t in_byte = 0;  // for depth <= 8
        uint8_t in_bits = 0;  // for depth <= 8
        uint16_t color = GxEPD_WHITE;
        file.seek(rowPosition);
        for (uint16_t col = 0; col < w; col++)  // for each pixel
        {
          // Time to read more pixel data?
          if (in_idx >= in_bytes)  // ok, exact match for 24bit also (size
                                   // IS multiple of 3)
          {
            in_bytes = file.read(input_buffer, in_remain > sizeof(input_buffer)
                                                   ? sizeof(input_buffer)
                                                   : in_remain);
            in_remain -= in_bytes;
            in_idx = 0;
          }
          switch (depth) {
            case 24:
              blue = input_buffer[in_idx++];
              green = input_buffer[in_idx++];
              red = input_buffer[in_idx++];
              whitish = (red + green + blue) > 3 * 0x80;
              // reddish or yellowish?
              colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0));
              color = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) |
                      ((blue & 0xF8) >> 3);
              break;
            case 16: {
              uint8_t lsb = input_buffer[in_idx++];
              uint8_t msb = input_buffer[in_idx++];
              if (format == 0)  // 555
              {
                blue = (lsb & 0x1F) << 3;
                green = ((msb & 0x03) << 6) | ((lsb & 0xE0) >> 2);
                red = (msb & 0x7C) << 1;
                color = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) |
                        ((blue & 0xF8) >> 3);
              } else  // 565
              {
                blue = (lsb & 0x1F) << 3;
                green = ((msb & 0x07) << 5) | ((lsb & 0xE0) >> 3);
                red = (msb & 0xF8);
                color = (msb << 8) | lsb;
              }
              whitish = (red + green + blue) > 3 * 0x80;
              // reddish or yellowish?
              colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0));
            } break;
            case 1:
            case 4:
            case 8: {
              if (0 == in_bits) {
                in_byte = input_buffer[in_idx++];
                in_bits = 8;
              }
              uint16_t pn = (in_byte >> bitshift) & bitmask;
              whitish = mono_palette_buffer[pn / 8] & (0x1 << pn % 8);
              colored = color_palette_buffer[pn / 8] & (0x1 << pn % 8);
              in_byte <<= depth;
              in_bits -= depth;
              color = rgb_palette_buffer[pn];
            } break;
          }
          if (whitish) {
            color = GxEPD_WHITE;
          } else if (colored) {
            color = GxEPD_COLORED;
          } else {
            color = GxEPD_BLACK;
          }
          uint16_t yrow = y + (flip ? h - row - 1 : row);
          disp->drawPixel(x + col, yrow, color);
        }  // end pixel
      }  // end line
    }
  }
  file.close();
  if (!valid) {
    error("BMP format not valid.");
  }
  debug("BMP loaded in %lu ms", millis() - startTime);
  return valid;
}

// Icons are pre-loaded into SPIFFS at flash time, so this is a plain read at
// the path layout the old downloader used. SPIFFS is mounted once in begin()
// and stays mounted - do not unmount here, the icons are read on every draw.
File Display::open_mdi_file(const char *name, uint16_t size) {
  if (name == nullptr || name[0] == '\0') return File{};
  if (!spiffs_mounted_) {
    error("SPIFFS not mounted, cannot open icon: %s", name);
    return File{};
  }
  MDIPath path("/mdi/%u/%s.bmp", static_cast<unsigned>(size), name);
  return SPIFFS.open(path.c_str(), FILE_READ);
}

void Display::draw_mdi(const char *name, uint16_t size, int16_t x, int16_t y) {
  bool draw_placeholder = false;
  File file = open_mdi_file(name, size);
  if (file) {
    // draw_bmp() closes the file
    if (!draw_bmp(file, x, y)) {
      error("Could not draw icon: %s", name);
      draw_placeholder = true;
    }
  } else {
    error("Icon not found: %s", name);
    draw_placeholder = true;
  }
  if (draw_placeholder) {
    if (size == MDI_SIZE_LARGE) {
      disp->drawXBitmap(x, y, file_question_outline_64x64, 64, 64, text_color);
    } else if (size == MDI_SIZE_SMALL) {
      disp->drawXBitmap(x, y, file_question_outline_48x48, 48, 48, text_color);
    }
  }
}
