#include "webhook.h"

#include <ArduinoJson.h>
#include <string.h>
#include <sys/time.h>

#include "config.h"

// Provided by the ESP-IDF certificate bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
// in sdkconfig.defaults). A bundle rather than a pinned root: the backend sits
// behind Cloudflare, which rotates edge CAs without notice, and upstream's
// single pinned root had already expired.
extern const uint8_t rootca_crt_bundle_start[] asm(
    "_binary_x509_crt_bundle_start");

void Webhook::begin() {
  if (begun_) return;
  client_.setCACertBundle(rootca_crt_bundle_start);
  client_.setTimeout(HTTP_TIMEOUT / 1000);  // seconds
  http_.setReuse(true);
  http_.setTimeout(HTTP_TIMEOUT);
  http_.setConnectTimeout(HTTP_TIMEOUT);
  begun_ = true;
  debug("webhook client ready");
}

bool Webhook::configured() const {
  return !device_state_.endpoint_url().empty();
}

size_t Webhook::_build_body(char* out, size_t out_size, const Event& event,
                            const char* event_kind, const char* reset_mode) {
  StaticJsonDocument<HTTP_PAYLOAD_SIZE> doc;
  doc["device"] = device_state_.factory().unique_id.c_str();
  doc["seq"] = event.seq;
  doc["event"] = event_kind;
  if (strcmp(event_kind, "press") == 0) {
    doc["counter"] = COUNTER_NAMES[event.counter_idx];
    doc["button"] = event.button_id;
    doc["delta"] = event.delta;
    doc["count"] = event.count;
  } else if (strcmp(event_kind, "reset") == 0) {
    doc["reset_mode"] = reset_mode;
    // One event covers the whole reset; the receiver iterates the object
    // rather than needing one request per counter.
    JsonObject counts = doc.createNestedObject("counts");
    for (uint8_t i = 0; i < NUM_COUNTERS; i++) {
      counts[COUNTER_NAMES[i]] = device_state_.counter(i);
    }
  }
  // The device has no RTC, so the receiver stamps wall-clock time. age_ms
  // lets it back-date a press that is only now being delivered.
  doc["age_ms"] = event.age_ms;
  doc["battery_pct"] = device_state_.sensors().battery_pct;
  doc["battery_v"] = device_state_.sensors().battery_voltage;
  doc["sw_version"] = SW_VERSION;
  doc["build"] = BUILD_ID;
  return serializeJson(doc, out, out_size);
}

bool Webhook::_post(char* body, size_t len) {
  if (!configured()) {
    warning("no endpoint configured, dropping event");
    return false;
  }
  begin();

  for (uint8_t attempt = 1; attempt <= HTTP_MAX_ATTEMPTS; attempt++) {
    if (!http_.begin(client_, device_state_.endpoint_url().c_str())) {
      error("http begin failed (attempt %u)", attempt);
      continue;
    }
    http_.addHeader("Content-Type", "application/json");
    const auto& token = device_state_.auth_token();
    if (!token.empty()) {
      http_.addHeader("Authorization", String("Bearer ") + token.c_str());
    }

    int code = http_.POST(reinterpret_cast<uint8_t*>(body), len);
    // Read before end(): every response carries the clock, so any request
    // is also a time sync.
    String response = (code > 0) ? http_.getString() : String();
    http_.end();  // with setReuse(true) this keeps the socket open

    if (code >= 200 && code < 300) {
      info("posted ok (%d) on attempt %u", code, attempt);
      _apply_time(response);
      return true;
    }
    // 4xx other than 408/429 will not improve on retry.
    if (code >= 400 && code < 500 && code != 408 && code != 429) {
      error("post rejected (%d), not retrying", code);
      return false;
    }
    warning("post failed (%d) on attempt %u", code, attempt);
    delay(200 * attempt);
  }
  error("post failed after %u attempts", HTTP_MAX_ATTEMPTS);
  return false;
}

bool Webhook::send_press(const Event& event) {
  char body[HTTP_PAYLOAD_SIZE];
  size_t len = _build_body(body, sizeof(body), event, "press", nullptr);
  debug("press body: %s", body);
  return _post(body, len);
}

bool Webhook::send_heartbeat() {
  Event event{};
  event.seq = device_state_.next_seq();
  char body[HTTP_PAYLOAD_SIZE];
  size_t len = _build_body(body, sizeof(body), event, "heartbeat", nullptr);
  debug("heartbeat body: %s", body);
  return _post(body, len);
}

bool Webhook::send_reset(const char* mode) {
  Event event{};
  event.seq = device_state_.next_seq();
  char body[HTTP_PAYLOAD_SIZE];
  size_t len = _build_body(body, sizeof(body), event, "reset", mode);
  info("reset body: %s", body);
  return _post(body, len);
}

bool Webhook::sync_time() {
  Event event{};
  // Still takes a sequence number: 'unique per request' is the invariant
  // the field documents, and reusing one would break a receiver that ever
  // dedupes outside the press branch.
  event.seq = device_state_.next_seq();
  char body[HTTP_PAYLOAD_SIZE];
  size_t len = _build_body(body, sizeof(body), event, "time", nullptr);
  debug("time sync body: %s", body);
  return _post(body, len);
}

void Webhook::_apply_time(const String& response) {
  if (response.length() == 0) return;
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    // A plain-text body is normal if the receiver was never configured to
    // return one; the clock simply stays as it was.
    debug("response not JSON (%s), no clock update", err.c_str());
    return;
  }
  if (!doc.containsKey("ts")) return;

  const uint32_t ts = doc["ts"].as<uint32_t>();
  const int32_t offset = doc["tz_offset"] | device_state_.tz_offset();
  if (ts < 1700000000UL) {  // sanity: anything before late 2023 is not a clock
    warning("ignoring implausible ts %u", ts);
    return;
  }

  struct timeval tv = {};
  tv.tv_sec = static_cast<time_t>(ts);
  settimeofday(&tv, nullptr);
  device_state_.set_clock_synced(ts, offset);
  info("clock synced: ts=%u offset=%d", ts, offset);
}
