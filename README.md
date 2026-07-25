# Home Buttons — Counter

A fork of [**nplan/HomeButtons**](https://github.com/nplan/HomeButtons) that turns
a *Home Buttons Original* (model A1) into a **two-channel tally counter** which
reports each press to an HTTPS webhook.

Press `+`, the number on the e-paper display goes up and a notification fires.
Press `−` to correct a miscount. Two independent counters, four buttons, two
spare.

This is a derivative work, not a drop-in replacement for upstream firmware —
MQTT and Home Assistant integration have been removed entirely.

---

## How it works

```
press +  ──▶  LED blinks immediately (tactile confirmation)
         ──▶  counter increments locally, display redraws
         ──▶  HTTPS POST to the webhook
         ──▶  stay awake ~30 s for more presses, then deep sleep
```

The **device is authoritative** for the counter. Every request carries the
absolute value as well as the delta, so a dropped request repairs itself on the
next press — there is no retry queue to get out of sync. The display never
waits on the network.

After the first press the Wi-Fi association and TLS session stay open for
`SESSION_IDLE_TIMEOUT`, so a burst of presses costs one handshake instead of
one per press. Presses 2..N land in roughly 200 ms rather than several seconds.

### Request format

```http
POST <your webhook URL>
Authorization: Bearer <token>
Content-Type: application/json
```
```jsonc
{
  "device":      "HBTNS-24011234-ABC123",  // eFuse unique id
  "seq":         837,                      // monotonic, for dedupe
  "event":       "press",                  // or "heartbeat"
  "counter":     "a",                      // "a" or "b"
  "button":      1,
  "delta":       1,                        // +1 or -1
  "count":       42,                       // absolute, authoritative
  "age_ms":      0,                        // >0 if this is a delayed retry
  "battery_pct": 87,
  "battery_v":   3.92,
  "sw_version":  "v3.0.0-counter.1"
}
```

Any `2xx` is success. `4xx` other than 408/429 is treated as permanent and not
retried.

`age_ms` exists because the device has no RTC — let the receiver stamp
wall-clock time and back-date by `age_ms` so a delayed press does not land with
the wrong timestamp.

### Notes for an n8n receiver

- Set the Webhook node to respond **immediately**, or put a *Respond to
  Webhook* node **before** the Telegram step. Otherwise the device holds the
  connection open until Telegram round-trips and burns awake time for nothing.
- Because `count` is absolute, storing it is a plain upsert — no
  read-modify-write, no `$getWorkflowStaticData` dance, no concurrency handling.
- Keep the last-seen `seq` per device and short-circuit on a repeat. That is the
  only state you need, and it is what stops a retry notifying twice.
- Use the **production** URL (`/webhook/…`). The test URL (`/webhook-test/…`)
  only responds while the editor is actively listening.

## Setup

Hold two buttons for 5 s → settings → setup. The device starts a Wi-Fi access
point; its password is derived per-device from the random ID printed on the
case (upstream shipped the same hardcoded password on every unit).

Portal fields: device name, **webhook URL**, **auth token**, static IP settings,
and the six button labels.

## Building

```bash
cd Firmware/HomeButtonsArduino
pio run -e original_release                # firmware
python tools/make_icons.py                 # generate icon BMPs
pio run -e original_release -t buildfs     # SPIFFS image
pio run -e original_release -t upload -t uploadfs
```

Only `original_release` and `original_debug` exist. The mini, pro and
industrial targets are gone.

---

## What changed from upstream

**Removed**

- MQTT client, Home Assistant discovery, and all topic handling
- Temperature/humidity sensor (battery reporting is kept)
- The runtime MDI icon downloader (`src/mdi/`) — icons are now flashed into
  SPIFFS as a data image
- The factory self-test and its serial provisioning hooks
- The mini, pro and industrial variants. The `pro_*` targets could not compile
  in upstream and had not been able to for some time.

**Added**

- HTTPS webhook transport (`src/webhook.{h,cpp}`)
- Persistent counters and a monotonic sequence number in NVS
- `SessionState` — the post-press awake window
- A CI job that actually builds the firmware, with a flash-headroom check

**Bugs fixed along the way** (all present upstream at `v2.6.1`)

| Fix | Was |
|---|---|
| `__builtin_ctzll` for the wake-pin decode | `log(mask)/log(2)` on floats — wrong pin with two buttons held, undefined for a zero mask |
| Elapsed-time network timeout | Compared absolute `millis()` against the timeout |
| `sizeof()` in `_nvs_2_efuse()` | Passed eFuse *bit* counts as buffer lengths into `char[9]` — a struct overflow |
| Bounded text-shrink loops | Could spin forever when the label was narrower than one glyph |
| Zero-initialised `_efuse_burned()` buffer | Declared `uint8_t buf[8]` and read 8 *bits* |
| Task names passed as arguments | Formatted buffer passed as the format string |
| Default member initialisers on `HardwareDefinition` | Any field a revision loader missed was indeterminate |
| Zero-initialised IP string buffer | `getString` does not guarantee NUL termination at the limit |
| Read-only NVS open in `load_persisted()` | Opened read-write on every boot |
| Per-device setup AP password | `"password123"` on every unit shipped |
| Certificate **bundle**, not a pinned root | The pinned "ISRG Root X1" was the DST cross-sign, expired 2024-09-30, while the comment claimed 2030 |
| Every request validates TLS | A custom icon server got `nullptr` as the CA — HTTPS with no verification at all |

The certificate change matters most in this deployment: the backend sits behind
Cloudflare, which rotates edge issuers, so pinning any single root would break
without warning.

**Flash budget.** Upstream's baseline build used **95.8%** of its 0x140000 app
partition. After the strip, and with SPIFFS shrunk to make room, this build uses
**69.3%** of 0x1A0000. CI fails if it crosses 90%.

---

## License

The firmware is **GPLv3**, inherited from upstream — see `Firmware/LICENSE.txt`.
Hardware designs remain **CERN-OHL-S-2.0** (`Hardware/LICENSE.txt`).

Original work © *PLab* / [nplan](https://github.com/nplan). Modifications
described above. This fork is not affiliated with or endorsed by PLab.
