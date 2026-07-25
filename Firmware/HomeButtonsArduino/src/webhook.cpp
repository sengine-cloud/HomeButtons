#include "webhook.h"

#include <ArduinoJson.h>

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
                            bool heartbeat) {
  StaticJsonDocument<HTTP_PAYLOAD_SIZE> doc;
  doc["device"] = device_state_.factory().unique_id.c_str();
  doc["seq"] = event.seq;
  doc["event"] = heartbeat ? "heartbeat" : "press";
  if (!heartbeat) {
    doc["counter"] = COUNTER_NAMES[event.counter_idx];
    doc["button"] = event.button_id;
    doc["delta"] = event.delta;
    doc["count"] = event.count;
  }
  // The device has no RTC, so the receiver stamps wall-clock time. age_ms
  // lets it back-date a press that is only now being delivered.
  doc["age_ms"] = event.age_ms;
  doc["battery_pct"] = device_state_.sensors().battery_pct;
  doc["battery_v"] = device_state_.sensors().battery_voltage;
  doc["sw_version"] = SW_VERSION;
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
    http_.end();  // with setReuse(true) this keeps the socket open

    if (code >= 200 && code < 300) {
      info("posted ok (%d) on attempt %u", code, attempt);
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
  size_t len = _build_body(body, sizeof(body), event, false);
  debug("press body: %s", body);
  return _post(body, len);
}

bool Webhook::send_heartbeat() {
  Event event{};
  event.seq = device_state_.next_seq();
  char body[HTTP_PAYLOAD_SIZE];
  size_t len = _build_body(body, sizeof(body), event, true);
  debug("heartbeat body: %s", body);
  return _post(body, len);
}
