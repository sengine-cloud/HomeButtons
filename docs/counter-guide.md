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
esptool --chip esp32s2 --port /dev/ttyACM0 erase-flash

esptool --chip esp32s2 --port /dev/ttyACM0 \
  --before default-reset --after hard-reset \
  write-flash -z --flash-mode dio --flash-freq 80m --flash-size 4MB \
  0x1000   bootloader.bin \
  0x8000   partitions.bin \
  0xe000   ota_data_initial.bin \
  0x10000  firmware.bin \
  0x350000 spiffs.bin
```

Command and option names are the esptool v5 spelling, with hyphens rather
than underscores (`write-flash`, not `write_flash`). v4 accepts only the
underscore form, so on an older install either upgrade with
`pip install -U esptool` or substitute underscores throughout.

All five images matter. Leaving `0x10000 firmware.bin` out is an easy
mistake to make and a confusing one, because the flash succeeds, the
device boots, and it carries on running whatever application was there
before.

### Picking the right port

`/dev/ttyACM0` above is only an example. If anything else USB-serial is
plugged in, the numbering is whatever order the kernel enumerated things,
so check rather than guess:

```bash
for d in /dev/ttyACM*; do
  printf '%s\t' "$d"
  udevadm info -q property -n "$d" | grep -E '^ID_VENDOR=|^ID_MODEL=' | tr '\n' ' '
  echo
done
```

The device is the one reporting `ID_VENDOR=Espressif`:

```
/dev/ttyACM1    ID_VENDOR=Flipper_Devices_Inc. ID_MODEL=Lagums
/dev/ttyACM2    ID_VENDOR=Espressif ID_MODEL=ESP32-S2
```

**Use the Espressif port, not a USB-to-serial adapter wired to the debug
header.** A serial adapter does reach the ROM bootloader, since it listens
on UART0 as well, and esptool will connect and report the chip correctly,
which makes it look like the right choice. Two things then go wrong:

- Anything much above the default baud rate fails partway through. The
  handshake succeeds, `Changing baud rate to 921600` succeeds, and the next
  read times out with `Unable to verify flash chip connection`. Drop
  `--baud` entirely on the Espressif port, where it is a native USB CDC and
  the number is ignored.
- `--before default-reset` cannot work, because DTR and RTS on the adapter
  are not wired to the chip's EN and IO0. It only appears to work if the
  device already happens to be in download mode. On the Espressif port
  esptool resets the running application into the bootloader over USB by
  itself, with no buttons.

**Expect the first attempt to fail** on the Espressif port with
`No such device` or `Failed to connect`. Triggering that reset tears down
the USB device esptool is talking through, invalidating its own handle.
The board is now in the bootloader, so run the same command again and it
succeeds. In a script, retry:

```bash
for i in 1 2 3; do esptool ... && break; sleep 3; done
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
| Then press **button 2** | Wi-Fi only — also carries the **Wi-Fi Country** field |
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
| **Awake Mode** | `1` keeps the device from deep sleeping, which is what you want while watching a serial log. `0` for normal use. Drains the battery fast |
| **Counter Reset** | When the counters clear themselves. `off`, `daily 03:00`, `weekly mon 03:00`, `monthly 1 03:00`. See below |
| Static IP / Gateway / Subnet / DNS / DNS 2 | Optional — leave blank for DHCP. All three of IP, gateway and subnet must be set for static to apply |
| Button 1-6 Label | See below |

**HTTPS is required.** The device attaches the ESP-IDF root CA bundle and
verifies the chain; a plain `http://` URL or an untrusted certificate will
fail the POST.

### Counter reset schedule

One free-text field, parsed into four modes:

| Value | Clears |
|---|---|
| `off` | Never |
| `daily 03:00` | Every day at 03:00 local |
| `weekly mon 03:00` | Mondays at 03:00 local. Day names are the first three letters, `sun` to `sat` |
| `monthly 1 03:00` | The 1st at 03:00 local. Day is capped at 28 so every month has one |

Anything unparseable falls back to `daily 03:00` rather than silently
disabling the reset. Times are 24-hour.

At the boundary the device clears both counters, redraws, and POSTs an
`"event": "reset"` carrying the cleared totals. On battery it schedules its
deep sleep wake for the boundary, so the clear happens on time rather than
at the next press.

**Local time comes from the receiver, not the device.** There is no RTC and
no NTP, so the offset is whatever the webhook response says it is, DST
included. If the response does not carry a clock the reset never fires at
all, and nothing else misbehaves to hint at it. See section 5.

Two behaviours worth knowing:

- If the clock has not been refreshed for `CLOCK_STALE_SECONDS` (48 h) the
  reset suspends rather than clearing on a guess.
- A period is cleared at most once. Local time moving backwards over the
  boundary, which happens at the autumn DST step and after a clock
  correction, does not clear again on the way back through.

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
  "sw_version":  "v3.0.0-counter.1",
  "build":       "3d33168b"    // git short SHA of the firmware
}
```

A scheduled reset sends `"event": "reset"` instead, with the cleared totals
as an object rather than one request per counter:

```jsonc
{
  "device":     "HBTNS-24011234-123XYZ",
  "seq":        838,
  "event":      "reset",
  "reset_mode": "daily",
  "counts":     { "a": 0, "b": 0 },
  "age_ms":     0,
  "battery_pct": 87,
  "battery_v":  3.92,
  "sw_version": "v3.0.0-counter.1",
  "build":      "3d33168b"
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

### The response is not optional

The device has no RTC and no NTP. **Its only source of time is the webhook
response**, so every reply must be JSON carrying an absolute UTC epoch and
the local offset in seconds:

```json
{ "ts": 1785114718, "tz_offset": 7200 }
```

In n8n, set the Webhook node's *Response Data* to an expression:

```
{{ JSON.stringify({ ts: Math.floor($now.toSeconds()),
                    tz_offset: $now.setZone("Europe/Warsaw").offset * 60 }) }}
```

Naming the zone explicitly is deliberate. `$now.offset` alone reports the
n8n **instance** timezone, which is UTC on a default install — the device
would then treat `daily 03:00` as 03:00 UTC. Pinning the zone in the
expression is DST-correct and does not depend on a setting nobody can see
from the device.

Two failure modes worth recognising, because neither looks like an error:

- **An unpublished draft.** n8n serves the last *published* version. Editing
  the response expression and not publishing leaves the device receiving
  `Workflow got started.` as plain text. It logs `response not JSON`, keeps
  counting perfectly, and simply never learns the time — so the scheduled
  reset never fires.
- **A missing `content-type`.** Add a `content-type: application/json`
  response header alongside the expression.

Check it from a shell rather than from the device:

```bash
curl -s -X POST https://your-n8n/webhook/<path> \
  -H 'content-type: application/json' -d '{"event":"sync"}'
# {"ts":1785114718,"tz_offset":7200}
```

## 6. Verify

1. Press **button 3** (the count button for counter A). Its LED lights and
   stays lit while the request is in flight, then goes out once the
   receiver has answered. Solid means pending, dark means delivered, three
   quick blinks means it failed.
2. The display updates within a second or two, without waiting on the
   network.
3. n8n shows an execution; Telegram gets a message.
4. Press three more times in quick succession. Only the first is slow.
   Presses 2-4 reuse the open TLS session and land in roughly 130 ms.
5. Press **button 5** to decrement, and confirm the count goes back down.
6. Press **button 1**. It is a title button, so it blinks twice and does
   nothing else. No POST, no change to the count.
7. After ~30 s of no input the device goes back to deep sleep.

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
| **Counters never clear at the scheduled time** | The clock is not trusted. `time` on the console reports `fresh 0` — the device suspends scheduled resets rather than clearing on a guess. Check the receiver is returning `ts`; `sync` forces the attempt |
| Device returns `ts` but the reset fires at the wrong hour | `tz_offset` is wrong. The device applies whatever the receiver sends and knows nothing about zones — see the n8n note below |

## 8. Reading logs

Two routes, both at 115200, and both live in either build:

| Route | How to read it | Survives deep sleep |
|---|---|---|
| **UART0** | `TX` + `GND` on the CMSIS-DAP header, via a 3.3 V USB-serial adapter | Yes — the port belongs to the adapter, not the device |
| **USB CDC** | Just the USB-C cable | No — the port vanishes and re-enumerates on every wake |

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

### Reflashing while iterating

The firmware presents a USB CDC device while the application runs, so
esptool resets it into the bootloader over USB-C on its own. You never need
BOOT+RST. For a code change with the icons untouched, only the application
image is worth rewriting:

```bash
esptool --chip esp32s2 --port /dev/ttyACM2 --after hard-reset \
  write-flash -z 0x10000 firmware.bin
```

Section 1 covers picking the port, why the first attempt fails, and why
`--baud` is best left off.

## 9. Serial console

Both serial routes accept commands as well as printing logs. Type `help`
for the list.

**Debug builds only** (`original_debug`). The console can rewrite the
webhook URL and auth token and reopen the setup portal, with no
authentication beyond physical access, so it is compiled out of
`original_release` entirely - 26 KB of flash and ~4 KB of RAM with it.
Build and flash `original_debug` when you need it.

| Command | What it does |
|---|---|
| `status` | Build stamps, uptime, heap, state machine, Wi-Fi, battery, counters, seq, endpoint, clock, schedule |
| `press <1-6>` | Injects a press through the real handler — counter, label, display, LED, POST |
| `counter [a\|b [n]]` | Shows or forces a counter. Clamped exactly as a press is |
| `sched [spec]` | Shows or sets the reset schedule (`off`, `daily 03:00`, `weekly mon 04:00`, `monthly 1 05:00`) |
| `reset` | Runs the boundary check now |
| `time` | UTC, local, offset, sync age, and whether the clock is trusted |
| `time set <epoch> [offset]` | Overrides the clock. Lets a weekly or monthly boundary be tested without waiting for it |
| `sync` / `post` | Forces a time-sync or heartbeat POST |
| `endpoint [url]` / `token [tok]` | Shows or sets the webhook target. The token is never echoed back, only its length |
| `wifi` | SSID, BSSID, channel, RSSI, applied vs configured country |
| `awake [0\|1]` | Shows or sets awake mode |
| `sleep [secs]` | Sleeps now, optionally forcing the wake time |
| `save` / `restart` / `setup` / `wifisetup` | Persist NVS · reboot · reboot into either portal |

`press` is the one that makes the rest testable: the whole counter flow can
be exercised without a finger on the device, and `time set` collapses a
day's wait into a second.

Two things worth knowing:

- **Commands are executed on the main task**, which blocks for the duration
  of an HTTPS POST. A reader task keeps buffering meanwhile, but the queue
  is four deep — paste a longer block than that while the network is slow
  and you will see `busy, command dropped` rather than silent loss.
- **`sleep` with no argument uses the schedule.** If the next boundary is
  20 hours away, that is how long the device is gone. Pass an explicit
  number of seconds when testing.

## 10. Compile-time settings

These have no portal field and need a rebuild — all in
`Firmware/HomeButtonsArduino/src/config.h`:

| Constant | Default | What it does |
|---|---|---|
| `SESSION_IDLE_TIMEOUT` | `30000` ms | How long to stay awake after a press. Raise it if presses in a burst are spaced further apart than this, at the cost of battery |
| `HEARTBEAT_INTERVAL_DFLT` | `720` min | Battery-only timer wake |
| `HTTP_TIMEOUT` | `10000` ms | Per-attempt timeout |
| `HTTP_MAX_ATTEMPTS` | `3` | Retries per press, within the awake window |
| `COUNTER_MIN` / `COUNTER_MAX` | `0` / `999999` | Clamp range |
| `RESET_CHECK_INTERVAL` | `10000` ms | How often a device left awake re-tests the reset boundary. Only bounds how late a clear can be |
| `CLOCK_STALE_SECONDS` | `48` h | Past this since the last sync, the clock is not trusted and scheduled resets suspend rather than fire on a guess |
| `CLOCK_RESYNC_SECONDS` | `6` h | How old the clock may get before a connect spends a request re-syncing it |
| `BTN_COUNTER_TITLE` | `{1, 2}` | Title buttons, per counter |
| `BTN_COUNTER_INC` | `{3, 4}` | Count / increment buttons |
| `BTN_COUNTER_DEC` | `{5, 6}` | Decrement buttons |
| `COUNTER_NAMES` | `"a"`, `"b"` | The `counter` field in the payload |
