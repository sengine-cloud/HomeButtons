#ifndef HOMEBUTTONS_WEBHOOK_H
#define HOMEBUTTONS_WEBHOOK_H

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "logger.h"
#include "state.h"
#include "types.h"

// Posts counter events to an HTTPS webhook (an n8n Webhook node in the
// intended deployment).
//
// The device is authoritative for the counter, so every request carries the
// absolute value as well as the delta. A dropped request therefore repairs
// itself on the next press rather than needing a durable retry queue. `seq`
// is monotonic per device and exists only so the receiver can dedupe a
// replayed request and avoid notifying twice.
class Webhook : public Logger {
 public:
  struct Event {
    uint8_t counter_idx = 0;  // 0-based
    uint8_t button_id = 0;    // 1-based, 0 for a heartbeat
    int32_t delta = 0;
    int32_t count = 0;      // absolute value after the delta was applied
    uint32_t seq = 0;
    uint32_t age_ms = 0;    // >0 when this is a delayed retry
  };

  explicit Webhook(DeviceState& device_state)
      : Logger("HOOK"), device_state_(device_state) {}
  Webhook(const Webhook&) = delete;

  // Must be called once after Wi-Fi is up, before the first send().
  void begin();

  // True when an endpoint URL has been configured in the setup portal.
  bool configured() const;

  // Posts a press. Retries up to HTTP_MAX_ATTEMPTS within the caller's
  // awake window. Returns true on a 2xx.
  bool send_press(const Event& event);

  // Posts battery level only, used by the timer wake.
  bool send_heartbeat();

  // Posts the cleared counters after a scheduled reset.
  bool send_reset(const char* mode);

  // Asks for nothing and reports nothing - exists purely so the response
  // can carry the clock. The receiver's press branch ignores it. Used at
  // first connect, and whenever the clock is too old to trust near a reset
  // boundary.
  bool sync_time();

 private:
  bool _post(char* body, size_t len);
  size_t _build_body(char* out, size_t out_size, const Event& event,
                     const char* event_kind, const char* reset_mode);
  // Reads `ts` and `tz_offset` out of a response and sets the system clock.
  // Every response carries them, so any request doubles as a clock sync.
  void _apply_time(const String& response);

  DeviceState& device_state_;
  WiFiClientSecure client_;
  HTTPClient http_;
  bool begun_ = false;
};

#endif  // HOMEBUTTONS_WEBHOOK_H
