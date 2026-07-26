# Counter — configuration guide

Everything needed to take a Home Buttons Original from a stock unit to a
working two-channel counter posting into n8n.

> The other pages under `docs/` are inherited from upstream and describe the
> **MQTT** firmware. They do not apply to this fork — there is no MQTT, no
> Home Assistant discovery and no temperature sensor here.

---

## 1. Flash

The partition layout differs from upstream, so this cannot be applied over
OTA — it has to go on over USB.

Grab the `firmware-original` artifact from a
[CI run](https://github.com/sengine-cloud/HomeButtons/actions), unzip, and:

```bash
# Recommended: start clean. eFuse identity (serial, model, HW rev) is burnt
# and survives this — only settings and Wi-Fi credentials are cleared.
esptool.py --chip esp32s2 --port /dev/ttyACM0 erase_flash

esptool.py --chip esp32s2 --port /dev/ttyACM0 --baud 921600 \
  --before default_reset --after hard_reset \
  write_flash -z --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x1000   bootloader.bin \
  0x8000   partitions.bin \
  0xe000   ota_data_initial.bin \
  0x10000  firmware.bin \
  0x350000 spiffs.bin
```

Or from a checkout: `pio run -e original_release -t upload -t uploadfs`.

**Erase first if the device previously ran stock firmware.** NVS survives a
plain flash, which means `setup_done` stays `true` while the new webhook
fields are empty — the device boots, counts locally, and silently logs
`no endpoint configured, dropping event`.

## 2. Enter setup

| Action | Result |
|---|---|
| Hold **any two buttons, 5 s** | Settings menu |
| Then press **button 1** | Setup portal (Wi-Fi + all settings) |
| Then press **button 2** | Wi-Fi only |
| Then press **button 3** | Restart |
| Then press **button 4** | Cancel |
| In settings, hold **button 1, 2 s** | Device Info screen |
| In settings, hold **button 3, 10 s** | Factory reset |

The settings menu times out after 30 s.

## 3. Join the setup access point

| | |
|---|---|
| **SSID** | `HB-<random-id>` — e.g. `HB-123XYZ` |
| **Password** | `HB-<serial-number>` — e.g. `HB-24011234` |

Both come from the burnt eFuse and are unique per device. The serial number
is *not* part of the SSID, which is why the password uses it — a password
built from the random ID would be readable over the air from the broadcast
SSID alone.

If you don't know the serial, read it off the **Device Info** screen: it is
the middle section of the unique ID, `HBTNS-<serial>-<random-id>`.

The portal opens at `http://192.168.4.1` and closes after 10 minutes.

## 4. Portal fields

| Field | Notes |
|---|---|
| **Device Name** | Cosmetic; shown on the settings screen |
| **Webhook URL** | Full HTTPS URL of the n8n **production** webhook. Max 128 chars |
| **Auth Token** | Sent as `Authorization: Bearer <token>`. Max 128 chars. Masked in the page |
| **Wi-Fi Country** | ISO code, e.g. `PL`, `DE`, `GB`, `US`. Blank uses the ESP-IDF default. **Set this if your router uses channel 12 or 13** — see below |
| **Awake Mode** | `1` keeps the device from deep sleeping — needed to hold a USB serial console open. `0` for normal use. Drains the battery fast |
| Static IP / Gateway / Subnet / DNS / DNS 2 | Optional — leave blank for DHCP. All three of IP, gateway and subnet must be set for static to apply |
| Button 1-6 Label | See below |

**HTTPS is required.** The device attaches the ESP-IDF root CA bundle and
verifies the chain; a plain `http://` URL or an untrusted certificate will
fail the POST.

### Button labels

The buttons are two columns of three, one counter per column:

```
      counter A   counter B
     ┌─────────┬─────────┐
row1 │ btn 1   │ btn 2   │  title  - your label or icon, no action
     ├─────────┼─────────┤
row2 │ btn 3   │ btn 4   │  count  - shows the total, press to add one
     ├─────────┼─────────┤
row3 │ btn 5   │ btn 6   │  minus  - press to correct
     └─────────┴─────────┘
```

| Button | Role | Label behaviour |
|---|---|---|
| 1, 2 | Title | Yours to set. Defaults `A` / `B`. Pressing does nothing (two blinks) |
| 3, 4 | Count, **+1** | **Overwritten** each press with the running total |
| 5, 6 | **−1** | Yours to set. Default `mdi:minus` |

Labels 3 and 4 carry the running totals, so anything you type there is
replaced on the next press. Everything else is left exactly as you set it.

### Icons

Labels support `mdi:<name>` for an icon, or `mdi:<name> Text` for both.

**Icon list: <https://pictogrammers.com/library/mdi/>** — use the name
exactly as shown there, lowercase and hyphenated (`bread-slice`, `coffee`,
`cash-register`).

The device downloads nothing at runtime, so an icon has to be baked into
the SPIFFS image first. `plus` and `minus` are drawn locally and always
present. Anything else goes in **`Firmware/HomeButtonsArduino/icons.txt`**,
one name per line:

```
food-drumstick
dog-service
```

Then rebuild and flash the image:

```bash
cd Firmware/HomeButtonsArduino
python tools/make_icons.py
pio run -e original_release -t buildfs -t uploadfs
```

CI reads the same file, so anything listed there is in the published
artifacts too. A name can also be passed on the command line for a one-off
(`python tools/make_icons.py coffee`).

SVGs come straight from the canonical
[Templarian/MaterialDesign](https://github.com/Templarian/MaterialDesign)
repository, pinned to a commit in `make_icons.py`, and are rasterised
locally with ImageMagick — no dependency on anyone's icon CDN staying up.
A name that doesn't exist fails the build rather than turning into a
placeholder glyph you'd only notice on the device.

Then set the label — e.g. button 1 to `mdi:food-drumstick`, or
`mdi:food-drumstick Wings` for icon plus text.

A label naming an icon that isn't in the image renders the
`file_question_outline` placeholder.

Counters clamp to `0 … 999999`. Decrementing at zero is a no-op, not an
error.

## 5. n8n receiver

Add a **Webhook** node:

| Setting | Value |
|---|---|
| HTTP Method | `POST` |
| Path | anything; use the **production** URL in the device |
| Authentication | **Header Auth** — name `Authorization`, value `Bearer <your token>` |
| Respond | **Immediately**, or via a *Respond to Webhook* node placed **before** the Telegram step |

Respond mode matters: on *When Last Node Finishes* the device holds the
connection open until Telegram round-trips, burning awake time on battery
for a result it does not use.

Request body:

```jsonc
{
  "device":      "HBTNS-24011234-123XYZ",
  "seq":         837,          // monotonic per device
  "event":       "press",      // or "heartbeat"
  "counter":     "a",          // "a" or "b"
  "button":      1,
  "delta":       1,            // +1 or -1
  "count":       42,           // absolute — this is the authoritative value
  "age_ms":      0,            // >0 if this is a delayed retry
  "battery_pct": 87,
  "battery_v":   3.92,
  "sw_version":  "v3.0.0-counter.1"
}
```

Two things that make the workflow simple:

- **Store `count` directly.** It is absolute, so it is a plain upsert — no
  read-modify-write, no `$getWorkflowStaticData`, no concurrency handling.
  A dropped request repairs itself on the next press.
- **Dedupe on `seq`.** Keep the last seen value per `device` and
  short-circuit on a repeat. This is the only state you need, and it is what
  stops a retry firing a second Telegram message.

Use `age_ms` to back-date: the device has no clock, so stamp the time on
receipt and subtract `age_ms` for a delayed delivery.

A `"event": "heartbeat"` request arrives on the timer wake (default every
12 h) carrying battery level only — no `counter`, `delta` or `count`.

## 6. Verify

1. Press button 1. The LED blinks immediately — that is local, and confirms
   nothing about the network.
2. The display updates within a second or two.
3. n8n shows an execution; Telegram gets a message.
4. Press three more times in quick succession. Only the first should be
   slow — presses 2-4 reuse the open TLS session and land in ~200 ms.
5. After ~30 s of no input the device goes back to deep sleep.

Serial at 115200 baud shows the whole flow (`pio device monitor`).

## 7. Troubleshooting

| Symptom | Cause |
|---|---|
| Count moves, nothing in n8n | No webhook URL set, or `setup_done` carried over from stock firmware. Log line: `no endpoint configured, dropping event` |
| `post failed (-1)` | TLS failure — check the URL is `https://`, and that the cert chains to a public root |
| `post rejected (403)` | Token mismatch, or Cloudflare Bot Fight Mode challenging the device. Exclude the webhook path with a WAF rule |
| `post rejected (404)` | Using the n8n **test** URL (`/webhook-test/…`), which only answers while the editor is listening |
| Two Telegram messages for one press | `seq` dedupe not implemented in the workflow |
| `Check connection!` on screen | Five consecutive failed timer-wake connections |
| Device never sleeps | It is on USB power, so it stays in awake mode |
| **Your network is missing from the setup scan list** | Router is on channel 12 or 13. The ESP-IDF default defers to the AP's advertised country and reverts on disconnect, so those channels are never scanned. Set **Wi-Fi Country** to a code whose range covers them (any EU code gives 1-13). `UA` is not supported by ESP-IDF — use `PL`. Also check the network is 2.4 GHz and not hidden |
| Placeholder glyph instead of an icon | Icon not in the SPIFFS image — only `plus` and `minus` ship. Re-run `tools/make_icons.py` and `-t uploadfs` |

## 8. Reading logs

Two routes, and they differ by build:

| Build | Console | How to read it |
|---|---|---|
| `original_release` | **UART0**, 115200 | `TX` + `GND` on the CMSIS-DAP header, via a 3.3 V USB-serial adapter |
| `original_debug` | **USB CDC**, 115200 | Just the USB-C cable |

```bash
pio run -e original_debug -t upload && pio device monitor
```

The `esp32_exception_decoder` filter is preconfigured, so panics come back
symbolised.

**For USB CDC, set Awake Mode to `1` first.** Deep sleep tears the USB device
down, so the port disappears and re-enumerates on every wake and your
terminal drops — right across the transition you probably want to watch.

Two things USB CDC cannot do, by construction:

- **Early boot is lost.** The port only exists after USB enumeration, so ROM
  bootloader and early IDF output never appear. Anything boot-related needs
  the UART pins.
- **It changes what you are observing.** USB-C supplies power, so
  `is_dc_connected()` goes true and the device may pick awake mode on its
  own. With the UART header you connect only `TX` and `GND` — leave `5V`
  and `3V3` alone — and the device keeps running on battery, so you see the
  real wake → connect → post → session → sleep cycle.

A CMSIS-DAP probe on the same header gives gdb as well:
`pio run -e original_debug -t upload` then `pio debug`, using the
`esp32s2_cmsisdap.cfg` already in the repo.

## 9. Compile-time settings

These have no portal field and need a rebuild — all in
`Firmware/HomeButtonsArduino/src/config.h`:

| Constant | Default | What it does |
|---|---|---|
| `SESSION_IDLE_TIMEOUT` | `30000` ms | How long to stay awake after a press. Your original ask was ~2 min; raise if a burst spans longer gaps |
| `HEARTBEAT_INTERVAL_DFLT` | `720` min | Battery-only timer wake |
| `HTTP_TIMEOUT` | `10000` ms | Per-attempt timeout |
| `HTTP_MAX_ATTEMPTS` | `3` | Retries per press, within the awake window |
| `COUNTER_MIN` / `COUNTER_MAX` | `0` / `999999` | Clamp range |
| `BTN_COUNTER_TITLE` | `{1, 2}` | Title buttons, per counter |
| `BTN_COUNTER_INC` | `{3, 4}` | Count / increment buttons |
| `BTN_COUNTER_DEC` | `{5, 6}` | Decrement buttons |
| `COUNTER_NAMES` | `"a"`, `"b"` | The `counter` field in the payload |
