// main_pip.cpp — Pip on M5StickC PLUS2 / S3 revision.
//
// Pip is a pocket-sized accessory for one of the three home pets
// (Muffin / Visu / Goo-Goo), not a pet itself. The display shows the
// next treat the user will throw (Apple / Carrot / Bone), a "Shake me!"
// hint, and the two button hints. Shaking sends an ESP-NOW broadcast
// to a paired pet via pip_link_send. Out of radio range Pip still
// shows the treat UI but no one's listening.
//
// State machine:
//   Idle      → display shows treat + hint, ready for input
//   Throwing  → ~800 ms post-shake animation (treat flies up + fades)
//   Sleeping  → 60 s idle → Zzz screen
//
// Power ramp:
//   30 s → display dims to BRIGHT_DIM
//   60 s → BRIGHT_DARK + Sleeping UI
//   3 min → ESP32 deep sleep, woken by the power button (GPIO35)
//
// Inputs:
//   BtnA  — cycle Apple → Carrot → Bone → Apple
//   BtnB  — force sleep (3 s grace, then deep sleep)
//   shake — throw the currently selected treat
//
// Compiled only in [env:pip] / [env:pip-s3].

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <math.h>
#include <esp32-hal-cpu.h>

#include "target_caps.h"
#include "i18n.h"
#include "pip/face_pip.h"
#include "pip/sound_pip.h"
#include "pip/pip_tuning.h"
#include "pip/pip_link_send.h"

namespace {

M5Canvas g_canvas(&M5.Display);

// ── Accessory state ────────────────────────────────────────────────────────
struct PipState {
    pip::UiState   state              = pip::UiState::Idle;
    pip::MenuPage  page               = pip::MenuPage::Empty;
    bool           forceSleep         = false;
    // Set when forceSleep flips from false to true. Used to gate the
    // grace period before we actually drop into deep sleep, so an
    // accidental BtnB press can be cancelled with a BtnA click.
    uint32_t       forceSleepStartMs  = 0;
    uint32_t       lastInteractionMs  = 0;
    uint32_t       throwStartMs       = 0;
    uint32_t       lastSavedMs        = 0;
    // Wand-page state — peaks accumulate while the user is mid-gesture,
    // then dispatch as msgType 18 once the gesture has been still for
    // kWandQuietMs. Reset to 0 on dispatch.
    uint8_t        wandPeakCount      = 0;
    uint32_t       wandLastPeakMs     = 0;
    // Language. 0 = DE, 1 = EN. Mirrors g_lang in i18n.cpp; the first-
    // run picker sets it before any user-facing text is rendered.
    uint8_t        language           = 0;
    bool           languageChosen     = false;
};
PipState g_pet;

// Wand: number of ms of stillness after the last peak before we dispatch
// the count to the home pet. Short enough that the "shake then stop"
// gesture feels responsive, long enough that the user can chain several
// flicks before they get sent.
constexpr uint32_t kWandQuietMs = 900;
// Cap so we don't spam the home pet with absurd hop counts.
constexpr uint8_t  kWandMaxPeaks = 9;

// ── IMU classifier ─────────────────────────────────────────────────────────
//
// Only shake detection is wired — the accessory has no petting / standing
// reactions. Tilt is still tracked as a smoothed gravity vector so the
// renderer can apply a tiny body lean to the treat icon.
struct Motion {
    float    gx = 0, gy = 0, gz = 1;
    bool     shakeAbove = false;
    uint32_t shakeFirstPeakMs = 0;
    uint8_t  shakePeakCount = 0;
    uint32_t lastShakeEvent = 0;
};
Motion g_motion;

// Returns true on a confirmed shake event.
bool updateMotion(uint32_t now) {
    float ax, ay, az;
    if (!M5.Imu.getAccelData(&ax, &ay, &az)) return false;
    // Track gravity slowly only for cosmetic body-lean in the renderer —
    // shake detection uses the raw magnitude method below so this EMA
    // doesn't absorb the shake itself.
    constexpr float G_ALPHA = 0.04f;
    g_motion.gx += (ax - g_motion.gx) * G_ALPHA;
    g_motion.gy += (ay - g_motion.gy) * G_ALPHA;
    g_motion.gz += (az - g_motion.gz) * G_ALPHA;
    // Shake metric = |a| − 1 g. Independent of orientation: at rest the
    // total accel is always ~1 g (gravity), so any translational motion
    // deviates the magnitude. Gravity-EMA-subtracted "linear accel" was
    // tried first but the EMA absorbed the motion too fast even at
    // alpha=0.04, especially during sustained shakes.
    float rawMag = sqrtf(ax*ax + ay*ay + az*az);
    float mag    = fabsf(rawMag - 1.0f);
    // Higher noise floor (0.10 g, ~20× sensor noise) so quiet desk
    // vibration / fan rumble while Pip rests does not mark "user is here"
    // and falsely wake the device from auto-sleep — that bounce caused
    // the audible "click" (= Sound::Wake replay) every wake-sleep cycle.
    if (mag > 0.10f) g_pet.lastInteractionMs = now;

    bool nowAbove = mag > pip::SHAKE_PEAK_G;
    bool nowBelow = mag < pip::SHAKE_LOW_HYS;
    if (nowAbove && !g_motion.shakeAbove) {
        if (g_motion.shakePeakCount == 0) g_motion.shakeFirstPeakMs = now;
        g_motion.shakePeakCount++;
        Serial.printf("[pip-imu] peak mag=%.2f count=%u\n",
                      mag, (unsigned)g_motion.shakePeakCount);
    }
    if (nowBelow)      g_motion.shakeAbove = false;
    else if (nowAbove) g_motion.shakeAbove = true;
    if (now - g_motion.shakeFirstPeakMs > pip::SHAKE_WINDOW_MS) {
        g_motion.shakePeakCount = 0;
    }
    if (g_motion.shakePeakCount >= pip::SHAKE_REQUIRED_PEAKS &&
        now - g_motion.lastShakeEvent > pip::SHAKE_COOLDOWN_MS) {
        g_motion.lastShakeEvent = now;
        g_motion.shakePeakCount = 0;
        return true;
    }
    return false;
}

// ── State helpers ─────────────────────────────────────────────────────────
void cyclePage(uint32_t now) {
    uint8_t k = (uint8_t)g_pet.page;
    k = (uint8_t)((k + 1) % pip::kMenuPageCount);
    g_pet.page = (pip::MenuPage)k;
    g_pet.lastInteractionMs = now;
    // Switching away from the Wand page mid-gesture aborts the count —
    // anything else would be confusing UI.
    g_pet.wandPeakCount = 0;
    Serial.printf("[pip] cycle page → %u\n", (unsigned)k);
    pip::play(pip::Sound::TreatCycle);
}

// Throw the currently-selected treat. Only valid on the Apple/Carrot/Bone
// pages; caller checks the page before invoking.
void enterThrow(uint32_t now) {
    uint8_t kind = 0;
    switch (g_pet.page) {
        case pip::MenuPage::Apple:  kind = 0; break;
        case pip::MenuPage::Carrot: kind = 1; break;
        case pip::MenuPage::Bone:   kind = 2; break;
        default: return;   // safety — shouldn't be called for Empty/Wand
    }
    g_pet.state = pip::UiState::Throwing;
    g_pet.throwStartMs = now;
    g_pet.lastInteractionMs = now;
    g_pet.forceSleep = false;
    pip::play(pip::Sound::Throw);
    // Fire the ESP-NOW broadcast — receiver-side dispatch happens on the
    // paired bigger pet via pip_link::tick.
    Serial.printf("[pip] shake → throw treat kind=%u\n", (unsigned)kind);
    pip::link::sendTreat(kind, /*animal=*/0);
}

// Register one peak in the wand-gesture accumulator. Doesn't dispatch
// immediately — the main loop watches for kWandQuietMs of stillness and
// fires sendWand() once the gesture has finished.
void registerWandPeak(uint32_t now) {
    if (g_pet.wandPeakCount < kWandMaxPeaks) g_pet.wandPeakCount++;
    g_pet.wandLastPeakMs = now;
    g_pet.lastInteractionMs = now;
    g_pet.forceSleep = false;
    // Same audio cue as a treat throw — short upward swoosh — so the
    // user has feedback that the peak was registered.
    pip::play(pip::Sound::Throw);
    Serial.printf("[pip] wand peak %u/%u\n",
                  (unsigned)g_pet.wandPeakCount, (unsigned)kWandMaxPeaks);
}

// Called every loop iteration on the Wand page. Once the user has been
// still for kWandQuietMs after the last peak, dispatch the accumulated
// count to the paired bigger pet and reset.
void tickWandDispatch(uint32_t now) {
    if (g_pet.wandPeakCount == 0) return;
    if (now - g_pet.wandLastPeakMs < kWandQuietMs) return;
    Serial.printf("[pip] wand dispatch peaks=%u\n",
                  (unsigned)g_pet.wandPeakCount);
    pip::link::sendWand(g_pet.wandPeakCount, /*animal=*/0);
    g_pet.wandPeakCount = 0;
}

void updateUiState(uint32_t now) {
    // Throwing → Idle once the animation has played out.
    if (g_pet.state == pip::UiState::Throwing &&
        now - g_pet.throwStartMs >= pip::THROW_ANIM_MS) {
        g_pet.state = pip::UiState::Idle;
    }
    // Force-sleep override (BtnB).
    if (g_pet.forceSleep) {
        g_pet.state = pip::UiState::Sleeping;
        return;
    }
    // Auto-sleep after long idle (Throwing animation doesn't count).
    if (g_pet.state == pip::UiState::Idle &&
        now - g_pet.lastInteractionMs >= pip::IDLE_TO_SLEEP_MS) {
        g_pet.state = pip::UiState::Sleeping;
        return;
    }
    // Wake on activity.
    if (g_pet.state == pip::UiState::Sleeping &&
        now - g_pet.lastInteractionMs < pip::IDLE_TO_SLEEP_MS &&
        !g_pet.forceSleep) {
        g_pet.state = pip::UiState::Idle;
        pip::play(pip::Sound::Wake);
    }
}

// ── Buttons ────────────────────────────────────────────────────────────────
//
// BtnA short  : cycle treat (or wake from sleep)
// BtnA long   : force sleep — primary discoverable sleep gesture since
//               the on-screen UI no longer lists button hints
// BtnB short  : also force sleep, kept as a secondary path
//
// M5.BtnA.wasClicked() / wasHold() are mutually exclusive on M5Unified —
// click fires on release if held < hold-threshold; hold fires once when
// the threshold is crossed during the press.
void handleButtons(uint32_t now) {
    if (M5.BtnA.wasHold()) {
        g_pet.forceSleep        = true;
        g_pet.forceSleepStartMs = now;
        pip::play(pip::Sound::Sleepy);
        return;
    }
    if (M5.BtnA.wasClicked()) {
        if (g_pet.forceSleep) {
            // Wake from forced sleep instead of cycling pages — only
            // works while we're still inside the grace window before
            // deep sleep kicks in.
            g_pet.forceSleep        = false;
            g_pet.forceSleepStartMs = 0;
            g_pet.lastInteractionMs = now;
            pip::play(pip::Sound::Wake);
            return;
        }
        cyclePage(now);
    }
    if (M5.BtnB.wasClicked()) {
        g_pet.forceSleep        = true;
        g_pet.forceSleepStartMs = now;
        pip::play(pip::Sound::Sleepy);
    }
}

// ── NVS persistence (just the selected menu page) ────────────────────────
constexpr char kNvsNs[] = "pip";

void loadState() {
    Preferences p;
    if (!p.begin(kNvsNs, true)) return;
    // "pg" replaces the older "trt" (treat-only) key from Phase 2.
    // Default page = Empty (safe carry mode), which protects an unattended
    // Pip from accidentally throwing treats every time the user picks the
    // device up before noticing.
    uint8_t k = p.getUChar("pg", (uint8_t)pip::MenuPage::Empty);
    if (k >= pip::kMenuPageCount) k = (uint8_t)pip::MenuPage::Empty;
    g_pet.page = (pip::MenuPage)k;
    g_pet.language       = p.getUChar("lng", 0);
    if (g_pet.language > 1) g_pet.language = 0;
    g_pet.languageChosen = (p.getUChar("lcs", 0) != 0);
    p.end();
}
void saveState() {
    Preferences p;
    if (!p.begin(kNvsNs, false)) return;
    p.putUChar("pg",  (uint8_t)g_pet.page);
    p.putUChar("lng", g_pet.language);
    p.putUChar("lcs", g_pet.languageChosen ? 1 : 0);
    p.end();
}

// ── Display power (mobile strategy) ────────────────────────────────────────
//
// Two-step idle ramp:
//   30 s → BRIGHT_DIM
//   60 s → BRIGHT_DARK + Sleeping UI (set by updateUiState)
//   3 min → ESP32 deep sleep, woken by the power button
//
// Force-sleep (BtnB / BtnA-hold) skips the ramp: it shows the Zzz UI for
// FORCE_SLEEP_GRACE_MS so an accidental press can be undone with BtnA,
// then drops into deep sleep too.
uint8_t g_brightness = pip::BRIGHT_NORMAL;
uint8_t g_curBrightness = 0xFF;

void enterDeepSleep() {
    Serial.println(F("[pip] entering deep sleep"));
    saveState();
    M5.Speaker.end();
    // M5.Power.deepSleep(0, true) handles M5.Display.sleep(), enables
    // ext0/ext1 wakeup on the board's _wakeupPin (GPIO35 = power button
    // on StickC Plus 2), then calls esp_deep_sleep_start(). 0 = no timer
    // wakeup; the device only comes back via the power button.
    M5.Power.deepSleep(0, true);
}

void applyDisplayPower(uint32_t now) {
    uint32_t idle = now - g_pet.lastInteractionMs;

    bool forceSleepRipe = g_pet.forceSleep
        && (now - g_pet.forceSleepStartMs) >= pip::FORCE_SLEEP_GRACE_MS;
    if (idle >= pip::DEEP_SLEEP_AFTER_MS || forceSleepRipe) {
        enterDeepSleep();
        // unreachable
    }

    uint8_t target;
    if      (idle >= pip::DARK_AFTER_MS) target = pip::BRIGHT_DARK;
    else if (idle >= pip::DIM_AFTER_MS)  target = pip::BRIGHT_DIM;
    else                                 target = g_brightness;

    if (target != g_curBrightness) {
        M5.Display.setBrightness(target);
        g_curBrightness = target;
    }
}

// ── First-run language picker ─────────────────────────────────────────────
//
// Rendered on first boot (before NVS has `lcs=1`). Two flag tiles stacked
// vertically, no text — interaction is BtnA = cycle selection, BtnB =
// confirm. Tiny ↻ and ✓ glyphs near each chip act as button-icon hints
// without leaning on labels.

static void drawFlagDE(M5Canvas& c, int x, int y, int w, int h, bool sel) {
    int stripe = h / 3;
    c.fillRect(x, y,                w, stripe,            c.color565( 25,  25,  25));
    c.fillRect(x, y + stripe,       w, stripe,            c.color565(220,  35,  35));
    c.fillRect(x, y + 2 * stripe,   w, h - 2 * stripe,    c.color565(255, 215,  60));
    if (sel) {
        // Bright accent border when this flag is the current pick.
        c.drawRect(x - 3, y - 3, w + 6, h + 6, c.color565(190, 130, 230));
        c.drawRect(x - 2, y - 2, w + 4, h + 4, c.color565(190, 130, 230));
    } else {
        c.drawRect(x, y, w, h, c.color565(120, 120, 140));
    }
}

static void drawFlagEN(M5Canvas& c, int x, int y, int w, int h, bool sel) {
    // St George's cross — white background, big red cross. Recognisable
    // and trivial to draw, no Union-Jack diagonals to mangle.
    c.fillRect(x, y, w, h, c.color565(245, 245, 245));
    int armW = h / 4;          // cross arm thickness
    c.fillRect(x,                y + (h - armW) / 2, w, armW, c.color565(220, 35, 35));
    c.fillRect(x + (w - armW) / 2, y,                armW, h, c.color565(220, 35, 35));
    if (sel) {
        c.drawRect(x - 3, y - 3, w + 6, h + 6, c.color565(190, 130, 230));
        c.drawRect(x - 2, y - 2, w + 4, h + 4, c.color565(190, 130, 230));
    } else {
        c.drawRect(x, y, w, h, c.color565(120, 120, 140));
    }
}

// Returns 0 (DE) or 1 (EN) once BtnB is pressed. Blocks until the user
// confirms — there's no good default for a brand-new device.
uint8_t runLangPicker() {
    constexpr int CW = TARGET_DISPLAY_W;
    constexpr int FLAG_W = 105, FLAG_H = 65;
    constexpr int FLAG_X = (CW - FLAG_W) / 2;
    constexpr int FLAG_Y_DE = 38;
    constexpr int FLAG_Y_EN = FLAG_Y_DE + FLAG_H + 28;

    uint8_t selected = 0;   // start on DE
    Serial.println(F("[pip] first-run language picker"));

    while (true) {
        M5.update();
        uint32_t now = millis();
        if (M5.BtnA.wasClicked()) {
            selected ^= 1;
            pip::play(pip::Sound::TreatCycle);
        }
        if (M5.BtnB.wasClicked()) {
            pip::play(pip::Sound::Greet);
            return selected;
        }

        g_canvas.fillSprite(g_canvas.color565(245, 220, 235));
        // Small "PIP" wordmark up top so the user knows the device is alive.
        g_canvas.setTextDatum(top_center);
        g_canvas.setTextSize(2);
        g_canvas.setTextColor(g_canvas.color565(190, 130, 230));
        g_canvas.drawString("PIP", CW / 2, 8);
        g_canvas.setTextDatum(top_left);

        drawFlagDE(g_canvas, FLAG_X, FLAG_Y_DE, FLAG_W, FLAG_H, selected == 0);
        drawFlagEN(g_canvas, FLAG_X, FLAG_Y_EN, FLAG_W, FLAG_H, selected == 1);

        g_canvas.pushSprite(0, 0);
        delay(33);
        (void)now;
    }
}

// ── Splash ─────────────────────────────────────────────────────────────────
void runSplash() {
    uint32_t start = millis();
    pip::play(pip::Sound::SplashHi);
    while (millis() - start < 2500) {
        g_canvas.fillSprite(g_canvas.color565(20, 8, 36));
        pip::drawSplash(g_canvas, millis(), start);
        g_canvas.pushSprite(0, 0);
        delay(33);
    }
}

}  // namespace

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println(F("[pip] BOOT 1 setup() entered"));

    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(0);   // portrait, 135 wide × 240 tall

    // Brownout protection: if the battery is too weak for full operation
    // we'd otherwise hit a reboot loop (display init pulls voltage below
    // the brownout threshold, ESP resets, repeats — battery drains even
    // faster). Show a brief notice and deep-sleep until USB is plugged.
    {
        int batt = M5.Power.getBatteryLevel();
        bool charging = M5.Power.isCharging();
        Serial.printf("[pip] battery=%d%% charging=%d\n", batt, (int)charging);
        if (batt >= 0 && batt < pip::LOW_BATTERY_PCT && !charging) {
            M5.Display.setBrightness(40);
            M5.Display.fillScreen(0);
            M5.Display.setTextDatum(middle_center);
            M5.Display.setTextColor(M5.Display.color565(255, 80, 80));
            M5.Display.setTextSize(1);
            int cx = M5.Display.width() / 2;
            int cy = M5.Display.height() / 2;
            M5.Display.drawString("LOW BATTERY", cx, cy - 12);
            M5.Display.drawString("Please charge",  cx, cy + 4);
            delay(2500);
            M5.Display.sleep();
            M5.Display.setBrightness(0);
            // 0 = unlimited — wakes on USB or power button.
            M5.Power.deepSleep(0);
        }
    }

    M5.Display.setBrightness(pip::BRIGHT_NORMAL);
    g_canvas.createSprite(TARGET_DISPLAY_W, TARGET_DISPLAY_H);

    Serial.println(F("[pip] BOOT 2 display + canvas ready"));

    loadState();
    Serial.printf("[pip] state loaded: page=%u lang=%u chosen=%d\n",
                  (unsigned)g_pet.page,
                  (unsigned)g_pet.language,
                  (int)g_pet.languageChosen);

    // First-run language picker — runs once, before splash. The chosen
    // language is persisted so this never blocks subsequent boots. All
    // user-facing text on Pip reads from g_lang via tr(Str::…) so the
    // selection takes effect immediately.
    if (!g_pet.languageChosen) {
        g_pet.language       = runLangPicker();
        g_pet.languageChosen = true;
        saveState();
        Serial.printf("[pip] language picked: %u\n", (unsigned)g_pet.language);
    }
    g_lang = g_pet.language;

    runSplash();
    Serial.println(F("[pip] BOOT 3 splash done"));

    // CPU stays at 240 MHz. Pip is normally only briefly on (short press →
    // throw → press to sleep), so the ~30 mA we'd save at 80 MHz isn't
    // worth the IMU-polling-frequency hit on shake detection.
    Serial.printf("[pip] cpu running at %u MHz\n",
                  (unsigned)getCpuFrequencyMhz());

    uint32_t now = millis();
    g_pet.lastInteractionMs = now;
    g_pet.lastSavedMs       = now;
    pip::play(pip::Sound::Greet);
}

void loop() {
    M5.update();
    uint32_t now = millis();

    // Input.
    bool shaken = updateMotion(now);
    handleButtons(now);

    // Shake handling forks by the current menu page:
    //   Empty → ignored (safe carry mode, no false throws in the pocket)
    //   Apple/Carrot/Bone → throw the matching treat
    //   Wand → register a peak; the wand-dispatch tick below fires the
    //          accumulated count after kWandQuietMs of stillness.
    if (shaken) {
        switch (g_pet.page) {
            case pip::MenuPage::Empty:                                  break;
            case pip::MenuPage::Wand:   registerWandPeak(now);          break;
            case pip::MenuPage::Apple:
            case pip::MenuPage::Carrot:
            case pip::MenuPage::Bone:   enterThrow(now);                break;
        }
    }
    if (g_pet.page == pip::MenuPage::Wand) tickWandDispatch(now);

    updateUiState(now);

    // Save NVS every 60 s.
    if (now - g_pet.lastSavedMs >= 60000) {
        g_pet.lastSavedMs = now;
        saveState();
    }

    // Display power (auto-dim, then deep sleep).
    applyDisplayPower(now);

    // Render.
    pip::PipView v{};
    v.now_ms        = now;
    v.state         = g_pet.state;
    v.page          = g_pet.page;
    v.throwStartMs  = g_pet.throwStartMs;
    v.wandPeakCount = g_pet.wandPeakCount;
    v.tiltX         = g_motion.gx;
    v.tiltY         = g_motion.gy;
    v.batteryPct    = M5.Power.getBatteryLevel();
    v.charging      = M5.Power.isCharging();
    v.language      = g_lang;
    pip::drawPip(g_canvas, v);
    g_canvas.pushSprite(0, 0);

    // 16 ms (60 fps) gives the shake detector twice as many polling
    // chances per second compared to 33 ms — needed because a sharp wrist
    // flick produces ~50 ms peaks and at 30 fps we sometimes miss them.
    delay(16);
}
