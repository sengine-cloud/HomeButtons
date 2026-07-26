#ifndef HOMEBUTTONS_DISPLAY_H
#define HOMEBUTTONS_DISPLAY_H

#include <FS.h>
#include <GxEPD2.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "static_string.h"
#include "state.h"
#include "logger.h"
#include "types.h"

// parameters for draw_bmp()
static constexpr uint16_t input_buffer_pixels = 800;
static constexpr uint16_t max_palette_pixels = 256;

// Icons are pre-loaded into SPIFFS at flash time under /mdi/<size>/<name>.bmp.
// Only the two sizes the Original layout uses are shipped.
static constexpr uint16_t MDI_SIZE_LARGE = 64;
static constexpr uint16_t MDI_SIZE_SMALL = 48;

// Top edge of a full-size icon, per button row. Both columns of a row share
// one value, so icons line up across the display. Text labels deliberately
// do not use these - they are placed per button index, which staggers the
// two columns - but a counter total is drawn as though it were an icon so
// the numbers sit level with each other and with icons in the other rows.
static constexpr uint16_t MDI_ROW_TOP_Y[3] = {17, 116, 215};

struct HardwareDefinition;

class DeviceState;

// display init stuff
#define GxEPD2_DISPLAY_CLASS GxEPD2_BW
#define GxEPD2_DRIVER_CLASS GxEPD2_290_T94_V2  // 128x296

#define MAX_DISPLAY_BUFFER_SIZE 65536ul  // e.g.
#define MAX_HEIGHT(EPD)                                      \
  (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) \
       ? EPD::HEIGHT                                         \
       : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))

class Display : public Logger {
 public:
  enum class State { IDLE, ACTIVE, CMD_END, ENDING };
  explicit Display(DeviceState& device_state)
      : Logger("Display"), device_state_(device_state) {}
  void begin(HardwareDefinition& HW);
  void end();
  void update();

  void disp_message(const char* message, uint32_t duration = 0);
  void disp_message_large(const char* message, uint32_t duration = 0);
  void disp_error(const char* message, uint32_t duration = 0);
  void disp_main();
  void disp_info();
  void disp_device_info();
  void disp_welcome();
  void disp_settings();
  void disp_ap_config();
  void disp_web_config();
  void disp_test(const char* text, const char* mdi_name, uint16_t mdi_size);

  UIState get_ui_state();
  void init_ui_state(UIState ui_state);  // used after wakeup
  State get_state();
  bool busy() { return redraw_in_progress; }

 private:
  State state = State::IDLE;

  UIState current_ui_state = {};
  UIState cmd_ui_state = {};
  UIState draw_ui_state = {};
  UIState pre_disappear_ui_state = {};

  bool new_ui_cmd = false;
  bool redraw_in_progress = false;
  bool spiffs_mounted_ = false;

  uint16_t text_color = GxEPD_BLACK;
  uint16_t bg_color = GxEPD_WHITE;

  DeviceState& device_state_;

  GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)>*
      disp;
  U8G2_FOR_ADAFRUIT_GFX u8g2;

  // ### buffers for draw_bmp()
  // up to depth 24
  uint8_t input_buffer[3 * input_buffer_pixels];
  // palette buffer for depth <= 8 b/w
  uint8_t mono_palette_buffer[max_palette_pixels / 8];
  // palette buffer for depth <= 8 c/w
  uint8_t color_palette_buffer[max_palette_pixels / 8];
  // palette buffer for depth <= 8 for buffered graphics, needed for 7-color
  // display
  uint16_t rgb_palette_buffer[max_palette_pixels];

  LabelType get_label_type(ButtonLabel label);
  MDIName get_mdi_name(ButtonLabel label);
  ButtonLabel get_text(ButtonLabel label);

  ButtonLabel trim_text(ButtonLabel label, uint16_t max_width);

  void set_cmd_state(UIState cmd);

  void draw_message(const UIState::MessageType& message, bool error = false,
                    bool large = false);
  void draw_main();
  void draw_info();
  void draw_device_info();
  void draw_welcome();
  void draw_settings();
  void draw_ap_config();
  void draw_web_config();
  void draw_test(const char* text, const char* mdi_name, uint16_t mdi_size);
  void draw_white();
  void draw_black();
  bool draw_bmp(File& file, int16_t x, int16_t y);
  // Opens the pre-loaded icon at /mdi/<size>/<name>.bmp. The returned File is
  // falsy when the icon was not flashed. SPIFFS must already be mounted.
  File open_mdi_file(const char* name, uint16_t size);
  void draw_mdi(const char* name, uint16_t size, int16_t x, int16_t y);
};

#endif  // HOMEBUTTONS_DISPLAY_H
