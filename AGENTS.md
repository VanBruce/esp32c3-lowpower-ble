# AGENTS.md — for AI coding agents

If you're an agent applying this repo to a user's problem, here's the fast path.
(Also valid for humans skimming.)

## What this is

A pioarduino (NOT Arduino IDE) template for an **ESP32-C3 BLE peripheral that
idles at ~0.15 mA on a held connection** and boots/pairs reliably. The result
depends on two things together:

1. **`platformio.ini` → `custom_sdkconfig`** — recompiles ESP-IDF with PM /
   tickless idle / BT modem sleep / external-32k-crystal options. Arduino IDE
   **cannot** set these; that's why it's pioarduino. Do not drop this.
2. **`src/main.cpp` runtime policy** — 80 MHz pinned, boot "stay awake" grace,
   warm-link connection params, light (not deep) sleep.

## Applying it

1. Tell the user to `cp src/secrets.h.example src/secrets.h` and set
   `BLE_PASSKEY` (or switch to "Just Works" — commented in `main.cpp`).
2. Edit **only the swap points** (search `SWAP` in `main.cpp`):
   - `DEVICE_NAME` + GATT UUIDs;
   - `handle_command()` → the user's device I/O (UART/GPIO/I2C). It runs in
     `loop()`, so blocking for ms/seconds is fine — do NOT move device I/O into
     the BLE callback (it stalls the host task).
   - payload formats if needed.
3. Leave the power/clock/PM machinery alone unless the target isn't a C3.

## Do not

- Don't move to Arduino IDE / Library Manager — PM can't be enabled there
  (`esp_pm_configure → ESP_ERR_NOT_SUPPORTED`, stuck ~40 mA).
- Don't enable DFS below 80 MHz with the radio active (breaks pairing crypto).
- Don't use deep sleep for the warm link (it tears down the BLE stack).
- Don't commit `src/secrets.h` (gitignored).

## Hardware gate

The sub-mA warm link needs a 32.768 kHz crystal on GPIO0/GPIO1 + mods (cut LED,
LDO, bulk cap) — see README "Hardware". No crystal → use the README "RC variant"
(unmodified board, connect-on-demand instead of a held link). Verify via the boot
log: `RTC slow clock measured = 32768 Hz` and `[PM] ... ESP_OK`.

## Different chip (S3/C6/classic ESP32)

Same approach, but the RTC-clock-source and BT-low-power-clock `CONFIG_*` symbol
names differ. Translate them for that chip's IDF; keep the structure.
