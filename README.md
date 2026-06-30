# Reliable sub-mA BLE peripheral on ESP32-C3 (Arduino/NimBLE)

A **clone-and-go pioarduino template** for an ESP32-C3 BLE peripheral that idles
at **~0.15 mA while holding its connection open** (a "warm link" — no
per-command reconnect) and **boots/pairs reliably**. With the board mods below
that's roughly **a year on a single cell**.

Getting this right from scratch is a long debugging arc — the power result comes
from a `custom_sdkconfig` recompile that the Arduino IDE can't do, plus a handful
of non-obvious clock/boot fixes. This repo packages all of it. Swap in your
device's I/O and you have a low-power BLE device.

> Extracted from a working door-lock build, generalized into a template. The
> demo here is a trivial command/state service; the **value is the power +
> reliability recipe**, not the demo protocol.

## Does this match your problem?

If you arrived with one of these, you're in the right place:

- **"ESP32-C3 NimBLE idle is ~40 mA"** / can't get a BLE peripheral under a mA.
- **`esp_pm_configure() ... ESP_ERR_NOT_SUPPORTED`** — PM isn't compiled in (the
  Arduino core ships it off; this template turns it on).
- **`ConnectionAttemptFailed`** / pairing fails intermittently.
- **Board boots ~1 in 10 after RESET**, no serial, no advertising — but
  power-cycling (unplug/replug) is more reliable than the reset button.
- **`RTC slow clock measured = ~136000` / `~149000`** instead of 32768 (your
  32 kHz crystal isn't starting).
- BLE works on USB but **won't run on battery / brown-outs at boot**.

The fixes for every one of these are below, each as *symptom → cause → exact
config line*.

## Quick start

```sh
# 1. PlatformIO (CLI shown; the VS Code extension works too)
pip install platformio

# 2. Set your pairing passkey
cp src/secrets.h.example src/secrets.h     # then edit BLE_PASSKEY

# 3. Build + flash + watch
pio run -t upload
pio device monitor
```

On boot the serial log prints the measured RTC clock and the PM result — your
two health checks:

```
[CLK] RTC slow clock measured = 32768 Hz (32768=crystal OK, ~136000=RC fallback)
[PM] light_sleep=0 -> ESP_OK
```

`ESP_OK` (not `ESP_ERR_NOT_SUPPORTED`) means the custom_sdkconfig took effect.
`32768 Hz` means the crystal is alive. If either is wrong, see Troubleshooting.

Then pair once (see [Pairing](#pairing)) and the device idles in the warm link.

## How it works (the recipe)

Two inseparable halves: **build config** (`platformio.ini`) and **runtime
policy** (`src/main.cpp`).

### Why pioarduino, not Arduino IDE

Light sleep needs the ESP-IDF compiled with `CONFIG_PM_ENABLE` (+ tickless idle +
BT modem sleep). The Arduino IDE/CLI build flow **cannot set sdkconfig** — it
ships a prebuilt core with PM **off**, so `esp_pm_configure()` returns
`ESP_ERR_NOT_SUPPORTED` at runtime and you're stuck at ~40 mA. The community
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform's
`custom_sdkconfig` recompiles the IDF libs with our deltas while keeping a normal
Arduino sketch. **That recompile is the whole reason this works.** (This is also
why it can't ship as an Arduino *library* — see [Distributing](#distributing).)

### The sdkconfig deltas — symptom → cause → fix

All in `platformio.ini` under `custom_sdkconfig`:

| Symptom | Cause | Config line |
|---|---|---|
| Idle ~40 mA, can't sleep; `esp_pm_configure → ESP_ERR_NOT_SUPPORTED` | PM not compiled in | `CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` |
| Radio never sleeps between BLE events | BT controller has no low-power clock | `CONFIG_BT_CTRL_MODEM_SLEEP=y` + `..._MODE_1=y` + `CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL=y` |
| Warm link drifts / drops; clock reads ~136000 | Using imprecise internal RC, not the crystal | `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` |
| Crystal "declared dead" → RC fallback at boot | Cal window too short for a slow-starting 32k crystal | `CONFIG_RTC_CLK_CAL_CYCLES=8000` |
| **Boots ~1/10 after RESET**, no serial/advertising | C3's crystal bootstrap default is **0** → crystal doesn't restart in the boot window → BLE controller fails to init | `CONFIG_ESP_SYSTEM_RTC_EXT_XTAL_BOOTSTRAP_CYCLES=32768` |

That last one is the nastiest and least-documented: a RESET power-cycles the RTC
oscillator, the crystal cold-restarts too slowly to be measured in time, the BLE
controller (pinned to it) fails to init, and the chip never reaches `setup()`.
Maxing the bootstrap cycles kick-starts the crystal (~1 s extra boot — fine for
most devices).

### The runtime policy — in `src/main.cpp`

- **CPU pinned to 80 MHz** (`min == max`). DFS dropping to 10 MHz starves the
  LE-Secure ECDH pairing crypto → `ConnectionAttemptFailed`. 80 MHz also halves
  active draw vs the 160 MHz default.
- **Boot "stay awake" grace** (`BOOT_AWAKE_MS`, 12 s): light sleep makes the
  radio miss connection windows, so we *don't* sleep right after boot — pairing
  and reconnect land reliably — then switch light sleep on once the link settles.
  Also stays awake while a connection is mid-pairing.
- **Warm-link connection params** (`onAuthenticationComplete`): once encrypted we
  raise slave latency (interval 200 ms, latency 9, timeout 6 s) so the ESP wakes
  only ~every 2 s while the central holds the link open. The crystal's precision
  is what keeps the link aligned across those sleep gaps.
- **Light sleep, not deep sleep:** deep sleep tears down the BLE stack. Light
  sleep keeps the connection and RAM, just gates the clocks between events.

## Hardware

The software runs on any ESP32-C3; the **sub-mA warm link needs the crystal +
mods**. Without the crystal, use the RC variant ([below](#no-crystal-rc-variant))
on an unmodified board.

### Add a 32.768 kHz crystal

- Solder a watch crystal between **GPIO0 (XTAL_32K_P)** and **GPIO1
  (XTAL_32K_N)** with two small load caps (~12 pF) to GND (match your crystal's
  spec).
- ⚠️ **GPIO0 is a strapping pin** on many C3 boards — the crystal load is light
  enough not to disturb boot strapping in practice, but if boot gets flaky check
  this first.
- Verify with the boot log: `RTC slow clock measured = 32768 Hz`.

> **Measuring current? Disconnect USB first.** With `ARDUINO_USB_CDC_ON_BOOT=1`
> (used here for the debug serial), the USB Serial/JTAG peripheral keeps the chip
> awake whenever a host cable is attached — so a meter on the USB cable shows tens
> of mA, *not* ~0.15 mA, and the recipe looks broken. Measure on the 3V3 rail with
> USB unplugged.

### Mods for the lowest idle (optional but that's the point)

These take a typical Super Mini from tens of mA to ~0.15 mA:

- **Cut the power LED** — it alone can be several mA.
- **Remove/bypass the onboard LDO** if it back-feeds/leaks with the rail powered
  directly (measure yours). Feed regulated 3V3 straight to the `3V3` pin.
- **Add a cap at the module's 3V3 pins** if the board "only boots on USB" — boot
  inrush can brown out a weak or long-wired supply. In practice a **10 µF ceramic
  right at the pins was enough** (possibly even 4.7 µF); placement at the pins
  matters more than sheer capacitance. Only step up to a larger bulk cap (100s of
  µF) if a weaker supply still browns out.

### No crystal? (RC variant)

Runs on any **unmodified** C3 — reliable boot and good idle, at the cost of a
**few-second reconnect per command** (the imprecise RC clock can't hold a warm
link, so it connects on demand instead). In `platformio.ini`'s
`custom_sdkconfig`, drop the three crystal lines (`RTC_CLK_SRC_EXT_CRYS`,
`RTC_CLK_CAL_CYCLES`, `RTC_EXT_XTAL_BOOTSTRAP_CYCLES`) and the
`BT_CTRL_LPCLK_SEL_EXT_32K_XTAL` line, and add:

```
CONFIG_RTC_CLK_SRC_INT_RC=y
CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y
```

Then have your central connect → command → disconnect rather than holding the
link open.

## Pairing

LE Secure Connections with a bonded passkey — pair once, on the bench. Example
with `bluetoothctl` on Linux (e.g. a Raspberry Pi central):

```sh
sudo bluetoothctl
power on
agent KeyboardDisplay
default-agent
scan on            # find: LowPowerBLE  AA:BB:CC:DD:EE:FF
scan off
pair AA:BB:CC:DD:EE:FF      # enter your BLE_PASSKEY
trust AA:BB:CC:DD:EE:FF
```

> **Reflashing wipes the ESP32's bond.** If pairing then fails with
> `ConnectionAttemptFailed`, remove the stale bond on the central first:
> `remove AA:BB:CC:DD:EE:FF`, then pair again.

Prefer no passkey? Switch to "Just Works" in `setup()` (commented in
`main.cpp`) — simpler, but no MITM protection.

## Adapting it to your device (the swap points)

Search `main.cpp` for `SWAP`:

1. **`DEVICE_NAME` + the GATT UUIDs** — give your device its identity.
2. **`handle_command()`** — replace the demo toggle with your real I/O (UART,
   GPIO, I2C, …). It runs in `loop()`, *not* the BLE callback, so it's free to
   block for ms/seconds without stalling the BLE host task. Update `g_state_val`
   with whatever you report back.
3. **Payload formats** — the demo uses ascii `on`/`off` and a 1-byte state; use
   whatever your central speaks.

The power/reliability machinery (CPU policy, boot grace, warm-link params,
crystal handling) needs no changes.

### Want authenticated commands?

A bonded link already encrypts traffic and stops a neighbor connecting. If you
need each *command* authenticated (e.g. a lock), layer an HMAC challenge-response
on top: peripheral sends `nonce ‖ counter`, central replies
`HMAC-SHA256(secret, action ‖ nonce ‖ counter)`, peripheral verifies with a
constant-time compare and a monotonic counter (anti-replay). That pattern is
device-agnostic; it lived in the original lock firmware this was extracted from.

## Distributing / why it's a template, not a library

The power behavior comes from **build configuration** (`custom_sdkconfig`
recompiling the IDF), which an Arduino Library Manager install **cannot set** — a
library ships source, not sdkconfig. Installed as a library it would compile and
sit at ~40 mA, the worst outcome (looks like it should work). So this ships as a
**"Use this template" repo**: clone, swap the I/O, power works. The portable bits
(a BLE-peripheral helper, an HMAC handshake) *could* be factored into a thin
PlatformIO library, but the power result still requires this template's
`platformio.ini`.

## Troubleshooting

| You see | Do |
|---|---|
| `[PM] ... ESP_ERR_NOT_SUPPORTED` | Build isn't using `custom_sdkconfig`. Confirm you're building this PlatformIO project (not Arduino IDE) and the platform downloaded. `pio run -t fullclean` then rebuild. |
| `RTC slow clock measured = ~136000 Hz` | Crystal not oscillating: check solder/caps/orientation on GPIO0/1, or use the [RC variant](#no-crystal-rc-variant). |
| Boots ~1/10 after reset | `RTC_EXT_XTAL_BOOTSTRAP_CYCLES` not applied (or too low). Confirm it's in `custom_sdkconfig`; it's maxed (32768) here. |
| `ConnectionAttemptFailed` when pairing | Stale bond from a reflash → `remove <MAC>` on the central, re-pair. Also confirm CPU stays at 80 MHz (this template does). |
| Runs on USB, dead on battery | Boot-inrush brownout → add a cap at the 3V3 pins (a 10 µF ceramic was enough here); go larger only if a weak supply still browns out. |
| Idle still tens of mA | Cut the power LED; check the LDO back-feed; confirm the crystal reads 32768 and `[PM]` shows `ESP_OK`. |
| Warm link randomly drops in use | A marginal crystal can start then quit with a high bootstrap value — try `CONFIG_ESP_SYSTEM_RTC_EXT_XTAL_BOOTSTRAP_CYCLES=4096`. |

## Other targets

S3 / C6 / classic ESP32 use **different** sdkconfig symbol names for the RTC
clock source and BT low-power clock. The *approach* is identical; the specific
`CONFIG_*` lines need translating for that chip's IDF.

## License

MIT — see [LICENSE](LICENSE). Dependencies (NimBLE-Arduino, ESP-IDF,
arduino-esp32) are fetched at build time, not redistributed here, and are all
permissively licensed.
