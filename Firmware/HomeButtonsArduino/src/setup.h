#ifndef HOMEBUTTONS_SETUP_H
#define HOMEBUTTONS_SETUP_H

#include "logger.h"

class App;

class HBSetup : public Logger {
 public:
  HBSetup(App& app) : Logger("Setup"), app_(app) {}

  void start_wifi_setup();
  void start_setup();

 private:
  App& app_;
  bool web_portal_saved_ = false;

  void save_params_callback();
  // The Wi-Fi-only portal's own callback. Fires the moment its params form
  // is submitted, rather than waiting for the portal loop to end - that
  // loop only exits on a connection attempt, a button, or the 600s
  // timeout, so a region entered and saved on its own was never read.
  void save_wifi_params_callback();
  bool wifi_region_changed_ = false;
};

#endif  // HOMEBUTTONS_SETUP_H
