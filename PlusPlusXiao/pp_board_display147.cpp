// Seeed XIAO 1.47" IPS touch display (nRF52840 Plus), -DPP_BOARD_DISPLAY147.
//
// Controls:
//   A (USR1, D19)  click       -> manual catch/spin press when connected;
//                                 while disconnected, fast advertising
//   A              double-tap  -> lock / unlock B and the touch screen
//   A              hold 5 s    -> wipe pairing (pp_app_lifecycle, raw pin);
//                                 not while locked
//   B (USR2, D15)  click       -> next mode: AUTO (catch + spin), CATCH,
//                                 SPIN, MANUAL
//   B              hold 1.5 s  -> backlight low / mid / high
//   touch: mode card = same as B; banner while disconnected = fast
//          advertising. A tap on a dark screen only wakes it.
//
// Locked, B and taps only show LOCKED and the 5 s wipe is off, so nothing
// gets changed by accident. A still presses. A single click waits
// kDoubleTapMs to see whether a second one follows.
//
// Catching is all or nothing and presses always go at the normal pace:
// this build has no "new Pokemon only" filter and no fast press speed.
//
// The screen (pp_ui147) shows the link, the battery, the last event, the
// mode, the counters and how long the phone has been connected. It goes dark 30 s after the last button, tap or
// warning; buttons still work with it dark. Warnings (bag full, box full,
// no balls) light it again.
//
// Power: on battery the board sleeps (System OFF) after 15 min without a
// connection, like the other builds; A or B wakes it. On USB it stays up.
//
// The USB console (115200 baud, no Enter needed):
//   p press   s auto spin   a advertise fast   b backlight   r restart screen
//   l lock / unlock   i status   W wipe pairing (capital W)   ? help
// The console keys work while locked.
#ifdef PP_BOARD_DISPLAY147
#include "pp_board.h"

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <nrf_soc.h>
#include <string.h>

#include "pp_app_lifecycle.h"
#include "pp_lcd147.h"
#include "pp_ui147.h"

using namespace Adafruit_LittleFS_Namespace;

namespace {

static PPLcd147 gLcd;

// ---------------- persisted settings ----------------

static const char* kSettingsPath = "/board_cfg.bin";

struct PersistedBoardSettings {
  uint32_t magic;      // 'PPDS'
  uint16_t version;    // 1
  uint16_t size;       // sizeof(PersistedBoardSettings)
  uint8_t catchMode;   // PP_CATCH_OFF or PP_CATCH_ALL (v0.1 also wrote new-only)
  uint8_t autoSpin;    // 0/1
  uint8_t unusedSpeed; // v0.1 press speed; read as normal, written as normal
  uint8_t advertise;   // 0/1
  uint8_t lastDropReason;
  uint8_t backlight;   // index into kBacklightLevels
  uint8_t unusedLastCatchMode;  // v2: screen timeout index
  uint8_t locked;      // 0/1 (v0.8; earlier builds wrote 0)
  uint32_t lifetimeConnSec;
};

static constexpr uint32_t kSettingsMagic = 0x53445050UL;  // "PPDS"

static uint8_t gCatchMode = PP_CATCH_ALL;
static bool gAutoSpin = true;
static bool gAdvertise = true;
static uint8_t gLastDropReason = 0;
static uint8_t gBacklight = 1;
static uint8_t gScreenTimeoutIndex = 1;
static const uint32_t kScreenTimeouts[] = { 15000, 30000, 60000, 120000 };
static uint32_t gLifetimeConnSec = 0;
static bool gLocked = false;
static bool gSettingsLoaded = false;
static bool gSavePending = false;
static uint32_t gSaveDueAt = 0;
static constexpr uint32_t kSaveDelayMs = 2000;

static const uint8_t kBacklightLevels[] = { 40, 120, 255 };
static const char* const kBacklightNames[] = { "low", "mid", "high" };

static bool loadSettings()
{
  PersistedBoardSettings rec;
  File f(InternalFS);
  if (!f.open(kSettingsPath, FILE_O_READ)) {
    return false;
  }
  int n = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
  f.close();
  if (n != static_cast<int>(sizeof(rec)) || rec.magic != kSettingsMagic || (rec.version != 1 && rec.version != 2) || rec.size != sizeof(rec)) {
    return false;
  }
  // A v0.1 record may say new-only: that is catching, so all.
  gCatchMode = rec.catchMode == PP_CATCH_OFF ? static_cast<uint8_t>(PP_CATCH_OFF) : static_cast<uint8_t>(PP_CATCH_ALL);
  gAutoSpin = rec.autoSpin != 0;
  gAdvertise = rec.advertise != 0;
  gLastDropReason = rec.lastDropReason;
  gBacklight = rec.backlight < sizeof(kBacklightLevels) ? rec.backlight : 1;
  gScreenTimeoutIndex = rec.version == 2 && rec.unusedLastCatchMode < 4 ? rec.unusedLastCatchMode : 1;
  gLifetimeConnSec = rec.lifetimeConnSec;
  gLocked = rec.locked != 0;
  return true;
}

static void saveSettings()
{
  PersistedBoardSettings rec = {};
  rec.magic = kSettingsMagic;
  rec.version = 2;
  rec.unusedLastCatchMode = gScreenTimeoutIndex;
  rec.size = sizeof(rec);
  rec.catchMode = gCatchMode;
  rec.autoSpin = gAutoSpin ? 1 : 0;
  rec.unusedSpeed = PP_SPEED_NORMAL;
  rec.advertise = gAdvertise ? 1 : 0;
  rec.lastDropReason = gLastDropReason;
  rec.backlight = gBacklight;
  rec.lifetimeConnSec = gLifetimeConnSec;
  rec.locked = gLocked ? 1 : 0;
  InternalFS.remove(kSettingsPath);
  File f(InternalFS);
  if (!f.open(kSettingsPath, FILE_O_WRITE)) {
    Serial.println("Board settings: write failed");
    gSavePending = true;
    gSaveDueAt = millis() + 10000;
    return;
  }
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
  f.close();
  gSavePending = written != sizeof(rec);
  if (gSavePending) gSaveDueAt = millis() + 10000;
}

// A save rewrites the file in flash, which holds up the loop for a while.
// Changes from the buttons and the screen are saved once things go quiet,
// so the screen shows them straight away (v0.6).
static void saveSettingsSoon()
{
  gSavePending = true;
  gSaveDueAt = millis() + kSaveDelayMs;
}

// ---------------- screen ----------------

static constexpr uint32_t kBannerMs = 3000;
static constexpr uint32_t kWarningBannerMs = 8000;

static bool gScreenOn = false;
static uint32_t gScreenOnSince = 0;
static bool gDrawn = false;            // gShown is what the glass shows
static PPUiState gShown = {};
static uint8_t gBanner = PP_UI_BANNER_IDLE;
static uint32_t gBannerUntil = 0;

static void screenWake()
{
  gScreenOnSince = millis();
  if (gScreenOn) return;
  gLcd.wake();
  gDrawn = false;                      // the next tick draws it all
  gScreenOn = true;
}

static void screenSleep()
{
  if (!gScreenOn) return;
  gLcd.sleep();
  gScreenOn = false;
}

static void showBanner(uint8_t banner, uint32_t ms)
{
  gBanner = banner;
  gBannerUntil = millis() + ms;
}

// ---------------- buttons ----------------

struct Button {
  uint8_t pin;
  bool stable;        // debounced level (true = pressed)
  bool raw;
  uint32_t rawSince;
  uint32_t pressedAt;
  bool holdFired;
};

static Button gBtnA = { PP_BOARD_PIN_CATCH, false, false, 0, 0, false };
static Button gBtnB = { PP_BOARD_PIN_BTN2, false, false, 0, 0, false };

static constexpr uint32_t kDebounceMs = 25;
static constexpr uint32_t kClickMaxMs = 600;
static constexpr uint32_t kHoldMs = 1500;
// Release to the next press for a double-tap.
static constexpr uint32_t kDoubleTapMs = 350;
static bool gAClickPending = false;
static uint32_t gAClickAt = 0;

// Returns 1 = click, 2 = hold (once), 0 = nothing.
static uint8_t pollButton(Button& b)
{
  uint32_t now = millis();
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.raw) {
    b.raw = raw;
    b.rawSince = now;
  }
  if (raw != b.stable && (now - b.rawSince) >= kDebounceMs) {
    b.stable = raw;
    if (raw) {
      b.pressedAt = now;
      b.holdFired = false;
    } else {
      uint32_t held = now - b.pressedAt;
      if (held < kClickMaxMs && !b.holdFired) return 1;
      return 0;
    }
  }
  if (b.stable && !b.holdFired && (now - b.pressedAt) >= kHoldMs) {
    b.holdFired = true;
    return 2;
  }
  return 0;
}

// ---------------- touch ----------------

static bool gTouchDown = false;

// Returns the zone touched, or PP_UI_ZONE_NONE. A touch counts the moment
// the finger lands rather than when it lifts (v0.6); one on a dark screen
// only wakes it.
static uint8_t pollTouch()
{
  int16_t x, y;
  bool down = gLcd.touchRead(x, y);
  if (down && !gTouchDown) {
    gTouchDown = true;
    bool woke = !gScreenOn;
    screenWake();
    return woke ? PP_UI_ZONE_NONE : ppUiHitTest(x, y);
  }
  if (!down && gTouchDown) {
    gTouchDown = false;
    screenWake();
    return PP_UI_ZONE_NONE;
  }
  if (down) screenWake();              // a held finger keeps the screen on
  return PP_UI_ZONE_NONE;
}

// ---------------- battery ----------------
//
// XIAO nRF52840 Plus: VBAT -> 1 M -> P0.31 (AIN7) -> 499 k -> P0.14. P0.14
// is driven low for good: high, it would leave P0.31 at the battery's full
// voltage, above the pin's limit. The divider then draws about 3 uA. The
// charger's ~CHG (P0.17) is low while charging.

static constexpr uint32_t kRawChg = 17;
static constexpr uint32_t kBatteryPeriodMs = 10000;
static uint32_t gBatteryAt = 0;
static uint16_t gBatteryMv = 0;
static uint8_t gBatteryPct = 0;
static bool gBatteryRead = false;

static bool usbPowered()
{
  uint32_t reg = 0;
  return sd_power_usbregstatus_get(&reg) == NRF_SUCCESS && (reg & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

static bool charging()
{
  return nrf_gpio_pin_read(kRawChg) == 0;
}

static uint8_t percentFromMv(uint16_t mv)
{
  // A rough Li-ion curve under a light load.
  static const uint16_t kMv[] = { 3300, 3500, 3600, 3700, 3750, 3800, 3900, 4000, 4100, 4180 };
  static const uint8_t kPct[] = { 0, 5, 10, 25, 35, 50, 65, 80, 92, 100 };
  if (mv <= kMv[0]) return 0;
  for (uint8_t i = 1; i < sizeof(kMv) / sizeof(kMv[0]); ++i) {
    if (mv < kMv[i]) {
      return static_cast<uint8_t>(kPct[i - 1] + (uint32_t) (kPct[i] - kPct[i - 1]) * (mv - kMv[i - 1]) / (kMv[i] - kMv[i - 1]));
    }
  }
  return 100;
}

static void readBattery()
{
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; ++i) sum += analogRead(PIN_VBAT);
  // 12-bit reading of 0..3.6 V, times (1000 + 499) / 499.
  uint32_t mv = (sum / 8) * 3600UL / 4095UL * 1499UL / 499UL;
  gBatteryMv = static_cast<uint16_t>(mv);
  uint8_t pct = percentFromMv(gBatteryMv);
  // Small steps are noise; show a change of 2 % or more.
  if (!gBatteryRead || pct + 2 <= gBatteryPct || pct >= gBatteryPct + 2 || pct == 0 || pct == 100) {
    gBatteryPct = pct;
  }
  gBatteryRead = true;
}

// ---------------- counters / link bookkeeping ----------------

static uint16_t gCountCaught = 0;
static uint16_t gCountFled = 0;
static uint16_t gCountSpins = 0;
static uint32_t gConnSince = 0;
static bool gWasConnected = false;
static bool gRngSeeded = false;
static bool gButtonsSeeded = false;

static const char* presetLabel(uint8_t p)
{
  switch (p) {
    case PP_UI_PRESET_AUTO: return "AUTO (catch + spin)";
    case PP_UI_PRESET_CATCH: return "CATCH (catch only)";
    case PP_UI_PRESET_SPIN: return "SPIN (spin only)";
    default: return "MANUAL";
  }
}

static const char* dropReasonText(uint8_t reason)
{
  switch (reason) {
    case 0x00: return "none yet";
    case 0x13: return "phone hung up";
    case 0x08: return "out of range";
    case 0x16: return "by the board";
    case 0x3e: return "setup failed";
    case 0x22: return "link timeout";
    case 0x3b: return "bad params";
    default: return "other";
  }
}

static void printSettings()
{
  Serial.print("Board: mode ");
  Serial.print(presetLabel(ppUiPreset(gCatchMode, gAutoSpin)));
  Serial.print(", catch ");
  Serial.print(gCatchMode == PP_CATCH_OFF ? "off" : "on");
  Serial.print(", spin ");
  Serial.print(gAutoSpin ? "on" : "off");
  Serial.print(", adv ");
  Serial.print(gAdvertise ? "on" : "off");
  Serial.print(", backlight ");
  Serial.print(kBacklightNames[gBacklight]);
  Serial.println(gLocked ? ", locked" : ", unlocked");
}

// ---------------- actions ----------------

static void actFastAdvertising(const BoardRefs& refs, const char* what)
{
  Serial.print(what);
  Serial.println(" while disconnected -> fast advertising window");
  refs.startAdvertising();
  refs.armDisconnectedSleepTimer("button");
  showBanner(PP_UI_BANNER_ADVERTISING, kBannerMs);
}

static void actCatchPress(const BoardRefs& refs, bool authenticated)
{
  if (authenticated) {
    Serial.println("Button A -> manual press");
    refs.manualButtonPress();
    showBanner(PP_UI_BANNER_PRESSED, 1000);
  } else {
    Serial.println("Button A pressed, not connected");
    showBanner(PP_UI_BANNER_NOT_CONNECTED, kBannerMs);
  }
}

static void applyPreset(uint8_t p)
{
  bool catching = (p == PP_UI_PRESET_AUTO || p == PP_UI_PRESET_CATCH);
  bool spin = (p == PP_UI_PRESET_AUTO || p == PP_UI_PRESET_SPIN);
  gCatchMode = catching ? PP_CATCH_ALL : PP_CATCH_OFF;
  gAutoSpin = spin;
  saveSettingsSoon();
  Serial.print("Mode -> ");
  Serial.println(presetLabel(p));
}

static void actNextPreset()
{
  applyPreset((ppUiPreset(gCatchMode, gAutoSpin) + 1) % 4);
}

static void actToggleAutoSpin()
{
  gAutoSpin = !gAutoSpin;
  saveSettingsSoon();
  Serial.print("Auto spin -> ");
  Serial.println(gAutoSpin ? "on" : "off");
}

static void actToggleLock()
{
  gLocked = !gLocked;
  saveSettingsSoon();
  showBanner(gLocked ? PP_UI_BANNER_LOCKED : PP_UI_BANNER_UNLOCKED, kBannerMs);
  Serial.println(gLocked ? "Controls -> locked" : "Controls -> unlocked");
}

// A locked control: say so, do nothing else.
static void actLockedNote(const char* what)
{
  Serial.print(what);
  Serial.println(" ignored: locked (double-tap A to unlock)");
  showBanner(PP_UI_BANNER_LOCKED, kBannerMs);
}

static void actCycleBacklight()
{
  gBacklight = (gBacklight + 1) % sizeof(kBacklightLevels);
  saveSettingsSoon();
  if (gScreenOn) gLcd.backlight(kBacklightLevels[gBacklight]);
  Serial.print("Backlight -> ");
  Serial.println(kBacklightNames[gBacklight]);
}

// ---------------- USB console ----------------

static constexpr uint32_t kSleepHoldMs = 5UL * 60UL * 1000UL;
static uint32_t gSleepHoldAt = 0;

static void printConsoleHelp()
{
  Serial.println("Keys: p press, s auto spin, a advertise fast, b backlight, r restart screen,");
  Serial.println("      l lock, t screen timeout, i status, W wipe pairing (capital W), ? this list");
}

static void printStatus(const BoardRefs& refs, bool connected)
{
  Serial.print("Link: ");
  Serial.print(connected ? refs.pairingStageLabel(*refs.pairingStage) : "not connected");
  Serial.print(", pairing stored ");
  Serial.print(*refs.persistentReconnectReady ? "yes" : "no");
  Serial.print(", last drop ");
  Serial.print(dropReasonText(gLastDropReason));
  Serial.print(", power ");
  Serial.print(usbPowered() ? (charging() ? "USB, charging" : "USB") : "battery");
  Serial.print(", battery ");
  Serial.print(gBatteryMv);
  Serial.print(" mV (");
  Serial.print(gBatteryPct);
  Serial.print(" %), radio ");
  Serial.print(Bluefruit.getTxPower());
  Serial.print(" dBm, regulator ");
  Serial.println(ppRegulatorStatusText());
  printSettings();
  Serial.print("Since boot: caught ");
  Serial.print(gCountCaught);
  Serial.print(", fled ");
  Serial.print(gCountFled);
  Serial.print(", spins ");
  Serial.println(gCountSpins);
  Serial.print("NFC pins as GPIO (button B): ");
  Serial.println((NRF_UICR->NFCPINS & UICR_NFCPINS_PROTECT_Msk) == 0 ? "yes" : "NO");
}

static void serviceConsole(const BoardRefs& refs, bool connected, bool authenticated)
{
  while (Serial.available() > 0) {
    switch (Serial.read()) {
      case 'p':
        if (connected) actCatchPress(refs, authenticated);
        else if (ppBoardAdvertiseEnabled()) actFastAdvertising(refs, "Console key");
        break;
      case 's': actToggleAutoSpin(); break;
      case 'b': actCycleBacklight(); break;
      case 't':
        gScreenTimeoutIndex = (gScreenTimeoutIndex + 1) % 4;
        saveSettingsSoon();
        screenWake();
        Serial.print("Screen timeout seconds: ");
        Serial.println(kScreenTimeouts[gScreenTimeoutIndex] / 1000);
        break;
      case 'l': actToggleLock(); break;
      case 'r':
        Serial.println("Screen -> restarted");
        if (gScreenOn) {
          gLcd.backlight(0);           // back on once the next tick has drawn
          gLcd.reinit();
          gDrawn = false;              // the next tick draws it all
        }
        screenWake();
        break;
      case 'a':
        if (!connected && ppBoardAdvertiseEnabled()) actFastAdvertising(refs, "Console key");
        break;
      case 'i': printStatus(refs, connected); break;
      case 'W': refs.resetPairing("console"); break;
      case '?':
      case 'h': printConsoleHelp(); break;
      default: break;  // line ends and anything else
    }
  }
}

static PPUiState currentState(bool connected, bool authenticated)
{
  PPUiState st = {};
  if (authenticated) st.link = PP_UI_LINK_READY;
  else if (connected) st.link = PP_UI_LINK_PAIRING;
  else st.link = gAdvertise ? PP_UI_LINK_SEARCHING : PP_UI_LINK_HIDDEN;
  st.catchMode = gCatchMode;
  st.spin = gAutoSpin;
  st.locked = gLocked;
  st.caught = gCountCaught;
  st.fled = gCountFled;
  st.spins = gCountSpins;
  st.linkedSec = (connected && gConnSince) ? (millis() - gConnSince) / 1000 : PP_UI_NOT_LINKED;
  if (usbPowered()) st.power = charging() ? PP_UI_POWER_CHARGING : PP_UI_POWER_USB;
  else st.power = PP_UI_POWER_BATTERY;
  st.batteryPct = gBatteryPct;
  st.banner = gBanner;
  return st;
}

static bool sameState(const PPUiState& a, const PPUiState& b)
{
  return a.link == b.link && a.catchMode == b.catchMode && a.spin == b.spin && a.locked == b.locked &&
         a.caught == b.caught && a.fled == b.fled && a.spins == b.spins && a.linkedSec == b.linkedSec && a.power == b.power &&
         a.batteryPct == b.batteryPct && a.banner == b.banner;
}

}  // namespace

// ---------------- public API ----------------

void ppBoardInit()
{
  // The XIAO's own RGB LED (active low) stays off: the screen shows status.
  pinMode(LED_RED, OUTPUT);   digitalWrite(LED_RED, HIGH);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE, OUTPUT);  digitalWrite(LED_BLUE, HIGH);
  pinMode(PP_BOARD_PIN_CATCH, INPUT_PULLUP);
  pinMode(PP_BOARD_PIN_BTN2, INPUT_PULLUP);
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);          // see "battery" above
  nrf_gpio_cfg_input(kRawChg, NRF_GPIO_PIN_PULLUP);
  analogReference(AR_INTERNAL);            // 0..3.6 V
  analogReadResolution(12);
  // Bluefruit would otherwise blink the blue LED while advertising.
  Bluefruit.autoConnLed(false);
  randomSeed(micros() ^ (analogRead(A0) << 8));
  InternalFS.begin();
  gSettingsLoaded = loadSettings();
  readBattery();

  gLcd.begin();
  gScreenOn = true;
  gScreenOnSince = millis();
  ppUiDraw(gLcd, currentState(false, false), nullptr);
  gShown = currentState(false, false);
  gDrawn = true;
  gLcd.backlight(kBacklightLevels[gBacklight]);
}

void ppBoardLogSettings()
{
  if (!gSettingsLoaded) {
    Serial.println("Board settings: no valid record, using defaults");
  }
  printSettings();
  Serial.println("XIAO 1.47\" display build: status on the screen, A = catch, B = mode");
  if ((NRF_UICR->NFCPINS & UICR_NFCPINS_PROTECT_Msk) != 0) {
    Serial.println("WARNING: NFC pins are not GPIOs, so button B (P0.10) cannot work");
  }
  printConsoleHelp();
}

void ppBoardPrepareSystemOff()
{
  if (gSavePending) saveSettings();
  gLcd.sleep();
  gScreenOn = false;
  // Hold the panel and the touch chip in reset while the nRF52 is off; the
  // wake-up is a full boot, which resets them again anyway.
  digitalWrite(D17, LOW);
}

bool ppBoardAutoSpin() { return gAutoSpin; }
uint8_t ppBoardCatchMode() { return gCatchMode; }
uint8_t ppBoardPressSpeed() { return PP_SPEED_NORMAL; }
bool ppBoardAdvertiseEnabled() { return gAdvertise; }
bool ppBoardResetHoldAllowed() { return !gLocked; }

void ppBoardOnEvent(PPLedEvent evt, const char* label)
{
  (void) label;
  switch (evt) {
    case PP_LED_POKEMON: showBanner(PP_UI_BANNER_POKEMON, kBannerMs); break;
    case PP_LED_NEW_POKEMON: showBanner(PP_UI_BANNER_NEW_POKEMON, kBannerMs); break;
    case PP_LED_POKESTOP: showBanner(PP_UI_BANNER_POKESTOP, kBannerMs); break;
    case PP_LED_CAUGHT: if (gCountCaught < UINT16_MAX) ++gCountCaught; showBanner(PP_UI_BANNER_CAUGHT, kBannerMs); break;
    case PP_LED_FLED: if (gCountFled < UINT16_MAX) ++gCountFled; showBanner(PP_UI_BANNER_FLED, kBannerMs); break;
    case PP_LED_ITEMS: if (gCountSpins < UINT16_MAX) ++gCountSpins; showBanner(PP_UI_BANNER_ITEMS, kBannerMs); break;
    // The app turned a spin down (a stop on cool-down, say): nothing to fix.
    case PP_LED_SPIN_FAILED: showBanner(PP_UI_BANNER_NO_SPIN, kBannerMs); break;
    // Warnings want action in the game: they light the screen.
    case PP_LED_BALLS_EMPTY: showBanner(PP_UI_BANNER_NO_BALLS, kWarningBannerMs); screenWake(); break;
    case PP_LED_BAG_FULL: showBanner(PP_UI_BANNER_BAG_FULL, kWarningBannerMs); screenWake(); break;
    case PP_LED_BOX_FULL: showBanner(PP_UI_BANNER_BOX_FULL, kWarningBannerMs); screenWake(); break;
    default: break;
  }
}

void ppBoardSetNote(const char* text)
{
  Serial.print("Note: ");
  Serial.println(text);
}

void ppBoardOnDisconnect(uint8_t reason)
{
  gLastDropReason = reason;
  saveSettingsSoon();
  Serial.print("Link dropped: ");
  Serial.print(dropReasonText(reason));
  Serial.print(" (0x");
  Serial.print(reason, HEX);
  Serial.println(")");
}

void ppBoardTick(const BoardRefs& refs)
{
  bool connected = (*refs.activeConnHandle != BLE_CONN_HANDLE_INVALID);
  bool authenticated = connected && (*refs.pairingStage == PAIR_STAGE_AUTHENTICATED);
  uint32_t now = millis();

  // Seed the press randomiser from the SoftDevice RNG once it is running.
  if (!gRngSeeded) {
    uint8_t rnd[4];
    if (sd_rand_application_vector_get(rnd, sizeof(rnd)) == NRF_SUCCESS) {
      randomSeed((uint32_t) rnd[0] | ((uint32_t) rnd[1] << 8) | ((uint32_t) rnd[2] << 16) | ((uint32_t) rnd[3] << 24));
      gRngSeeded = true;
    }
  }

  // Link bookkeeping (lifetime total persisted on disconnect only).
  if (connected && !gWasConnected) {
    gConnSince = now | 1;
  } else if (!connected && gWasConnected && gConnSince) {
    gLifetimeConnSec += (now - gConnSince) / 1000;
    gConnSince = 0;
    saveSettings();
  }
  gWasConnected = connected;

  if ((now - gBatteryAt) >= kBatteryPeriodMs) {
    gBatteryAt = now;
    readBattery();
  }

  if (gSavePending && static_cast<int32_t>(now - gSaveDueAt) >= 0) saveSettings();

  // Buttons. On the first poll, whatever is already pressed (the press that
  // woke us from System OFF) is the baseline: ignored until released.
  if (!gButtonsSeeded) {
    Button* const buttons[] = { &gBtnA, &gBtnB };
    for (Button* b : buttons) {
      bool down = digitalRead(b->pin) == LOW;
      b->raw = down;
      b->stable = down;
      b->rawSince = now;
      b->pressedAt = now;
      b->holdFired = true;
    }
    gButtonsSeeded = true;
  } else {
    uint8_t a = pollButton(gBtnA);
    uint8_t b = pollButton(gBtnB);
    if (a || b || gBtnA.stable || gBtnB.stable) screenWake();
    // A click waits for a possible second one; two make a double-tap.
    if (a == 1) {
      if (gAClickPending) {
        gAClickPending = false;
        actToggleLock();
      } else {
        gAClickPending = true;
        gAClickAt = now;
      }
    }
    if (gAClickPending && !gBtnA.stable && (now - gAClickAt) > kDoubleTapMs) {
      gAClickPending = false;
      if (connected) actCatchPress(refs, authenticated);
      else if (ppBoardAdvertiseEnabled()) actFastAdvertising(refs, "Button A");
      else actCatchPress(refs, false);
    }
    if (b && gLocked) {
      if (b == 1) actLockedNote("Button B");
    } else if (b == 1) {
      if (!connected && ppBoardAdvertiseEnabled()) actFastAdvertising(refs, "Button B");
      actNextPreset();
    } else if (b == 2) {
      actCycleBacklight();
    }
  }

  uint8_t zone = pollTouch();
  if (zone != PP_UI_ZONE_NONE && gLocked) {
    actLockedNote("Tap");
  } else {
    switch (zone) {
      case PP_UI_ZONE_BANNER:
        if (!connected && ppBoardAdvertiseEnabled()) actFastAdvertising(refs, "Banner tap");
        else gBannerUntil = now;         // dismiss
        break;
      case PP_UI_ZONE_MODE: actNextPreset(); break;
      default: break;
    }
  }

  serviceConsole(refs, connected, authenticated);

  // On battery the board sleeps after 15 min without a connection (the
  // shared timer); on USB the timer is pushed out before it runs down.
  if (!connected && (now - gSleepHoldAt) >= kSleepHoldMs) {
    gSleepHoldAt = now;
    if (usbPowered()) refs.armDisconnectedSleepTimer("usb power");
  }

  if (gBanner != PP_UI_BANNER_IDLE && static_cast<int32_t>(now - gBannerUntil) >= 0) {
    gBanner = PP_UI_BANNER_IDLE;
  }

  if (gScreenOn && (now - gScreenOnSince)  >= kScreenTimeouts[gScreenTimeoutIndex] && !gTouchDown) {
    screenSleep();
  }
  if (gScreenOn) {
    PPUiState st = currentState(connected, authenticated);
    if (!gDrawn) {
      ppUiDraw(gLcd, st, nullptr);
      gLcd.backlight(kBacklightLevels[gBacklight]);
      gDrawn = true;
    } else if (!sameState(st, gShown)) {
      ppUiDraw(gLcd, st, &gShown);
    }
    gShown = st;
  }
}

void ppBoardFatal(const char* what)
{
  if (gLcd.asleep()) gLcd.wake();
  ppUiDrawFatal(gLcd, what);
  gLcd.backlight(255);
  for (;;) {
    Serial.print("FATAL: ");
    Serial.println(what);
    delay(2000);
  }
}

#endif  // PP_BOARD_DISPLAY147
