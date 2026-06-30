// Reliable sub-mA BLE peripheral on ESP32-C3 (Arduino/NimBLE) — TEMPLATE.
//
// A minimal, working low-power BLE peripheral you clone and adapt. Out of the
// box it exposes one writable "command" characteristic and one "state" notify
// characteristic over a bonded/encrypted link, and idles at ~0.15 mA while
// holding the connection open (the "warm link" — no per-command reconnect).
//
// The value here is the POWER + RELIABILITY recipe, not the demo protocol. The
// recipe is half this file (CPU policy, boot grace, warm-link conn params) and
// half platformio.ini (the sdkconfig that turns PM on at all). See README.
//
// >>> SWAP POINTS (search for "SWAP") — the only parts you change for your device:
//     1. DEVICE_NAME and the GATT UUIDs.
//     2. handle_command() — do your device I/O here (UART, GPIO, I2C, ...).
//     3. (optional) the command/state payload formats.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "esp_pm.h"
#include "esp_private/esp_clk.h"   // esp_clk_slowclk_cal_get() — crystal diagnostic
#include "secrets.h"               // defines BLE_PASSKEY (copy secrets.h.example)

// ---- config (SWAP 1) ------------------------------------------------------
static const char* DEVICE_NAME = "LowPowerBLE";

// 16-bit-style UUIDs in the 128-bit base. Pick your own; just match your central.
static const char* SVC_UUID  = "0000fff0-0000-1000-8000-00805f9b34fb";
static const char* CHR_CMD   = "0000fff1-0000-1000-8000-00805f9b34fb"; // write  (enc)
static const char* CHR_STATE = "0000fff2-0000-1000-8000-00805f9b34fb"; // notify (enc)

static NimBLECharacteristic* g_state = nullptr;
static uint8_t g_state_val = 0;

// Set by the BLE callback once a command arrives; serviced in loop() so a slow
// device transaction (UART, mechanical actuator, ...) never blocks the BLE host
// task. Copy the payload out so it stays valid until loop() consumes it.
static volatile bool g_pending = false;
static uint8_t       g_cmd[32];
static size_t        g_cmd_len = 0;

// ---- power management: boot/pairing "stay awake" grace ---------------------
// Light sleep can make the radio miss connection windows, which is why pairing
// otherwise needs a couple of retries. So we stay FULLY AWAKE (no light sleep)
// for a grace window after boot — and while a connection is still mid-pairing —
// then drop into the low-power held-link mode. Cost: ~12s of higher draw per
// boot, which is rare. Reset the board right before pairing to open the window.
static const uint32_t BOOT_AWAKE_MS    = 12000;  // stay awake this long after reset
static const uint32_t CONNECT_AWAKE_MS = 8000;   // cap on the mid-pairing extension

static volatile bool     g_connected      = false;  // a central is connected
static volatile bool     g_authenticated  = false;  // ...and the link is encrypted
static volatile uint32_t g_connect_ms     = 0;      // millis() at onConnect
static bool              g_light_sleep_on = false;  // currently-applied PM mode

// ---- your device I/O (SWAP 2) ---------------------------------------------
// Runs in loop(), NOT the BLE callback, because real device I/O can block for
// many ms (or seconds). Do whatever your device needs here and update g_state_val
// with whatever you want to report back. This demo just toggles a byte and echoes.
static void handle_command(const uint8_t* cmd, size_t len) {
  Serial.printf("[cmd] %.*s (%u bytes)\n", (int)len, (const char*)cmd, (unsigned)len);

  // --- replace this block with your UART/GPIO/I2C/etc. transaction ---
  if (len == 2 && memcmp(cmd, "on", 2) == 0)        g_state_val = 1;
  else if (len == 3 && memcmp(cmd, "off", 3) == 0)  g_state_val = 0;
  else                                              g_state_val ^= 1;  // toggle
  // -------------------------------------------------------------------
}

// ---- BLE callbacks ---------------------------------------------------------
static void notify_state() {
  if (g_state) {
    g_state->setValue(&g_state_val, 1);
    g_state->notify();
  }
}

class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (!info.isEncrypted()) return;            // bonded/encrypted link required
    if (g_pending) return;                      // prior command still actuating
    std::string v = c->getValue();
    if (v.empty() || v.size() > sizeof(g_cmd)) return;
    memcpy(g_cmd, v.data(), v.size());
    g_cmd_len = v.size();
    g_pending = true;                           // hand off to loop()
  }
};

class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    g_connected = true;
    g_authenticated = false;        // not encrypted yet; stay awake to pair
    g_connect_ms = millis();
    Serial.printf("[BLE] connected: %s\n", info.getAddress().toString().c_str());
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    g_authenticated = info.isEncrypted();   // link settled -> loop() may sleep
    Serial.printf("[BLE] auth complete: encrypted=%d bonded=%d\n",
                  info.isEncrypted(), info.isBonded());
    if (info.isEncrypted()) {
      // WARM LINK (needs the external 32k crystal): the central holds the
      // connection open; slave latency lets the ESP skip events and sleep, waking
      // ~every 2s. The crystal is precise enough to keep the link aligned across
      // those gaps.  wake = interval*(latency+1) = 200ms*(9+1) = 2000ms;
      // supervision timeout must exceed 2*(latency+1)*interval = 4s -> use 6s.
      // Args: handle, min*1.25ms, max*1.25ms, latency, timeout*10ms.
      NimBLEDevice::getServer()->updateConnParams(info.getConnHandle(), 160, 160, 9, 600);
      Serial.println("[BLE] warm params: interval=200ms latency=9 timeout=6s");
    }
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    g_connected = false;
    g_authenticated = false;
    Serial.printf("[BLE] disconnected, reason=%d\n", reason);
    NimBLEDevice::startAdvertising();      // reachable again
  }
};

// Apply the CPU power policy. light_sleep=false keeps the chip fully awake at
// 80MHz (responsive radio — good for pairing); true enables automatic light
// sleep between BLE events (the low-power warm-link mode). min==max==80MHz either
// way: DFS down to 10MHz starves the LE Secure Connections ECDH pairing crypto
// and gives ConnectionAttemptFailed.
static void apply_power_mode(bool light_sleep) {
  esp_pm_config_t pm = {
      .max_freq_mhz = 80,
      .min_freq_mhz = 80,
      .light_sleep_enable = light_sleep,
  };
  esp_err_t pe = esp_pm_configure(&pm);
  g_light_sleep_on = light_sleep;
  // If this prints ESP_ERR_NOT_SUPPORTED, PM is not compiled in -> your build is
  // not using platformio.ini's custom_sdkconfig (or you're on Arduino IDE).
  Serial.printf("[PM] light_sleep=%d -> %s\n", light_sleep, esp_err_to_name(pe));
}

// ---- setup / loop ----------------------------------------------------------
void setup() {
  // Pin to 80MHz: halves active draw vs the 160MHz default, and (with min==max
  // in apply_power_mode) DISABLES DFS — which matters because DFS down to 10MHz
  // starves the LE-Secure ECDH pairing crypto. The power saving comes from LIGHT
  // SLEEP gating the clock between BLE events, NOT from frequency scaling.
  setCpuFrequencyMhz(80);

  Serial.begin(115200);
  delay(300);                 // let USB CDC enumerate so boot prints aren't lost

  // Crystal diagnostic: the boot-time RTC slow-clock calibration reports the
  // ACTUAL measured frequency. ~32768 Hz = the external crystal is oscillating;
  // ~136000 Hz = it didn't start and the chip fell back to the internal RC (your
  // warm link will be unstable -> check the crystal/caps, or use the RC variant).
  {
    uint32_t cal = esp_clk_slowclk_cal_get();
    float slow_hz = cal ? (1000000.0f * (1 << 19)) / cal : 0.0f;
    Serial.printf("[CLK] RTC slow clock measured = %.0f Hz "
                  "(32768=crystal OK, ~136000=RC fallback)\n", slow_hz);
  }

  NimBLEDevice::init(DEVICE_NAME);
  // LE Secure Connections + bonding + MITM protection. Pair ONCE on the bench.
  // DISPLAY_ONLY + a fixed passkey = authenticated (MITM-resistant) pairing
  // without a screen: the central enters BLE_PASSKEY during pairing, then the
  // bond keys take over and the passkey is never used again.
  //   For passkey-free "Just Works" instead (simpler, no MITM protection), use:
  //     setSecurityAuth(true, false, true);
  //     setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(BLE_PASSKEY);  // defined in secrets.h
  // Tell NimBLE which keys to exchange during bonding. Without this the LTK/IRK
  // distribution is empty and pairing aborts (org.bluez ConnectionAttemptFailed).
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new SrvCB());

  NimBLEService* svc = server->createService(SVC_UUID);

  NimBLECharacteristic* cmd = svc->createCharacteristic(
      CHR_CMD, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  cmd->setCallbacks(new CmdCB());

  g_state = svc->createCharacteristic(
      CHR_STATE, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setName(DEVICE_NAME);
  // Advertising only happens when disconnected (initial connect or after a drop),
  // so keep it snappy for fast (re)connect. Units are 0.625ms.
  adv->setMinInterval(0x140);  // ~200 ms
  adv->setMaxInterval(0x320);  // ~500 ms
  NimBLEDevice::startAdvertising();

  // Power: start FULLY AWAKE for the boot grace window so pairing / warm-link
  // reconnect is reliable — light sleep can make the radio miss connection
  // windows. loop() switches automatic light sleep on once the grace expires and
  // the link is settled. Needs the sdkconfig PM flags (PM_ENABLE + tickless idle)
  // + the BT low-power clock on the 32k crystal. NOT deep sleep — that drops BLE.
  apply_power_mode(false);
  Serial.printf("[PM] boot grace: awake %lu ms for reliable pairing\n",
                (unsigned long)BOOT_AWAKE_MS);
}

void loop() {
  // Power policy: awake during the boot grace window and while a connection is
  // mid-pairing (not yet encrypted), capped by CONNECT_AWAKE_MS so a stalled
  // handshake can't pin us awake. Otherwise enable light sleep (low-power link).
  {
    // Latch the boot grace so it ends exactly once. (A bare `millis() <
    // BOOT_AWAKE_MS` would become true again every ~49.7 days at the millis()
    // wrap, briefly kicking the device back to full power.)
    static bool boot_grace_done = false;
    if (!boot_grace_done && millis() >= BOOT_AWAKE_MS) boot_grace_done = true;

    bool awake = !boot_grace_done ||
                 (g_connected && !g_authenticated &&
                  (uint32_t)(millis() - g_connect_ms) < CONNECT_AWAKE_MS);
    bool want_light_sleep = !awake;
    if (want_light_sleep != g_light_sleep_on) apply_power_mode(want_light_sleep);
  }

  if (g_pending) {
    handle_command(g_cmd, g_cmd_len);   // SWAP 2: your device I/O
    notify_state();
    g_pending = false;
  }
  delay(5);
}
