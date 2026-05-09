#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>      // settimeofday() im RTC-Fallback-Pfad

#include "target_caps.h"
#include "face.h"
#if TARGET_HAS_CAMERA
#include "face_detect.h"
#endif
#include "i18n.h"
#include "net.h"
#include "pet_state.h"
#if TARGET_HAS_CAMERA
#include "photo_store.h"
#endif
#include "sounds/sounds.h"
#if TARGET_HAS_LLM
#include "voice_pipeline.h"
#endif
#if TARGET_HAS_WIFI
#include "webradio.h"
#include "pip_link.h"
#endif
#include "screenshot.h"
#include "wifi_config.h"
#include "world.h"

#if TARGET_HAS_LLM
// ─── Voice — System-Prompt ──────────────────────────────────────────────────

static const char* SYSTEM_PROMPT =
    "You are a tag classifier for a virtual pet. Output 1 to 3 words from "
    "this exact list, comma-separated, lowercase, in the order spoken:\n"
    "eat, pet, love, laugh, sleep, wake, greet, sad, startle, sing, dance, "
    "ball, mouse, rattle, butterfly, plush, movie, game, internet, social, "
    "friends, radio, idle.\n"
    "Input is German or English. German hints: lieb/herz=love, "
    "streicheln=pet, futter/essen=eat, tanzen=dance, schlaf=sleep, "
    "lachen=laugh, radio/sender=radio.\n"
    "If the input does not clearly match any action, output exactly: idle\n"
    "Never output code, explanations, sentences, or anything outside the list.\n"
    "Examples:\n"
    "Input: streichel mich -> pet\n"
    "Input: mach radio an -> radio\n"
    "Input: ich will tanzen und essen -> dance, eat\n"
    "Input: keine ahnung was -> idle";

// Voice setup runs on a FreeRTOS task on Core 0 so the splash animation
// on Core 1 keeps running in the meantime.
static volatile bool g_voiceSetupDone = false;
static volatile bool g_voiceSetupOk   = false;
#endif  // TARGET_HAS_LLM

// ─── Tuning ──────────────────────────────────────────────────────────────────

// Shake = positive "play" gesture now, so thresholds are friendly.
constexpr float    SHAKE_PEAK_G          = 1.0f;
constexpr uint32_t SHAKE_WINDOW_MS       = 700;
constexpr uint8_t  SHAKE_REQUIRED_PEAKS  = 2;
constexpr uint32_t SHAKE_COOLDOWN_MS     = 1500;

// Vertical (up-down) shake → rain shower. The dominance ratio is strict
// (vertical must exceed horizontal) so brief vertical bumps inside a
// side-to-side shake don't count — only motion the user clearly intended
// as up-down qualifies. Long cooldown stops accidental re-triggers.
constexpr float    VSHAKE_PEAK_G         = 1.0f;
constexpr float    VSHAKE_LOW_HYS        = 0.5f;
constexpr float    VSHAKE_DOMINANCE      = 1.0f;     // vAbs > horizMag
constexpr uint32_t VSHAKE_WINDOW_MS      = 800;
constexpr uint8_t  VSHAKE_REQUIRED_PEAKS = 2;
constexpr uint32_t VSHAKE_COOLDOWN_MS    = 6000;     // long, so rain isn't spammed

// Stroke detection: only counts *rising edges* of motion peaks (with
// hysteresis), so a single slow tilt — which produces one slow rise in
// linear acceleration — only ever counts as one peak. Active back-and-forth
// petting produces several peaks within the window.
constexpr float    STROKE_PEAK_G         = 0.20f;
constexpr float    STROKE_LOW_HYSTERESIS = 0.10f;   // must dip below this between peaks
constexpr uint32_t STROKE_WINDOW_MS      = 1500;
constexpr uint8_t  STROKE_REQUIRED_PEAKS = 4;
constexpr uint32_t STROKE_COOLDOWN_MS    = 1200;
constexpr float    SHAKE_LOW_HYSTERESIS  = 0.5f;

constexpr uint8_t  BRIGHTNESS_LEVELS[4] = { 64, 128, 191, 255 };

constexpr uint32_t SAVE_INTERVAL_MS    = 60000;
constexpr uint32_t TOUCH_ZONE_COOLDOWN_MS = 300;
constexpr uint32_t TOUCH_EYE_COOLDOWN_MS  = 600;

constexpr uint32_t SLEEP_REGEN_FLOAT_MS = 10000;     // one Z every 10 s

// Pet face center (kept in sync with face.cpp constants)
constexpr int FACE_CX = 160;
constexpr int FACE_CY = 128;

// ─── Motion classifier (unchanged) ───────────────────────────────────────────

struct Motion {
  float gx = 0, gy = 0, gz = 1;
  // Raw accel magnitude this frame (|a| in g). Different from the
  // gravity-EMA magnitude (which sits at ~1 g) and from the linear-accel
  // magnitude (|a − g|, which sits at ~0 g at rest). |a_raw| is the
  // metric used by the sport-jump detector: dips to ~0 g during freefall,
  // spikes above ~1.4 g on landing — orientation-independent.
  float rawMag = 1.0f;
  uint32_t lastInteractionMs = 0;

  // Shake — high-magnitude rising-edge peaks (mostly horizontal)
  bool     shakeAbove = false;
  uint32_t lastShakeEvent = 0;
  uint32_t shakeFirstPeakMs = 0;
  uint8_t  shakePeakCount = 0;

  // Stroke — lower-magnitude rising-edge peaks (oscillation = petting)
  bool     strokeAbove = false;
  uint32_t lastStrokeEvent = 0;
  uint32_t strokeFirstPeakMs = 0;
  uint8_t  strokePeakCount = 0;

  // Vertical shake — peaks of linear acc projected onto the world-vertical
  // (i.e., gravity direction). Triggers rain shower regardless of how the
  // device is held.
  bool     vShakeAbove = false;
  uint32_t lastVShakeEvent = 0;
  uint32_t vShakeFirstPeakMs = 0;
  uint8_t  vShakePeakCount = 0;
};
static Motion g_motion;

static int updateMotion(uint32_t now) {
  float ax, ay, az;
  if (!M5.Imu.getAccelData(&ax, &ay, &az)) return 0;

  // Faster gravity tracking so rocking gestures (~1 Hz) actually push gx.
  // Was 0.02 (cutoff ~0.1 Hz) which barely tracked rocking at all.
  constexpr float G_ALPHA = 0.10f;
  g_motion.gx += (ax - g_motion.gx) * G_ALPHA;
  g_motion.gy += (ay - g_motion.gy) * G_ALPHA;
  g_motion.gz += (az - g_motion.gz) * G_ALPHA;

  float lx = ax - g_motion.gx;
  float ly = ay - g_motion.gy;
  float lz = az - g_motion.gz;
  float mag = sqrtf(lx*lx + ly*ly + lz*lz);
  // Raw-accel magnitude — published for sport-jump detection (freefall
  // dip + landing spike). Don't use the EMA-magnitude here: the EMA is
  // unit-magnitude gravity and never deviates enough to cross thresholds.
  g_motion.rawMag = sqrtf(ax * ax + ay * ay + az * az);

  // Any meaningful movement keeps the pet awake (without spawning hearts).
  if (mag > 0.05f) g_motion.lastInteractionMs = now;

  // ── Vertical shake → rain shower ─────────────────────────────────────
  // Checked BEFORE the generic-shake branch so an up-and-down jolt that
  // also happens to clear the play-magnitude threshold reliably routes to
  // the shower. Project the linear acc onto the unit gravity direction —
  // |vAcc| spikes when the device is moved up/down in world coordinates
  // regardless of grip orientation.
  float gMag = sqrtf(g_motion.gx * g_motion.gx +
                     g_motion.gy * g_motion.gy +
                     g_motion.gz * g_motion.gz);
  if (gMag < 0.5f) gMag = 1.0f;
  float vAcc = (lx * g_motion.gx + ly * g_motion.gy + lz * g_motion.gz) / gMag;
  float vAccAbs = fabsf(vAcc);
  float horizMag2 = mag * mag - vAcc * vAcc;
  float horizMag  = (horizMag2 > 0.0f) ? sqrtf(horizMag2) : 0.0f;
  // Looser dominance criterion (60 % of the horizontal share is enough)
  // so realistic up-down motions with normal hand-grip jitter qualify.
  bool verticalDominant = vAccAbs > horizMag * VSHAKE_DOMINANCE;

  if (vAccAbs > VSHAKE_PEAK_G && verticalDominant && !g_motion.vShakeAbove) {
    g_motion.vShakeAbove = true;
    if (now - g_motion.lastVShakeEvent > VSHAKE_COOLDOWN_MS) {
      if (g_motion.vShakePeakCount == 0 ||
          now - g_motion.vShakeFirstPeakMs > VSHAKE_WINDOW_MS) {
        g_motion.vShakeFirstPeakMs = now;
        g_motion.vShakePeakCount   = 1;
      } else {
        g_motion.vShakePeakCount++;
      }
      if (g_motion.vShakePeakCount >= VSHAKE_REQUIRED_PEAKS) {
        g_motion.lastVShakeEvent = now;
        g_motion.vShakePeakCount = 0;
        g_motion.shakePeakCount  = 0;   // also clear in-flight play counter
        g_motion.strokePeakCount = 0;
        return 3;       // vertical shake → shower
      }
    }
  } else if (vAccAbs < VSHAKE_LOW_HYS) {
    g_motion.vShakeAbove = false;
  }
  if (g_motion.vShakePeakCount > 0 &&
      now - g_motion.vShakeFirstPeakMs > VSHAKE_WINDOW_MS) {
    g_motion.vShakePeakCount = 0;
  }

  // ── Horizontal/general shake → play ──────────────────────────────────
  // Falls through here only when the motion wasn't already classified as
  // a vertical shower trigger, so a real sideways shake fires play and a
  // genuinely vertical jolt fires the shower instead.
  if (mag > SHAKE_PEAK_G && !g_motion.shakeAbove) {
    g_motion.shakeAbove = true;
    if (now - g_motion.lastShakeEvent > SHAKE_COOLDOWN_MS) {
      if (g_motion.shakePeakCount == 0 ||
          now - g_motion.shakeFirstPeakMs > SHAKE_WINDOW_MS) {
        g_motion.shakeFirstPeakMs = now;
        g_motion.shakePeakCount   = 1;
      } else {
        g_motion.shakePeakCount++;
      }
      if (g_motion.shakePeakCount >= SHAKE_REQUIRED_PEAKS) {
        g_motion.lastShakeEvent = now;
        g_motion.shakePeakCount = 0;
        g_motion.strokePeakCount = 0;
        return 2;
      }
    }
  } else if (mag < SHAKE_LOW_HYSTERESIS) {
    g_motion.shakeAbove = false;
  }
  if (g_motion.shakePeakCount > 0 &&
      now - g_motion.shakeFirstPeakMs > SHAKE_WINDOW_MS) {
    g_motion.shakePeakCount = 0;
  }

  // ── Stroke: rising edges of mag > STROKE_PEAK_G with hysteresis ──
  if (mag > STROKE_PEAK_G && !g_motion.strokeAbove) {
    g_motion.strokeAbove = true;
    if (now - g_motion.lastStrokeEvent > STROKE_COOLDOWN_MS) {
      if (g_motion.strokePeakCount == 0 ||
          now - g_motion.strokeFirstPeakMs > STROKE_WINDOW_MS) {
        g_motion.strokeFirstPeakMs = now;
        g_motion.strokePeakCount   = 1;
      } else {
        g_motion.strokePeakCount++;
      }
      if (g_motion.strokePeakCount >= STROKE_REQUIRED_PEAKS) {
        g_motion.lastStrokeEvent = now;
        g_motion.strokePeakCount = 0;
        return 1;
      }
    }
  } else if (mag < STROKE_LOW_HYSTERESIS) {
    g_motion.strokeAbove = false;
  }
  if (g_motion.strokePeakCount > 0 &&
      now - g_motion.strokeFirstPeakMs > STROKE_WINDOW_MS) {
    g_motion.strokePeakCount = 0;
  }

  return 0;
}

// ─── Pet state ───────────────────────────────────────────────────────────────

struct Pet {
  Persisted persisted;
  Face     face = Face::Idle;
  uint32_t enteredFaceAtMs = 0;
  uint32_t flashUntilMs = 0;
  Face     flashFace = Face::Idle;

  uint32_t lastDecayMs = 0;
  uint32_t lastSavedMs = 0;
  uint32_t lastPurrMs = 0;
  uint32_t lastSnoreMs = 0;
  uint32_t lastWakeMs = 0;
  uint32_t lastTouchZoneMs[9] = {0};   // indexed by TouchZone (incl. Ears)

  uint8_t  recentStrokes = 0;
  uint32_t recentStrokesResetMs = 0;

  Gaze        gaze;
  Tilt        tilt;
  Wander      wander;
  Micro       micro;
  BubbleState bubble;
  FloatPool   floats;

  // Per-need change times for bar pulse
  uint32_t happLastChangeMs = 0;
  uint32_t engLastChangeMs  = 0;
  uint32_t fullLastChangeMs = 0;

  uint32_t lastSleepRegenMs = 0;
  uint32_t lastWanderFunSoundMs = 0;
  uint32_t lastExcitedSoundMs = 0;
  uint32_t lastLoveSoundMs    = 0;
  uint32_t lastSadSoundMs     = 0;
  // Set when the user tries to feed but has nothing in inventory. While
  // active (now < this), the basket button pulses to point them at it.
  uint32_t basketHintUntilMs  = 0;
  // Rain shower (vertical-shake reaction). 0 = no rain; otherwise the
  // start timestamp. Phases derive from elapsed time.
  uint32_t rainStartMs = 0;

  // Set by BtnC ("send to sleep"). Cleared by any explicit wake action.
  // Overrides the energy/inactivity-based face logic so the pet stays asleep
  // even if the user is still holding the device.
  bool     forceSleep = false;

  // Listen mode — ear-tap toggles. Mic streams audio levels which drive
  // the dance animation; sound playback is suppressed.
  bool     listenMode = false;
  uint32_t lastListenLoveMs = 0;
  uint32_t lastListenPeakMs = 0;     // for 90 s silence-based auto-exit

  // Voice listening: active from wake word until the LLM answers. Pet opens
  // its eyes (overriding Sleeping), shows a listening / thinking expression
  // (speaking face with mouth animation) and suppresses snore/yawn.
  // Safety timeout: after 30 s without an answer the loop cleans up.
  bool     voiceListening      = false;
  uint32_t voiceListeningUntilMs = 0;

  // Last time the user touched the screen — drives modal auto-close
  // (2 min without input → exit modal). Distinct from g_motion's
  // lastInteractionMs which also gets bumped by IMU motion.
  uint32_t lastTouchMs = 0;
  int      micLevel = 0;
  float    micLevelSmoothed = 0.0f;

  // Upside-down detection — pet complains when the screen has been
  // face-down for a moment. lastComplaintMs throttles the periodic moans.
  uint32_t upsideDownSinceMs = 0;     // 0 = currently right-side-up
  uint32_t lastComplaintMs   = 0;

  // Standing-orientation tracking — separate from face-down (which is
  // the device lying on its back/face on a table). Detects whether the
  // device is currently propped up on its bottom / top / left / right
  // edge, with hysteresis + 500 ms stability so brief flips don't
  // trigger reactions.
  uint8_t  curStand           = 0;     // index into kStandReactCooldownMs
  uint32_t standStableSinceMs = 0;
  uint32_t lastStandReactMs[5] = {0};

  // Brief wobble animation triggered by giggle on side-edge.
  uint32_t sideShakeStartMs = 0;

  // Somersault — set when the user draws a circle on the screen. The
  // renderer sees this and arches the pet over a parabolic / loop path
  // for ~800 ms. 0 = idle.
  uint32_t somersaultStartMs = 0;

  // Two-finger spread / pinch-out: while two fingers are down on the pet
  // view, stretch the pet between them via rubber-band lines, then on
  // release trigger a snap-back wobble.
  bool     spreadActive    = false;
  int16_t  spreadF0X = 0, spreadF0Y = 0;   // finger 0 position
  int16_t  spreadF1X = 0, spreadF1Y = 0;   // finger 1 position
  float    spreadStartDist = 0.0f;         // distance between fingers when 2-touch began
  float    spreadAmount    = 0.0f;         // 0..1 stretch ratio (clamped)
  uint32_t snapStartMs     = 0;            // damped horizontal wobble after release

  // Two-finger ambiguous-phase tracking. Both Spread and hand-warming begin
  // with 2 fingers down. The first ~800 ms watch which way they go (apart
  // → spread, motionless → hold) and lock to one mode.
  uint32_t holdStartMs       = 0;          // 0 = no 2-finger contact in flight
  int16_t  holdF0X = 0, holdF0Y = 0;       // 2-touch initial position, finger 0
  int16_t  holdF1X = 0, holdF1Y = 0;       // initial position, finger 1
  bool     holdLocked        = false;      // committed to hold (snuggle path)

  // Snuggle (hand warming): set when the steady 2-finger hold passes its
  // 3 s warm-up. Drives a slow purr loop, hearts drifting up from the body
  // and a gentle needs regen. After 25 s the pet falls into deep sleep.
  uint32_t snuggleStartMs       = 0;
  bool     snuggleSleepingPhase = false;
  uint32_t lastSnugglePurrMs    = 0;
  uint32_t lastSnuggleHeartMs   = 0;
  uint32_t lastSnuggleNeedsMs   = 0;
  uint32_t lastSnuggleEndMs     = 0;       // anchor for 4 s post-release cooldown

  // Rapid-tap burst → hop chain. While the user taps the pet quickly the
  // counter goes up; once they pause for >500 ms with 3..10 taps banked
  // the pet hops that many times in a row, laughing on each hop.
  uint8_t  tapBurstCount   = 0;
  uint32_t tapBurstLastMs  = 0;
  uint8_t  hopChainCount   = 0;          // 0 = idle, otherwise total hops planned
  uint32_t hopChainStartMs = 0;
  int8_t   hopChainLastIdx = -1;         // last hop whose effects already fired

  // Singing — triggered by the upright + left + right tilt sequence. Plays
  // singing.wav (8 s) then listens via mic for 5 s for applause and rewards
  // a loud peak with a happy reaction.
  uint8_t  singSeqStep         = 0;      // 0 = idle, 1 = saw upright + left tilt
  uint32_t singSeqLastMs       = 0;
  uint32_t lastUprightMs       = 0;      // last time gy > 0.6 (anchor for the sequence)
  bool     wasTiltLeft         = false;  // edge-detection state for gx
  bool     wasTiltRight        = false;
  uint32_t singStartMs         = 0;      // 0 = not singing
  uint32_t lastSingHeartMs     = 0;      // for periodic note/heart spawns
  uint32_t applauseListenStartMs = 0;    // 0 = not listening for applause
  int      applausePeak        = 0;      // max int16 sample seen during listen
  uint32_t lastApplauseHeartMs = 0;      // throttle for live heart spawning

  // Circle-gesture trail: ring buffer of the last few touch samples while
  // the user is dragging on the pet view. Cleared on touch release. Buffer
  // size kCircleBufN is a file-scope constant declared just below the
  // PetState definition.
  int16_t  circleX[16] = {0};
  int16_t  circleY[16] = {0};
  uint8_t  circleCount = 0;     // 0..kCircleBufN
  uint8_t  circleHead  = 0;     // next write index
  uint32_t lastCircleMs = 0;    // cooldown anchor

  // Hygiene — piles in the landscape, plus a separate "dirt level" on the
  // pet itself. The two are decoupled so cleanup-mode (sweep piles) and
  // water shower (wash dirt) are distinct actions.
  static constexpr int MAX_PILES = 12;
  int16_t  pileX[MAX_PILES] = {0};
  int16_t  pileY[MAX_PILES] = {0};
  uint8_t  pileCount = 0;
  uint8_t  feedCounter = 0;          // every 3rd feed → drop a pile
  uint8_t  dirtLevel = 0;            // 0..100, accumulates when piles are many
  uint32_t lastDirtTickMs = 0;
  uint32_t lastDirtPenaltyMs = 0;

  // Cleaning mode — modal screen for sweeping piles away.
  bool     cleaningMode = false;
  uint32_t cleanedAtMs = 0;          // timestamp when count hit 0 (for celebration)

  // Foraging mode — modal screen for collecting food (apples / berries /
  // fish). Inventory counts live in `persisted`, the in-flight items live in
  // a separate g_forage struct since they are session-only.
  bool     foragingMode = false;

  // Toy-select mode — modal screen for choosing which toy is used during
  // play (triggered by horizontal shake). Selection persists.
  bool     toySelectMode = false;

  // Media consumption (TV / games / internet). Active session = pet glued
  // to a screen — square-eyed render + accelerated need decay until the
  // user hits the media button again to stop. Session-only (not persisted)
  // so a reboot leaves the pet free.
  Media    mediaActive   = Media::None;
  bool     mediaSelectMode = false;
  uint32_t mediaStartMs  = 0;
  uint32_t lastMediaHapDecayMs  = 0;
  uint32_t lastMediaEngDecayMs  = 0;
  uint32_t lastMediaFullDecayMs = 0;
  uint32_t lastMediaSoundMs     = 0;

  // Photo + gallery (cores3/visu only). cameraMode = live preview with
  // shutter; galleryMode = thumb grid or full-size view (galleryViewing).
  bool     cameraMode             = false;
  uint32_t cameraEnteredMs        = 0;
  bool     cameraFlashActive      = false;
  uint32_t cameraFlashStartMs     = 0;
  bool     cameraSaveOverlay      = false;
  uint32_t cameraSaveOverlayUntilMs = 0;
  uint32_t lastCameraRewardMs     = 0;     // 60 s cooldown against photo spam
  bool     galleryMode            = false;
  bool     galleryViewing         = false; // false = thumb grid, true = full size
  uint8_t  galleryIdx             = 0;     // display index (0 = newest)
  bool     galleryDeleteConfirm   = false;
  uint32_t lastGalleryRewardMs    = 0;     // 60 s cooldown against spam

  // Travel-select mode — modal screen for choosing which scene the pet is
  // in. The selected scene index lives in `persisted.selectedScene`.
  bool     travelSelectMode = false;

  // 8-second cinematic transition between scenes. Triggered after a
  // successful scene change in handleTouchTravelSelect. Session-only.
  bool     travelTransitionMode    = false;
  uint32_t travelTransitionStartMs = 0;
  Scene    travelTransitionTo      = Scene::Meadow;

  // ── Parental play-time limit ────────────────────────────────────────────
  // Each boot starts a fresh 30-minute play session. When the limit hits
  // we wait for a safe moment (no active mini-game / foraging / etc.),
  // play a bedtime sequence, persist a 30-min lockout, then power off.
  // Manual shutdown via the PWR button uses the same sequence but skips
  // the lockout (commitsLockout=false) so adults can power down without
  // being locked out.
  uint32_t sessionStartMs       = 0;
  bool     shutdownPending      = false;
  bool     shutdownCommitsLockout = false;
  uint32_t shutdownAnnouncedAtMs = 0;     // 0 → bedtime sequence not started yet

  // Activity (mini-game) mode — when on, the play screen replaces the pet
  // view. Game state itself lives in g_activity.
  bool     activityMode = false;

  // Toy play animation — driven by the shake handler. Animation runs in
  // face.cpp, but we own the start time + bored flag here.
  uint32_t toyPlayStartMs = 0;
  uint32_t toyPlayDurMs   = 0;
  Toy      toyPlayWhich   = Toy::Ball;
  bool     toyPlayBored   = false;
  bool     toyPlayFromLeft = true;

  // Boredom babble — fires Sound::GooGoo at random intervals when the pet
  // has been idle for a while.
  uint32_t nextBabbleMs = 0;

  // USB-power tracking — pet gets a small happiness bump when plugged in
  // and a slow trickle while charging. Session-only.
  bool     prevCharging = false;
  uint32_t lastChargeBoostMs = 0;

  // Random weather per scene. Session-only — re-randomized when the user
  // travels to a different scene or after the weather-cycle interval.
  Weather  weather          = Weather::Sunny;

  // Sport mode — pet-view button → exercise picker → workout → done.
  // exercise: 0=Squat, 1=Jump, 2=Yoga.
  bool     sportSelectMode  = false;
  bool     sportWorkoutMode = false;
  bool     sportDoneMode    = false;
  uint8_t  sportExercise    = 0;
  uint8_t  sportPhase       = 0;     // 0=intro/safety, 1=active
  uint32_t sportPhaseStartMs = 0;
  uint8_t  sportRepCount    = 0;
  uint8_t  sportTargetReps  = 10;
  uint8_t  sportMotionPhase = 0;     // squat: 0=up,1=down ; jump/yoga: state
  uint32_t sportLastRepMs   = 0;     // debounce
  uint32_t sportYogaStillStartMs = 0;
  uint32_t sportYogaLastTickMs   = 0;

  // Friends-mode (ESP-NOW peer discovery via Media → Freunde). When
  // active, takes over the canvas + renders FriendsView; the actual
  // networking lives in net.cpp.
  bool     friendsMode = false;
  uint32_t friendsModeStartMs = 0;     // wall-clock for the 60-s session timeout
  uint8_t  friendsSentCount   = 0;     // 0..5 — how many items the user sent
  uint32_t friendsLastTapMs   = 0;     // 400 ms tap throttle
  uint8_t  friendsLastSentKind = 0;    // last item kind for the UI pulse
  // bothDone trailing window: timestamp when bothDone was first observed.
  // We keep the Sending state alive for ~2 s after that so the partner
  // still hears our Done (asymmetric RF: we may have caught their Done
  // while they missed ours, and friendsEnd() would silence us).
  uint32_t friendsBothDoneAtMs = 0;
  // Post-session playback — drives the per-item reaction sequence in the
  // pet view after the sending screen closes.
  bool     friendsPlaybackActive = false;
  uint8_t  friendsPlaybackIdx    = 0;
  uint32_t friendsPlaybackNextMs = 0;
  bool     friendsPlaybackPlayedAny = false;
  uint8_t  friendsPlaybackKind   = 0;       // currently displayed item kind
  uint8_t  friendsPlaybackAnimal = 0;
  uint32_t friendsPlaybackItemStartMs = 0;
  bool     friendsCelebrationDone = false;
  Scene    weatherScene     = Scene::Meadow;   // last scene we picked weather for
  uint32_t nextWeatherChangeMs = 0;
  uint32_t lastSunshineBoostMs = 0;

  // ── Timer ──────────────────────────────────────────────────────────────
  // Session-only countdown. The user enters via tapping the clock area on
  // the pet view; it can also be stopped or reconfigured from there.
  bool     timerActive      = false;
  uint32_t timerEndMs       = 0;
  uint32_t timerStartMs     = 0;     // for displaying total duration if needed
  uint32_t timerDurationMs  = 0;

  // Expiry animation — pet hops, plays Goo-Goo a few times, vibrates once.
  bool     timerExpiring    = false;
  uint32_t timerExpiredAtMs = 0;
  uint8_t  timerExpiredPlays = 0;
  uint32_t lastTimerExpiredSoundMs = 0;

  // Modal screens
  bool     timerScreenMode  = false;     // preset / running screen
  bool     timerCustomEdit  = false;     // tap-to-increment custom-time editor
  uint8_t  customMinutes    = 5;
  uint8_t  customSeconds    = 0;
};
static Pet g_pet;

static uint8_t g_volume          = 51;     // ~20 %
static uint8_t g_brightnessLevel = 3;

struct Settings {
  bool    open = false;
  uint8_t page = 0;            // 0 = daily controls, 1 = lang/help/credits/reset, 2 = wifi
  bool    timeEditing = false;
  bool    animalSelecting = false;
  bool    helpOpen = false;
  uint8_t helpPage = 0;
  bool    creditsOpen = false;
  bool    langOpen = false;
  bool    resetConfirmOpen     = false;   // modal "Wirklich zuruecksetzen?"
  bool    wifiResetConfirmOpen = false;   // modal "WLAN-Daten loeschen?"
  bool    wifiSetupOpen        = false;   // captive-portal setup view active
  bool    parentsHelpOpen      = false;   // dedicated parents-help screen
  bool    parentServerOn       = false;   // toggle state for the row UI
  bool    parentServerPageOpen = false;   // dedicated parent-server sub-page
  bool    locationPageOpen     = false;   // dedicated Standort sub-page
  bool    locationRefreshing   = false;   // ip-api refresh in flight
  uint32_t locationRefreshAtMs = 0;       // when the refresh started
};
// g_lang lives in i18n.cpp (extern in i18n.h). It's synced from
// g_pet.persisted.language at boot and whenever the user changes it.
static Settings g_settings;

// Adjust hours/minutes on the RTC (with wrap-around). Seconds preserved.
static void adjustHour(int delta) {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) return;
  int h = ((int)dt.time.hours + delta + 24) % 24;
  dt.time.hours = (uint8_t)h;
  M5.Rtc.setDateTime(&dt);
}
static void adjustMinute(int delta) {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) return;
  int m = ((int)dt.time.minutes + delta + 60) % 60;
  dt.time.minutes = (uint8_t)m;
  M5.Rtc.setDateTime(&dt);
}

static M5Canvas g_canvas(&M5.Display);
static uint32_t g_lastDrawMs = 0;

// ─── Need-change helpers ─────────────────────────────────────────────────────

// When a need is already high, positive boosts have diminished effect.
// Negative deltas pass through unchanged so penalties always sting.
static int diminish(int delta, uint8_t currentValue) {
  if (delta > 1 && currentValue >= 80) {
    return (delta + 1) / 2;   // round up so +2 stays +1, +3 → +2, +8 → +4
  }
  return delta;
}

static void changeHappiness(int delta, uint32_t now) {
  uint8_t cur = g_pet.persisted.needs.happiness;
  if (delta > 0) delta = diminish(delta, cur);
  int v = (int)cur + delta;
  if (v > 100) v = 100; if (v < 0) v = 0;
  if (v != (int)cur) {
    g_pet.persisted.needs.happiness = (uint8_t)v;
    g_pet.happLastChangeMs = now;
  }
}
static void changeEnergy(int delta, uint32_t now) {
  uint8_t cur = g_pet.persisted.needs.energy;
  if (delta > 0) delta = diminish(delta, cur);
  int v = (int)cur + delta;
  if (v > 100) v = 100; if (v < 0) v = 0;
  if (v != (int)cur) {
    g_pet.persisted.needs.energy = (uint8_t)v;
    g_pet.engLastChangeMs = now;
  }
}
static void changeFullness(int delta, uint32_t now) {
  uint8_t cur = g_pet.persisted.needs.fullness;
  if (delta > 0) delta = diminish(delta, cur);
  int v = (int)cur + delta;
  if (v > 100) v = 100; if (v < 0) v = 0;
  if (v != (int)cur) {
    g_pet.persisted.needs.fullness = (uint8_t)v;
    g_pet.fullLastChangeMs = now;
  }
}

// Current pet head position (includes wander). Used for float-spawn sources
// and touch-zone classification so both follow the wandering pet.
static int petHeadX() { return FACE_CX + (int)g_pet.wander.x; }
static int petHeadY() { return FACE_CY + (int)g_pet.wander.y; }

// Spawn a float that lands on the given need bar.
enum class TargetBar { Happiness, Energy, Fullness };
static void spawnFloatToBar(FloatType t, int fromX, int fromY, TargetBar tb,
                            uint32_t now) {
  int slotX;
  switch (tb) {
    case TargetBar::Happiness: slotX = kHappinessSlotX; break;
    case TargetBar::Energy:    slotX = kEnergySlotX;    break;
    case TargetBar::Fullness:  slotX = kFullnessSlotX;  break;
  }
  spawnFloat(g_pet.floats, t, fromX, fromY,
             needBarCenterX(slotX), needBarCenterY(), now);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline bool insideRect(int x, int y, const Rect& r) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void setFace(Face f, uint32_t now) {
  if (g_pet.face == f) return;

  // Celebration sounds — fire on entering Excited/Love but rate-limited so
  // continuous petting (which oscillates the face Idle ↔ Excited every
  // ~1.5s) doesn't spam them.
  if (f == Face::Excited && now - g_pet.lastExcitedSoundMs > 5000) {
    g_pet.lastExcitedSoundMs = now;
    playSound(Sound::Excited);
  } else if (f == Face::Love && now - g_pet.lastLoveSoundMs > 10000) {
    g_pet.lastLoveSoundMs = now;
    playSound(Sound::Love);
  } else if (f == Face::Sad && now - g_pet.lastSadSoundMs > 12000) {
    g_pet.lastSadSoundMs = now;
    playSound(Sound::Sad);
  }

  if (g_pet.face == Face::Sleeping) g_pet.lastWakeMs = now;
  g_pet.face = f;
  g_pet.enteredFaceAtMs = now;
}

static void flashFace(Face f, uint32_t durMs, uint32_t now) {
  g_pet.flashFace = f;
  g_pet.flashUntilMs = now + durMs;
}

// ─── Display power management ──────────────────────────────────────────
//
// Three modes drive the LCD backlight + panel state:
//   0 = normal: user-configured brightness (BRIGHTNESS_LEVELS[g_brightnessLevel])
//   1 = dimmed: forced to level 0 while the pet is sleeping
//   2 = off:    backlight 0 + LCD-controller sleep after 5 min idle
// applyDisplayPower() recomputes the target each frame and only triggers
// SPI traffic when the mode actually changes. Touches still register
// regardless of LCD-sleep state — the FT6336U is independent of the
// display panel — so any tap wakes the screen on the next frame.
constexpr uint32_t kIdleDisplayOffMs = 5UL * 60UL * 1000UL;
static uint8_t g_displayPowerMode = 0;

static void setBrightnessLevel(uint8_t level) {
  if (level > 3) level = 3;
  g_brightnessLevel = level;
  // Only push a fresh brightness to hardware when we're in normal mode —
  // dim/off modes own the backlight and shouldn't be undermined.
  if (g_displayPowerMode == 0) {
    M5.Display.setBrightness(BRIGHTNESS_LEVELS[level]);
  }
}

static bool inAnyModalScreen() {
  return g_settings.open || g_pet.cleaningMode || g_pet.foragingMode ||
         g_pet.toySelectMode || g_pet.mediaSelectMode ||
         g_pet.travelSelectMode || g_pet.activityMode ||
         g_pet.timerScreenMode ||
         g_pet.sportSelectMode || g_pet.sportWorkoutMode ||
         g_pet.sportDoneMode ||
         g_pet.cameraMode || g_pet.galleryMode ||
         g_pet.friendsMode || g_pet.travelTransitionMode ||
         g_pet.shutdownAnnouncedAtMs != 0 || g_pet.timerExpiring;
}

static void applyDisplayPower(uint32_t now) {
  uint8_t target = 0;
  if (!inAnyModalScreen()) {
    bool sleeping  = (g_pet.face == Face::Sleeping);
    uint32_t idleMs = now - g_motion.lastInteractionMs;
    if (idleMs >= kIdleDisplayOffMs) {
      target = 2;
    } else if (sleeping) {
      target = 1;
    }
  }
  if (target == g_displayPowerMode) return;

  if (g_displayPowerMode == 2) {
    // Coming out of off — wake panel before re-applying brightness.
    M5.Display.wakeup();
  }
  switch (target) {
    case 0:
      M5.Display.setBrightness(BRIGHTNESS_LEVELS[g_brightnessLevel]);
      break;
    case 1:
      M5.Display.setBrightness(BRIGHTNESS_LEVELS[0]);
      break;
    case 2:
      M5.Display.setBrightness(0);
      M5.Display.sleep();
      break;
  }
  g_displayPowerMode = target;
  // Camera power is decided per frame in updateCameraState() so that
  // voice listening + running tag animations can disable the camera —
  // not just display-mode transitions.
}

static void rememberStroke(uint32_t now) {
  if (now - g_pet.recentStrokesResetMs > 5000) {
    g_pet.recentStrokes = 0;
    g_pet.recentStrokesResetMs = now;
  }
  if (g_pet.recentStrokes < 255) g_pet.recentStrokes++;
}

// Push the current UI settings into the Persisted struct before saving.
static void syncSettingsToPersisted() {
  g_pet.persisted.brightnessLevel = g_brightnessLevel;
  g_pet.persisted.volume          = g_volume;
}

static void persistIfDue(uint32_t now) {
  if (now - g_pet.lastSavedMs < SAVE_INTERVAL_MS) return;
  g_pet.lastSavedMs = now;
  g_pet.persisted.lastSeen = currentDate();
  syncSettingsToPersisted();
  savePersisted(g_pet.persisted);
}

// ─── Hygiene: piles & dirt ──────────────────────────────────────────────────

static void spawnPile(uint32_t now) {
  if (g_pet.pileCount >= Pet::MAX_PILES) return;
  // Cheap pseudo-random from time so successive piles spread out.
  uint32_t h = now * 2654435761u;
  // x avoids the cleaning-button corner (x>=280).
  int x = 30 + (int)((h        ) % 240);
  int y = 195 + (int)((h >> 11) %  22);
  g_pet.pileX[g_pet.pileCount] = (int16_t)x;
  g_pet.pileY[g_pet.pileCount] = (int16_t)y;
  g_pet.pileCount++;
}

// Removes any pile within radius of the given point. Returns true if any
// were removed.
static bool removePilesAt(int x, int y, int radius) {
  bool any = false;
  for (int i = 0; i < g_pet.pileCount; ) {
    int dx = x - g_pet.pileX[i];
    int dy = y - g_pet.pileY[i];
    if (dx*dx + dy*dy < radius*radius) {
      // Compact the array by shifting the tail down.
      for (int j = i; j < g_pet.pileCount - 1; ++j) {
        g_pet.pileX[j] = g_pet.pileX[j+1];
        g_pet.pileY[j] = g_pet.pileY[j+1];
      }
      g_pet.pileCount--;
      any = true;
    } else {
      ++i;
    }
  }
  return any;
}

static bool isDirty() { return g_pet.dirtLevel >= 30; }

// Per second: dirt accumulates if too many piles are around.
static void tickDirt(uint32_t now) {
  if (now - g_pet.lastDirtTickMs < 1000) return;
  g_pet.lastDirtTickMs = now;
  if (g_pet.pileCount >= 5 && g_pet.dirtLevel < 100) {
    g_pet.dirtLevel++;
  }
}

// Called by the rain shower path — water washes the pet body, but doesn't
// remove piles from the landscape.
static void washPetBody() {
  g_pet.dirtLevel = 0;
}

// ─── Weather (per-scene random ambience) ────────────────────────────────────

// Which weathers are allowed for a given scene. Indoor/space scenes are
// always Sunny. Desert swaps Rain/Fog for Sandstorm.
static int weatherCandidatesFor(Scene s, Weather out[5]) {
  switch (s) {
    case Scene::Bedroom:
    case Scene::Space:
      out[0] = Weather::Sunny;
      return 1;
    case Scene::Desert:
      out[0] = Weather::Sunny;
      out[1] = Weather::Cloudy;
      out[2] = Weather::Sandstorm;
      return 3;
    default:        // Meadow, Forest, Beach, City
      out[0] = Weather::Sunny;
      out[1] = Weather::Cloudy;
      out[2] = Weather::Rainy;
      out[3] = Weather::Foggy;
      return 4;
  }
}

// Re-roll the weather to a (probably-different) compatible value.
static void rerollWeather(Scene s, uint32_t now) {
  Weather opts[5];
  int n = weatherCandidatesFor(s, opts);
  if (n <= 0) {
    g_pet.weather = Weather::Sunny;
    return;
  }
  uint32_t r = (now * 2654435761u) ^ 0xC0FFEE5Eu;
  Weather pick = opts[(r >> 7) % n];
  // Try once to avoid picking the same again, if multiple options exist
  if (pick == g_pet.weather && n > 1) {
    pick = opts[((r >> 7) + 1) % n];
  }
  g_pet.weather = pick;
}

// Periodic weather update + sunny-day happiness trickle.
static void tickWeather(uint32_t now) {
  // If the user just travelled to a different scene, re-roll immediately.
  Scene s = (Scene)(g_pet.persisted.selectedScene < kSceneCount
                      ? g_pet.persisted.selectedScene : 0);
  if (s != g_pet.weatherScene) {
    g_pet.weatherScene = s;
    rerollWeather(s, now);
    // Schedule next change in 60-120s
    g_pet.nextWeatherChangeMs = now + 60000 + (now % 60000);
    return;
  }
  if (g_pet.nextWeatherChangeMs == 0) {
    g_pet.nextWeatherChangeMs = now + 60000;
  }
  if (now >= g_pet.nextWeatherChangeMs) {
    rerollWeather(s, now);
    g_pet.nextWeatherChangeMs = now + 60000 + ((now ^ 0x9E3779B9) % 60000);
  }
  // Sunny days lift the mood very gently
  if (g_pet.weather == Weather::Sunny &&
      now - g_pet.lastSunshineBoostMs > 60000) {
    g_pet.lastSunshineBoostMs = now;
    changeHappiness(+1, now);
  }
}

// ─── Vibration motor ────────────────────────────────────────────────────────
//
// Short pulse using the AXP192's vibration channel (M5.Power.setVibration).
// Non-blocking: the pulse is started here and turned off in the loop once
// the duration has elapsed.

namespace {
struct VibState { uint32_t startMs; uint32_t durMs; };
VibState g_vib = {};
}

static void vibratePulse(uint32_t now, uint32_t durMs, uint8_t strength = 255) {
  M5.Power.setVibration(strength);
  g_vib.startMs = now;
  g_vib.durMs   = durMs;
}

static void tickVibration(uint32_t now) {
  if (g_vib.durMs == 0) return;
  if (now - g_vib.startMs >= g_vib.durMs) {
    M5.Power.setVibration(0);
    g_vib.durMs = 0;
  }
}

// ─── Mini-games (per-scene activities) ──────────────────────────────────────

// Hard cap on consecutive replays before the user is forced to leave the
// activity (back to pet view). The minigame already costs energy + has a
// post-play cooldown for fresh entries; this bounds the bypass via
// "Nochmal".
static constexpr uint8_t kMaxPlaysPerSession = 5;


struct ActivityState {
  Scene    scene       = Scene::Meadow;
  uint32_t startMs     = 0;
  uint16_t score       = 0;
  bool     gameOver    = false;
  uint32_t gameOverAtMs = 0;
  bool     rewardApplied = false;     // happiness boost only fires once

  // Replay limit — counts how many runs have happened in this activity
  // session (1 on the initial entry, +1 on each "Nochmal" tap). Caps at
  // kMaxPlaysPerSession, after which the Nochmal button is dimmed out.
  uint8_t  playsInSession = 0;

  // Snapshot of the last reward — used by the game-over overlay.
  uint8_t  lastStars     = 0;
  uint8_t  lastBoostHap  = 0;
  uint8_t  lastBoostEng  = 0;
  bool     lastNewBest   = false;

  // Butterfly minigame
  static constexpr int kBfCount = ActivityView::kBfCount;
  int16_t  bfX [kBfCount];
  int16_t  bfY [kBfCount];
  int8_t   bfVx[kBfCount];
  uint8_t  bfPhase[kBfCount];          // sin offset
  uint8_t  bfState[kBfCount];          // 0 alive, 1 caught, 2 gone
  uint32_t bfCaughtMs[kBfCount];
  uint8_t  bfColor[kBfCount];

  // Common touch tracking (for swipe segment in butterfly game)
  int16_t  prevTouchX = 0, prevTouchY = 0;
  bool     prevTouchValid = false;

  // Stack minigame
  uint8_t  stackCount = 0;
  int16_t  stackX[ActivityView::kStackMax];
  int16_t  stackY[ActivityView::kStackMax];
  int16_t  stackW[ActivityView::kStackMax];
  int16_t  movingX   = 0;
  int16_t  movingY   = 0;
  int16_t  movingW   = 0;
  int8_t   movingDir = 1;     // +1 right, -1 left
  uint16_t movingSpeed = 4;   // px per frame

  // Mushroom minigame
  uint8_t  mushKind[ActivityView::kMushSlots];     // 0/1/2
  uint32_t mushSpawnedMs[ActivityView::kMushSlots];
  uint32_t mushNextSpawnMs;

  // Common: pet position + jump physics
  float petPosX = 160.0f;
  float petPosY = 200.0f;
  float petVelY = 0.0f;
  bool  onGround = true;
  // Variable-jump tracking: while jumpHeld is true (touch still down) and
  // we're inside the boost window after a fresh jump, additional upward
  // thrust is applied so the user can decide jump height.
  bool     jumpHeld = false;
  uint32_t jumpStartMs = 0;

  // Common: obstacles array
  int16_t obsX[ActivityView::kObsCount];
  int16_t obsY[ActivityView::kObsCount];
  int8_t  obsVx[ActivityView::kObsCount];
  int8_t  obsVy[ActivityView::kObsCount];
  uint8_t obsType[ActivityView::kObsCount];
  uint32_t lastObsSpawnMs = 0;
  uint16_t obsSpawnInterval = 1500;

  // Lives (used by Surf, Asteroids)
  uint8_t  lives = 3;

  // Frogger row position (0 = bottom, 5 = goal reached)
  int8_t   petRow = 0;
};
static ActivityState g_activity;

static constexpr uint32_t kButterflyDurMs = 30000;

static void initButterflyGame(uint32_t now) {
  // 6 butterflies scattered, each with random x,y velocities and color.
  for (int i = 0; i < ActivityState::kBfCount; ++i) {
    uint32_t r = ((uint32_t)i + 1) * 2654435761u ^ now;
    g_activity.bfX[i] = 30 + (int)((r >> 7)  % 260);
    g_activity.bfY[i] = 50 + (int)((r >> 17) % 130);
    g_activity.bfVx[i] = (int8_t)((int)((r >> 3) & 3) - 1);  // -1, 0, 1, 2
    if (g_activity.bfVx[i] == 0) g_activity.bfVx[i] = 1;
    g_activity.bfPhase[i] = (uint8_t)((r >> 11) & 0xFF);
    g_activity.bfState[i] = 0;
    g_activity.bfCaughtMs[i] = 0;
    g_activity.bfColor[i] = (uint8_t)((r >> 21) % 5);
  }
}

static void initStackGame(uint32_t now) {
  g_activity.stackCount = 1;
  // Foundation block: centered, 80 wide
  g_activity.stackX[0] = 120;
  g_activity.stackY[0] = 220;
  g_activity.stackW[0] = 80;
  // First moving block hovers ONE ROW above the topmost placed block, so
  // the user can clearly see where it will land.
  g_activity.movingW   = 80;
  g_activity.movingX   = 0;
  g_activity.movingY   = g_activity.stackY[0] - 14;
  g_activity.movingDir = 1;
  g_activity.movingSpeed = 4;
  (void)now;
}

static void initMushroomGame(uint32_t now) {
  for (int i = 0; i < ActivityView::kMushSlots; ++i) {
    g_activity.mushKind[i] = 0;
    g_activity.mushSpawnedMs[i] = 0;
  }
  g_activity.mushNextSpawnMs = now + 300;
}

static void initSurfGame(uint32_t now) {
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    g_activity.obsType[i] = 0;
  }
  g_activity.lastObsSpawnMs   = now;
  g_activity.obsSpawnInterval = 1300;
  g_activity.lives = 3;
  g_activity.petPosX = 160.0f;
  g_activity.petPosY = 195.0f;
}

static void initScorpionGame(uint32_t now) {
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    g_activity.obsType[i] = 0;
  }
  g_activity.lastObsSpawnMs   = now;
  g_activity.obsSpawnInterval = 1500;
  g_activity.petPosX = 70.0f;
  g_activity.petPosY = 210.0f;
  g_activity.petVelY = 0.0f;
  g_activity.onGround = true;
  g_activity.lives = 1;     // one strike = game over
}

static void initAsteroidsGame(uint32_t now) {
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    g_activity.obsType[i] = 0;
  }
  g_activity.lastObsSpawnMs   = now;
  g_activity.obsSpawnInterval = 1100;
  g_activity.petPosX = 160.0f;
  g_activity.petPosY = 210.0f;
  g_activity.lives = 3;
}

static void initCrossGame(uint32_t now) {
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    g_activity.obsType[i] = 0;
  }
  g_activity.petRow = 0;
  g_activity.lastObsSpawnMs   = now;
  g_activity.obsSpawnInterval = 800;
  g_activity.lives = 1;
}

static void startActivity(Scene s, uint32_t now) {
  g_activity = ActivityState{};       // zero-init
  g_activity.scene  = s;
  g_activity.startMs = now;
  g_activity.score  = 0;
  g_activity.gameOver = false;
  g_activity.rewardApplied = false;
  switch (s) {
    case Scene::Meadow:  initButterflyGame(now); break;
    case Scene::Bedroom: initStackGame    (now); break;
    case Scene::Forest:  initMushroomGame (now); break;
    case Scene::Beach:   initSurfGame     (now); break;
    case Scene::Desert:  initScorpionGame (now); break;
    case Scene::Space:   initAsteroidsGame(now); break;
    case Scene::City:    initCrossGame    (now); break;
  }
}

static void applyActivityReward(uint32_t now) {
  if (g_activity.rewardApplied) return;
  g_activity.rewardApplied = true;
  uint16_t s = g_activity.score;
  uint16_t* bestPtr = nullptr;
  int t1 = 5, t2 = 10, t3 = 18;     // generic defaults
  switch (g_activity.scene) {
    case Scene::Meadow:
      bestPtr = &g_pet.persisted.bestButterflies;
      t1 = 5; t2 = 10; t3 = 18; break;
    case Scene::Bedroom:
      bestPtr = &g_pet.persisted.bestStack;
      t1 = 3; t2 = 8;  t3 = 15; break;
    case Scene::Forest:
      bestPtr = &g_pet.persisted.bestMushrooms;
      t1 = 5; t2 = 12; t3 = 22; break;
    case Scene::Beach:
      bestPtr = &g_pet.persisted.bestSurf;
      t1 = 10; t2 = 25; t3 = 50; break;
    case Scene::Desert:
      bestPtr = &g_pet.persisted.bestScorpion;
      t1 = 3; t2 = 8;  t3 = 15; break;
    case Scene::Space:
      bestPtr = &g_pet.persisted.bestAsteroids;
      t1 = 5; t2 = 15; t3 = 30; break;
    case Scene::City:
      bestPtr = &g_pet.persisted.bestCross;
      t1 = 1; t2 = 3;  t3 = 6;  break;
  }
  // Stars: 0 (below t1), 1 (t1..t2), 2 (t2..t3), 3 (≥t3)
  uint8_t stars = 0;
  int boostHap = 2, boostEng = 0;       // tiny participation prize
  if      (s >= t3) { stars = 3; boostHap = 15; boostEng = 5; }
  else if (s >= t2) { stars = 2; boostHap = 10; boostEng = 2; }
  else if (s >= t1) { stars = 1; boostHap =  5; boostEng = 0; }

  bool newBest = false;
  if (bestPtr && s > *bestPtr) {
    *bestPtr = s;
    newBest = true;
  }

  changeHappiness(+boostHap, now);
  spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                  TargetBar::Happiness, now);
  if (boostEng > 0) {
    changeEnergy(+boostEng, now);
  }
  if (stars > 0) {
    playSound(Sound::Happy);
  }

  // Snapshot for the game-over overlay
  g_activity.lastStars    = stars;
  g_activity.lastBoostHap = (uint8_t)boostHap;
  g_activity.lastBoostEng = (uint8_t)boostEng;
  g_activity.lastNewBest  = newBest;

  savePersisted(g_pet.persisted);    // best score + cooldown end
}

static void updateButterflyGame(uint32_t now) {
  if (g_activity.gameOver) return;
  uint32_t elapsed = now - g_activity.startMs;
  if (elapsed >= kButterflyDurMs) {
    g_activity.gameOver     = true;
    g_activity.gameOverAtMs = now;
    applyActivityReward(now);
    return;
  }
  // Move alive butterflies; flying-up animation for caught ones.
  for (int i = 0; i < ActivityState::kBfCount; ++i) {
    if (g_activity.bfState[i] == 2) continue;
    if (g_activity.bfState[i] == 1) {
      // caught → fly up + fade
      g_activity.bfY[i] -= 3;
      if (now - g_activity.bfCaughtMs[i] > 700 || g_activity.bfY[i] < -10) {
        g_activity.bfState[i] = 2;
      }
      continue;
    }
    // Linear x
    g_activity.bfX[i] += g_activity.bfVx[i] * 2;
    if (g_activity.bfX[i] < -20) g_activity.bfX[i] = 340;
    if (g_activity.bfX[i] > 340) g_activity.bfX[i] = -20;
    // Sinus y around an anchor
    float t = (float)(now / 30u + g_activity.bfPhase[i]) * 0.05f;
    int dy = (int)(sinf(t) * 1.2f);
    g_activity.bfY[i] += dy;
    if (g_activity.bfY[i] < 40)  g_activity.bfY[i] = 40;
    if (g_activity.bfY[i] > 180) g_activity.bfY[i] = 180;
  }

  // Respawn caught/gone butterflies if there are too few alive (keeps the
  // play field populated for the full 30 s).
  int alive = 0;
  for (int i = 0; i < ActivityState::kBfCount; ++i) {
    if (g_activity.bfState[i] == 0) alive++;
  }
  if (alive < 3) {
    for (int i = 0; i < ActivityState::kBfCount; ++i) {
      if (g_activity.bfState[i] == 2) {
        uint32_t r = (uint32_t)now * 2654435761u + i;
        g_activity.bfX[i] = (r & 1) ? -10 : 330;
        g_activity.bfY[i] = 60 + (int)((r >> 7) % 100);
        g_activity.bfVx[i] = (g_activity.bfX[i] < 0) ? 1 : -1;
        g_activity.bfState[i] = 0;
        break;
      }
    }
  }
}

static void updateStackGame(uint32_t /*now*/) {
  if (g_activity.gameOver) return;
  // Slide moving block
  g_activity.movingX += g_activity.movingDir * (int)g_activity.movingSpeed;
  if (g_activity.movingX <= 0) {
    g_activity.movingX = 0;
    g_activity.movingDir = +1;
  }
  if (g_activity.movingX + g_activity.movingW >= 320) {
    g_activity.movingX = 320 - g_activity.movingW;
    g_activity.movingDir = -1;
  }
}

// ── Mushroom (Forest) ──────────────────────────────────────────────────────
static constexpr uint32_t kMushroomDurMs    = 30000;
static constexpr uint32_t kMushroomLifetime = 1600;     // visible time per spawn

static void updateMushroomGame(uint32_t now) {
  if (g_activity.gameOver) return;
  if (now - g_activity.startMs >= kMushroomDurMs) {
    g_activity.gameOver     = true;
    g_activity.gameOverAtMs = now;
    applyActivityReward(now);
    return;
  }
  // Despawn aged mushrooms
  for (int i = 0; i < ActivityView::kMushSlots; ++i) {
    if (g_activity.mushKind[i] == 0) continue;
    if (now - g_activity.mushSpawnedMs[i] >= kMushroomLifetime) {
      g_activity.mushKind[i] = 0;
    }
  }
  // Spawn into a free slot at intervals
  if (now >= g_activity.mushNextSpawnMs) {
    int free[ActivityView::kMushSlots];
    int nf = 0;
    for (int i = 0; i < ActivityView::kMushSlots; ++i) {
      if (g_activity.mushKind[i] == 0) free[nf++] = i;
    }
    if (nf > 0) {
      uint32_t r = (uint32_t)now * 2654435761u;
      int slot = free[(r >> 7) % nf];
      // Gold mushroom ~12% of the time
      g_activity.mushKind[slot] = ((r >> 13) % 100 < 12) ? 2 : 1;
      g_activity.mushSpawnedMs[slot] = now;
    }
    // Speed up gradually
    uint32_t nextDelay = 600;
    uint32_t elapsed = now - g_activity.startMs;
    if (elapsed > 5000)  nextDelay = 500;
    if (elapsed > 12000) nextDelay = 380;
    if (elapsed > 20000) nextDelay = 280;
    g_activity.mushNextSpawnMs = now + nextDelay;
  }
}

static void mushroomTouch(int tx, int ty, uint32_t now) {
  if (g_activity.gameOver) return;
  for (int i = 0; i < ActivityView::kMushSlots; ++i) {
    if (g_activity.mushKind[i] == 0) continue;
    int dx = tx - kMushSlotX[i];
    int dy = ty - kMushSlotY[i];
    if (dx*dx + dy*dy <= 22*22) {
      uint8_t k = g_activity.mushKind[i];
      g_activity.mushKind[i] = 0;
      g_activity.score += (k == 2) ? 3 : 1;
      playSound(k == 2 ? Sound::Excited : Sound::CollectFood);
      return;
    }
  }
}

// ── Surf (Beach) ───────────────────────────────────────────────────────────
static constexpr int kSurfPetSize = 18;

static void updateSurfGame(uint32_t now) {
  if (g_activity.gameOver) return;
  // Tilt-controlled position. -gx so left tilt → left (mirrors wander).
  float target = 160.0f + (-g_motion.gx) * 220.0f;
  if (target <  30.0f) target =  30.0f;
  if (target > 290.0f) target = 290.0f;
  g_activity.petPosX += (target - g_activity.petPosX) * 0.20f;

  // Score = elapsed seconds (mostly survival)
  uint32_t elapsed = now - g_activity.startMs;
  g_activity.score = elapsed / 1000;

  // Spawn obstacles
  if (now - g_activity.lastObsSpawnMs >= g_activity.obsSpawnInterval) {
    g_activity.lastObsSpawnMs = now;
    for (int i = 0; i < ActivityView::kObsCount; ++i) {
      if (g_activity.obsType[i] == 0) {
        uint32_t r = (uint32_t)now * 2654435761u;
        g_activity.obsX[i] = 30 + (int)((r >> 7) % 260);
        g_activity.obsY[i] = -10;
        // Type 1=rock, 2=jellyfish, 3=shell
        uint8_t t = ((r >> 17) % 10);
        g_activity.obsType[i] = (t < 4) ? 1 : (t < 8) ? 2 : 3;
        break;
      }
    }
    if (g_activity.obsSpawnInterval > 600) g_activity.obsSpawnInterval -= 20;
  }

  // Move obstacles
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    if (g_activity.obsType[i] == 0) continue;
    g_activity.obsY[i] += 4;
    // Collision with pet
    int dx = g_activity.obsX[i] - (int)g_activity.petPosX;
    int dy = g_activity.obsY[i] - (int)g_activity.petPosY;
    if (dx*dx + dy*dy <= kSurfPetSize * kSurfPetSize) {
      uint8_t t = g_activity.obsType[i];
      g_activity.obsType[i] = 0;
      if (t == 3) {
        g_activity.score += 5;       // shell collectible
        playSound(Sound::CollectFood);
      } else {
        if (g_activity.lives > 0) g_activity.lives--;
        playSound(Sound::Sad);
        if (g_activity.lives == 0) {
          g_activity.gameOver = true;
          g_activity.gameOverAtMs = now;
          applyActivityReward(now);
          return;
        }
      }
    }
    if (g_activity.obsY[i] > 250) g_activity.obsType[i] = 0;
  }
}

// ── Scorpion (Desert) ──────────────────────────────────────────────────────
//
// Physics is integrated per Activity-tick (throttled to 30 Hz), so the per-
// frame deltas are stable regardless of how fast the main loop spins.
//
// Variable jump: a quick tap pops the pet ~18 px (clears the low scorpion
// hitbox only), while holding the touch adds steady upward thrust for up
// to 320 ms so the user can clear the tall cactus hitbox. The lift is
// strong relative to gravity so the difference between tap-only and full-
// hold is dramatic.
static constexpr float    kScorpGround     = 210.0f;
static constexpr float    kScorpGravity    =  0.55f;
static constexpr float    kScorpJumpVel    = -4.0f;    // initial pop (low)
static constexpr float    kScorpJumpLift   = -1.0f;    // upward thrust while held
static constexpr float    kScorpJumpCap    = -10.0f;   // velocity cap
static constexpr uint32_t kScorpHoldMaxMs  =  320;     // boost window

static void updateScorpionGame(uint32_t now) {
  if (g_activity.gameOver) return;

  // Track touch release — if no finger is on the screen, the jump button is
  // released regardless of the touch dispatcher (which only runs while a
  // touch is active).
  if (M5.Touch.getCount() == 0) g_activity.jumpHeld = false;

  // Pet jump physics with variable height.
  if (!g_activity.onGround) {
    g_activity.petVelY += kScorpGravity;
    bool inBoostWindow = (now - g_activity.jumpStartMs) < kScorpHoldMaxMs;
    if (g_activity.jumpHeld && inBoostWindow && g_activity.petVelY < 0) {
      g_activity.petVelY += kScorpJumpLift;
      if (g_activity.petVelY < kScorpJumpCap) g_activity.petVelY = kScorpJumpCap;
    }
    g_activity.petPosY += g_activity.petVelY;
    if (g_activity.petPosY >= kScorpGround) {
      g_activity.petPosY  = kScorpGround;
      g_activity.petVelY  = 0;
      g_activity.onGround = true;
      g_activity.jumpHeld = false;
    }
  }
  // Score = obstacles dodged (advanced when an obstacle leaves screen)
  // Spawn obstacles
  if (now - g_activity.lastObsSpawnMs >= g_activity.obsSpawnInterval) {
    g_activity.lastObsSpawnMs = now;
    for (int i = 0; i < ActivityView::kObsCount; ++i) {
      if (g_activity.obsType[i] == 0) {
        uint32_t r = (uint32_t)now * 2654435761u;
        g_activity.obsX[i] = 330;
        g_activity.obsY[i] = (int16_t)kScorpGround;
        g_activity.obsType[i] = (uint8_t)(((r >> 7) & 1) + 1);
        break;
      }
    }
    if (g_activity.obsSpawnInterval > 700) g_activity.obsSpawnInterval -= 25;
  }
  // Move obstacles. Hitboxes match the sprite shapes — scorpions are low &
  // wide, cacti are tall & narrow — so a low hop clears scorpions but only
  // a held (high) jump clears cacti.
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    if (g_activity.obsType[i] == 0) continue;
    g_activity.obsX[i] -= 5;
    int dx = g_activity.obsX[i] - (int)g_activity.petPosX;
    int dy = g_activity.obsY[i] - (int)g_activity.petPosY;
    int dxT, dyT;
    if (g_activity.obsType[i] == 2) {     // cactus
      dxT = 12; dyT = 26;
    } else {                              // scorpion
      dxT = 14; dyT =  8;
    }
    if (abs(dx) < dxT && abs(dy) < dyT) {
      g_activity.gameOver = true;
      g_activity.gameOverAtMs = now;
      applyActivityReward(now);
      playSound(Sound::Sad);
      return;
    }
    if (g_activity.obsX[i] < -20) {
      g_activity.obsType[i] = 0;
      g_activity.score++;
    }
  }
}

static void scorpionJump(uint32_t now) {
  if (g_activity.gameOver || !g_activity.onGround) return;
  g_activity.petVelY    = kScorpJumpVel;
  g_activity.onGround   = false;
  g_activity.jumpHeld   = true;
  g_activity.jumpStartMs = now;
  playSound(Sound::Tickle);
}

// ── Asteroids (Space) ──────────────────────────────────────────────────────
static constexpr int kAsteroidPetSize = 18;

static void updateAsteroidsGame(uint32_t now) {
  if (g_activity.gameOver) return;
  // Tilt-controlled position
  float target = 160.0f + (-g_motion.gx) * 220.0f;
  if (target <  30.0f) target =  30.0f;
  if (target > 290.0f) target = 290.0f;
  g_activity.petPosX += (target - g_activity.petPosX) * 0.22f;

  // Score grows slowly with time
  uint32_t elapsed = now - g_activity.startMs;
  if (elapsed / 2000u > g_activity.score) {
    g_activity.score = elapsed / 2000u;
  }

  // Spawn objects
  if (now - g_activity.lastObsSpawnMs >= g_activity.obsSpawnInterval) {
    g_activity.lastObsSpawnMs = now;
    for (int i = 0; i < ActivityView::kObsCount; ++i) {
      if (g_activity.obsType[i] == 0) {
        uint32_t r = (uint32_t)now * 2654435761u;
        g_activity.obsX[i] = 25 + (int)((r >> 7) % 270);
        g_activity.obsY[i] = -12;
        // 70% asteroid, 30% star
        g_activity.obsType[i] = ((r >> 17) % 10 < 7) ? 1 : 2;
        break;
      }
    }
    if (g_activity.obsSpawnInterval > 500) g_activity.obsSpawnInterval -= 18;
  }
  // Move objects
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    if (g_activity.obsType[i] == 0) continue;
    int speed = (g_activity.obsType[i] == 1) ? 4 : 3;
    g_activity.obsY[i] += speed;
    int dx = g_activity.obsX[i] - (int)g_activity.petPosX;
    int dy = g_activity.obsY[i] - (int)g_activity.petPosY;
    if (dx*dx + dy*dy <= kAsteroidPetSize * kAsteroidPetSize) {
      uint8_t t = g_activity.obsType[i];
      g_activity.obsType[i] = 0;
      if (t == 2) {
        g_activity.score += 2;          // star
        playSound(Sound::CollectFood);
      } else {
        if (g_activity.lives > 0) g_activity.lives--;
        playSound(Sound::Sad);
        if (g_activity.lives == 0) {
          g_activity.gameOver = true;
          g_activity.gameOverAtMs = now;
          applyActivityReward(now);
          return;
        }
      }
    }
    if (g_activity.obsY[i] > 250) g_activity.obsType[i] = 0;
  }
}

// ── Cross (City) ───────────────────────────────────────────────────────────
static void updateCrossGame(uint32_t now) {
  if (g_activity.gameOver) return;
  // Move cars on each lane
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    if (g_activity.obsType[i] == 0) continue;
    g_activity.obsX[i] += g_activity.obsVx[i];
    if (g_activity.obsX[i] < -40 || g_activity.obsX[i] > 360) {
      g_activity.obsType[i] = 0;
      continue;
    }
    // Collision with pet (only when on a road row, not row 0 = sidewalk)
    if (g_activity.petRow >= 1 && g_activity.petRow <= 5) {
      int laneIdx = g_activity.petRow - 1;
      int laneY   = kCrossLaneY[laneIdx];
      // Cars are drawn at y..y+16; pet is at laneY-2 (top), small.
      if (abs(g_activity.obsY[i] + 8 - (laneY - 2)) < 16) {
        int dx = g_activity.obsX[i] - kCrossPetX;
        if (abs(dx) < 22) {
          g_activity.gameOver = true;
          g_activity.gameOverAtMs = now;
          applyActivityReward(now);
          playSound(Sound::Sad);
          return;
        }
      }
    }
  }
  // Spawn cars
  if (now - g_activity.lastObsSpawnMs >= g_activity.obsSpawnInterval) {
    g_activity.lastObsSpawnMs = now;
    for (int i = 0; i < ActivityView::kObsCount; ++i) {
      if (g_activity.obsType[i] == 0) {
        uint32_t r = (uint32_t)now * 2654435761u;
        int lane = (r >> 7) % 5;     // 0..4
        bool leftToRight = (lane & 1) == 0;     // alternate per lane
        g_activity.obsX[i]  = leftToRight ? -30 : 350;
        g_activity.obsY[i]  = kCrossLaneY[lane];
        g_activity.obsVx[i] = leftToRight ? (3 + (int)((r >> 11) % 3))
                                          : -(3 + (int)((r >> 11) % 3));
        // Type 1..4 with high bit signaling direction (for headlight side).
        // Bits 3-4 optionally carry a driver-pet id (1=Bear, 2=Cat, 3=Dog,
        // 0=empty car). About half the cars get a driver — readable but
        // not so dense that the lanes look uniform.
        uint8_t k = (uint8_t)(((r >> 17) % 4) + 1);
        if (((r >> 23) & 1u) == 0) {
          uint8_t driver = (uint8_t)(((r >> 24) % 3u) + 1u);
          k = (uint8_t)(k | (driver << 3));
        }
        if (!leftToRight) k |= 0x80;
        g_activity.obsType[i] = k;
        break;
      }
    }
    if (g_activity.obsSpawnInterval > 350) g_activity.obsSpawnInterval -= 12;
  }
}

static void crossStep(uint32_t now) {
  if (g_activity.gameOver) return;
  g_activity.petRow++;
  if (g_activity.petRow >= 6) {
    // Crossing complete — score, reset to bottom, raise difficulty
    g_activity.score++;
    g_activity.petRow = 0;
    if (g_activity.obsSpawnInterval > 350) g_activity.obsSpawnInterval -= 30;
    playSound(Sound::Happy);
  }
}

// All activities use frame-locked physics (per-tick velocities). The main
// loop runs much faster than 30 Hz on this chip, so without throttling the
// physics would multiply by the loop rate and any tunable curve (like the
// scorpion's variable jump) collapses. Throttle to a stable 30 Hz tick.
static uint32_t g_lastActivityTickMs = 0;

static void updateActivity(uint32_t now) {
  if (now - g_lastActivityTickMs < 33) return;
  g_lastActivityTickMs = now;
  switch (g_activity.scene) {
    case Scene::Meadow:  updateButterflyGame(now); break;
    case Scene::Bedroom: updateStackGame    (now); break;
    case Scene::Forest:  updateMushroomGame (now); break;
    case Scene::Beach:   updateSurfGame     (now); break;
    case Scene::Desert:  updateScorpionGame (now); break;
    case Scene::Space:   updateAsteroidsGame(now); break;
    case Scene::City:    updateCrossGame    (now); break;
  }
}

static int activitySegSqDist(int sx, int sy, int ex, int ey, int px, int py) {
  int dx = ex - sx, dy = ey - sy;
  int len2 = dx*dx + dy*dy;
  float t = 0.0f;
  if (len2 > 0) {
    t = (float)((px - sx) * dx + (py - sy) * dy) / (float)len2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
  }
  float qx = sx + t * dx;
  float qy = sy + t * dy;
  float ddx = qx - px, ddy = qy - py;
  return (int)(ddx * ddx + ddy * ddy);
}

static void butterflyTouch(int sx, int sy, int ex, int ey, uint32_t now) {
  if (g_activity.gameOver) return;
  static constexpr int hitR = 20;
  for (int i = 0; i < ActivityState::kBfCount; ++i) {
    if (g_activity.bfState[i] != 0) continue;
    if (activitySegSqDist(sx, sy, ex, ey, g_activity.bfX[i], g_activity.bfY[i])
          <= hitR * hitR) {
      g_activity.bfState[i] = 1;
      g_activity.bfCaughtMs[i] = now;
      g_activity.score++;
      playSound(Sound::CollectFood);
    }
  }
}

static void stackTouch(uint32_t now) {
  if (g_activity.gameOver) return;
  // Place the moving block. Compute overlap with the previous block.
  int top = g_activity.stackCount - 1;
  int prevX = g_activity.stackX[top];
  int prevW = g_activity.stackW[top];
  int newX  = g_activity.movingX;
  int newW  = g_activity.movingW;
  int leftLap  = (newX > prevX) ? newX : prevX;
  int rightLap = ((newX + newW) < (prevX + prevW)) ? (newX + newW) : (prevX + prevW);
  int overlap = rightLap - leftLap;
  if (overlap <= 0) {
    // Total miss → game over
    g_activity.gameOver = true;
    g_activity.gameOverAtMs = now;
    applyActivityReward(now);
    playSound(Sound::Sad);
    return;
  }
  if (g_activity.stackCount < ActivityView::kStackMax) {
    int idx = g_activity.stackCount++;
    g_activity.stackX[idx] = leftLap;
    g_activity.stackW[idx] = overlap;
    g_activity.stackY[idx] = g_activity.stackY[idx - 1] - 14;
    g_activity.score++;
    playSound(Sound::Happy);
  } else {
    // Filled the stack — auto game-over with full credit.
    g_activity.gameOver = true;
    g_activity.gameOverAtMs = now;
    applyActivityReward(now);
    return;
  }
  // Next moving block hovers one row above the new top, with the trimmed
  // width.
  int newTop = g_activity.stackCount - 1;
  g_activity.movingW = overlap;
  g_activity.movingY = g_activity.stackY[newTop] - 14;
  g_activity.movingX = (g_activity.movingDir > 0) ? 0 : (320 - overlap);
  // Tower reached the top of the screen → won.
  if (g_activity.movingY < 30) {
    g_activity.gameOver = true;
    g_activity.gameOverAtMs = now;
    applyActivityReward(now);
    return;
  }
  // Gentle speed-up so it stays playable.
  if (g_activity.movingSpeed < 9) g_activity.movingSpeed++;
}

static void handleTouchActivity(int tx, int ty, bool pressed, bool wasPressed,
                                uint32_t now) {
  // Back X always works.
  if (wasPressed && insideRect(tx, ty, cornerHitRect(kActivityBackRect))) {
    g_pet.activityMode = false;
    g_activity.prevTouchValid = false;
    return;
  }
  // Game-over overlay → retry, OK, und back X sind live.
  if (g_activity.gameOver) {
    if (wasPressed && insideRect(tx, ty, kActivityRetryRect)) {
      // Cap consecutive replays per session — once exhausted the button
      // is dimmed by the renderer and taps are silently ignored.
      if (g_activity.playsInSession < kMaxPlaysPerSession) {
        uint8_t nextCount = (uint8_t)(g_activity.playsInSession + 1);
        startActivity(g_activity.scene, now);
        g_activity.playsInSession = nextCount;
      }
    } else if (wasPressed && insideRect(tx, ty, kActivityOkRect)) {
      // OK = exit the game, back to the pet view.
      g_pet.activityMode = false;
      g_activity.prevTouchValid = false;
    }
    return;
  }
  // Per-game touch
  switch (g_activity.scene) {
    case Scene::Meadow:
      if (pressed) {
        int16_t sx = g_activity.prevTouchValid ? g_activity.prevTouchX : (int16_t)tx;
        int16_t sy = g_activity.prevTouchValid ? g_activity.prevTouchY : (int16_t)ty;
        butterflyTouch(sx, sy, tx, ty, now);
        g_activity.prevTouchX     = (int16_t)tx;
        g_activity.prevTouchY     = (int16_t)ty;
        g_activity.prevTouchValid = true;
      }
      break;
    case Scene::Bedroom:
      if (wasPressed) stackTouch(now);
      break;
    case Scene::Forest:
      if (wasPressed) mushroomTouch(tx, ty, now);
      break;
    case Scene::Desert:
      if (wasPressed) scorpionJump(now);
      // While the finger stays down on a fresh jump, keep the boost flag
      // alive — releasing cuts the jump short, holding extends it.
      if (pressed && !g_activity.onGround) g_activity.jumpHeld = true;
      break;
    case Scene::City:
      if (wasPressed) crossStep(now);
      break;
    case Scene::Beach:
    case Scene::Space:
      // Tilt-controlled — touch is a no-op (back X handled above).
      break;
  }
}

// ─── Foraging (food collection) ─────────────────────────────────────────────

// Targets per-type — collected items respawn until these caps are hit so the
// scene always has something to swipe at.
static constexpr int kForageTargetApples  = 3;
static constexpr int kForageTargetBerries = 2;
static constexpr int kForageTargetFish    = 2;

// Item hitbox radius for swipe-collection.
static constexpr int kForageHitR = 22;

// Slot bounding boxes — random spawns choose a position inside.
static constexpr int kTreeX0 = 28,  kTreeX1 = 120, kTreeY0 = 70,  kTreeY1 = 130;
static constexpr int kBushX0 = 200, kBushX1 = 285, kBushY0 = 142, kBushY1 = 175;
static constexpr int kFishY0 = 200, kFishY1 = 230;

struct ForagingState {
  ForageItemView items[kForageMaxItems];
  uint32_t lastSpawnMs = 0;
  int16_t  prevTouchX  = 0, prevTouchY = 0;
  bool     prevTouchValid = false;
};
static ForagingState g_forage;

static void initForage() {
  for (auto& it : g_forage.items) {
    it.type = ForageItem::None;
    it.flyStartMs = 0;
  }
  g_forage.lastSpawnMs = 0;
  g_forage.prevTouchValid = false;
}

static int countForageAlive(ForageItem t) {
  int c = 0;
  for (auto& it : g_forage.items) {
    if (it.type == t && it.flyStartMs == 0) c++;
  }
  return c;
}

static int findForageSlot(uint32_t now) {
  for (int i = 0; i < kForageMaxItems; ++i) {
    auto& it = g_forage.items[i];
    if (it.type == ForageItem::None) return i;
    // Already-flown items can be recycled.
    if (it.flyStartMs != 0 && now - it.flyStartMs > 400) return i;
  }
  return -1;
}

static uint32_t forageRand(uint32_t seed) {
  return seed * 2654435761u ^ 0xDEADBEEFu;
}

static void spawnForageItem(ForageItem type, uint32_t now) {
  int slot = findForageSlot(now);
  if (slot < 0) return;
  auto& it = g_forage.items[slot];
  it.type        = type;
  it.flyStartMs  = 0;
  it.facingLeft  = false;
  uint32_t r = forageRand(now + (uint32_t)slot);
  switch (type) {
    case ForageItem::Apple:
      it.x = kTreeX0 + (int)((r >> 4)  % (kTreeX1 - kTreeX0));
      it.y = kTreeY0 + (int)((r >> 13) % (kTreeY1 - kTreeY0));
      break;
    case ForageItem::Berry:
      it.x = kBushX0 + (int)((r >> 4)  % (kBushX1 - kBushX0));
      it.y = kBushY0 + (int)((r >> 13) % (kBushY1 - kBushY0));
      break;
    case ForageItem::Fish: {
      bool fromLeft = (r & 1);
      it.x = fromLeft ? -10 : 330;
      it.y = kFishY0 + (int)((r >> 13) % (kFishY1 - kFishY0));
      it.facingLeft = !fromLeft;   // tail trails behind direction of motion
      break;
    }
    case ForageItem::None: break;
  }
}

static void updateForage(uint32_t now, bool touchActive) {
  if (!touchActive) g_forage.prevTouchValid = false;

  // Move fish + advance fly-out animation + recycle.
  for (auto& it : g_forage.items) {
    if (it.type == ForageItem::None) continue;
    if (it.flyStartMs != 0) {
      if (now - it.flyStartMs > 400) {
        it.type = ForageItem::None;
        it.flyStartMs = 0;
      }
      continue;
    }
    if (it.type == ForageItem::Fish) {
      // Swim ~1 px / frame in the facing direction. facingLeft → moving left.
      int step = it.facingLeft ? -1 : 1;
      it.x += step;
      if (it.x < -20 || it.x > 340) {
        it.type = ForageItem::None;
      }
    }
  }

  // Spawn cadence — every ~1 s, top up the type that's most under target.
  if (now - g_forage.lastSpawnMs > 900) {
    g_forage.lastSpawnMs = now;
    int aps = countForageAlive(ForageItem::Apple);
    int bps = countForageAlive(ForageItem::Berry);
    int fps = countForageAlive(ForageItem::Fish);
    ForageItem candidates[3];
    int nc = 0;
    if (aps < kForageTargetApples)  candidates[nc++] = ForageItem::Apple;
    if (bps < kForageTargetBerries) candidates[nc++] = ForageItem::Berry;
    if (fps < kForageTargetFish)    candidates[nc++] = ForageItem::Fish;
    if (nc > 0) {
      uint32_t r = forageRand(now);
      spawnForageItem(candidates[(r >> 7) % nc], now);
    }
  }
}

// Returns squared distance from point (px,py) to the segment (sx,sy)→(ex,ey).
static int forageSegDistSq(int sx, int sy, int ex, int ey, int px, int py) {
  int dx = ex - sx, dy = ey - sy;
  int len2 = dx*dx + dy*dy;
  float t = 0.0f;
  if (len2 > 0) {
    t = (float)((px - sx) * dx + (py - sy) * dy) / (float)len2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
  }
  float qx = sx + t * dx;
  float qy = sy + t * dy;
  float ddx = qx - px, ddy = qy - py;
  return (int)(ddx * ddx + ddy * ddy);
}

static int16_t inventoryIconX(ForageItem t) {
  switch (t) {
    case ForageItem::Apple: return kForageInvApplePosX;
    case ForageItem::Berry: return kForageInvBerryPosX;
    case ForageItem::Fish:  return kForageInvFishPosX;
    default:                return 0;
  }
}

// Hard cap per food type. The user can't accumulate more than this many
// of any single item — anything past this is dropped on the floor.
static constexpr uint16_t kFoodMaxPerType = 20;

// ─── Activity gating: cooldowns + energy costs ──────────────────────────────
// Each scene's mini-game has its own cooldown (wall-clock seconds) and an
// energy cost paid at start. Indexed by Scene enum.
static constexpr uint32_t kActivityCooldownSec[7] = {
  180,    // Meadow      — Butterflies (3 min)
  240,    // Bedroom     — Stack       (4 min)
  180,    // Forest      — Mushrooms   (3 min)
  360,    // Beach       — Surf        (6 min)
  300,    // Desert      — Scorpion    (5 min)
  360,    // Space       — Asteroids   (6 min)
  300,    // City        — Cross       (5 min)
};
static constexpr uint8_t kActivityEnergyCost[7] = {
  3, 3, 3, 5, 5, 5, 5,
};

// Returns the wall-clock epoch second for the given pet uptime, or 0 when
// the RTC hasn't been set yet (in which case cooldowns are skipped so the
// user isn't blocked by an unsynced clock).
static uint32_t nowEpoch() {
  if (!rtcAvailable()) return 0;
  time_t t = time(nullptr);
  return (t > 0) ? (uint32_t)t : 0;
}

static uint32_t activityCooldownRemainingSec(Scene s) {
  uint32_t end = g_pet.persisted.gameCooldownEnd[(int)s];
  if (end == 0) return 0;
  uint32_t e = nowEpoch();
  if (e == 0) return 0;     // RTC unset → don't gate
  return (end > e) ? (end - e) : 0;
}

static bool activityHasEnoughEnergy(Scene s) {
  uint8_t cost = kActivityEnergyCost[(int)s];
  return g_pet.persisted.needs.energy >= cost;
}

// ─── Travel gating: cooldown + energy ───────────────────────────────────────
//
// Switching to a different scene costs energy and triggers a cooldown
// before the user can travel again. Same persistence semantics as the
// activity cooldowns (wall-clock epoch in NVS).

static constexpr uint8_t  kTravelEnergyCost     = 8;
static constexpr uint32_t kTravelCooldownMinSec = 300;     // 5 min
static constexpr uint32_t kTravelCooldownMaxSec = 600;     // 10 min

static uint32_t travelCooldownRemainingSec() {
  uint32_t end = g_pet.persisted.travelCooldownEnd;
  if (end == 0) return 0;
  uint32_t e = nowEpoch();
  if (e == 0) return 0;          // RTC unset → don't gate
  return (end > e) ? (end - e) : 0;
}

static bool travelHasEnoughEnergy() {
  return g_pet.persisted.needs.energy >= kTravelEnergyCost;
}

// Picks a fresh random cooldown duration in the configured range.
static uint32_t pickTravelCooldownSec(uint32_t now) {
  uint32_t range = kTravelCooldownMaxSec - kTravelCooldownMinSec + 1;
  uint32_t r = (uint32_t)now * 2654435761u;
  return kTravelCooldownMinSec + ((r >> 11) % range);
}

// ─── Parental play-time limit ───────────────────────────────────────────────

// Break duration stays fixed; the play-time limit is configurable via the
// parent web page and is stored in g_pet.persisted.sessionLimitMin.
static constexpr uint32_t kBreakDurationSec     = 30u * 60u;           // 30 min
static inline uint32_t kPlaySessionLimitMs() {
  uint8_t mins = g_pet.persisted.sessionLimitMin;
  if (mins < 5)   mins = 30;     // unset → default 30 min
  if (mins > 120) mins = 120;
  return (uint32_t)mins * 60u * 1000u;
}
static constexpr uint32_t kLockoutScreenMs      =  5000;               // 5 s
static constexpr uint32_t kBedtimeAnnounceMs    =  3000;               // 3 s
static constexpr uint32_t kBedtimeSleepAnimMs   =  6000;               // 6 s
static constexpr uint32_t kBedtimeTotalMs       = kBedtimeAnnounceMs + kBedtimeSleepAnimMs;

// Safe moment to interrupt the user — don't kick them out of a mini-game,
// food collection, scene-transition cinematic or active timer alarm.
static bool isSafeToShutdown() {
  return !g_pet.activityMode &&
         !g_pet.foragingMode &&
         !g_pet.timerExpiring &&
         !g_pet.travelTransitionMode;
}

static void incrementInventory(ForageItem t) {
  uint16_t* slot = nullptr;
  switch (t) {
    case ForageItem::Apple: slot = &g_pet.persisted.apples;  break;
    case ForageItem::Berry: slot = &g_pet.persisted.berries; break;
    case ForageItem::Fish:  slot = &g_pet.persisted.fish;    break;
    default: return;
  }
  if (*slot < kFoodMaxPerType) (*slot)++;
}

// Sweep the swipe segment against alive items; flag any hit for fly-out.
static void collectAlongSwipe(int sx, int sy, int ex, int ey, uint32_t now) {
  int rsq = kForageHitR * kForageHitR;
  for (auto& it : g_forage.items) {
    if (it.type == ForageItem::None || it.flyStartMs != 0) continue;
    if (forageSegDistSq(sx, sy, ex, ey, it.x, it.y) <= rsq) {
      incrementInventory(it.type);
      it.flyStartMs = now;
      it.flyToX = inventoryIconX(it.type);
      it.flyToY = kForageInvY;
      playSound(Sound::CollectFood);
    }
  }
}

// Pull one item from inventory in apple→berry→fish order and apply the
// usual feed effect (fullness + small happiness, eat sound, eating face,
// pile counter). Returns false when the inventory is empty — callers should
// give the user a clear "out of food" reaction in that case.
static bool tryFeedFromInventory(uint32_t now, int floatFromX, int floatFromY) {
  uint16_t* slot = nullptr;
  int fullnessGain = 20;
  if (g_pet.persisted.apples > 0) {
    slot = &g_pet.persisted.apples;
    fullnessGain = 20;
  } else if (g_pet.persisted.berries > 0) {
    slot = &g_pet.persisted.berries;
    fullnessGain = 15;
  } else if (g_pet.persisted.fish > 0) {
    slot = &g_pet.persisted.fish;
    fullnessGain = 30;
  }
  if (!slot) return false;
  (*slot)--;
  changeFullness(+fullnessGain, now);
  changeHappiness(+1, now);
  spawnFloatToBar(FloatType::Apple, floatFromX, floatFromY,
                  TargetBar::Fullness, now);
  playSound(Sound::Eat);
  flashFace(Face::Eating, 1000, now);
  if (++g_pet.feedCounter >= 3) {
    g_pet.feedCounter = 0;
    spawnPile(now);
  }
  return true;
}

// ─── Listen mode (microphone-driven dance) ──────────────────────────────────

// Mic ↔ Speaker share the I2S bus on the Core2 — only one can be live at a
// time. We allocate a small buffer for level metering.
static constexpr size_t MIC_BUF_LEN = 256;     // 16 ms at 16 kHz
static int16_t g_micBuf[MIC_BUF_LEN];
static bool    g_micPending = false;

static void enterListenMode(uint32_t now) {
  if (g_pet.listenMode) return;
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  setSoundEnabled(false);
  g_pet.listenMode       = true;
  g_pet.forceSleep       = false;     // ear-tap also wakes the pet
  g_pet.lastListenLoveMs = now;
  g_pet.lastListenPeakMs = now;       // silence timer starts fresh
  g_pet.micLevelSmoothed = 0.0f;
  g_pet.micLevel         = 0;
  g_micPending           = false;
  g_motion.lastInteractionMs = now;
}

static void exitListenMode() {
  if (!g_pet.listenMode) return;
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(g_volume);
  setSoundEnabled(true);
  g_pet.listenMode = false;
}

// Polls the mic and feeds the smoothed peak amplitude into g_pet.micLevel.
// Each call processes at most one 256-sample buffer (~16 ms of audio).
static void updateMicLevel() {
  if (!g_pet.listenMode) return;
  if (M5.Mic.isRecording() != 0) return;        // previous buffer still filling

  if (g_micPending) {
    int peak = 0;
    for (size_t i = 0; i < MIC_BUF_LEN; ++i) {
      int v = g_micBuf[i];
      if (v < 0) v = -v;
      if (v > peak) peak = v;
    }
    // Smooth so the dance doesn't jitter, then expose as int.
    g_pet.micLevelSmoothed += (peak - g_pet.micLevelSmoothed) * 0.40f;
    g_pet.micLevel = (int)g_pet.micLevelSmoothed;
    // Track the last meaningful audio so we can auto-exit listen mode
    // after extended silence (saves the mic's ~30 mA when forgotten).
    constexpr int kListenSilenceThreshold = 1500;
    if (peak > kListenSilenceThreshold) {
      g_pet.lastListenPeakMs = millis();
    }
  }
  M5.Mic.record(g_micBuf, MIC_BUF_LEN, 16000, false);
  g_micPending = true;

  constexpr uint32_t kListenSilenceTimeoutMs = 90000;
  if (millis() - g_pet.lastListenPeakMs > kListenSilenceTimeoutMs) {
    Serial.println(F("[listen] 90 s of silence — auto-exit"));
    exitListenMode();
  }
}

// ─── Pet logic ───────────────────────────────────────────────────────────────

static void updateFaceFromState(uint32_t now) {
  Needs& n = g_pet.persisted.needs;

  // Listen mode pre-empts everything — pet is "alive and dancing", regardless
  // of energy/sleep state. Face follows mood (Love/Excited/Happy).
  if (g_pet.listenMode) {
    uint8_t mood = computeMood(n);
    if      (mood >= 90) setFace(Face::Love,    now);
    else if (mood >= 75) setFace(Face::Excited, now);
    else                 setFace(Face::Happy,   now);
    return;
  }

  // Voice listening: from wake word until the LLM answers. Open eyes,
  // alternate Speaking / Excited expressions ("listen → think").
  if (g_pet.voiceListening) {
    // Phase flips every 600 ms — Speaking during microphone capture,
    // Excited as an attentive pause (gives the impression of thinking).
    bool talking = ((now / 600) & 1) == 0;
    setFace(talking ? Face::Speaking : Face::Excited, now);
    return;
  }

  // Web radio: pet listens (Excited) — or is sad when the stream is stuck
  // in the error state (no WiFi, stream offline, etc.). Body sway and note
  // symbols only play in the Playing state in the renderer.
#if TARGET_HAS_WIFI
  if (g_pet.mediaActive == Media::Radio) {
    bool radioErr = (webradio::state() == webradio::State::Error);
    setFace(radioErr ? Face::Sad : Face::Excited, now);
    return;
  }
#else
  if (g_pet.mediaActive == Media::Radio) {
    setFace(Face::Excited, now);
    return;
  }
#endif

  // Explicit "send to sleep" via BtnC — wins over any motion or mood logic.
  if (g_pet.forceSleep) {
    setFace(Face::Sleeping, now);
    if (now - g_pet.lastSnoreMs > 4000) {
      g_pet.lastSnoreMs = now;
      playSound(Sound::Snore);
    }
    return;
  }

  if (n.energy < 10 || (currentPhase() == TimePhase::Night &&
                        now - g_motion.lastInteractionMs > 30000)) {
    setFace(Face::Sleeping, now);
    if (now - g_pet.lastSnoreMs > 4000) {
      g_pet.lastSnoreMs = now;
      playSound(Sound::Snore);
    }
    return;
  }
  if (n.energy < 25 || now - g_motion.lastInteractionMs > 45000) {
    Face prev = g_pet.face;
    setFace(Face::Sleepy, now);
    if (prev != Face::Sleepy && prev != Face::Sleeping) {
      playSound(Sound::Yawn);
    }
    return;
  }

  uint32_t sincePet = now - g_motion.lastStrokeEvent;
  if (sincePet < 1500) {
    uint8_t mood = computeMood(n);
    if (mood >= 90)      setFace(Face::Love, now);
    else if (mood >= 75) setFace(Face::Excited, now);
    else                 setFace(Face::Happy, now);
    return;
  }

  uint8_t mood = computeMood(n);
  if (mood < 20)       setFace(Face::Sad,  now);
  else if (mood < 50)  setFace(Face::Idle, now);
  else if (mood < 75)  setFace(Face::Happy, now);
  else                 setFace(Face::Idle, now);
}

// ─── Touch zones / gestures ──────────────────────────────────────────────────

static void onTouchZone(TouchZone z, int touchX, int touchY, uint32_t now) {
  uint32_t cooldown = (z == TouchZone::LeftEye || z == TouchZone::RightEye)
                        ? TOUCH_EYE_COOLDOWN_MS : TOUCH_ZONE_COOLDOWN_MS;
  if (now - g_pet.lastTouchZoneMs[(int)z] < cooldown) return;
  g_pet.lastTouchZoneMs[(int)z] = now;

  g_motion.lastInteractionMs = now;
  g_pet.forceSleep = false;

  switch (z) {
    case TouchZone::Forehead:
      changeHappiness(+8, now);
      changeEnergy(+1, now);                  // attentive petting energises a bit
      rememberStroke(now);
      spawnFloatToBar(FloatType::Heart, touchX, touchY, TargetBar::Happiness, now);
      // Use Happy (not Pet) — purr is naturally quiet and shared a cooldown
      // with the IMU stroke handler that often skipped the sound entirely.
      playSound(Sound::Happy);
      break;
    case TouchZone::LeftCheek:
      changeHappiness(+5, now);
      changeEnergy(+1, now);
      rememberStroke(now);
      spawnFloatToBar(FloatType::Heart, touchX, touchY, TargetBar::Happiness, now);
      playSound(Sound::Happy);
      flashFace(Face::Happy, 600, now);
      break;
    case TouchZone::RightCheek:
      // Right cheek is ticklish — laughing reaction!
      changeHappiness(+6, now);
      changeEnergy(+1, now);
      rememberStroke(now);
      spawnFloatToBar(FloatType::Heart, touchX, touchY, TargetBar::Happiness, now);
      playSound(Sound::Tickle);
      flashFace(Face::Laughing, 1000, now);
      break;
    case TouchZone::LeftEye:
    case TouchZone::RightEye:
      // No mood penalty — likely a misaim, just startle briefly.
      flashFace(Face::Startled, 400, now);
      playSound(Sound::Startle);
      break;
    case TouchZone::Mouth:
      // Feeding is gated on the inventory now — collect food via the basket
      // button first. Empty inventory → small sad flash + pulse the basket
      // button so the user knows where to go next.
      if (!tryFeedFromInventory(now, touchX, touchY)) {
        flashFace(Face::Sad, 600, now);
        g_pet.basketHintUntilMs = now + 2500;
      }
      break;
    case TouchZone::LeftEar:
      // Left ear: toggle listen mode (also serves as the exit gesture).
      if (g_pet.listenMode) exitListenMode();
      else                  enterListenMode(now);
      break;
    case TouchZone::RightEar:
      // Right ear: enter only — left ear is the canonical exit.
      if (!g_pet.listenMode) enterListenMode(now);
      break;
    case TouchZone::None:
      break;
  }
}

// ─── Touch dispatcher (different for each top-level state) ──────────────────

static void handleTouchTimeEdit(int tx, int ty, bool wasPressed) {
  if (!wasPressed) return;
  if (insideRect(tx, ty, kTimeBackRect))   { g_settings.timeEditing = false; return; }
  if (insideRect(tx, ty, kHourUpRect))     { adjustHour(+1);   return; }
  if (insideRect(tx, ty, kHourDownRect))   { adjustHour(-1);   return; }
  if (insideRect(tx, ty, kMinUpRect))      { adjustMinute(+1); return; }
  if (insideRect(tx, ty, kMinDownRect))    { adjustMinute(-1); return; }
}

static void handleTouchHelp(int tx, int ty, bool wasPressed) {
  if (!wasPressed) return;
  if (insideRect(tx, ty, cornerHitRect(kHelpBackRect))) {
    g_settings.helpOpen = false;
    g_settings.helpPage = 0;
    return;
  }
  if (g_settings.helpPage > 0 && insideRect(tx, ty, kHelpPrevRect)) {
    g_settings.helpPage--;
    return;
  }
  if (g_settings.helpPage + 1 < kHelpPageCount &&
      insideRect(tx, ty, kHelpNextRect)) {
    g_settings.helpPage++;
    return;
  }
}

static void handleTouchCredits(int tx, int ty, bool wasPressed) {
  if (!wasPressed) return;
  if (insideRect(tx, ty, cornerHitRect(kHelpBackRect))) {
    g_settings.creditsOpen = false;
  }
}

static void handleTouchLang(int tx, int ty, bool wasPressed) {
  if (!wasPressed) return;
  if (insideRect(tx, ty, kLangSelectBackRect)) {
    g_settings.langOpen = false;
    return;
  }
  for (int i = 0; i < 2; ++i) {
    if (insideRect(tx, ty, kLangChoiceRect[i])) {
      g_pet.persisted.language = (uint8_t)i;
      g_lang = (uint8_t)i;
      savePersisted(g_pet.persisted);
      g_settings.langOpen = false;
      return;
    }
  }
}

static void handleTouchSettings(int tx, int ty, bool pressed, bool wasPressed,
                                uint32_t now) {
  // Reset-confirm modal owns the screen while open — handle first so taps
  // can't bleed through to the underlying settings rows.
  if (g_settings.resetConfirmOpen) {
    if (wasPressed) {
      if (insideRect(tx, ty, kSettingsResetConfirmYesRect)) {
        // Pet factory reset — wipes the pet NVS namespace (needs, scores,
        // language choice, lockout, etc.) so first-run picker fires again.
        // WiFi credentials are deliberately preserved; clear them via
        // Settings → WLAN zuruecksetzen if needed.
        clearPersisted();
        playSound(Sound::Wake);
        delay(400);
        ESP.restart();
        return;
      }
      if (insideRect(tx, ty, kSettingsResetConfirmNoRect) ||
          insideRect(tx, ty, cornerHitRect(kSettingsBackRect))) {
        g_settings.resetConfirmOpen = false;
        return;
      }
    }
    return;
  }
  if (g_settings.wifiResetConfirmOpen) {
    if (wasPressed) {
      if (insideRect(tx, ty, kSettingsResetConfirmYesRect)) {
        // If the parent server is running, take it down first — it would
        // otherwise dangle on a network it can no longer reach.
        if (parentServerGetState() != ParentServerState::Off) {
          parentServerDisable();
          g_settings.parentServerOn = false;
        }
        wifiClearCreds();
        // Drop the location/weather cache too. Otherwise a wrong IP-geo
        // hit ("fake Trier") would survive the WiFi reset and look like a
        // valid fresh cache on the next boot.
        invalidateLocationCache();
        g_settings.wifiResetConfirmOpen = false;
        return;
      }
      if (insideRect(tx, ty, kSettingsResetConfirmNoRect) ||
          insideRect(tx, ty, cornerHitRect(kSettingsBackRect))) {
        g_settings.wifiResetConfirmOpen = false;
        return;
      }
    }
    return;
  }
  if (g_settings.helpOpen) {
    handleTouchHelp(tx, ty, wasPressed);
    return;
  }
  if (g_settings.creditsOpen) {
    handleTouchCredits(tx, ty, wasPressed);
    return;
  }
  if (g_settings.langOpen) {
    handleTouchLang(tx, ty, wasPressed);
    return;
  }
  if (g_settings.timeEditing) {
    handleTouchTimeEdit(tx, ty, wasPressed);
    return;
  }
  // Parents-help screen — only Back X / X-button closes it.
  if (g_settings.parentsHelpOpen) {
    if (wasPressed && (insideRect(tx, ty, kParentsHelpBackRect) ||
                       insideRect(tx, ty, cornerHitRect(kSettingsBackRect)))) {
      g_settings.parentsHelpOpen = false;
    }
    return;
  }
  // Location sub-page — Refresh button + Back X.
  if (g_settings.locationPageOpen) {
    if (wasPressed) {
      if (insideRect(tx, ty, kLocationBackRect) ||
          insideRect(tx, ty, cornerHitRect(kSettingsBackRect))) {
        g_settings.locationPageOpen = false;
        return;
      }
      if (insideRect(tx, ty, kLocationRefreshRect) &&
          !g_settings.locationRefreshing) {
        g_settings.locationRefreshing = true;
        g_settings.locationRefreshAtMs = now;
      }
    }
    return;
  }

  // Parent-server sub-page — toggle button + Back X.
  if (g_settings.parentServerPageOpen) {
    if (wasPressed) {
      if (insideRect(tx, ty, kParentSrvBackRect) ||
          insideRect(tx, ty, cornerHitRect(kSettingsBackRect))) {
        g_settings.parentServerPageOpen = false;
        return;
      }
      if (insideRect(tx, ty, kParentSrvToggleRect)) {
        ParentServerState st = parentServerGetState();
        if (st == ParentServerState::Off || st == ParentServerState::Failed) {
          parentServerEnable();
          g_settings.parentServerOn = true;
        } else {
          parentServerDisable();
          g_settings.parentServerOn = false;
        }
        return;
      }
    }
    return;
  }
  // WiFi setup view owns the screen when active.
  if (g_settings.wifiSetupOpen) {
    if (wasPressed) {
      WifiSetupState st = wifiSetupGetState();
      if (insideRect(tx, ty, kWifiSetupCancelRect) ||
          insideRect(tx, ty, kWifiSetupBackRect) ||
          insideRect(tx, ty, cornerHitRect(kSettingsBackRect))) {
        if (st == WifiSetupState::Success || st == WifiSetupState::Failed) {
          // Done — clean exit.
          g_settings.wifiSetupOpen = false;
          // Already torn down in tick; just reset state.
        } else {
          wifiSetupCancel();
          g_settings.wifiSetupOpen = false;
        }
        return;
      }
    }
    return;
  }
  if (g_settings.animalSelecting) {
    if (wasPressed) {
      if (insideRect(tx, ty, kAnimalBackRect)) {
        g_settings.animalSelecting = false;
        return;
      }
      for (int i = 0; i < 3; ++i) {
        if (insideRect(tx, ty, kAnimalChoiceRect[i])) {
          g_pet.persisted.animal = (uint8_t)i;
          syncSettingsToPersisted();
          savePersisted(g_pet.persisted);
          g_settings.animalSelecting = false;
          return;
        }
      }
    }
    return;
  }

  if (wasPressed) {
    if (insideRect(tx, ty, cornerHitRect(kSettingsBackRect))) {
      g_settings.open = false;
      g_settings.page = 0;       // always come back to page 0 next time
      return;
    }
    // Page-flip controls (visible on all pages — 0 → 1 → 2 → 3).
    if (g_settings.page < 3 && insideRect(tx, ty, kSettingsNextPageRect)) {
      g_settings.page++;
      return;
    }
    if (g_settings.page > 0 && insideRect(tx, ty, kSettingsPrevPageRect)) {
      g_settings.page--;
      return;
    }

    if (g_settings.page == 0) {
      // Volume is handled by the pressed slider (see below); here we
      // check taps on brightness dots / instructions / credits.
      for (int i = 0; i < 4; ++i) {
        if (insideRect(tx, ty, kBrightDotRect[i])) {
          setBrightnessLevel((uint8_t)i);
          return;
        }
      }
      if (insideRect(tx, ty, kSettingsHelpRect)) {
        g_settings.helpOpen = true;
        g_settings.helpPage = 0;
        return;
      }
      if (insideRect(tx, ty, kSettingsCreditsRect)) {
        g_settings.creditsOpen = true;
        return;
      }
    } else if (g_settings.page == 1) {
      if (insideRect(tx, ty, kSettingsLangRect)) {
        g_settings.langOpen = true;
        return;
      }
      if (insideRect(tx, ty, kSettingsTimeRect)) {
        g_settings.timeEditing = true;
        return;
      }
      if (insideRect(tx, ty, kSettingsAnimalRect)) {
        g_settings.animalSelecting = true;
        return;
      }
      if (insideRect(tx, ty, kSettingsResetRect)) {
        // Destructive — show a confirmation modal. The actual wipe runs
        // from the Yes branch in the modal handler.
        g_settings.resetConfirmOpen = true;
        return;
      }
    } else if (g_settings.page == 2) {
      if (insideRect(tx, ty, kSettingsWifiRect)) {
        // Kick off the captive-portal setup mode. The setup view takes
        // over until the user cancels or finishes.
        wifiSetupBegin();
        g_settings.wifiSetupOpen = true;
        return;
      }
      if (insideRect(tx, ty, kSettingsParentsRect)) {
        g_settings.parentsHelpOpen = true;
        return;
      }
      if (insideRect(tx, ty, kSettingsParentSrvRect)) {
        // Open the dedicated parent-server sub-page so the toggle + URL
        // get enough room.
        g_settings.parentServerPageOpen = true;
        return;
      }
      if (insideRect(tx, ty, kSettingsLocationRect)) {
        g_settings.locationPageOpen = true;
        return;
      }
      if (insideRect(tx, ty, kSettingsWifiResetRect)) {
        // Destructive — show confirmation before wiping all known WLANs.
        g_settings.wifiResetConfirmOpen = true;
        return;
      }
    } else if (g_settings.page == 3) {
      // Page 3 — companion devices. Currently just the Pip-Modus toggle.
      if (insideRect(tx, ty, kSettingsPipModeRect)) {
        bool now_on = (g_pet.persisted.pipMode == 0);
        g_pet.persisted.pipMode = now_on ? 1 : 0;
        savePersisted(g_pet.persisted);
#if TARGET_HAS_WIFI
        if (now_on) pip_link::begin(true);
        else        pip_link::end();
#endif
        return;
      }
    }
  }
  // Volume slider drag — only on page 0.
  if (g_settings.page == 0 && pressed &&
      insideRect(tx, ty, kSettingsVolumeRect)) {
    int rel = tx - kSettingsVolumeRect.x;
    if (rel < 0) rel = 0;
    if (rel > kSettingsVolumeRect.w) rel = kSettingsVolumeRect.w;
    int newVolume = ((uint32_t)rel * 255) / kSettingsVolumeRect.w;
    if (abs(newVolume - g_volume) >= 2) {
      g_volume = (uint8_t)newVolume;
      M5.Speaker.setVolume(g_volume);
#if TARGET_HAS_WIFI
      // Live-update the webradio volume too — otherwise the slider only
      // affects pet sounds, and the streaming MP3 stays at whatever
      // value was active when start() was called.
      webradio::setVolume(g_volume);
#endif
    }
  }
  (void)now;
}

static void recordTapForBurst(uint32_t now);   // defined further down

// Three touch areas along the bottom edge: Greet (left), Tickle (centre),
// Sleep (right). Positions match drawButtonHints in face.cpp. On CoreS3 these
// are the only action buttons. On Core2 they work alongside the physical
// A/B/C buttons under the display — the Tickle touch even adds an action
// that is not reachable via a hardware button (BtnB = Feed, centre touch =
// Tickle).
constexpr Rect kBtnGreetRect  = {  10, 216, 100, 24 };
constexpr Rect kBtnTickleRect = { 110, 216, 100, 24 };
constexpr Rect kBtnSleepRect  = { 210, 216, 100, 24 };

static void handleTouchPet(int tx, int ty, bool pressed, bool wasPressed,
                           uint32_t now) {
  // ── Radio-listen lock ─────────────────────────────────────────────────
  // While webradio is playing, the pet is "in listen mode" — every
  // interaction is suppressed except a tap on the Media button (which
  // still toggles the radio off). Otherwise tickling/greeting/feeding
  // etc. would yank the pet into a sound-effect animation and the user
  // experience is "the music stops every time I touch the pet".
  // Stroking (continuous `pressed` without `wasPressed`) is also gated.
  if (g_pet.mediaActive == Media::Radio) {
    if (wasPressed && insideRect(tx, ty, kMediaButtonRect)) {
      // Fall through to the existing media-toggle handling further down.
    } else {
      return;
    }
  }

  if (wasPressed) {
    // Touch buttons at the bottom of the display. On CoreS3 they replace
    // the missing A/B/C hardware buttons; on Core2 they supplement them.
    if (insideRect(tx, ty, kBtnGreetRect)) {
      g_pet.forceSleep = false;
      g_motion.lastInteractionMs = now;
      changeHappiness(+3, now);
      changeEnergy(+2, now);
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now);
      playSound(g_pet.face == Face::Sleeping ? Sound::Wake : Sound::Greet);
      return;
    }
    if (insideRect(tx, ty, kBtnTickleRect)) {
      g_pet.forceSleep = false;
      g_motion.lastInteractionMs = now;
      changeHappiness(+5, now);
      changeEnergy(+1, now);
      rememberStroke(now);
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now);
      playSound(Sound::Tickle);
      flashFace(Face::Laughing, 1000, now);
      return;
    }
    if (insideRect(tx, ty, kBtnSleepRect)) {
      g_pet.forceSleep = true;
      playSound(Sound::Yawn);
      return;
    }

    if (insideRect(tx, ty, kClockTouchRect)) {
      // Tap on the clock → open the timer screen. Doubles as an entry
      // affordance with no extra UI footprint.
      g_pet.timerScreenMode = true;
      g_pet.timerCustomEdit = false;
      return;
    }
    if (insideRect(tx, ty, cornerHitRect(kGearButtonRect))) {
      g_settings.open = true;
      return;
    }
    if (insideRect(tx, ty, kForagingButtonRect)) {
      // Enter foraging mode (exits any conflicting modes first).
      if (g_pet.listenMode) exitListenMode();
      g_pet.foragingMode = true;
      g_forage.prevTouchValid = false;
      return;
    }
    if (insideRect(tx, ty, kToyButtonRect)) {
      if (g_pet.listenMode) exitListenMode();
      g_pet.toySelectMode = true;
      return;
    }
    if (insideRect(tx, ty, kMediaButtonRect)) {
      // Toggle: stop ongoing media, otherwise open the chooser. The pet
      // recovers its needs through normal interaction once stopped.
      if (g_pet.mediaActive != Media::None) {
#if TARGET_HAS_WIFI
        // Radio braucht eigenes Teardown (Audio-Stack stoppen, WiFi-Ref
        // freigeben, Voice-Pipeline wieder aktivieren).
        if (g_pet.mediaActive == Media::Radio) webradio::stop();
#endif
        g_pet.mediaActive  = Media::None;
        g_pet.mediaStartMs = 0;
        M5.Speaker.stop();
      } else {
        if (g_pet.listenMode) exitListenMode();
        g_pet.mediaSelectMode = true;
      }
      return;
    }
    if (insideRect(tx, ty, kSportButtonRect)) {
      if (g_pet.listenMode) exitListenMode();
      g_pet.sportSelectMode = true;
      return;
    }
    if (insideRect(tx, ty, kTravelButtonRect)) {
      // Same gating semantics as the activity button — block on cooldown,
      // block on insufficient energy. The actual energy deduction + new
      // cooldown happen inside the travel-select handler when the user
      // picks a different scene.
      if (travelCooldownRemainingSec() > 0) {
        flashFace(Face::Sad, 400, now);
        return;
      }
      if (!travelHasEnoughEnergy()) {
        flashFace(Face::Sleepy, 700, now);
        return;
      }
      if (g_pet.listenMode) exitListenMode();
      g_pet.travelSelectMode = true;
      return;
    }
    {
      Scene curScene = (Scene)(g_pet.persisted.selectedScene < kSceneCount
                                 ? g_pet.persisted.selectedScene : 0);
      if (sceneHasActivity(curScene) &&
          insideRect(tx, ty, kActivityButtonRect)) {
        // Gate on cooldown first, then on energy. Both give the user
        // immediate visual feedback so they know why nothing happened.
        if (activityCooldownRemainingSec(curScene) > 0) {
          flashFace(Face::Sad, 400, now);
          return;
        }
        if (!activityHasEnoughEnergy(curScene)) {
          flashFace(Face::Sleepy, 700, now);
          return;
        }
        if (g_pet.listenMode) exitListenMode();
        // Pay the energy cost + start the cooldown immediately so a quick
        // back-out doesn't bypass the limit.
        changeEnergy(-(int)kActivityEnergyCost[(int)curScene], now);
        uint32_t e = nowEpoch();
        if (e > 0) {
          g_pet.persisted.gameCooldownEnd[(int)curScene] =
              e + kActivityCooldownSec[(int)curScene];
          savePersisted(g_pet.persisted);
        }
        g_pet.activityMode = true;
        startActivity(curScene, now);
        g_activity.playsInSession = 1;     // first run of this session
        return;
      }
    }
    if (g_pet.pileCount > 0 && insideRect(tx, ty, kCleaningButtonRect)) {
      // Enter cleaning mode (exits any conflicting modes first).
      if (g_pet.listenMode) exitListenMode();
      g_pet.cleaningMode = true;
      g_pet.cleanedAtMs  = 0;
      return;
    }
    TouchZone z = classifyTouchZone(tx, ty, /*sliderVisible=*/false,
                                    (int)g_pet.wander.x, (int)g_pet.wander.y);
    if (z != TouchZone::None) {
      onTouchZone(z, tx, ty, now);
      return;
    }
    // Background tap (outside any UI button and outside the pet itself) —
    // counts toward the rapid-tap burst that fires the hop chain.
    recordTapForBurst(now);
  }
  (void)pressed;
}

// ─── Timer ──────────────────────────────────────────────────────────────────

static void startTimerMinutes(uint32_t mins, uint32_t now) {
  if (mins == 0) return;
  uint32_t dur = mins * 60u * 1000u;
  g_pet.timerActive     = true;
  g_pet.timerStartMs    = now;
  g_pet.timerDurationMs = dur;
  g_pet.timerEndMs      = now + dur;
  g_pet.timerExpiring   = false;
}

static void startTimerCustom(uint32_t now) {
  uint32_t total = (uint32_t)g_pet.customMinutes * 60u + g_pet.customSeconds;
  if (total == 0) return;
  uint32_t dur = total * 1000u;
  g_pet.timerActive     = true;
  g_pet.timerStartMs    = now;
  g_pet.timerDurationMs = dur;
  g_pet.timerEndMs      = now + dur;
  g_pet.timerExpiring   = false;
}

static void stopTimer() {
  g_pet.timerActive     = false;
  g_pet.timerEndMs      = 0;
  g_pet.timerDurationMs = 0;
  g_pet.timerExpiring   = false;
}

static void dismissTimerExpiry() {
  g_pet.timerExpiring   = false;
  g_pet.timerExpiredAtMs = 0;
  g_pet.timerExpiredPlays = 0;
  M5.Speaker.stop();
}

static void handleTouchTimerCustom(int tx, int ty, bool wasPressed,
                                   uint32_t now) {
  if (!wasPressed) return;
  if (insideRect(tx, ty, kTimerCustomBackRect)) {
    g_pet.timerCustomEdit = false;          // back to preset list
    return;
  }
  if (insideRect(tx, ty, kTimerCustomMinRect)) {
    g_pet.customMinutes++;
    if (g_pet.customMinutes > 99) g_pet.customMinutes = 0;
    return;
  }
  if (insideRect(tx, ty, kTimerCustomSecRect)) {
    g_pet.customSeconds += 10;
    if (g_pet.customSeconds >= 60) g_pet.customSeconds = 0;
    return;
  }
  if (insideRect(tx, ty, kTimerCustomResetRect)) {
    g_pet.customMinutes = 0;
    g_pet.customSeconds = 0;
    return;
  }
  if (insideRect(tx, ty, kTimerCustomStartRect)) {
    if (g_pet.customMinutes == 0 && g_pet.customSeconds == 0) return;
    startTimerCustom(now);
    g_pet.timerCustomEdit = false;
    g_pet.timerScreenMode = false;          // back to pet view
    return;
  }
}

static void handleTouchTimer(int tx, int ty, bool wasPressed, uint32_t now) {
  if (g_pet.timerCustomEdit) {
    handleTouchTimerCustom(tx, ty, wasPressed, now);
    return;
  }
  if (!wasPressed) return;
  if (insideRect(tx, ty, kTimerBackRect)) {
    g_pet.timerScreenMode = false;
    return;
  }
  if (g_pet.timerActive) {
    if (insideRect(tx, ty, kTimerStopRect)) {
      stopTimer();
      g_pet.timerScreenMode = false;
    }
    return;
  }
  for (int i = 0; i < 6; ++i) {
    if (!insideRect(tx, ty, kTimerPresetRect[i])) continue;
    if (i == 5) {
      g_pet.timerCustomEdit = true;
      return;
    }
    startTimerMinutes((uint32_t)kTimerPresetMinutes[i], now);
    g_pet.timerScreenMode = false;     // close immediately, return to pet view
    return;
  }
}

static void handleTouchTravelSelect(int tx, int ty, bool pressed, bool wasPressed,
                                    uint32_t now) {
  if (!wasPressed) { (void)pressed; (void)now; return; }
  if (insideRect(tx, ty, kTravelSelectBackRect)) {
    g_pet.travelSelectMode = false;
    return;
  }
  for (int i = 0; i < kSceneCount; ++i) {
    if (insideRect(tx, ty, kTravelChoiceRect[i])) {
      // Tapping the current scene is a no-op (no cost, no cooldown).
      bool sameScene = (g_pet.persisted.selectedScene == (uint8_t)i);
      if (!sameScene) {
        // Pay the energy cost + start a fresh random cooldown so the user
        // can't bounce back and forth between scenes.
        changeEnergy(-(int)kTravelEnergyCost, now);
        uint32_t e = nowEpoch();
        if (e > 0) {
          g_pet.persisted.travelCooldownEnd = e + pickTravelCooldownSec(now);
        }
        // Cinematic 8-second transition with a scene-appropriate vehicle.
        g_pet.travelTransitionMode    = true;
        g_pet.travelTransitionStartMs = now;
        g_pet.travelTransitionTo      = (Scene)i;
        // Pet giggles once as the trip kicks off — same sample as the
        // ticklish-cheek reaction. Plays for every vehicle, only at start.
        playSound(Sound::Tickle);
      }
      g_pet.persisted.selectedScene = (uint8_t)i;
      savePersisted(g_pet.persisted);
      g_pet.travelSelectMode = false;       // tap-to-pick auto-closes
      return;
    }
  }
  (void)pressed;
}

static void handleTouchMediaSelect(int tx, int ty, bool pressed, bool wasPressed,
                                   uint32_t now) {
  if (!wasPressed) { (void)pressed; (void)now; return; }
  if (insideRect(tx, ty, cornerHitRect(kMediaSelectBackRect))) {
    g_pet.mediaSelectMode = false;
    return;
  }
  // Reihenfolge muss zu kMediaChoiceRect[] in face.h passen — sonst
  // landet ein Camera-/Gallery-Tap im falschen Branch.
#if TARGET_HAS_WIFI && TARGET_HAS_CAMERA
  static const Media types[8] = {
    Media::Movies, Media::Games, Media::Internet, Media::Social,
    Media::Friends, Media::Radio, Media::Camera, Media::Gallery,
  };
#elif TARGET_HAS_WIFI
  static const Media types[6] = {
    Media::Movies, Media::Games, Media::Internet, Media::Social,
    Media::Friends, Media::Radio,
  };
#else
  static const Media types[5] = {
    Media::Movies, Media::Games, Media::Internet, Media::Social,
    Media::Friends,
  };
#endif
  static_assert(sizeof(types) / sizeof(types[0]) == kMediaChoiceCount,
                "types[] must match kMediaChoiceCount cells in face.h");
  for (int i = 0; i < kMediaChoiceCount; ++i) {
    if (insideRect(tx, ty, kMediaChoiceRect[i])) {
      if (types[i] == Media::Friends) {
        g_pet.mediaSelectMode      = false;
        g_pet.friendsMode          = true;
        g_pet.friendsModeStartMs   = now;
        g_pet.friendsSentCount     = 0;
        g_pet.friendsLastTapMs     = 0;
        g_pet.friendsLastSentKind  = 0;
        g_pet.friendsBothDoneAtMs  = 0;
        friendsBegin((uint8_t)(g_pet.persisted.animal & 3),
                     (uint8_t)g_pet.persisted.language);
        return;
      }
#if TARGET_HAS_WIFI
      if (types[i] == Media::Radio) {
        // No WiFi credentials stored → can't start radio. Show a brief
        // "Brauche WLAN" overlay and bounce back to the chooser, the
        // user can then go to Settings → WiFi-Setup. We only check
        // creds existence here, not actual connectivity — that takes
        // 5–10 s and would feel like a freeze. If creds are there but
        // the AP is unreachable, the existing connect-timeout path
        // surfaces an Error face after ~10 s.
        if (!wifiHasCreds()) {
          drawNeedsWifiOverlay(g_canvas);
          g_canvas.pushSprite(0, 0);
          M5.delay(1800);
          g_pet.mediaSelectMode = false;
          return;
        }
        // Start web radio — takes over I2S, pauses the voice pipeline,
        // grabs the WiFi keep-alive slot. End any other media mode that
        // may be active (movies / games / social etc.).
        g_pet.mediaActive   = Media::Radio;
        g_pet.mediaStartMs  = now;
        g_pet.lastMediaHapDecayMs  = now;
        g_pet.lastMediaEngDecayMs  = now;
        g_pet.lastMediaFullDecayMs = now;
        g_pet.lastMediaSoundMs     = 0;
        g_pet.mediaSelectMode = false;
        // Show the "Connecting..." overlay immediately — webradio::start
        // can block for up to 5 s (WiFi reconnect via wifiKeepAlive). The
        // render loop doesn't run during that time; without this synchronous
        // update the user would just see a frozen screen after the tap.
        drawRadioConnectingOverlay(g_canvas);
        g_canvas.pushSprite(0, 0);
        // Push the user's current volume slider into the audio library
        // before starting the stream. Otherwise the radio uses the
        // hardcoded webradio default (200/255 ≈ 80 %) and the settings
        // slider has no effect — only restored on the next setVolume()
        // call.
        webradio::setVolume(g_volume);
        webradio::start((uint8_t)g_pet.persisted.language);
        return;
      }
#endif
#if TARGET_HAS_CAMERA
      if (types[i] == Media::Camera) {
        // Open live preview. Briefly pause face-detect — the sensor is
        // about to be switched over for capture.
        g_pet.mediaSelectMode  = false;
        g_pet.cameraMode       = true;
        g_pet.cameraEnteredMs  = now;
        g_pet.cameraFlashActive = false;
        g_pet.cameraSaveOverlay = false;
        face_detect::setEnabled(false);
        return;
      }
      if (types[i] == Media::Gallery) {
        g_pet.mediaSelectMode = false;
        g_pet.galleryMode     = true;
        g_pet.galleryViewing  = false;   // Start: Thumb-Grid
        g_pet.galleryIdx      = 0;
        g_pet.galleryDeleteConfirm = false;
        return;
      }
#endif
      g_pet.mediaActive   = types[i];
      g_pet.mediaStartMs  = now;
      g_pet.lastMediaHapDecayMs   = now;
      g_pet.lastMediaEngDecayMs   = now;
      g_pet.lastMediaFullDecayMs  = now;
      g_pet.lastMediaSoundMs      = 0;     // play immediately on first tick
      g_pet.mediaSelectMode = false;
      return;
    }
  }
  (void)pressed;
}

// ─── Sport mode ───────────────────────────────────────────────────────

static void startSportWorkout(uint8_t exercise, uint32_t now) {
  g_pet.sportSelectMode    = false;
  g_pet.sportWorkoutMode   = true;
  g_pet.sportDoneMode      = false;
  g_pet.sportExercise      = exercise;
  g_pet.sportPhase         = 0;          // intro / safety hint
  g_pet.sportPhaseStartMs  = now;
  g_pet.sportRepCount      = 0;
  g_pet.sportTargetReps    = (exercise == 0) ? 10 :
                             (exercise == 1) ?  8 : 30;
  g_pet.sportMotionPhase   = 0;
  g_pet.sportLastRepMs     = 0;
  g_pet.sportYogaStillStartMs = 0;
  g_pet.sportYogaLastTickMs   = 0;
}

static void exitSportEntirely() {
  g_pet.sportSelectMode  = false;
  g_pet.sportWorkoutMode = false;
  g_pet.sportDoneMode    = false;
}

static void completeSportWorkout(uint32_t now) {
  // Reward.
  changeEnergy(+30, now);
  changeHappiness(+20, now);
  // Streak update — increment when we hit a different calendar day, OR
  // start at 1 the first time. Reset to 1 if the gap is more than 1 day.
  WallDate today = currentDate();
  if (today.year != 0) {
    WallDate& last = g_pet.persisted.sportLastDay;
    if (last.year == 0) {
      g_pet.persisted.sportStreakDays = 1;
    } else {
      int delta = daysBetween(last, today);
      if (delta == 0) {
        // Same day — keep streak.
      } else if (delta == 1) {
        g_pet.persisted.sportStreakDays++;
      } else {
        g_pet.persisted.sportStreakDays = 1;
      }
    }
    g_pet.persisted.sportLastDay = today;
  }
  g_pet.persisted.sportTotalReps += g_pet.sportRepCount;
  savePersisted(g_pet.persisted);
  vibratePulse(now, 700);
  playSound(Sound::Excited);
  g_pet.sportWorkoutMode = false;
  g_pet.sportDoneMode    = true;
}

static void handleTouchSportSelect(int tx, int ty, bool /*pressed*/,
                                   bool wasPressed, uint32_t now) {
  if (!wasPressed) return;
  if (insideRect(tx, ty, kSportSelectBackRect)) {
    exitSportEntirely();
    return;
  }
  for (int i = 0; i < 3; ++i) {
    if (insideRect(tx, ty, kSportSelectChoice[i])) {
      startSportWorkout((uint8_t)i, now);
      return;
    }
  }
}

static void handleTouchSportWorkout(int tx, int ty, bool /*pressed*/,
                                    bool wasPressed, uint32_t /*now*/) {
  if (!wasPressed) return;
  if (insideRect(tx, ty, kSportWorkoutBackRect)) {
    // Cancel — no penalty, no reward.
    exitSportEntirely();
  }
}

static void handleTouchSportDone(int tx, int ty, bool /*pressed*/,
                                 bool wasPressed, uint32_t /*now*/) {
  if (!wasPressed) return;
  if (insideRect(tx, ty, kSportDoneOkRect) ||
      insideRect(tx, ty, kSportSelectBackRect)) {
    exitSportEntirely();
  }
}

// IMU-based rep detection — runs every loop iteration while the workout
// is active. Plays vibration + sound on each rep.
static void tickSportDetection(uint32_t now) {
  if (!g_pet.sportWorkoutMode) return;

  constexpr uint32_t kIntroMs = 1800;
  if (g_pet.sportPhase == 0) {
    if (now - g_pet.sportPhaseStartMs >= kIntroMs) {
      g_pet.sportPhase = 1;
      g_pet.sportPhaseStartMs = now;
    }
    return;
  }

  // Done check happens at end after rep update.
  bool repFired = false;

  if (g_pet.sportExercise == 0) {
    // Squat — gz oscillation between high (~+1.0 standing) and lower
    // (~+0.8 crouched). Crossing thresholds with hysteresis.
    float gz = g_motion.gz;
    if (g_pet.sportMotionPhase == 0) {
      // Currently UP — wait for DOWN.
      if (gz < 0.85f && (now - g_pet.sportLastRepMs) > 600) {
        g_pet.sportMotionPhase = 1;
      }
    } else {
      // Currently DOWN — wait for UP, that completes a rep.
      if (gz > 0.95f && (now - g_pet.sportLastRepMs) > 600) {
        g_pet.sportMotionPhase = 0;
        g_pet.sportRepCount++;
        g_pet.sportLastRepMs = now;
        repFired = true;
      }
    }
  } else if (g_pet.sportExercise == 1) {
    // Jump — freefall (raw |a| dips toward 0) followed by landing
    // (raw |a| spikes above ~1.4 g). Earlier code read g_motion.gx/gy/gz
    // (the gravity EMA, which always has magnitude ~1 g) and so the
    // freefall + spike thresholds were never crossed → reps never
    // counted. We now consume g_motion.rawMag, populated each frame in
    // updateMotion from the raw accel before any EMA smoothing.
    float mag = g_motion.rawMag;
    if (g_pet.sportMotionPhase == 0) {
      // Wait for freefall.
      if (mag < 0.5f) g_pet.sportMotionPhase = 1;
    } else {
      // Saw freefall — wait for landing spike.
      if (mag > 1.4f && (now - g_pet.sportLastRepMs) > 500) {
        g_pet.sportMotionPhase = 0;
        g_pet.sportRepCount++;
        g_pet.sportLastRepMs = now;
        repFired = true;
      } else if (now - g_pet.sportLastRepMs > 3000) {
        // Lost the freefall context — reset.
        g_pet.sportMotionPhase = 0;
      }
    }
  } else {
    // Yoga — raw |a| within ±0.1 g of 1 g for 1 s = 1 s counted toward
    // the goal. Same earlier bug as the Jump branch: previous code
    // computed mag from the gravity EMA which is unit-magnitude by
    // construction (always inside the band) and therefore granted reps
    // even while the user was actively moving.
    float mag = g_motion.rawMag;
    bool still = (mag > 0.92f && mag < 1.08f);
    if (still) {
      if (g_pet.sportYogaStillStartMs == 0) {
        g_pet.sportYogaStillStartMs = now;
        g_pet.sportYogaLastTickMs   = now;
      }
      // Tick once per second of accumulated stillness.
      if (now - g_pet.sportYogaLastTickMs >= 1000) {
        g_pet.sportYogaLastTickMs = now;
        g_pet.sportRepCount++;
        repFired = true;
      }
    } else {
      // Movement breaks the chain (small grace before reset).
      if (now - g_pet.sportYogaLastTickMs > 700) {
        g_pet.sportYogaStillStartMs = 0;
      }
    }
  }

  if (repFired) {
    vibratePulse(now, 80);
    if ((g_pet.sportRepCount % 5) == 0) {
      playSound(Sound::GooGoo);
    }
  }
  if (g_pet.sportRepCount >= g_pet.sportTargetReps) {
    completeSportWorkout(now);
  }
}

static void handleTouchToySelect(int tx, int ty, bool pressed, bool wasPressed,
                                 uint32_t now) {
  if (!wasPressed) { (void)pressed; (void)now; return; }
  if (insideRect(tx, ty, kToySelectBackRect)) {
    g_pet.toySelectMode = false;
    savePersisted(g_pet.persisted);
    return;
  }
  for (int i = 0; i < kToyCount; ++i) {
    if (insideRect(tx, ty, kToyChoiceRect[i])) {
      // Picking a different toy resets the boredom counter — that's the
      // whole point of having a selector.
      if ((Toy)i != (Toy)g_pet.persisted.selectedToy) {
        g_pet.persisted.toyRepeatCount = 0;
      }
      g_pet.persisted.selectedToy = (uint8_t)i;
      savePersisted(g_pet.persisted);
      return;
    }
  }
  (void)pressed;
}

static void handleTouchForaging(int tx, int ty, bool pressed, bool wasPressed,
                                uint32_t now) {
  if (wasPressed && insideRect(tx, ty, kForagingBackRect)) {
    g_pet.foragingMode = false;
    savePersisted(g_pet.persisted);          // persist newly-collected food
    g_forage.prevTouchValid = false;
    return;
  }
  if (pressed) {
    int16_t sx = g_forage.prevTouchValid ? g_forage.prevTouchX : (int16_t)tx;
    int16_t sy = g_forage.prevTouchValid ? g_forage.prevTouchY : (int16_t)ty;
    collectAlongSwipe(sx, sy, tx, ty, now);
    g_forage.prevTouchX     = (int16_t)tx;
    g_forage.prevTouchY     = (int16_t)ty;
    g_forage.prevTouchValid = true;
  }
}

static void handleTouchCleaning(int tx, int ty, bool pressed, bool wasPressed,
                                uint32_t now) {
  // Back button exits regardless of pile state.
  if (wasPressed && insideRect(tx, ty, kCleaningBackRect)) {
    g_pet.cleaningMode = false;
    return;
  }
  // Sweep removal — every frame the finger is down, knock out anything
  // under the touch point.
  if (pressed) {
    if (removePilesAt(tx, ty, 22)) {
      if (g_pet.pileCount == 0 && g_pet.cleanedAtMs == 0) {
        g_pet.cleanedAtMs = now;
      }
    }
  }
  // Auto-exit a moment after the screen reaches "Sauber!".
  if (g_pet.cleanedAtMs != 0 && now - g_pet.cleanedAtMs > 1800) {
    g_pet.cleaningMode = false;
    g_pet.cleanedAtMs  = 0;
  }
}

// ─── Camera + Gallery (cores3 + visu) ──────────────────────────────────────
//
// Pet-Selfie: Live-Preview der Frontkamera mit kleinem Pet-Sprite in der
// linken unteren Ecke, Tap = Aufnahme. Gespeicherte JPEGs liegen im
// LittleFS, max 5, Round-Robin. Galerie zeigt sie der Reihe nach
// (neueste zuerst), mit Tap-Navigation links/rechts und Delete-Button.

#if TARGET_HAS_CAMERA

constexpr Rect kPhotoBackRect    = { 280,   4, 36, 32 };   // gallery + camera
constexpr Rect kPhotoDeleteRect  = { 270, 200, 44, 36 };   // gallery full view
constexpr Rect kPhotoYesRect     = {  60, 110, 90, 40 };   // delete-confirm
constexpr Rect kPhotoNoRect      = { 170, 110, 90, 40 };
constexpr int  kPetOverlayCx     = 40;                     // bottom-left corner
constexpr int  kPetOverlayCy     = 200;
constexpr int  kPetOverlayR      = 28;
// Shutter button: round trigger on the right edge, vertically centred —
// so the finger doesn't cover the camera lens (top centre) when tapping.
constexpr int  kShutterCx        = 288;
constexpr int  kShutterCy        = 140;
constexpr int  kShutterR         = 28;
// Bounding rect for touch (circle hit-test via dx² + dy² <= R²).
constexpr Rect kShutterHitRect   = { kShutterCx - kShutterR - 4,
                                     kShutterCy - kShutterR - 4,
                                     2 * (kShutterR + 4),
                                     2 * (kShutterR + 4) };
// Thumb grid: 3 on top, 2 below, each 80x60 (JPEG_DIV_4 resolution).
constexpr int  kThumbW           = 80;
constexpr int  kThumbH           = 60;
constexpr Rect kThumbRects[5]    = {
    {  20, 50, kThumbW, kThumbH },
    { 120, 50, kThumbW, kThumbH },
    { 220, 50, kThumbW, kThumbH },
    {  70,140, kThumbW, kThumbH },
    { 170,140, kThumbW, kThumbH },
};

constexpr uint32_t kCameraFlashMs        = 200;
constexpr uint32_t kCameraSaveOverlayMs  = 1500;
constexpr uint32_t kCameraRewardCdMs     = 60000;
constexpr uint32_t kGalleryRewardCdMs    = 60000;

static bool insideCircle(int x, int y, int cx, int cy, int r) {
  int dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

// Cleanly exits camera mode (sensor back, face-detect on).
static void exitCameraMode() {
  g_pet.cameraMode          = false;
  g_pet.cameraFlashActive   = false;
  g_pet.cameraSaveOverlay   = false;
  face_detect::setEnabled(true);
}
static void exitGalleryMode() {
  g_pet.galleryMode           = false;
  g_pet.galleryViewing        = false;
  g_pet.galleryDeleteConfirm  = false;
}

// When opening a full-size image: pet gets Love+ if its cooldown is up.
static void rewardGalleryView(uint32_t now) {
  if (now - g_pet.lastGalleryRewardMs < kGalleryRewardCdMs) return;
  g_pet.lastGalleryRewardMs = now;
  changeHappiness(+5, now);
  spawnFloatToBar(FloatType::Heart, 160, 120,
                  TargetBar::Happiness, now);
  playSound(Sound::Love);
  flashFace(Face::Love, 800, now);
}

static void handleTouchCameraMode(int tx, int ty, bool pressed,
                                  bool wasPressed, uint32_t now) {
  (void)pressed;
  if (!wasPressed) return;
  // Block everything during the save overlay — the action is still running.
  if (g_pet.cameraSaveOverlay) return;
  // Back-X (top right) closes without taking a photo.
  if (insideRect(tx, ty, cornerHitRect(kPhotoBackRect))) {
    exitCameraMode();
    return;
  }
  // Shutter button (circle, bottom centre).
  if (!insideRect(tx, ty, kShutterHitRect) ||
      !insideCircle(tx, ty, kShutterCx, kShutterCy, kShutterR + 4)) {
    return;   // Tap outside: ignore — no accidental shutter release.
  }
  // Capture. Short flash + grab JPEG + save. captureJpeg() does a camera
  // reinit (~400 ms); the UI is locked during the save overlay.
  g_pet.cameraFlashActive  = true;
  g_pet.cameraFlashStartMs = now;

  uint8_t* jpg = nullptr;
  size_t   len = 0;
  bool ok = face_detect::captureJpeg(&jpg, &len);
  if (ok && jpg && len > 0) {
    ok = photo_store::save(jpg, len);
  }
  free(jpg);

  g_pet.cameraSaveOverlay        = true;
  g_pet.cameraSaveOverlayUntilMs = now + kCameraSaveOverlayMs;

  // Reward only if (a) actually saved and (b) cooldown is up —
  // prevents rapid spam-tapping from constantly feeding the pet.
  if (ok && now - g_pet.lastCameraRewardMs >= kCameraRewardCdMs) {
    g_pet.lastCameraRewardMs = now;
    changeHappiness(+8, now);
    spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                    TargetBar::Happiness, now);
    playSound(Sound::Love);
    flashFace(Face::Love, kCameraSaveOverlayMs, now);
  }
}

static void handleTouchGalleryMode(int tx, int ty, bool pressed,
                                   bool wasPressed, uint32_t now) {
  (void)pressed;
  if (!wasPressed) return;
  uint8_t total = photo_store::count();

  // Delete-confirm modal (only reachable in full-size view).
  if (g_pet.galleryDeleteConfirm) {
    if (insideRect(tx, ty, kPhotoYesRect)) {
      photo_store::deleteByDisplayIndex(g_pet.galleryIdx);
      uint8_t left = photo_store::count();
      g_pet.galleryDeleteConfirm = false;
      if (left == 0) {
        // Nothing left → exit completely.
        exitGalleryMode();
        return;
      }
      // Back to the thumb grid, since the current image is gone.
      g_pet.galleryViewing = false;
      if (g_pet.galleryIdx >= left) g_pet.galleryIdx = left - 1;
      return;
    }
    if (insideRect(tx, ty, kPhotoNoRect)) {
      g_pet.galleryDeleteConfirm = false;
      return;
    }
    return;
  }

  if (g_pet.galleryViewing) {
    // Full-size mode.
    if (insideRect(tx, ty, cornerHitRect(kPhotoBackRect))) {
      // Back: return to the thumb grid (not all the way out).
      g_pet.galleryViewing = false;
      return;
    }
    if (insideRect(tx, ty, kPhotoDeleteRect) && total > 0) {
      g_pet.galleryDeleteConfirm = true;
      return;
    }
    return;
  }

  // Thumb-Grid-Modus.
  if (insideRect(tx, ty, cornerHitRect(kPhotoBackRect))) {
    exitGalleryMode();
    return;
  }
  for (uint8_t i = 0; i < total && i < 5; ++i) {
    if (insideRect(tx, ty, kThumbRects[i])) {
      g_pet.galleryIdx     = i;
      g_pet.galleryViewing = true;
      rewardGalleryView(now);
      return;
    }
  }
}

// Small pet sprite in the photo overlay (bear head, circular). Shown in
// the camera live preview and over gallery images as a selfie accent.
static void drawPetOverlaySmall(int cx, int cy) {
  auto& d = g_canvas;
  uint16_t fur     = d.color565(180, 140, 110);
  uint16_t furDark = d.color565(120,  90,  70);
  uint16_t snout   = d.color565(245, 220, 200);
  uint16_t white   = d.color565(255, 255, 255);
  uint16_t dark    = d.color565( 30,  20,  20);
  // Semi-transparent shadow as an accent.
  d.fillCircle(cx + 1, cy + 2, kPetOverlayR + 1, d.color565(40, 30, 20));
  // Ears
  d.fillCircle(cx - 18, cy - 18, 8, fur);
  d.fillCircle(cx + 18, cy - 18, 8, fur);
  d.fillCircle(cx - 18, cy - 18, 4, furDark);
  d.fillCircle(cx + 18, cy - 18, 4, furDark);
  // Head
  d.fillCircle(cx, cy, kPetOverlayR, fur);
  // Snout
  d.fillEllipse(cx, cy + 8, 12, 8, snout);
  d.fillCircle (cx, cy + 4, 3, dark);
  // Eyes
  d.fillCircle(cx - 9, cy - 4, 3, white);
  d.fillCircle(cx + 9, cy - 4, 3, white);
  d.fillCircle(cx - 9, cy - 3, 1, dark);
  d.fillCircle(cx + 9, cy - 3, 1, dark);
}

// Draw the live preview + pet overlay + UI.
static void drawCameraScreen(uint32_t now) {
  // Grab the live frame (RGB565 QVGA) and push it 1:1 onto the canvas.
  // If the camera isn't ready yet, just a black background with a hint.
  face_detect::FrameView fv{};
  if (face_detect::acquireFrame(&fv) &&
      fv.data && fv.width == 320 && fv.height == 240) {
    g_canvas.pushImage(0, 0, 320, 240, (const uint16_t*)fv.data);
    face_detect::releaseFrame(fv);
  } else {
    g_canvas.fillSprite(g_canvas.color565(8, 8, 12));
  }
  // Pet overlay, bottom-left.
  drawPetOverlaySmall(kPetOverlayCx, kPetOverlayCy);

  // Shutter button (bottom centre): white double ring + red inner circle,
  // clearly recognisable as the trigger.
  {
    uint16_t white  = g_canvas.color565(245, 245, 245);
    uint16_t shadow = g_canvas.color565( 20,  20,  30);
    uint16_t inner  = g_canvas.color565(220,  60,  60);
    g_canvas.fillCircle(kShutterCx + 1, kShutterCy + 2, kShutterR + 2, shadow);
    g_canvas.fillCircle(kShutterCx, kShutterCy, kShutterR + 2, white);
    g_canvas.fillCircle(kShutterCx, kShutterCy, kShutterR - 2, shadow);
    g_canvas.fillCircle(kShutterCx, kShutterCy, kShutterR - 5, inner);
  }

  // Back-X.
  {
    const Rect& r = kPhotoBackRect;
    uint16_t panel = g_canvas.color565(240, 240, 240);
    uint16_t glyph = g_canvas.color565( 60,  50,  50);
    g_canvas.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    g_canvas.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    g_canvas.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    g_canvas.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
  }

  // Flash overlay right after the shutter trigger.
  if (g_pet.cameraFlashActive &&
      now - g_pet.cameraFlashStartMs < kCameraFlashMs) {
    uint8_t a = (uint8_t)(255 - 255 * (now - g_pet.cameraFlashStartMs) /
                                kCameraFlashMs);
    (void)a;   // We flash a hard white, no alpha — M5GFX doesn't support
               //   that here without extra work.
    g_canvas.fillSprite(g_canvas.color565(255, 255, 255));
  } else if (g_pet.cameraFlashActive) {
    g_pet.cameraFlashActive = false;
  }

  // Save overlay after capture.
  if (g_pet.cameraSaveOverlay) {
    if ((int32_t)(g_pet.cameraSaveOverlayUntilMs - now) > 0) {
      uint16_t panel = g_canvas.color565(220, 250, 220);
      uint16_t glyph = g_canvas.color565( 30,  90,  40);
      g_canvas.fillRoundRect(60, 90, 200, 60, 10, panel);
      g_canvas.drawRoundRect(60, 90, 200, 60, 10, glyph);
      g_canvas.setTextDatum(middle_center);
      g_canvas.setTextSize(2);
      g_canvas.setTextColor(glyph);
      g_canvas.drawString(tr(Str::CameraSaved), 160, 120);
      g_canvas.setTextDatum(top_left);
    } else {
      // Overlay time expired → back to the pet view.
      exitCameraMode();
    }
  }
}

// Draw the gallery — either thumb grid or full-size view.
static void drawGalleryScreen(uint32_t /*now*/) {
  g_canvas.fillSprite(g_canvas.color565(15, 15, 25));
  uint8_t total = photo_store::count();
  uint16_t white = g_canvas.color565(240, 240, 240);
  uint16_t glyph = g_canvas.color565( 60,  50,  50);

  // ── Full-size mode ──────────────────────────────────────────────────────
  if (g_pet.galleryViewing && total > 0) {
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (photo_store::readByDisplayIndex(g_pet.galleryIdx, &data, &len)) {
      g_canvas.drawJpg(data, len, 0, 0);
    } else {
      g_canvas.fillSprite(g_canvas.color565(60, 30, 30));
      g_canvas.setTextDatum(middle_center);
      g_canvas.setTextSize(2);
      g_canvas.setTextColor(white);
      g_canvas.drawString("?!", 160, 120);
      g_canvas.setTextDatum(top_left);
    }
    // Pet overlay, bottom-left — happy that the photo is being looked at.
    drawPetOverlaySmall(kPetOverlayCx, kPetOverlayCy);
    // Counter, top-left.
    char buf[8];
    snprintf(buf, sizeof(buf), "%u/%u",
             (unsigned)(g_pet.galleryIdx + 1), (unsigned)total);
    g_canvas.fillRoundRect(8, 8, 48, 22, 4, g_canvas.color565(20, 20, 30));
    g_canvas.setTextDatum(middle_center);
    g_canvas.setTextSize(2);
    g_canvas.setTextColor(white);
    g_canvas.drawString(buf, 8 + 24, 8 + 11);
    g_canvas.setTextDatum(top_left);
    // Trash button, bottom-right.
    {
      const Rect& r = kPhotoDeleteRect;
      uint16_t panel = g_canvas.color565(220, 60, 60);
      g_canvas.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
      g_canvas.drawRoundRect(r.x, r.y, r.w, r.h, 6, white);
      int cx = r.x + r.w/2, cy = r.y + r.h/2;
      g_canvas.fillRect(cx - 9, cy - 8, 18, 2, white);
      g_canvas.drawRect(cx - 7, cy - 6, 14, 14, white);
      g_canvas.drawFastVLine(cx - 3, cy - 4, 10, white);
      g_canvas.drawFastVLine(cx + 3, cy - 4, 10, white);
    }
  }
  // ── Thumb grid ──────────────────────────────────────────────────────────
  else {
    if (total == 0) {
      g_canvas.setTextDatum(middle_center);
      g_canvas.setTextSize(2);
      g_canvas.setTextColor(white);
      g_canvas.drawString(tr(Str::GalleryEmpty), 160, 120);
      g_canvas.setTextDatum(top_left);
    } else {
      // Header at the top (counter "N/5").
      char buf[12];
      snprintf(buf, sizeof(buf), "%u/%u", (unsigned)total,
               (unsigned)photo_store::kMaxPhotos);
      g_canvas.fillRoundRect(8, 8, 48, 22, 4, g_canvas.color565(20, 20, 30));
      g_canvas.setTextDatum(middle_center);
      g_canvas.setTextSize(2);
      g_canvas.setTextColor(white);
      g_canvas.drawString(buf, 8 + 24, 8 + 11);
      g_canvas.setTextDatum(top_left);

      // Thumbnails (3 on top, 2 below). drawJpg scale 0.25 → 80x60
      // from a 320x240 source.
      for (uint8_t i = 0; i < photo_store::kMaxPhotos; ++i) {
        const Rect& r = kThumbRects[i];
        if (i < total) {
          const uint8_t* tdata = nullptr;
          size_t tlen = 0;
          if (photo_store::readByDisplayIndex(i, &tdata, &tlen)) {
            g_canvas.drawJpg(tdata, tlen, r.x, r.y, r.w, r.h,
                             0, 0, 0.25f, 0.25f);
          } else {
            g_canvas.fillRect(r.x, r.y, r.w, r.h,
                              g_canvas.color565(60, 30, 30));
          }
          g_canvas.drawRect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, white);
        } else {
          // Empty slot — dotted frame with a small dash in the middle.
          g_canvas.drawRect(r.x, r.y, r.w, r.h,
                            g_canvas.color565( 80,  80, 100));
          g_canvas.drawFastHLine(r.x + r.w/2 - 6, r.y + r.h/2, 12,
                                 g_canvas.color565( 80,  80, 100));
        }
      }
    }
  }

  // Back-X, top-right (always).
  {
    const Rect& r = kPhotoBackRect;
    uint16_t panel = g_canvas.color565(240, 240, 240);
    g_canvas.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    g_canvas.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    g_canvas.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    g_canvas.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
  }

  // Delete-confirm modal (takes the centre area).
  if (g_pet.galleryDeleteConfirm) {
    g_canvas.fillRoundRect(40, 70, 240, 100, 12,
                           g_canvas.color565(245, 245, 245));
    g_canvas.drawRoundRect(40, 70, 240, 100, 12,
                           g_canvas.color565( 30,  30,  40));
    g_canvas.setTextDatum(middle_center);
    g_canvas.setTextSize(2);
    g_canvas.setTextColor(g_canvas.color565( 30, 30, 40));
    g_canvas.drawString(tr(Str::GalleryDelete), 160, 90);
    // Yes (red) / No (grey)
    auto button = [&](const Rect& r, uint16_t bg, uint16_t fg, const char* t) {
      g_canvas.fillRoundRect(r.x, r.y, r.w, r.h, 6, bg);
      g_canvas.drawRoundRect(r.x, r.y, r.w, r.h, 6, g_canvas.color565(20,20,30));
      g_canvas.setTextColor(fg);
      g_canvas.drawString(t, r.x + r.w/2, r.y + r.h/2);
    };
    button(kPhotoYesRect, g_canvas.color565(230,  80,  80), white,
           tr(Str::GalleryYes));
    button(kPhotoNoRect,  g_canvas.color565(200, 200, 210),
           g_canvas.color565(40, 40, 50), tr(Str::GalleryNo));
    g_canvas.setTextDatum(top_left);
  }
}

#endif   // TARGET_HAS_CAMERA

// ─── Circle-gesture detection (somersault trigger) ─────────────────────────
//
// The user can draw a circle anywhere on the pet view and the pet does a
// happy somersault. Detection is a small ring buffer of touch samples + a
// "did the angle around the centroid wrap a full turn" check.

static constexpr uint8_t kCircleBufN = 16;

static bool checkCircleClosed() {
  if (g_pet.circleCount < kCircleBufN) return false;
  // Centroid
  int sx = 0, sy = 0;
  for (uint8_t i = 0; i < kCircleBufN; ++i) {
    sx += g_pet.circleX[i]; sy += g_pet.circleY[i];
  }
  float cx = (float)sx / kCircleBufN;
  float cy = (float)sy / kCircleBufN;
  // Walk samples in chronological order (oldest at head, newest at head-1).
  float prev_a = 0;
  float total  = 0;
  float minR = 1e9f, maxR = 0.0f, sumR = 0.0f;
  for (uint8_t i = 0; i < kCircleBufN; ++i) {
    uint8_t idx = (g_pet.circleHead + i) % kCircleBufN;
    float dx = g_pet.circleX[idx] - cx;
    float dy = g_pet.circleY[idx] - cy;
    float r  = sqrtf(dx * dx + dy * dy);
    if (r < minR) minR = r;
    if (r > maxR) maxR = r;
    sumR += r;
    float a = atan2f(dy, dx);
    if (i > 0) {
      float da = a - prev_a;
      while (da >  (float)PI) da -= 2.0f * (float)PI;
      while (da < -(float)PI) da += 2.0f * (float)PI;
      total += da;
    }
    prev_a = a;
  }
  float avgR = sumR / kCircleBufN;
  if (avgR < 25.0f)                       return false;   // too tiny → likely noise
  if (minR < 1.0f || maxR / minR > 3.0f)  return false;   // not round enough
  if (fabsf(total) < 5.2f)                return false;   // < ~300° wrap
  return true;
}

static void detectCircleGesture(bool active, int x, int y, uint32_t now) {
  // Only on the main pet view, awake, no modals, no active transition.
  // Also suppress while a second finger is involved — two-finger gestures
  // would otherwise feed a wandering trail into the circle buffer.
  bool anyModal = g_settings.open || g_pet.cleaningMode || g_pet.foragingMode ||
                  g_pet.toySelectMode || g_pet.mediaSelectMode ||
                  g_pet.travelSelectMode || g_pet.activityMode ||
                  g_pet.timerScreenMode ||
                  g_pet.cameraMode || g_pet.galleryMode ||
                  g_pet.sportSelectMode || g_pet.sportWorkoutMode ||
                  g_pet.sportDoneMode ||
                  g_pet.friendsMode;
  bool blocked = anyModal || g_pet.travelTransitionMode ||
                 g_pet.shutdownAnnouncedAtMs != 0 ||
                 g_pet.timerExpiring || g_pet.face == Face::Sleeping ||
                 g_pet.somersaultStartMs != 0 ||
                 M5.Touch.getCount() > 1;

  if (!active || blocked) {
    g_pet.circleCount = 0;
    g_pet.circleHead  = 0;
    return;
  }

  // Throttle: only sample if at least 5 px from the previous sample so
  // the buffer represents a meaningful path, not a stationary jitter.
  if (g_pet.circleCount > 0) {
    uint8_t last = (g_pet.circleHead + kCircleBufN - 1) % kCircleBufN;
    int dx = x - g_pet.circleX[last];
    int dy = y - g_pet.circleY[last];
    if (dx * dx + dy * dy < 5 * 5) return;
  }
  g_pet.circleX[g_pet.circleHead] = (int16_t)x;
  g_pet.circleY[g_pet.circleHead] = (int16_t)y;
  g_pet.circleHead = (g_pet.circleHead + 1) % kCircleBufN;
  if (g_pet.circleCount < kCircleBufN) g_pet.circleCount++;

  // Cooldown so a continuous swirl doesn't keep retriggering.
  if (now - g_pet.lastCircleMs < 3000) return;
  if (!checkCircleClosed()) return;

  // Fire!
  g_pet.lastCircleMs      = now;
  g_pet.somersaultStartMs = now;
  g_pet.circleCount       = 0;
  flashFace(Face::Laughing, 1100, now);
  changeHappiness(+10, now);
  playSound(Sound::Tickle);
  // Hearts erupting outward in four directions.
  for (int i = 0; i < 4; ++i) {
    int hx = petHeadX() + ((i & 1) ? 30 : -30);
    int hy = petHeadY() + ((i < 2) ? -25 : 12);
    spawnFloat(g_pet.floats, FloatType::Heart,
               petHeadX(), petHeadY(), hx, hy, now, 800);
  }
}

// ─── Rapid-tap burst → hop chain ───────────────────────────────────────────

static constexpr uint32_t kTapBurstWindowMs =  500;   // gap > this ends a burst
static constexpr uint32_t kHopDurationMs    =  400;   // length of one hop
static constexpr uint8_t  kHopMinCount      =    3;
static constexpr uint8_t  kHopMaxCount      =   10;

static void recordTapForBurst(uint32_t now) {
  // Suppress while a chain is already running so the user has to wait it
  // out before starting a new burst.
  if (g_pet.hopChainCount != 0) return;
  if (g_pet.tapBurstCount > 0 &&
      now - g_pet.tapBurstLastMs > kTapBurstWindowMs) {
    g_pet.tapBurstCount = 0;          // previous burst expired, reset
  }
  if (g_pet.tapBurstCount < 250) g_pet.tapBurstCount++;
  g_pet.tapBurstLastMs = now;
}

static void tickHopChain(uint32_t now) {
  // Burst evaluation: once the pause window passes, decide whether to fire.
  if (g_pet.tapBurstCount > 0 &&
      g_pet.hopChainCount == 0 &&
      now - g_pet.tapBurstLastMs > kTapBurstWindowMs) {
    uint8_t count = g_pet.tapBurstCount;
    if (count > kHopMaxCount) count = kHopMaxCount;
    if (count >= kHopMinCount) {
      g_pet.hopChainCount   = count;
      g_pet.hopChainStartMs = now;
      g_pet.hopChainLastIdx = -1;
    }
    g_pet.tapBurstCount = 0;
  }

  // Drive the chain: trigger laugh + heart on each hop transition.
  if (g_pet.hopChainCount == 0) return;
  uint32_t elapsed = now - g_pet.hopChainStartMs;
  int8_t idx = (int8_t)(elapsed / kHopDurationMs);
  if (idx >= (int8_t)g_pet.hopChainCount) {
    g_pet.hopChainCount   = 0;
    g_pet.hopChainStartMs = 0;
    g_pet.hopChainLastIdx = -1;
    return;
  }
  if (idx != g_pet.hopChainLastIdx) {
    g_pet.hopChainLastIdx = idx;
    flashFace(Face::Laughing, 380, now);
    playSound(Sound::Tickle);
    changeHappiness(+2, now);
    int dx = (idx & 1) ? 22 : -22;
    spawnFloat(g_pet.floats, FloatType::Heart,
               petHeadX(), petHeadY(),
               petHeadX() + dx, petHeadY() - 30,
               now, 700);
  }
}

// ─── Singing (upright + left + right tilt → sing → listen for applause) ──

static constexpr uint32_t kSingDurationMs   = 8000;
static constexpr uint32_t kApplauseListenMs = 5000;
static constexpr uint32_t kSingSeqTimeoutMs = 5000;
static constexpr int      kApplauseThresh   = 6000;   // int16 peak threshold

static void detectSingingGesture(uint32_t now);   // defined further down

static void startSinging(uint32_t now) {
  if (g_pet.listenMode) exitListenMode();
  g_pet.singStartMs           = now;
  g_pet.lastSingHeartMs       = now;
  g_pet.applauseListenStartMs = 0;
  g_pet.applausePeak          = 0;
  flashFace(Face::Excited, kSingDurationMs, now);
  playSound(Sound::Singing);
  g_motion.lastInteractionMs  = now;
  // Tip the pet's mood up a bit just for performing.
  changeHappiness(+3, now);
}

static void enterApplauseListen(uint32_t now) {
  // Take the I2S bus over for input — same dance as enterListenMode but
  // without entering the listen-mode UI state.
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  setSoundEnabled(false);
  g_micPending = false;
  g_pet.applauseListenStartMs = now;
  g_pet.applausePeak          = 0;
  g_pet.lastApplauseHeartMs   = now;
  // Eyes-closed shy-listening pose. The renderer paints big red cheeks on
  // top of this while applause is active.
  flashFace(Face::Sleepy, kApplauseListenMs, now);
}

static void exitApplauseListen(uint32_t now) {
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(g_volume);
  setSoundEnabled(true);
  int peak = g_pet.applausePeak;
  g_pet.applauseListenStartMs = 0;
  g_pet.applausePeak          = 0;
  if (peak >= kApplauseThresh) {
    // Audience clapped — pet beams.
    flashFace(Face::Excited, 2200, now);
    playSound(Sound::Happy);
    changeHappiness(+15, now);
    for (int i = 0; i < 5; ++i) {
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now);
    }
  } else {
    // No clap — small disappointed look.
    flashFace(Face::Sleepy, 1200, now);
  }
}

static void tickSinging(uint32_t now) {
  // Run the gesture detector every frame.
  detectSingingGesture(now);

  // Sequence-step timeout — if user takes too long, reset.
  if (g_pet.singSeqStep != 0 &&
      now - g_pet.singSeqLastMs > kSingSeqTimeoutMs) {
    g_pet.singSeqStep = 0;
  }

  // Singing in progress — heart trail + termination.
  if (g_pet.singStartMs != 0) {
    if (now - g_pet.lastSingHeartMs >= 700) {
      g_pet.lastSingHeartMs = now;
      int side = ((now / 700) & 1) ? 1 : -1;
      spawnFloat(g_pet.floats, FloatType::Heart,
                 petHeadX() + side * 30, petHeadY() - 10,
                 petHeadX() + side * 60, petHeadY() - 70,
                 now, 1300);
    }
    if (now - g_pet.singStartMs >= kSingDurationMs) {
      g_pet.singStartMs = 0;
      enterApplauseListen(now);
    }
  }

  // Applause listen — sample mic, track peak, spawn live hearts on each
  // loud buffer, and end the window after 5 s.
  if (g_pet.applauseListenStartMs != 0) {
    if (M5.Mic.isRecording() == 0) {
      if (g_micPending) {
        int peak = 0;
        for (size_t i = 0; i < MIC_BUF_LEN; ++i) {
          int v = g_micBuf[i];
          if (v < 0) v = -v;
          if (v > peak) peak = v;
        }
        if (peak > g_pet.applausePeak) g_pet.applausePeak = peak;

        // Live heart shower — number proportional to clap intensity, with
        // a 130 ms throttle so a single loud clap doesn't drain the pool.
        constexpr int kHeartSpawnThresh = 2500;   // lower than detect threshold
        if (peak > kHeartSpawnThresh &&
            now - g_pet.lastApplauseHeartMs >= 130) {
          g_pet.lastApplauseHeartMs = now;
          int extra = (peak - kHeartSpawnThresh) / 3000;
          if (extra > 4) extra = 4;
          int n = 1 + extra;             // 1..5 hearts per loud buffer
          for (int i = 0; i < n; ++i) {
            int dx = ((i * 23 + (int)(now / 17)) % 100) - 50;
            int dy = -45 - ((i * 17) % 35);
            spawnFloat(g_pet.floats, FloatType::Heart,
                       petHeadX(), petHeadY(),
                       petHeadX() + dx, petHeadY() + dy,
                       now, 1100);
          }
        }
      }
      M5.Mic.record(g_micBuf, MIC_BUF_LEN, 16000, false);
      g_micPending = true;
    }
    if (now - g_pet.applauseListenStartMs >= kApplauseListenMs) {
      exitApplauseListen(now);
    }
  }
}

// Direct gx/gy sampling — much more forgiving than the curStand state
// machine, which can pass through "Other" between Upright and LeftEdge and
// would lose the sequence. Anchor on a recent upright posture (last 4 s),
// then watch for the rising edges of a left and right tilt within 5 s.
static void detectSingingGesture(uint32_t now) {
  // Anchor: gy > 0.6 means the device is at least mostly upright.
  if (g_motion.gy > 0.6f) g_pet.lastUprightMs = now;

  bool tiltLeft  = (g_motion.gx < -0.5f);
  bool tiltRight = (g_motion.gx >  0.5f);

  bool busy = g_pet.singStartMs != 0 || g_pet.applauseListenStartMs != 0 ||
              g_pet.travelTransitionMode || g_pet.shutdownAnnouncedAtMs != 0 ||
              g_pet.timerExpiring || g_pet.face == Face::Sleeping;

  // Rising edge of a LEFT tilt — arm the sequence if we were upright recently.
  if (tiltLeft && !g_pet.wasTiltLeft && !busy) {
    if (g_pet.lastUprightMs != 0 &&
        now - g_pet.lastUprightMs <= 4000) {
      g_pet.singSeqStep   = 1;
      g_pet.singSeqLastMs = now;
    }
  }
  // Rising edge of a RIGHT tilt — fire if we already saw the left tilt.
  if (tiltRight && !g_pet.wasTiltRight && !busy) {
    if (g_pet.singSeqStep == 1 &&
        now - g_pet.singSeqLastMs <= kSingSeqTimeoutMs) {
      startSinging(now);
      g_pet.singSeqStep = 0;
    }
  }

  g_pet.wasTiltLeft  = tiltLeft;
  g_pet.wasTiltRight = tiltRight;
}

// ─── Two-finger gestures (spread + hand-warming hold) ──────────────────────
//
// Both gestures begin with 2 fingers on the pet view. During the first
// ~800 ms we watch which direction the user is going:
//   - Fingers spreading apart → spread mode (snap-back wobble on release)
//   - Fingers staying still   → hold mode (3 s warm-up → snuggle / sleep)
// After the lock, the other detector is ignored until release.

static constexpr float kSpreadDeltaPx        =  30.0f;   // distance grew this much → spread
static constexpr float kHoldDriftLockPx      =   5.0f;   // each finger moved < this → hold lock
static constexpr float kHoldDriftSnugglePx   =  22.0f;   // looser drift while already snuggling
static constexpr float kHoldDriftWarmingPx   =  12.0f;   // warming → cancels above this
static constexpr uint32_t kAmbiguousMs       =   800;    // commit window for hold/spread
static constexpr uint32_t kWarmingMs         =  3000;    // total warm-up before snuggle
static constexpr uint32_t kSnuggleSleepMs    = 25000;    // → deep-sleep face after this
static constexpr uint32_t kSnuggleCooldownMs =  4000;    // post-release block on re-trigger
static constexpr uint32_t kSnugglePurrMs     =  3500;
static constexpr uint32_t kSnuggleHeartMs    =  1500;
static constexpr uint32_t kSnuggleNeedsMs    =  2000;

static void endSnuggleIfActive(uint32_t now) {
  if (g_pet.snuggleStartMs == 0) return;
  g_pet.snuggleStartMs       = 0;
  g_pet.snuggleSleepingPhase = false;
  g_pet.lastSnuggleEndMs     = now;
  // Soft sigh + one last heart drifting up off the pet's head.
  playSound(Sound::Yawn);
  spawnFloat(g_pet.floats, FloatType::Heart,
             petHeadX(), petHeadY() - 10,
             petHeadX(), petHeadY() - 60, now, 1100);
}

static void resetTwoFingerState() {
  g_pet.spreadActive    = false;
  g_pet.spreadAmount    = 0.0f;
  g_pet.holdStartMs     = 0;
  g_pet.holdLocked      = false;
}

static void detectTwoFingerGesture(uint32_t now) {
  bool anyModal = g_settings.open || g_pet.cleaningMode || g_pet.foragingMode ||
                  g_pet.toySelectMode || g_pet.mediaSelectMode ||
                  g_pet.travelSelectMode || g_pet.activityMode ||
                  g_pet.timerScreenMode ||
                  g_pet.cameraMode || g_pet.galleryMode ||
                  g_pet.sportSelectMode || g_pet.sportWorkoutMode ||
                  g_pet.sportDoneMode ||
                  g_pet.friendsMode;
  bool blocked = anyModal || g_pet.travelTransitionMode ||
                 g_pet.shutdownAnnouncedAtMs != 0 ||
                 g_pet.timerExpiring;

  if (blocked) {
    endSnuggleIfActive(now);
    resetTwoFingerState();
    return;
  }

  int n = M5.Touch.getCount();

  // ── Release branch ────────────────────────────────────────────────────
  if (n < 2) {
    if (g_pet.spreadActive && g_pet.spreadAmount > 0.3f) {
      // Spread → snap-back wobble + giggle.
      g_pet.snapStartMs = now;
      flashFace(Face::Laughing, 1100, now);
      changeHappiness(+8, now);
      playSound(Sound::Tickle);
      for (int i = 0; i < 3; ++i) {
        int hx = petHeadX() + ((i % 2) ? 28 : -28);
        int hy = petHeadY() - 22;
        spawnFloat(g_pet.floats, FloatType::Heart,
                   petHeadX(), petHeadY(), hx, hy, now, 750);
      }
    }
    endSnuggleIfActive(now);
    resetTwoFingerState();
    return;
  }

  auto t0 = M5.Touch.getDetail(0);
  auto t1 = M5.Touch.getDetail(1);

  // ── First frame of 2-finger contact ───────────────────────────────────
  if (g_pet.holdStartMs == 0) {
    if (now - g_pet.lastSnuggleEndMs < kSnuggleCooldownMs) {
      return;   // post-snuggle cooldown blocks both gestures briefly
    }
    g_pet.holdStartMs = now;
    g_pet.holdF0X = (int16_t)t0.x; g_pet.holdF0Y = (int16_t)t0.y;
    g_pet.holdF1X = (int16_t)t1.x; g_pet.holdF1Y = (int16_t)t1.y;
    float dx = (float)(t1.x - t0.x);
    float dy = (float)(t1.y - t0.y);
    g_pet.spreadStartDist = sqrtf(dx * dx + dy * dy);
    g_pet.spreadActive = false;
    g_pet.holdLocked   = false;
    return;
  }

  uint32_t age = now - g_pet.holdStartMs;
  float fdx = (float)(t1.x - t0.x);
  float fdy = (float)(t1.y - t0.y);
  float curDist = sqrtf(fdx * fdx + fdy * fdy);
  float d0x = (float)(t0.x - g_pet.holdF0X);
  float d0y = (float)(t0.y - g_pet.holdF0Y);
  float d1x = (float)(t1.x - g_pet.holdF1X);
  float d1y = (float)(t1.y - g_pet.holdF1Y);
  float drift0 = sqrtf(d0x * d0x + d0y * d0y);
  float drift1 = sqrtf(d1x * d1x + d1y * d1y);
  float driftMax = (drift0 > drift1) ? drift0 : drift1;

  // ── Spread already locked ─────────────────────────────────────────────
  if (g_pet.spreadActive) {
    float ratio = (g_pet.spreadStartDist > 1.0f)
                    ? curDist / g_pet.spreadStartDist : 1.0f;
    float amt = (ratio - 1.0f) / 0.5f;
    if (amt < 0.0f) amt = 0.0f;
    if (amt > 1.0f) amt = 1.0f;
    g_pet.spreadAmount = amt;
    g_pet.spreadF0X = (int16_t)t0.x; g_pet.spreadF0Y = (int16_t)t0.y;
    g_pet.spreadF1X = (int16_t)t1.x; g_pet.spreadF1Y = (int16_t)t1.y;
    return;
  }

  // ── Hold already locked ───────────────────────────────────────────────
  if (g_pet.holdLocked) {
    float maxDrift = (g_pet.snuggleStartMs != 0)
                       ? kHoldDriftSnugglePx : kHoldDriftWarmingPx;
    if (driftMax > maxDrift) {
      // User broke the hold — graceful cancel.
      endSnuggleIfActive(now);
      resetTwoFingerState();
      return;
    }
    // Threshold crossed → enter snuggle.
    if (g_pet.snuggleStartMs == 0 && age >= kWarmingMs) {
      g_pet.snuggleStartMs       = now;
      g_pet.snuggleSleepingPhase = false;
      g_pet.lastSnugglePurrMs    = now - 9999;   // play immediately
      g_pet.lastSnuggleHeartMs   = now;
      g_pet.lastSnuggleNeedsMs   = now;
    }
    if (g_pet.snuggleStartMs != 0) {
      // Periodic snuggle effects.
      if (now - g_pet.lastSnugglePurrMs >= kSnugglePurrMs) {
        g_pet.lastSnugglePurrMs = now;
        playSound(Sound::Pet);
      }
      if (now - g_pet.lastSnuggleHeartMs >= kSnuggleHeartMs) {
        g_pet.lastSnuggleHeartMs = now;
        spawnFloat(g_pet.floats, FloatType::Heart,
                   petHeadX(), petHeadY() - 8,
                   petHeadX() + ((int)(now / 17) % 30 - 15),
                   petHeadY() - 60, now, 1400);
      }
      if (now - g_pet.lastSnuggleNeedsMs >= kSnuggleNeedsMs) {
        g_pet.lastSnuggleNeedsMs = now;
        changeHappiness(+1, now);
        changeEnergy(+1, now);
      }
      // Deeper sleep phase.
      if (!g_pet.snuggleSleepingPhase &&
          now - g_pet.snuggleStartMs >= kSnuggleSleepMs) {
        g_pet.snuggleSleepingPhase = true;
      }
    }
    return;
  }

  // ── Ambiguous phase (0..kAmbiguousMs) ─────────────────────────────────
  // Early-out if the user clearly spread.
  if (curDist > g_pet.spreadStartDist + kSpreadDeltaPx) {
    g_pet.spreadActive = true;
    return;
  }
  if (age < kAmbiguousMs) {
    return;   // keep watching
  }
  // Window closed — commit based on current state.
  if (driftMax < kHoldDriftLockPx) {
    g_pet.holdLocked = true;
  }
  // Otherwise: still ambiguous (slow drift but no spread). Keep waiting,
  // the next frame may flip into hold or spread once thresholds cross.
}

static void handleTouch(uint32_t now) {
  if (M5.Touch.getCount() == 0) return;
  auto t = M5.Touch.getDetail(0);
  int  tx = t.x, ty = t.y;
  bool wasPressed = t.wasPressed();
  bool pressed    = t.isPressed();
  if (wasPressed) g_pet.lastTouchMs = now;

  // Bedtime sequence + travel transition both swallow all input — the
  // animation has to play out before anything else happens.
  if (g_pet.shutdownAnnouncedAtMs != 0 || g_pet.travelTransitionMode) {
    (void)tx; (void)ty; (void)wasPressed; (void)pressed; (void)now;
    return;
  }
  // Then: timer expiry overlay swallows the tap to dismiss.
  if (g_pet.timerExpiring) {
    if (wasPressed) dismissTimerExpiry();
    return;
  }
  if (g_pet.timerScreenMode) {
    handleTouchTimer(tx, ty, wasPressed, now);
    (void)pressed;
    return;
  }

  if (g_pet.friendsMode) {
    if (wasPressed) {
      FriendsState fs = friendsGetState();
      // Back-X — cancel the session early. Whatever was received still
      // gets played back in the pet view afterwards.
      if (insideRect(tx, ty, cornerHitRect(kFriendsBackRect))) {
        if (fs == FriendsState::Sending) {
          // End early — playback will run with whatever's in the rx queue.
          g_pet.friendsSentCount = 0xFF;     // marker: forced exit
        } else if (fs == FriendsState::Rendezvous) {
          // Im Rendezvous noch keine Items — direkt sauber raus.
          friendsEnd();
          g_pet.friendsMode = false;
        }
        return;
      }
      if (fs == FriendsState::Rendezvous) {
        // Big "Meet up" button — only register when the local ready
        // isn't already active (otherwise it would be a double-tap that
        // would trigger the match-window reset).
        if (insideRect(tx, ty, kFriendsRendezvousBtnRect) &&
            !friendsLocalReadyActive()) {
          friendsTriggerRendezvous();
        }
      } else if (fs == FriendsState::Sending) {
        // Cap check BEFORE sending — otherwise after 5/5 you could keep
        // tapping forever (counter would show 20/5 etc.) and
        // friendsSignalDone would be called again on every extra tap.
        // The tap is silently ignored once the cap is reached.
        if (g_pet.friendsSentCount >= 5 &&
            g_pet.friendsSentCount != 0xFF) return;
        // Item buttons — 400 ms tap throttle so taps land cleanly.
        if (now - g_pet.friendsLastTapMs < 400) return;
        const struct { Rect r; uint8_t kind; } btns[4] = {
          { kFriendsBtnGift,  FriendsItemGift  },
          { kFriendsBtnHeart, FriendsItemHeart },
          { kFriendsBtnFood,  FriendsItemFood  },
          { kFriendsBtnGame,  FriendsItemGame  },
        };
        for (int i = 0; i < 4; ++i) {
          if (insideRect(tx, ty, btns[i].r)) {
            friendsSendItem(btns[i].kind);
            g_pet.friendsSentCount++;
            g_pet.friendsLastSentKind = btns[i].kind;
            g_pet.friendsLastTapMs    = now;
            // Cap erreicht? Dann Partner sagen "ich bin durch" — Session
            // bleibt aber offen bis beide done sind oder 60-s-Timeout
            // greift. So kann der schnellere Tapper die Items des
            // langsamen noch komplett empfangen.
            if (g_pet.friendsSentCount >= 5 &&
                g_pet.friendsSentCount != 0xFF) {
              friendsSignalDone();
            }
            break;
          }
        }
      }
    }
    (void)pressed;
    return;
  }

  if      (g_pet.cleaningMode)     handleTouchCleaning    (tx, ty, pressed, wasPressed, now);
  else if (g_pet.foragingMode)     handleTouchForaging    (tx, ty, pressed, wasPressed, now);
  else if (g_pet.toySelectMode)    handleTouchToySelect   (tx, ty, pressed, wasPressed, now);
  else if (g_pet.mediaSelectMode)  handleTouchMediaSelect (tx, ty, pressed, wasPressed, now);
  else if (g_pet.travelSelectMode) handleTouchTravelSelect(tx, ty, pressed, wasPressed, now);
  else if (g_pet.activityMode)     handleTouchActivity    (tx, ty, pressed, wasPressed, now);
  else if (g_pet.sportSelectMode)  handleTouchSportSelect (tx, ty, pressed, wasPressed, now);
  else if (g_pet.sportWorkoutMode) handleTouchSportWorkout(tx, ty, pressed, wasPressed, now);
  else if (g_pet.sportDoneMode)    handleTouchSportDone   (tx, ty, pressed, wasPressed, now);
#if TARGET_HAS_CAMERA
  else if (g_pet.cameraMode)       handleTouchCameraMode  (tx, ty, pressed, wasPressed, now);
  else if (g_pet.galleryMode)      handleTouchGalleryMode (tx, ty, pressed, wasPressed, now);
#endif
  else if (g_settings.open)        handleTouchSettings    (tx, ty, pressed, wasPressed, now);
  else                             handleTouchPet         (tx, ty, pressed, wasPressed, now);
}

// ─── Render ──────────────────────────────────────────────────────────────────

#if TARGET_HAS_CAMERA
static void updateCameraState(uint32_t now);   // forward decl (def. weiter unten)
#endif

static void render(uint32_t now) {
  applyDisplayPower(now);
#if TARGET_HAS_CAMERA
  updateCameraState(now);
#endif
  // While the display is fully off, save the SPI bus + GPU time — the
  // panel is asleep, no one's watching.
  if (g_displayPowerMode == 2) return;
  if (now - g_lastDrawMs < 33) return;
  g_lastDrawMs = now;

  Scene currentScene = (Scene)(g_pet.persisted.selectedScene < kSceneCount
                                 ? g_pet.persisted.selectedScene : 0);

  // Bedtime sequence has the absolute highest priority — overrides
  // everything (including travel transitions / mini-games) once it
  // starts.
  if (g_pet.shutdownAnnouncedAtMs != 0) {
    uint32_t since = now - g_pet.shutdownAnnouncedAtMs;
    if (since < kBedtimeAnnounceMs) {
      // Phase 1: render whatever was on screen + announcement overlay
      Face f = g_pet.face;
      if ((int32_t)(g_pet.flashUntilMs - now) > 0) f = g_pet.flashFace;
      PetView v{};
      v.face          = f;
      v.now_ms        = now;
      v.needs         = g_pet.persisted.needs;
      v.phase         = currentPhase();
      v.gazeX         = g_pet.gaze.currentX;
      v.gazeY         = g_pet.gaze.currentY;
      v.tiltX         = g_pet.tilt.offsetX;
      v.tiltY         = g_pet.tilt.offsetY;
      v.micro         = g_pet.micro.current;
      v.microStartMs  = g_pet.micro.startMs;
      v.microUntilMs  = g_pet.micro.untilMs;
      v.bubble        = g_pet.bubble.current;
      v.bubbleStartMs = g_pet.bubble.startMs;
      v.bubbleUntilMs = g_pet.bubble.untilMs;
      v.rainStartMs   = 0;
      v.listenMode    = false;
      v.micLevel      = 0;
      v.pileX         = g_pet.pileX;
      v.pileY         = g_pet.pileY;
      v.pileCount     = g_pet.pileCount;
      v.dirty         = false;
      v.animal        = (AnimalType)(g_pet.persisted.animal & 3);
      v.wanderX       = g_pet.wander.x;
      v.wanderY       = g_pet.wander.y;
      v.floats        = &g_pet.floats;
      v.scene         = currentScene;
      v.weather       = g_pet.weather;
      v.activityCooldownSec = 0;
      v.activityCanPlay     = true;
      v.travelCooldownSec   = 0;
      v.travelCanPlay       = true;
      drawFace(g_canvas, v);
      drawBedtimeAnnouncement(g_canvas, since);
    } else {
      uint32_t animElapsed = since - kBedtimeAnnounceMs;
      drawBedtimeSleepAnim(g_canvas, animElapsed,
                            (AnimalType)(g_pet.persisted.animal & 3));
    }
    g_canvas.pushSprite(0, 0);
    return;
  }

  // Travel transition has the highest priority — it owns the whole canvas
  // for 8 s, blocking every other view.
  if (g_pet.travelTransitionMode) {
    uint32_t elapsed = now - g_pet.travelTransitionStartMs;
    drawTravelTransition(g_canvas,
                         g_pet.travelTransitionTo,
                         (AnimalType)(g_pet.persisted.animal & 3),
                         elapsed,
                         currentPhase());
    g_canvas.pushSprite(0, 0);
    return;
  }

  if (g_pet.timerScreenMode) {
    if (g_pet.timerCustomEdit) {
      TimerCustomView tcv{};
      tcv.now_ms  = now;
      tcv.minutes = g_pet.customMinutes;
      tcv.seconds = g_pet.customSeconds;
      drawTimerCustomScreen(g_canvas, tcv);
    } else {
      TimerView tv{};
      tv.now_ms     = now;
      tv.active     = g_pet.timerActive;
      tv.durationMs = g_pet.timerDurationMs;
      tv.remainingMs = (g_pet.timerActive && now < g_pet.timerEndMs)
                         ? (g_pet.timerEndMs - now) : 0;
      drawTimerScreen(g_canvas, tv);
    }
  } else if (g_pet.cleaningMode) {
    CleaningView cv{};
    cv.now_ms      = now;
    cv.phase       = currentPhase();
    cv.scene       = currentScene;
    cv.pileX       = g_pet.pileX;
    cv.pileY       = g_pet.pileY;
    cv.pileCount   = g_pet.pileCount;
    cv.cleanedAtMs = g_pet.cleanedAtMs;
    drawCleaningScreen(g_canvas, cv);
  } else if (g_pet.foragingMode) {
    ForagingView fv{};
    fv.now_ms  = now;
    fv.phase   = currentPhase();
    fv.scene   = currentScene;
    fv.apples  = g_pet.persisted.apples;
    fv.berries = g_pet.persisted.berries;
    fv.fish    = g_pet.persisted.fish;
    for (int i = 0; i < kForageMaxItems; ++i) fv.items[i] = g_forage.items[i];
    drawForagingScreen(g_canvas, fv);
  } else if (g_pet.toySelectMode) {
    ToySelectView tv{};
    tv.now_ms      = now;
    tv.current     = (Toy)(g_pet.persisted.selectedToy < kToyCount
                             ? g_pet.persisted.selectedToy : 0);
    tv.repeatCount = g_pet.persisted.toyRepeatCount;
    drawToySelectScreen(g_canvas, tv);
  } else if (g_pet.mediaSelectMode) {
    MediaSelectView mv{};
    mv.now_ms = now;
    mv.current = g_pet.mediaActive;
    drawMediaSelectScreen(g_canvas, mv);
#if TARGET_HAS_CAMERA
  } else if (g_pet.cameraMode) {
    drawCameraScreen(now);
  } else if (g_pet.galleryMode) {
    drawGalleryScreen(now);
#endif
  } else if (g_pet.sportSelectMode) {
    SportSelectView sv{};
    sv.now_ms = now;
    drawSportSelectScreen(g_canvas, sv);
  } else if (g_pet.sportWorkoutMode) {
    SportWorkoutView sv{};
    sv.now_ms       = now;
    sv.exercise     = g_pet.sportExercise;
    sv.phase        = g_pet.sportPhase;
    sv.phaseStartMs = g_pet.sportPhaseStartMs;
    sv.repCount     = g_pet.sportRepCount;
    sv.targetReps   = g_pet.sportTargetReps;
    sv.motionPhase  = g_pet.sportMotionPhase;
    sv.myAnimal     = (uint8_t)(g_pet.persisted.animal & 3);
    drawSportWorkoutScreen(g_canvas, sv);
  } else if (g_pet.sportDoneMode) {
    SportDoneView dv{};
    dv.now_ms   = now;
    dv.exercise = g_pet.sportExercise;
    dv.reps     = g_pet.sportRepCount;
    dv.streak   = g_pet.persisted.sportStreakDays;
    dv.myAnimal = (uint8_t)(g_pet.persisted.animal & 3);
    drawSportDoneScreen(g_canvas, dv);
  } else if (g_pet.travelSelectMode) {
    TravelSelectView tvv{};
    tvv.now_ms  = now;
    tvv.current = (Scene)(g_pet.persisted.selectedScene < kSceneCount
                            ? g_pet.persisted.selectedScene : 0);
    tvv.phase   = currentPhase();
    drawTravelSelectScreen(g_canvas, tvv);
  } else if (g_pet.activityMode) {
    ActivityView av{};
    av.now_ms      = now;
    av.scene       = g_activity.scene;
    av.phase       = currentPhase();
    av.animal      = (AnimalType)(g_pet.persisted.animal & 3);
    av.score       = g_activity.score;
    av.gameOver    = g_activity.gameOver;
    av.gameOverAtMs= g_activity.gameOverAtMs;
    av.stars       = g_activity.lastStars;
    av.bonusHap    = g_activity.lastBoostHap;
    av.bonusEng    = g_activity.lastBoostEng;
    av.newBest     = g_activity.lastNewBest;
    av.playsInSession     = g_activity.playsInSession;
    av.maxPlaysPerSession = kMaxPlaysPerSession;
    if (g_activity.scene == Scene::Meadow) {
      uint32_t elapsed = now - g_activity.startMs;
      av.timeLeftMs = (elapsed < kButterflyDurMs)
                        ? (kButterflyDurMs - elapsed) : 0;
      av.best = g_pet.persisted.bestButterflies;
      for (int i = 0; i < ActivityView::kBfCount; ++i) {
        av.bfX[i]        = g_activity.bfX[i];
        av.bfY[i]        = g_activity.bfY[i];
        av.bfState[i]    = g_activity.bfState[i];
        av.bfCaughtMs[i] = g_activity.bfCaughtMs[i];
        av.bfColor[i]    = g_activity.bfColor[i];
      }
    } else if (g_activity.scene == Scene::Bedroom) {
      av.best = g_pet.persisted.bestStack;
      av.stackCount = g_activity.stackCount;
      for (int i = 0; i < g_activity.stackCount; ++i) {
        av.stackX[i] = g_activity.stackX[i];
        av.stackY[i] = g_activity.stackY[i];
        av.stackW[i] = g_activity.stackW[i];
      }
      av.movingX = g_activity.movingX;
      av.movingY = g_activity.movingY;
      av.movingW = g_activity.movingW;
    } else if (g_activity.scene == Scene::Forest) {
      uint32_t elapsed = now - g_activity.startMs;
      av.timeLeftMs = (elapsed < 30000u) ? (30000u - elapsed) : 0;
      av.best = g_pet.persisted.bestMushrooms;
      for (int i = 0; i < ActivityView::kMushSlots; ++i) {
        av.mushKind[i]      = g_activity.mushKind[i];
        av.mushSpawnedMs[i] = g_activity.mushSpawnedMs[i];
      }
    } else if (g_activity.scene == Scene::Beach) {
      av.best = g_pet.persisted.bestSurf;
      av.petPosX = (int16_t)g_activity.petPosX;
      av.petPosY = (int16_t)g_activity.petPosY;
      av.lives   = g_activity.lives;
      for (int i = 0; i < ActivityView::kObsCount; ++i) {
        av.obsX[i]    = g_activity.obsX[i];
        av.obsY[i]    = g_activity.obsY[i];
        av.obsType[i] = g_activity.obsType[i];
      }
    } else if (g_activity.scene == Scene::Desert) {
      av.best = g_pet.persisted.bestScorpion;
      av.petPosX = (int16_t)g_activity.petPosX;
      av.petPosY = (int16_t)g_activity.petPosY;
      for (int i = 0; i < ActivityView::kObsCount; ++i) {
        av.obsX[i]    = g_activity.obsX[i];
        av.obsY[i]    = g_activity.obsY[i];
        av.obsType[i] = g_activity.obsType[i];
      }
    } else if (g_activity.scene == Scene::Space) {
      av.best = g_pet.persisted.bestAsteroids;
      av.petPosX = (int16_t)g_activity.petPosX;
      av.petPosY = (int16_t)g_activity.petPosY;
      av.lives   = g_activity.lives;
      for (int i = 0; i < ActivityView::kObsCount; ++i) {
        av.obsX[i]    = g_activity.obsX[i];
        av.obsY[i]    = g_activity.obsY[i];
        av.obsType[i] = g_activity.obsType[i];
      }
    } else if (g_activity.scene == Scene::City) {
      av.best = g_pet.persisted.bestCross;
      av.petRow = g_activity.petRow;
      for (int i = 0; i < ActivityView::kObsCount; ++i) {
        av.obsX[i]    = g_activity.obsX[i];
        av.obsY[i]    = g_activity.obsY[i];
        av.obsType[i] = g_activity.obsType[i];
      }
    }
    drawActivityScreen(g_canvas, av);
  } else if (g_settings.open && g_settings.helpOpen) {
    HelpView hv{};
    hv.now_ms = now;
    hv.page   = g_settings.helpPage;
    drawHelpScreen(g_canvas, hv);
  } else if (g_settings.open && g_settings.creditsOpen) {
    drawCreditsScreen(g_canvas, now);
  } else if (g_settings.open && g_settings.langOpen) {
    LangSelectView lv{};
    lv.now_ms  = now;
    lv.current = g_pet.persisted.language;
    lv.firstRun = false;
    drawLangSelectScreen(g_canvas, lv);
  } else if (g_pet.friendsMode) {
    FriendsView fv{};
    fv.now_ms          = now;
    fv.state           = (uint8_t)friendsGetState();
    fv.stateStartedAt  = friendsStateStartedAt();
    fv.myAnimal        = (uint8_t)(g_pet.persisted.animal & 3);
    fv.peerAnimal      = friendsPeerAnimal();
    constexpr uint32_t kFriendsSessionMs = 60000;
    uint32_t age = now - g_pet.friendsModeStartMs;
    fv.secondsLeft = (age < kFriendsSessionMs)
                       ? (kFriendsSessionMs - age + 999) / 1000 : 0;
    fv.sentCount     = (g_pet.friendsSentCount == 0xFF) ? 0
                                                        : g_pet.friendsSentCount;
    fv.maxSends      = 5;
    fv.lastSentAtMs  = g_pet.friendsLastTapMs;
    fv.lastSentKind  = g_pet.friendsLastSentKind;
    fv.localReady    = friendsLocalReadyActive();
    fv.remoteReady   = friendsRemoteReadyActive();
    fv.localDone     = friendsLocalDone();
    fv.remoteDone    = friendsRemoteDone();
    fv.rxItemCount   = friendsRxItemCount();
    drawFriendsScreen(g_canvas, fv);
  } else if (g_settings.open && g_settings.parentsHelpOpen) {
    drawParentsHelpScreen(g_canvas, now);
  } else if (g_settings.open && g_settings.parentServerPageOpen) {
    ParentSrvView pv{};
    pv.now_ms = now;
    pv.state  = (uint8_t)parentServerGetState();
    pv.ip     = parentServerGetIp();
    drawParentSrvScreen(g_canvas, pv);
  } else if (g_settings.open && g_settings.locationPageOpen) {
    LocationView lv{};
    lv.now_ms      = now;
    const WorldLocation& loc = worldLocation();
    lv.valid       = loc.valid && loc.city[0] != '\0';
    lv.city        = loc.city;
    lv.countryCode = loc.countryCode;
    lv.lat         = loc.lat;
    lv.lon         = loc.lon;
    uint32_t epoch = nowEpoch();
    if (loc.lastUpdatedEpoch > 0 && epoch > loc.lastUpdatedEpoch) {
      lv.lastUpdatedAgoSec = epoch - loc.lastUpdatedEpoch;
    } else {
      lv.lastUpdatedAgoSec = 0;
    }
    lv.refreshing = g_settings.locationRefreshing;
    drawLocationScreen(g_canvas, lv);
  } else if (g_settings.open && g_settings.wifiSetupOpen) {
    WifiSetupView wv{};
    wv.now_ms      = now;
    wv.state       = (uint8_t)wifiSetupGetState();
    wv.apName      = TARGET_AP_NAME;
    wv.apIp        = "192.168.4.1";
    wv.pendingSsid = wifiSetupPendingSsid();
    drawWifiSetupScreen(g_canvas, wv);
  } else if (g_settings.open && g_settings.timeEditing) {
    drawTimeEditScreen(g_canvas, now);
  } else if (g_settings.open && g_settings.animalSelecting) {
    AnimalSelectView av{};
    av.now_ms = now;
    av.current = (AnimalType)(g_pet.persisted.animal & 3);
    drawAnimalSelectScreen(g_canvas, av);
  } else if (g_settings.open) {
    SettingsView sv{};
    sv.now_ms = now;
    sv.volume = g_volume;
    sv.brightnessLevel = g_brightnessLevel;
    sv.animal = (AnimalType)(g_pet.persisted.animal & 3);
    sv.page   = g_settings.page;
    sv.language = g_pet.persisted.language;
    sv.resetConfirmOpen     = g_settings.resetConfirmOpen;
    sv.wifiResetConfirmOpen = g_settings.wifiResetConfirmOpen;
    sv.parentSrvState       = (uint8_t)parentServerGetState();
    sv.parentSrvIp          = parentServerGetIp();
    sv.pipMode              = (g_pet.persisted.pipMode != 0);
    drawSettingsScreen(g_canvas, sv);
  } else {
    Face f = g_pet.face;
    if ((int32_t)(g_pet.flashUntilMs - now) > 0) f = g_pet.flashFace;
    // Snuggle override — once warmed up the pet has its eyes shut. After
    // 25 s in snuggle the deeper sleeping face takes over.
    if (g_pet.snuggleStartMs != 0) {
      f = g_pet.snuggleSleepingPhase ? Face::Sleeping : Face::Sleepy;
    } else if (g_pet.holdLocked &&
               now - g_pet.holdStartMs >= 1500) {
      // Halfway through the warm-up the pet starts to droop visibly.
      f = Face::Sleepy;
    }

    PetView v{};
    v.face          = f;
    v.now_ms        = now;
    v.needs         = g_pet.persisted.needs;
    v.phase         = currentPhase();
    v.gazeX         = g_pet.gaze.currentX;
    v.gazeY         = g_pet.gaze.currentY;
    v.tiltX         = g_pet.tilt.offsetX;
    v.tiltY         = g_pet.tilt.offsetY;
    v.micro         = g_pet.micro.current;
    v.microStartMs  = g_pet.micro.startMs;
    v.microUntilMs  = g_pet.micro.untilMs;
    v.bubble        = g_pet.bubble.current;
    v.bubbleStartMs = g_pet.bubble.startMs;
    v.bubbleUntilMs = g_pet.bubble.untilMs;
    v.rainStartMs   = g_pet.rainStartMs;
    v.listenMode    = g_pet.listenMode;
    v.micLevel      = g_pet.micLevel;
    v.pileX         = g_pet.pileX;
    v.pileY         = g_pet.pileY;
    v.pileCount     = g_pet.pileCount;
    v.dirty         = isDirty();
    v.animal        = (AnimalType)(g_pet.persisted.animal & 3);
    v.wanderX       = g_pet.wander.x;
    v.wanderY       = g_pet.wander.y;
    v.happLastChangeMs = g_pet.happLastChangeMs;
    v.engLastChangeMs  = g_pet.engLastChangeMs;
    v.fullLastChangeMs = g_pet.fullLastChangeMs;
    v.floats        = &g_pet.floats;
    v.toyPlayStartMs   = g_pet.toyPlayStartMs;
    v.toyPlayDurMs     = g_pet.toyPlayDurMs;
    v.toyPlayWhich     = g_pet.toyPlayWhich;
    v.toyPlayBored     = g_pet.toyPlayBored;
    v.toyPlayFromLeft  = g_pet.toyPlayFromLeft;
    v.mediaActive      = g_pet.mediaActive;
    v.scene            = currentScene;
    v.weather          = g_pet.weather;
    // Real weather + sun + moon for "earthly" scenes (home and the
    // local landscapes around it). Beach / Desert / Space stay in their
    // own random world so a trip feels like a different climate.
    bool useRealWeather =
        (currentScene == Scene::Bedroom ||
         currentScene == Scene::Meadow  ||
         currentScene == Scene::Forest  ||
         currentScene == Scene::City);
    if (useRealWeather) {
      const WorldWeather& ww = worldWeather();
      if (ww.valid) {
        Weather mapped;
        uint16_t c = ww.weatherCode;
        if      (c == 0 || c == 1)                 mapped = Weather::Sunny;
        else if (c == 2 || c == 3)                 mapped = Weather::Cloudy;
        else if (c == 45 || c == 48)               mapped = Weather::Foggy;
        else if ((c >= 51 && c <= 67) ||
                 (c >= 80 && c <= 82) ||
                 (c >= 95 && c <= 99))             mapped = Weather::Rainy;
        else if ((c >= 71 && c <= 77) ||
                 (c >= 85 && c <= 86))             mapped = Weather::Cloudy;
        else                                       mapped = Weather::Sunny;
        v.weather = mapped;
      }
    }
    v.activityCooldownSec = activityCooldownRemainingSec(currentScene);
    v.activityCanPlay     = activityHasEnoughEnergy(currentScene);
    v.travelCooldownSec   = travelCooldownRemainingSec();
    v.travelCanPlay       = travelHasEnoughEnergy();
    v.sideShakeStartMs    = g_pet.sideShakeStartMs;
    {
      const WorldMoon& m = worldMoon();
      // Encode as 0..255 (0xFF = no data sentinel — renderer falls back).
      uint32_t epochCheck = nowEpoch();
      if (epochCheck > 0) {
        uint8_t mp = (uint8_t)(m.phase01 * 254.0f);
        if (mp == 0xFF) mp = 0xFE;
        v.moonPhase255 = mp;
      } else {
        v.moonPhase255 = 0xFF;
      }
    }
    v.basketHintUntilMs   = g_pet.basketHintUntilMs;
    v.somersaultStartMs   = g_pet.somersaultStartMs;
    v.spreadActive        = g_pet.spreadActive;
    v.spreadF0X           = g_pet.spreadF0X;
    v.spreadF0Y           = g_pet.spreadF0Y;
    v.spreadF1X           = g_pet.spreadF1X;
    v.spreadF1Y           = g_pet.spreadF1Y;
    v.spreadAmount        = g_pet.spreadAmount;
    v.snapStartMs         = g_pet.snapStartMs;

    // Hand-warming / snuggle render fields.
    v.warmHoldActive  = g_pet.holdLocked && g_pet.snuggleStartMs == 0;
    v.snuggleActive   = (g_pet.snuggleStartMs != 0);
    v.snuggleSleeping = g_pet.snuggleSleepingPhase;
    v.warmPhase       = 0.0f;
    if (g_pet.holdLocked && g_pet.snuggleStartMs == 0) {
      uint32_t age = now - g_pet.holdStartMs;
      float p = (float)age / 3000.0f;
      if (p < 0.0f) p = 0.0f;
      if (p > 1.0f) p = 1.0f;
      v.warmPhase = p;
    } else if (g_pet.snuggleStartMs != 0) {
      v.warmPhase = 1.0f;
    }
    v.warmMidX = (int16_t)((g_pet.holdF0X + g_pet.holdF1X) / 2);
    v.warmMidY = (int16_t)((g_pet.holdF0Y + g_pet.holdF1Y) / 2);
    v.hopChainCount   = g_pet.hopChainCount;
    v.hopChainStartMs = g_pet.hopChainStartMs;
    v.friendsPlaybackKind        = g_pet.friendsPlaybackActive
                                      ? g_pet.friendsPlaybackKind : 0;
    v.friendsPlaybackAnimal      = g_pet.friendsPlaybackAnimal;
    v.friendsPlaybackItemStartMs = g_pet.friendsPlaybackItemStartMs;
    // Fill the gift bar while playback (incl. celebration) runs.
    // friendsRxItemCount() keeps returning >0 until friendsClearRxQueue()
    // runs in the cleanup branch; after that == 0 and the bar disappears.
    if (g_pet.friendsPlaybackActive) {
        uint8_t total = friendsRxItemCount();
        if (total > 5) total = 5;
        v.friendsPlaybackTotal    = total;
        v.friendsPlaybackIdxView  = g_pet.friendsPlaybackIdx;
        for (uint8_t i = 0; i < total; ++i) {
            const FriendsRxItem* it = friendsRxItem(i);
            v.friendsPlaybackKinds[i] = it ? it->kind : 0;
        }
    } else {
        v.friendsPlaybackTotal    = 0;
        v.friendsPlaybackIdxView  = 0;
    }
    v.singStartMs     = g_pet.singStartMs;
    v.applauseListenStartMs = g_pet.applauseListenStartMs;

    drawFace(g_canvas, v);
  }

  bool anyModal = g_settings.open || g_pet.cleaningMode || g_pet.foragingMode ||
                  g_pet.toySelectMode || g_pet.mediaSelectMode ||
                  g_pet.travelSelectMode || g_pet.activityMode ||
                  g_pet.timerScreenMode ||
                  g_pet.cameraMode || g_pet.galleryMode ||
                  g_pet.sportSelectMode || g_pet.sportWorkoutMode ||
                  g_pet.sportDoneMode ||
                  g_pet.friendsMode;

  // Top-right clock — skipped on every modal overlay (their own back X is
  // there). Also skipped while a timer is running on the pet view; the
  // timer badge takes that slot instead.
  if (!anyModal && !g_pet.timerActive) {
    drawClock(g_canvas, currentPhase());
  }
  // Timer countdown badge — visible everywhere a timer is running (except
  // on the timer screen itself, which already shows the countdown big).
  if (g_pet.timerActive && !g_pet.timerScreenMode) {
    uint32_t rem = (now < g_pet.timerEndMs) ? (g_pet.timerEndMs - now) : 0;
    drawTimerBadge(g_canvas, rem, now, /*topRight=*/!anyModal);
  }

  // Battery icon — hidden on the main settings page (title collides), on
  // any modal overlay, and while a timer is running on the pet view (the
  // timer badge takes that corner).
  if ((!g_settings.open || g_settings.timeEditing) &&
      !g_pet.cleaningMode && !g_pet.foragingMode &&
      !g_pet.toySelectMode && !g_pet.mediaSelectMode &&
      !g_pet.travelSelectMode && !g_pet.activityMode &&
      !g_pet.friendsMode && !g_pet.sportWorkoutMode &&
      !g_pet.sportSelectMode && !g_pet.sportDoneMode &&
      !g_pet.timerActive) {
    bool dark = g_settings.timeEditing ||
                (!g_settings.open && currentPhase() == TimePhase::Night);
    drawBatteryIcon(g_canvas, 236, 11,
                    M5.Power.getBatteryLevel(),
                    M5.Power.isCharging(),
                    dark);
  }

  // Bottom button-hint strip — only on the pet view; modal overlays own
  // the screen.
  if (!anyModal) {
    drawButtonHints(g_canvas, currentPhase());
  }

  // Top layer of all: timer expiry overlay. Draws over whatever is on
  // screen so the user can't miss it.
  if (g_pet.timerExpiring) {
    drawTimerExpiryOverlay(g_canvas, now, g_pet.timerExpiredAtMs);
  }

  g_canvas.pushSprite(0, 0);
}

// ─── Boot-time WiFi/NTP sync ─────────────────────────────────────────────────
//
// Credentials live in NVS now and are entered through the in-app captive
// portal (Settings → WLAN einrichten). On every boot we briefly try to
// connect, run NTP, set the RTC, and disconnect again to save battery. If
// no credentials are saved, we skip the whole thing silently and the RTC
// keeps ticking from its battery-backed value.

// Returns true when a date is "today or later" relative to the wall
// clock (handles the RTC-unset case by treating any data as stale).
static bool isNtpFreshForToday() {
  WallDate today = currentDate();
  if (today.year == 0) return false;
  const WallDate& last = g_pet.persisted.lastNtpDay;
  if (last.year == 0) return false;
  return last.year == today.year && last.month == today.month &&
         last.day == today.day;
}

// ─── Async Boot-Sync (WiFi/NTP/World) ────────────────────────────────────
//
// Runs as a FreeRTOS task on Core 0 in parallel to the splash animation.
// Status is published via volatile atomics so the splash loop can see the
// stage value and stretch accordingly — and if it takes longer than the
// splash animation, the post-splash overlay shows the current stage.

enum class SyncStage : uint8_t {
  Idle,    // not started yet OR skipped (cache fresh / no creds)
  Wifi,    // STA connect running
  Time,    // SNTP + HTTP-Date fallback
  World,   // ip-api + open-meteo
  Done,    // finished (success or fail)
};

static volatile SyncStage g_syncStage = SyncStage::Idle;
static volatile bool      g_syncTaskActive = false;
// Globals for post-task persistence — main loop writes them out once the
// task is done (no NVS write from the task itself; it doesn't run in the
// same heap safety context as the main loop).
static volatile bool g_syncStampNtpDay = false;

static void bootSyncTask(void* /*param*/) {
  g_syncStage = SyncStage::Wifi;
  bool connected = wifiConnect(10000);
  if (connected) {
    g_syncStage = SyncStage::Time;
    bool ntpOk = wifiSyncTime(8000);
    if (ntpOk) g_syncStampNtpDay = true;
    g_syncStage = SyncStage::World;
    if (wifiIsConnected()) {
      uint32_t epoch = (uint32_t)time(nullptr);
      fetchWorldIfStale(epoch);
    }
  }
  wifiPowerOff();
  g_syncStage     = SyncStage::Done;
  g_syncTaskActive = false;
  vTaskDelete(nullptr);
}

// Spawns the boot-sync task (or skips immediately if the cache is fresh).
// Returns non-blocking; the splash loop and post-splash overlay handle the
// UI while the task is running.
static void startBootSyncAsync() {
  wifiLoadCreds();
  if (!wifiHasCreds()) {
    g_syncStage = SyncStage::Done;
    return;
  }
  bool needNtp   = !isNtpFreshForToday();
  bool needWorld = !worldHasFreshData() &&
                   (worldWeather().lastUpdatedEpoch == 0 ||
                    (uint32_t)time(nullptr) -
                        worldWeather().lastUpdatedEpoch > 60UL * 60UL);
  if (!needNtp && !needWorld) {
    Serial.println(F("[boot] NTP + world fresh, skipping WiFi cycle"));
    g_syncStage = SyncStage::Done;
    return;
  }
  // Call wifiBeginAsync on the calling thread before the task —
  // populateMulti may touch NVS and we don't want that mixed into task
  // scheduling.
  wifiBeginAsync();
  g_syncTaskActive = true;
  g_syncStage      = SyncStage::Wifi;
  xTaskCreatePinnedToCore(bootSyncTask, "boot-sync", 8192,
                          nullptr, 1, nullptr, 0);
}

// ─── Animated splash screen ─────────────────────────────────────────────────
//
// Drawn for 4 seconds at boot before anything else takes over the canvas.
// Big purple/pink wordmark (TARGET_NAME) with a pulsing glow, drifting sparkles,
// and a tagline that fades in mid-animation. Uses g_canvas + pushSprite so
// it's flicker-free.

// Brief 1.5 s "Hello from <city>!" panel right after the splash. Skipped
// when no city has been resolved yet (offline boot, never fetched, etc.).
static void showCityGreeting() {
  const WorldLocation& loc = worldLocation();
  if (!loc.valid || loc.city[0] == '\0') return;

  uint32_t start = millis();
  while (millis() - start < 1500) {
    uint32_t e = millis() - start;
    g_canvas.fillSprite(g_canvas.color565(20, 8, 36));
    // Soft pulsing globe icon at the top center.
    int icx = 160, icy = 90;
    uint16_t blue   = g_canvas.color565( 80, 140, 220);
    uint16_t blueHi = g_canvas.color565(140, 200, 255);
    uint16_t green  = g_canvas.color565( 80, 170,  90);
    g_canvas.fillCircle(icx, icy, 28, blue);
    g_canvas.fillEllipse(icx - 8, icy - 4, 12, 6, green);
    g_canvas.fillEllipse(icx + 6, icy + 6,  8, 5, green);
    g_canvas.drawCircle(icx, icy, 28, blueHi);

    uint8_t alpha = (e < 250)              ? (uint8_t)(e * 255 / 250)
                  : (e > 1250)             ? (uint8_t)((1500 - e) * 255 / 250)
                                           : 255;
    uint8_t r = (uint8_t)(255 * alpha / 255);
    uint8_t g = (uint8_t)(220 * alpha / 255);
    uint8_t b = (uint8_t)(255 * alpha / 255);
    g_canvas.setTextDatum(top_center);
    g_canvas.setTextSize(2);
    g_canvas.setTextColor(g_canvas.color565(r * 4 / 5, g * 4 / 5, b));
    g_canvas.drawString(tr(Str::BootGreetingFrom), 160, 140);
    g_canvas.setTextSize(3);
    g_canvas.setTextColor(g_canvas.color565(r, g, b));
    g_canvas.drawString(loc.city, 160, 168);
    g_canvas.setTextDatum(top_left);
    g_canvas.pushSprite(0, 0);
    delay(33);
  }
}

static void showStartscreen() {
  uint32_t start = millis();
  static const struct { int16_t x, y; uint8_t hueOffset; uint8_t size; } sparks[26] = {
    {  20,  35,  0, 1}, {  60,  85, 30, 2}, { 100, 200, 60, 1}, { 140,  50, 90, 1},
    { 180, 165, 20, 2}, { 220,  25, 50, 1}, { 260, 195, 80, 2}, { 300,  75, 10, 1},
    {  35, 200, 40, 1}, {  80,  20, 70, 2}, { 130, 215,  5, 1}, { 175,  90, 35, 1},
    { 215,  55, 65, 2}, { 255, 130, 95, 1}, { 290, 220, 25, 2}, {  10, 145, 55, 1},
    {  55, 175, 85, 1}, { 105, 130, 15, 2}, { 155, 220, 45, 1}, { 200, 200, 75, 1},
    { 245,  60, 90, 1}, { 280,  30, 20, 2}, {  45,  60, 50, 1}, { 115,  60, 80, 1},
    { 165, 130, 10, 2}, { 195,  35, 40, 1},
  };
  // Splash duration:
  //   - MIN 2.5 s so the wordmark + tagline actually have time to land
  //     (used to be 4 s, felt too long).
  //   - MAX depends on parallel tasks: WiFi sync always runs (unless
  //     skipped), voice setup only on TARGET_HAS_LLM. The splash stays
  //     until both are done OR MAX is reached — after that
  //     drawSyncProgressOverlay() takes over until sync really finishes.
  constexpr uint32_t SPLASH_MIN_MS = 2500;
#if TARGET_HAS_LLM
  constexpr uint32_t SPLASH_MAX_MS = 12000;   // rough voice-setup upper bound
#else
  constexpr uint32_t SPLASH_MAX_MS = 8000;    // without voice, only sync to wait for
#endif
  while (true) {
    uint32_t e = millis() - start;
    if (e >= SPLASH_MAX_MS) break;
    if (e >= SPLASH_MIN_MS) {
      bool voiceReady = true;
#if TARGET_HAS_LLM
      voiceReady = g_voiceSetupDone;
#endif
      bool syncReady = (g_syncStage == SyncStage::Done);
      if (voiceReady && syncReady) break;
    }
    g_canvas.fillSprite(g_canvas.color565(8, 0, 18));

    // Sparkles — twinkle individually
    for (int i = 0; i < 26; ++i) {
      uint8_t phase = (uint8_t)((e / 24 + sparks[i].hueOffset) & 0xFF);
      float ph = (float)phase / 255.0f;
      uint8_t bri = (uint8_t)(120 + 130 * (0.5f + 0.5f * sinf(ph * 6.283f)));
      uint16_t col = g_canvas.color565(bri, bri / 3, bri);
      if (sparks[i].size == 1) g_canvas.drawPixel (sparks[i].x, sparks[i].y, col);
      else                     g_canvas.fillCircle(sparks[i].x, sparks[i].y, 1, col);
    }

    // Pulsing halo behind the wordmark — concentric ellipses with breathing radii
    float gt = (e % 1800) / 1800.0f;
    float pulse = 0.5f + 0.5f * sinf(gt * 6.283f);
    int rxOuter = 130 + (int)(8 * pulse);
    int ryOuter =  44 + (int)(4 * pulse);
    for (int o = 0; o < 4; ++o) {
      uint8_t a = (uint8_t)(60 - o * 14);
      g_canvas.drawEllipse(160, 116, rxOuter + o*3, ryOuter + o*2,
                           g_canvas.color565(a, a / 3, a + 30));
    }

    // Small purple pet icon centered above the wordmark — fades in with
    // the rest of the splash and does a tiny vertical bob for liveliness.
    {
      float alpha = e < 700 ? (float)e / 700.0f : 1.0f;
      int   icx   = 160;
      int   icy   = 42 + (int)(sinf(e / 600.0f) * 1.5f);
      uint16_t body = g_canvas.color565((uint8_t)(150 * alpha),
                                        (uint8_t)( 80 * alpha),
                                        (uint8_t)(200 * alpha));
      uint16_t outl = g_canvas.color565((uint8_t)(100 * alpha),
                                        (uint8_t)( 40 * alpha),
                                        (uint8_t)(160 * alpha));
      uint16_t hi   = g_canvas.color565((uint8_t)(195 * alpha),
                                        (uint8_t)(135 * alpha),
                                        (uint8_t)(230 * alpha));
      uint16_t eye  = g_canvas.color565((uint8_t)( 30 * alpha),
                                        (uint8_t)(  5 * alpha),
                                        (uint8_t)( 50 * alpha));
      // Ears
      g_canvas.fillCircle(icx - 15, icy - 12, 6, body);
      g_canvas.fillCircle(icx + 15, icy - 12, 6, body);
      g_canvas.drawCircle(icx - 15, icy - 12, 6, outl);
      g_canvas.drawCircle(icx + 15, icy - 12, 6, outl);
      // Head
      g_canvas.fillEllipse(icx, icy, 22, 18, body);
      g_canvas.drawEllipse(icx, icy, 22, 18, outl);
      // Forehead highlight
      g_canvas.fillEllipse(icx, icy - 9, 12, 3, hi);
      // Eyes
      g_canvas.fillCircle(icx - 7, icy - 2, 2, eye);
      g_canvas.fillCircle(icx + 7, icy - 2, 2, eye);
      // Smile — bottom arc of a circle, ~130° sweep, two-pixel thick so
      // it reads cleanly even at this small scale.
      {
        const int kSmileR = 7;
        for (int step = 0; step <= 14; ++step) {
          float frac = (float)step / 14.0f;
          float deg  = 25.0f + frac * 130.0f;     // 25..155
          float rad  = deg * (float)PI / 180.0f;
          int sx = icx + (int)(cosf(rad) * kSmileR);
          int sy = icy + 4 + (int)(sinf(rad) * kSmileR);
          g_canvas.fillCircle(sx, sy, 1, eye);
        }
      }
    }

    // Wordmark fade-in over the first 700ms (alpha-fake by darkening color)
    uint8_t pinR, pinG, pinB, purR, purG, purB;
    {
      float alpha = e < 700 ? (float)e / 700.0f : 1.0f;
      // Pink tint
      pinR = (uint8_t)(255 * alpha);
      pinG = (uint8_t)(140 * alpha);
      pinB = (uint8_t)(220 * alpha);
      // Deeper purple shadow
      purR = (uint8_t)(120 * alpha);
      purG = (uint8_t)( 30 * alpha);
      purB = (uint8_t)(180 * alpha);
    }

    g_canvas.setTextDatum(middle_center);

    // Umbrella brand — small "PIXEL PETS" wordmark above the per-target
    // wordmark, fades in early. Signals the project family without
    // overshadowing the device's own name.
    {
      uint8_t aBrand = (e > 250) ? 220 : (uint8_t)(e * 220 / 250);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(g_canvas.color565(180 * aBrand / 255,
                                               130 * aBrand / 255,
                                               210 * aBrand / 255));
      g_canvas.drawString("PIXEL PETS", 160, 80);
    }

    g_canvas.setTextSize(6);
    // Drop shadow (offset)
    g_canvas.setTextColor(g_canvas.color565(purR, purG, purB));
    g_canvas.drawString(TARGET_NAME, 163, 119);
    g_canvas.drawString(TARGET_NAME, 161, 117);
    // Outer outline pass — slightly bigger, darker, gives a "thicker" look
    g_canvas.setTextColor(g_canvas.color565(purR / 2, purG / 2, purB / 2));
    g_canvas.drawString(TARGET_NAME, 158, 116);
    g_canvas.drawString(TARGET_NAME, 162, 116);
    // Main wordmark — gentle hue pulse (pink ↔ magenta)
    uint8_t mix = (uint8_t)(100 + 80 * sinf(e / 280.0f));
    g_canvas.setTextColor(g_canvas.color565(pinR, mix, pinB));
    g_canvas.drawString(TARGET_NAME, 160, 115);

    // Tagline fades in after 1.2s
    if (e > 1200) {
      uint8_t a = (e > 2200) ? 255 : (uint8_t)((e - 1200) * 255 / 1000);
      uint8_t r = (uint8_t)(200 * a / 255);
      uint8_t g = (uint8_t)(120 * a / 255);
      uint8_t b = (uint8_t)(220 * a / 255);
      g_canvas.setTextSize(2);
      g_canvas.setTextColor(g_canvas.color565(r, g, b));
      g_canvas.drawString(tr(Str::AppTagline), 160, 188);
    }
    // Credit line — fades in slightly later, smaller and softer than the
    // tagline.
    if (e > 1700) {
      uint8_t a = (e > 2700) ? 255 : (uint8_t)((e - 1700) * 255 / 1000);
      uint8_t r = (uint8_t)(170 * a / 255);
      uint8_t g = (uint8_t)(100 * a / 255);
      uint8_t b = (uint8_t)(190 * a / 255);
      g_canvas.setTextSize(2);
      g_canvas.setTextColor(g_canvas.color565(r, g, b));
      g_canvas.drawString(tr(Str::SplashCredit), 160, 220);
    }

    // Two thin diagonal sparkle streaks crossing the wordmark every cycle —
    // simulated "shine" without per-pixel reads.
    if (e > 800) {
      int sweepX = (int)(((e - 800) % 2400) * 360 / 2400) - 20;
      uint16_t shine = g_canvas.color565(255, 230, 255);
      for (int dy = 0; dy < 40; dy += 2) {
        int x = sweepX + dy / 2;
        if (x < 0 || x >= 320) continue;
        g_canvas.drawPixel(x, 96 + dy, shine);
      }
    }

    g_canvas.setTextDatum(top_left);
    g_canvas.pushSprite(0, 0);
    delay(33);
  }
}

// ─── First-run blocking pickers ─────────────────────────────────────────────
//
// Run once on the very first boot, right after the splash. Each helper
// blocks until the user taps a choice, then returns the index. No back X
// — the user has to make a selection before the game starts.

static uint8_t runFirstRunLangPicker() {
  uint8_t picked = 0xFF;
  while (picked == 0xFF) {
    M5.update();
    if (M5.Touch.getCount() > 0) {
      auto t = M5.Touch.getDetail(0);
      if (t.wasPressed()) {
        for (int i = 0; i < 2; ++i) {
          if (insideRect(t.x, t.y, kLangChoiceRect[i])) {
            picked = (uint8_t)i;
            break;
          }
        }
      }
    }
    LangSelectView lv{};
    lv.now_ms  = millis();
    lv.current = 0xFF;     // no current selection on first run
    lv.firstRun = true;
    drawLangSelectScreen(g_canvas, lv);
    g_canvas.pushSprite(0, 0);
    delay(33);
  }
  return picked;
}

static uint8_t runFirstRunPetPicker() {
  uint8_t picked = 0xFF;
  while (picked == 0xFF) {
    M5.update();
    if (M5.Touch.getCount() > 0) {
      auto t = M5.Touch.getDetail(0);
      if (t.wasPressed()) {
        for (int i = 0; i < 3; ++i) {
          if (insideRect(t.x, t.y, kAnimalChoiceRect[i])) {
            picked = (uint8_t)i;
            break;
          }
        }
      }
    }
    AnimalSelectView av{};
    av.now_ms  = millis();
    av.current = (AnimalType)0;     // shows bear highlighted by default
    av.firstRun = true;
    drawAnimalSelectScreen(g_canvas, av);
    g_canvas.pushSprite(0, 0);
    delay(33);
  }
  return picked;
}

#if TARGET_HAS_LLM
// ─── Voice — Tag → Pet-Action Mapping ───────────────────────────────────────

static void applyVoiceTag(const String& tag) {
    uint32_t now = millis();
    int cx = petHeadX();
    int cy = petHeadY();
    g_motion.lastInteractionMs = now;
    g_pet.forceSleep = false;

    if (tag == "EAT") {
        if (!tryFeedFromInventory(now, cx, cy)) {
            changeFullness(+15, now);
            spawnFloatToBar(FloatType::Apple, cx, cy, TargetBar::Fullness, now);
            playSound(Sound::Eat);
            flashFace(Face::Eating, 1000, now);
        }
    } else if (tag == "PET") {
        changeHappiness(+8, now); changeEnergy(+1, now); rememberStroke(now);
        spawnFloatToBar(FloatType::Heart, cx, cy, TargetBar::Happiness, now);
        playSound(Sound::Happy); flashFace(Face::Happy, 700, now);
    } else if (tag == "LOVE") {
        changeHappiness(+10, now);
        spawnFloatToBar(FloatType::Heart, cx, cy, TargetBar::Happiness, now);
        playSound(Sound::Love); flashFace(Face::Love, 1500, now);
    } else if (tag == "LAUGH") {
        changeHappiness(+5, now);
        playSound(Sound::Tickle); flashFace(Face::Laughing, 1200, now);
    } else if (tag == "SLEEP") {
        g_pet.forceSleep = true;
        playSound(Sound::Yawn); flashFace(Face::Sleepy, 1500, now);
    } else if (tag == "WAKE") {
        playSound(Sound::Wake); flashFace(Face::Idle, 400, now);
    } else if (tag == "GREET") {
        changeHappiness(+3, now); changeEnergy(+2, now);
        spawnFloatToBar(FloatType::Heart, cx, cy, TargetBar::Happiness, now);
        playSound(Sound::Greet); flashFace(Face::Happy, 900, now);
    } else if (tag == "SAD") {
        changeHappiness(-5, now);
        playSound(Sound::Sad); flashFace(Face::Sad, 1500, now);
    } else if (tag == "STARTLE") {
        changeHappiness(-3, now);
        playSound(Sound::Startle); flashFace(Face::Startled, 600, now);
    } else if (tag == "SING") {
        playSound(Sound::Singing); flashFace(Face::Speaking, 2000, now);
    } else if (tag == "DANCE") {
        playSound(Sound::MediaBabble); flashFace(Face::Speaking, 2500, now);
        changeHappiness(+3, now);
    } else if (tag.startsWith("TOY_")) {
        Toy which = Toy::Ball;
        if      (tag == "TOY_MOUSE")     which = Toy::Mouse;
        else if (tag == "TOY_RATTLE")    which = Toy::Rattle;
        else if (tag == "TOY_BUTTERFLY") which = Toy::Butterfly;
        else if (tag == "TOY_PLUSH")     which = Toy::Plush;
        g_pet.persisted.selectedToy = (uint8_t)which;
        g_pet.persisted.toyRepeatCount = 0;
        // Toy-Play-Animation triggern
        g_pet.toyPlayStartMs   = now;
        g_pet.toyPlayDurMs     = 3000;
        g_pet.toyPlayWhich     = which;
        g_pet.toyPlayBored     = false;
        g_pet.toyPlayFromLeft  = (now & 1) == 0;
        changeHappiness(+5, now); changeEnergy(-2, now);
        spawnFloatToBar(FloatType::Heart, cx, cy, TargetBar::Happiness, now);
        playSound(Sound::Excited); flashFace(Face::Excited, 1500, now);
    } else if (tag.startsWith("MEDIA_")) {
        if (tag == "MEDIA_MOVIE")         g_pet.mediaActive = Media::Movies;
        else if (tag == "MEDIA_GAME")     g_pet.mediaActive = Media::Games;
        else if (tag == "MEDIA_INTERNET") g_pet.mediaActive = Media::Internet;
        else if (tag == "MEDIA_SOCIAL")   g_pet.mediaActive = Media::Social;
        else if (tag == "MEDIA_FRIENDS")  g_pet.mediaActive = Media::Friends;
        else if (tag == "MEDIA_RADIO")    g_pet.mediaActive = Media::Radio;
        if (g_pet.mediaActive == Media::Friends) {
            changeHappiness(+5, now);
            playSound(Sound::Greet);
            flashFace(Face::Happy, 1000, now);
        } else if (g_pet.mediaActive == Media::Radio) {
            // Radio per Voice-Befehl: gleiche Setup wie Tap auf Modal-Eintrag.
            g_pet.mediaStartMs         = now;
            g_pet.lastMediaHapDecayMs  = now;
            g_pet.lastMediaEngDecayMs  = now;
            g_pet.lastMediaFullDecayMs = now;
            g_pet.lastMediaSoundMs     = 0;
            flashFace(Face::Excited, 1200, now);
#if TARGET_HAS_WIFI
            webradio::setVolume(g_volume);
            webradio::start((uint8_t)g_pet.persisted.language);
#endif
        } else {
            changeHappiness(-2, now);
            playSound(Sound::MediaBabble);
        }
    }
    // IDLE → no-op
}

// ─── Voice — Callbacks ──────────────────────────────────────────────────────

static String g_lastKwsId, g_lastVadId, g_lastWhisperId, g_lastLlmId;

static void onVoiceSetup(const char* unit, const String& work_id) {
    Serial.printf("[setup] %s -> work_id='%s'\n", unit, work_id.c_str());
    String u(unit);
    if      (u == "kws")     g_lastKwsId     = work_id;
    else if (u == "vad")     g_lastVadId     = work_id;
    else if (u == "whisper") g_lastWhisperId = work_id;
    else if (u == "llm")     g_lastLlmId     = work_id;
}
static void onVoiceWake() {
    Serial.println("[wake]");
    playSound(Sound::SayHello);
    // Wake the pet and switch to listening / thinking mode. The
    // updateFaceFromState logic then shows the Speaking/Excited
    // expression until onVoiceTags() ends the mode again.
    g_pet.forceSleep            = false;
    g_pet.voiceListening        = true;
    g_pet.voiceListeningUntilMs = millis() + 30000;   // 30 s safety timeout
    g_motion.lastInteractionMs  = millis();
}
static void onVoiceSpeechEnd() {
    Serial.println("[speech-end]");
    playSound(Sound::SayOkay);
}
static void onVoiceTranscribed(const String& text) {
    Serial.printf("[whisper] %s\n", text.c_str());
}
// The LLM can answer with 1..MAX_TAG_SEQUENCE tags. Instead of running
// them all at once we queue the tags and trigger one per step in the
// main loop, with gaps in between, so the animations don't overlap.
static String   g_tagQueue[voice::MAX_TAG_SEQUENCE];
static int      g_tagQueueLen      = 0;
static int      g_tagQueueHead     = 0;
static uint32_t g_tagNextDispatchMs = 0;

// Pause between listen-end and the first reaction tag. Lets the
// Speaking/Excited alternating expression finish cleanly and gives the
// SayOkay tone time to play before the first reaction starts.
constexpr uint32_t VOICE_LISTEN_SETTLE_MS = 800;
// Small buffer between consecutive tag animations, added on top of the
// actual flashFace duration of the previous tag.
constexpr uint32_t TAG_GAP_BUFFER_MS = 250;
// Fallback duration for tags that don't set a flashFace (e.g. MEDIA_GAME)
// so the next tag still has a reasonable gap.
constexpr uint32_t TAG_NO_FLASH_DUR_MS = 800;

static bool voiceModalOpen() {
    return g_settings.open || g_pet.shutdownPending ||
           g_pet.cleaningMode || g_pet.foragingMode ||
           g_pet.toySelectMode || g_pet.mediaSelectMode ||
           g_pet.travelSelectMode || g_pet.activityMode ||
           g_pet.timerScreenMode || g_pet.travelTransitionMode ||
           g_pet.friendsMode;
}

static void onVoiceTags(const String tags[], int count, const String& raw) {
    Serial.printf("[parsed-tags] %d tag(s) (raw: '%s'):", count, raw.c_str());
    for (int i = 0; i < count; ++i) Serial.printf(" %s", tags[i].c_str());
    Serial.println();

    // LLM responded → end the listen / think animation.
    g_pet.voiceListening = false;

    if (voiceModalOpen()) return;

    // A new sequence replaces any one still running.
    g_tagQueueLen  = 0;
    g_tagQueueHead = 0;
    for (int i = 0; i < count && i < voice::MAX_TAG_SEQUENCE; ++i) {
        g_tagQueue[g_tagQueueLen++] = tags[i];
    }
    // Only start after the settle delay so the listening animation ends
    // cleanly and the confirmation tone doesn't bleed into the first reaction.
    g_tagNextDispatchMs = millis() + VOICE_LISTEN_SETTLE_MS;
}

static void dispatchPendingTag(uint32_t now) {
    if (g_tagQueueHead >= g_tagQueueLen) return;
    if ((int32_t)(now - g_tagNextDispatchMs) < 0) return;
    if (voiceModalOpen()) {
        // Modal opened in the meantime → discard the sequence.
        g_tagQueueLen = g_tagQueueHead = 0;
        return;
    }
    String tag = g_tagQueue[g_tagQueueHead++];
    Serial.printf("[dispatch-tag] %s (%d/%d)\n",
                  tag.c_str(), g_tagQueueHead, g_tagQueueLen);
    // applyVoiceTag typically calls flashFace() and thereby rewrites
    // g_pet.flashUntilMs. We remember the prior value and schedule the
    // next tag for exactly "animation end + buffer". That way the gap
    // adapts automatically to longer tags (SING/DANCE) instead of using
    // a fixed value that would cut them off.
    uint32_t preFlash = g_pet.flashUntilMs;
    applyVoiceTag(tag);
    uint32_t until = (g_pet.flashUntilMs != preFlash)
                         ? g_pet.flashUntilMs
                         : (now + TAG_NO_FLASH_DUR_MS);
    g_tagNextDispatchMs = until + TAG_GAP_BUFFER_MS;
}

static void voiceSetupTask(void* pv) {
    voice::Config vcfg;
    vcfg.system_prompt = SYSTEM_PROMPT;
    // Tie Whisper's ASR language to the persisted UI language so an EN-set
    // pet also transcribes English audio. whisper-base is multilingual, so
    // the module side doesn't change — only the language code we send in
    // the whisper.setup call. New setting takes effect on next boot.
    vcfg.whisper_language = (g_pet.persisted.language == 1) ? "en" : "de";
    g_voiceSetupOk   = voice::begin(Serial2, vcfg);
    g_voiceSetupDone = true;
    vTaskDelete(nullptr);
}
#endif  // TARGET_HAS_LLM

#if TARGET_HAS_CAMERA
// Set camera power per frame. setEnabled() is idempotent — the call is
// cheap and lets us combine several independent conditions:
//   - Display off (mode 1 or 2)   → camera off (sleep / long inactivity)
//   - Sleep face / forceSleep     → camera off (otherwise every passer-by
//                                    wakes the pet)
//   - Voice busy (HAS_LLM)        → camera off, from the wake word until
//                                    the last reaction animation fades
//   - Camera/Gallery mode         → face_detect::tick() off, otherwise its
//                                    acquireFrame()/release() steals the
//                                    one framebuffer (fb_count=1) and
//                                    drawCameraScreen never gets a live frame.
static void updateCameraState(uint32_t now) {
    bool displayOn = (g_displayPowerMode == 0);
    bool sleeping  = (g_pet.face == Face::Sleeping) || g_pet.forceSleep;
#if TARGET_HAS_LLM
    // Voice busy = wake → LLM answer → all tag animations played out.
    // Ends when Listening is false AND the tag queue is empty AND no
    // flashFace is running. flashFace is also set from other places
    // (e.g. touch), so we only check it in combination with the voice
    // indicators.
    bool voiceListening = g_pet.voiceListening;
    bool tagsPending    = (g_tagQueueHead < g_tagQueueLen);
    bool tagAnimRunning = (tagsPending ||
                           (int32_t)(g_tagNextDispatchMs - now) > 0) &&
                          (int32_t)(g_pet.flashUntilMs - now) > 0;
    bool voiceBusy = voiceListening || tagsPending || tagAnimRunning;
#else
    bool voiceBusy = false;
#endif
    bool cameraInUse = g_pet.cameraMode || g_pet.galleryMode;
    // Radio running → face_detect off. Skin analysis + frame capture are
    // the biggest PSRAM/CPU competitors to the audio library on the CoreS3;
    // without this lock the stream stutters noticeably.
    bool radioActive = (g_pet.mediaActive == Media::Radio);
    face_detect::setEnabled(displayOn && !sleeping && !voiceBusy &&
                            !cameraInUse && !radioActive);
}
#endif  // TARGET_HAS_CAMERA

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
  // Earliest possible Serial output — if the device hangs before this,
  // the linker/bootloader rejected the binary. If you see "BOOT 1" in
  // `pio device monitor` then setup() entered, narrow down from there.
  Serial.begin(115200);
  delay(50);
  Serial.println(F("[boot] BOOT 1 setup() entered"));

  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.println(F("[boot] BOOT 2 M5.begin OK"));

  M5.Display.setRotation(1);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setBrightness(BRIGHTNESS_LEVELS[g_brightnessLevel]);
  Serial.println(F("[boot] BOOT 3 display ready"));

  // Fallback: when no NTP could set the clock (e.g. WiFi failed) and the
  // RTC is still at its factory default, seed it with a known time so the
  // time-of-day features keep working offline. The battery-backed RTC then
  // ticks from there across reboots, so this branch only fires once on a
  // fresh device.
  bool rtcFromFallback = false;
  {
    m5::rtc_datetime_t check;
    if (!M5.Rtc.getDateTime(&check) || check.date.year < 2024) {
      m5::rtc_datetime_t fallback{ {2026, 4, 25}, {23, 18, 0} };
      M5.Rtc.setDateTime(&fallback);
      // Also pull the libc system clock along — M5.begin loads it from
      // the RTC *before* this point, so otherwise time(nullptr) would
      // still hold the year-2000 garbage value. Consequence: any
      // playLockoutEndSec committed in this session would be persisted
      // against the wrong epoch (~26 years off → break display in
      // thousands of hours).
      setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
      tzset();
      struct tm tmFb{};
      tmFb.tm_year = 2026 - 1900;
      tmFb.tm_mon  = 4 - 1;
      tmFb.tm_mday = 25;
      tmFb.tm_hour = 23;
      tmFb.tm_min  = 18;
      tmFb.tm_sec  = 0;
      tmFb.tm_isdst = -1;
      time_t fbEpoch = mktime(&tmFb);
      if (fbEpoch > 0) {
        struct timeval tv{};
        tv.tv_sec = fbEpoch;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
      }
      rtcFromFallback = true;
    }
  }

  M5.Display.fillScreen(TFT_BLACK);

  g_canvas.setPsram(true);
  g_canvas.setColorDepth(16);
  if (!g_canvas.createSprite(M5.Display.width(), M5.Display.height())) {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.println("Canvas alloc failed!");
    while (true) delay(1000);
  }
  g_canvas.setTextSize(2);

  screenshot::begin(&g_canvas);
  screenshot::registerAnimalAccessors(
      []() -> uint8_t { return (uint8_t)(g_pet.persisted.animal & 3); },
      [](uint8_t a)   { g_pet.persisted.animal = (uint8_t)(a & 3); });
  screenshot::registerLangSetter(
      [](uint8_t l) {
        g_lang = l ? 1 : 0;
        g_pet.persisted.language = g_lang;
      });

  M5.Speaker.setVolume(g_volume);

  // Load persisted state BEFORE the splash so we can:
  //   1) honour the parental 30-min lockout if it's still active,
  //   2) apply the saved brightness/volume in time for the splash itself.
  Serial.println(F("[boot] BOOT 4 canvas + speaker ready"));

  if (!loadPersisted(g_pet.persisted)) {
    g_pet.persisted.needs = { 60, 100, 100 };
    g_pet.persisted.born = currentDate();
    g_pet.persisted.lastSeen = g_pet.persisted.born;
    g_pet.persisted.brightnessLevel = g_brightnessLevel;
    g_pet.persisted.volume          = g_volume;
    g_pet.persisted.animal          = 0;             // default Bear
    g_pet.persisted.apples          = 3;             // start the user with a few
    g_pet.persisted.berries         = 0;
    g_pet.persisted.fish            = 0;
    g_pet.persisted.selectedToy     = 0;             // default Ball
    g_pet.persisted.toyRepeatCount  = 0;
    g_pet.persisted.selectedScene   = 0;             // default Meadow
    g_pet.persisted.bestButterflies = 0;
    g_pet.persisted.bestStack       = 0;
    g_pet.persisted.bestMushrooms   = 0;
    g_pet.persisted.bestSurf        = 0;
    g_pet.persisted.bestScorpion    = 0;
    g_pet.persisted.bestAsteroids   = 0;
    g_pet.persisted.bestCross       = 0;
    for (int i = 0; i < 7; ++i) g_pet.persisted.gameCooldownEnd[i] = 0;
    g_pet.persisted.travelCooldownEnd = 0;
    g_pet.persisted.playLockoutEndSec = 0;
    g_pet.persisted.language          = 0;     // default DE
    g_pet.persisted.languageChosen    = 0;     // → triggers first-run picker
    g_pet.persisted.sessionLimitMin   = 30;    // default 30 min
    g_pet.persisted.totalPlayMin      = 0;
    g_pet.persisted.sportLastDay      = WallDate{0, 0, 0};
    g_pet.persisted.sportStreakDays   = 0;
    g_pet.persisted.sportTotalReps    = 0;
    g_pet.persisted.lastNtpDay        = WallDate{0, 0, 0};
    g_pet.persisted.pipMode           = 0;     // default off (opt-in)
  } else {
    if (g_pet.persisted.selectedToy   >= kToyCount)   g_pet.persisted.selectedToy   = 0;
    if (g_pet.persisted.selectedScene >= kSceneCount) g_pet.persisted.selectedScene = 0;
    if (g_pet.persisted.born.year == 0) g_pet.persisted.born = currentDate();
    if (g_pet.persisted.animal > 2)     g_pet.persisted.animal = 0;
    // Restore the user's last brightness/volume choice
    if (g_pet.persisted.brightnessLevel <= 3) {
      g_brightnessLevel = g_pet.persisted.brightnessLevel;
      M5.Display.setBrightness(BRIGHTNESS_LEVELS[g_brightnessLevel]);
    }
    g_volume = g_pet.persisted.volume;
    M5.Speaker.setVolume(g_volume);
  }

  // RTC came via the fallback path above (no battery-backed RTC or dead
  // coin cell): in that case the persisted playLockoutEndSec was set
  // against a once-real NTP time and is no longer meaningfully comparable
  // to the current ~6-days-in-the-past fallback epoch. The difference
  // would be several thousand "minutes of break". Drop the value once —
  // the next real NTP sync will set a fresh lockout if one is due.
  if (rtcFromFallback && g_pet.persisted.playLockoutEndSec != 0) {
    Serial.println(F("[lockout] RTC from fallback — clearing stale lockout"));
    g_pet.persisted.playLockoutEndSec = 0;
    savePersisted(g_pet.persisted);
  }

  // Parental break-time check — if a 30-min lockout from a previous
  // session hasn't expired yet, show how much time is left for 5 s and
  // power the device back off. Skipped when the RTC isn't synced (we'd
  // otherwise lock the user out forever).
  //
  // Hidden parent override: holding a finger **anywhere on the screen**
  // for 2 seconds during the lockout-screen window clears the lockout
  // and resumes booting. Single-touch is more robust than the previous
  // BtnA+BtnC combo (Core2's bezel "buttons" are capacitive zones — the
  // FT6336U doesn't reliably report two simultaneous touches there).
  {
    uint32_t epoch = nowEpoch();
    Serial.printf("[lockout] epoch=%lu lockEnd=%lu\n",
                  (unsigned long)epoch,
                  (unsigned long)g_pet.persisted.playLockoutEndSec);
    // Plausibility check: lockEnd may be at most kBreakDurationSec in
    // the future. Anything above that means lockEnd was at some point
    // written against a different time domain (e.g. RTC garbage, NTP
    // glitch) and is now unusable. Rather than risk a "Break: 60000 min"
    // display we clear the value once.
    if (epoch > 0 &&
        g_pet.persisted.playLockoutEndSec > epoch + kBreakDurationSec) {
      Serial.printf("[lockout] lockEnd %lu s in future (>%lu s) — corrupt, clearing\n",
                    (unsigned long)(g_pet.persisted.playLockoutEndSec - epoch),
                    (unsigned long)kBreakDurationSec);
      g_pet.persisted.playLockoutEndSec = 0;
      savePersisted(g_pet.persisted);
    }
    if (epoch > 0 && g_pet.persisted.playLockoutEndSec > epoch) {
      uint32_t remainingSec0 = g_pet.persisted.playLockoutEndSec - epoch;
      AnimalType lockAnimal =
          (AnimalType)(g_pet.persisted.animal & 3);
      // Stretch the screen window so adults have a chance to figure out
      // the override even on the first try.
      constexpr uint32_t kLockoutWindowMs = 8000;
      constexpr uint32_t kHoldNeededMs    = 2000;
      uint32_t start     = millis();
      uint32_t holdStart = 0;
      bool     resetFired = false;
      Serial.printf("[lockout] %lu s remaining — hold finger 2s to clear\n",
                    (unsigned long)remainingSec0);
      while (millis() - start < kLockoutWindowMs && !resetFired) {
        M5.update();
        bool touching = (M5.Touch.getCount() > 0);
        if (touching) {
          if (holdStart == 0) {
            holdStart = millis();
            Serial.println(F("[lockout] touch begin"));
          }
          if (millis() - holdStart >= kHoldNeededMs) {
            g_pet.persisted.playLockoutEndSec = 0;
            savePersisted(g_pet.persisted);
            Serial.println(F("[lockout] OVERRIDE — lockout cleared"));
            resetFired = true;
            break;
          }
        } else if (holdStart != 0) {
          Serial.println(F("[lockout] touch released early — resetting hold"));
          holdStart = 0;
        }

        uint32_t elapsedSec = (millis() - start) / 1000u;
        uint32_t rem = (remainingSec0 > elapsedSec)
                         ? (remainingSec0 - elapsedSec) : 0;
        drawLockoutScreen(g_canvas, rem, millis(), lockAnimal);

        // Progress bar appears only while a finger is actually held —
        // hidden the rest of the time so a curious kid sees no clue.
        if (holdStart != 0) {
          uint32_t held = millis() - holdStart;
          if (held > kHoldNeededMs) held = kHoldNeededMs;
          int barW = (int)(180 * held / kHoldNeededMs);
          g_canvas.fillRoundRect(70,  62, 180, 8, 4,
                                 g_canvas.color565(40, 50, 80));
          g_canvas.fillRoundRect(70,  62, barW, 8, 4,
                                 g_canvas.color565(120, 220, 140));
          g_canvas.drawRoundRect(70,  62, 180, 8, 4,
                                 g_canvas.color565(180, 220, 200));
        }

        g_canvas.pushSprite(0, 0);
        delay(40);
      }
      if (!resetFired) {
        Serial.println(F("[lockout] window elapsed without override, powering off"));
        M5.Power.powerOff();
        return;
      }
    }
  }

  // Sync the active language from the saved value before we show anything
  // textual to the user.
  g_lang = (g_pet.persisted.language < 2) ? g_pet.persisted.language : 0;

  // First-run flow — language picker fires BEFORE the splash so both the
  // splash text and the subsequent pet picker render in the chosen
  // language.
  bool firstRun = !g_pet.persisted.languageChosen;
  if (firstRun) {
    g_pet.persisted.language = runFirstRunLangPicker();
    g_lang = g_pet.persisted.language;
  }

  Serial.println(F("[boot] BOOT 5 persisted loaded, language synced"));

  // Pull cached world data (city, weather, sun) into RAM. Fresh fetch
  // happens after WiFi comes up below; the local moon phase doesn't need
  // network at all and gets computed as soon as the RTC has time.
  loadWorldCache();
  {
    uint32_t epoch = nowEpoch();
    if (epoch > 0) recomputeMoonPhase(epoch);
  }

  // Start WiFi connect + NTP sync — as a task on Core 0, runs in parallel
  // with the splash on Core 1. Result stamping (lastNtpDay) and a possible
  // wait overlay happen AFTER the splash, see below.
  startBootSyncAsync();
  Serial.println(F("[boot] BOOT 6 sync task spawned"));

#if TARGET_HAS_LLM
  // Voice pipeline: open Serial2 to the LLM module, register callbacks,
  // then spawn the setup task on Core 0 — splash animation keeps running
  // on Core 1 in the meantime.
  {
    int rxd = M5.getPin(m5::pin_name_t::port_c_rxd);
    int txd = M5.getPin(m5::pin_name_t::port_c_txd);
    Serial2.begin(115200, SERIAL_8N1, rxd, txd);
    voice::onSetup(onVoiceSetup);
    voice::onWake(onVoiceWake);
    voice::onSpeechEnd(onVoiceSpeechEnd);
    voice::onTranscribed(onVoiceTranscribed);
    voice::onTags(onVoiceTags);
    xTaskCreatePinnedToCore(voiceSetupTask, "voice-setup", 8192,
                             nullptr, 1, nullptr, 0);
  }
  Serial.println(F("[boot] voice setup task spawned"));
#endif  // TARGET_HAS_LLM

  // Splash screen — only reached if the lockout check let us through.
  showStartscreen();
  Serial.println(F("[boot] BOOT 7 splash done"));

  // If the boot-sync task takes longer than the splash, keep polling here
  // with a progress overlay until Done or hard timeout (12 s).
  if (g_syncStage != SyncStage::Done) {
    uint32_t waitStart = millis();
    while (g_syncStage != SyncStage::Done && millis() - waitStart < 12000) {
      drawSyncProgressOverlay(g_canvas, (uint8_t)g_syncStage);
      g_canvas.pushSprite(0, 0);
      delay(120);
    }
    Serial.printf("[boot] sync wait-overlay done (stage=%u, elapsed=%lu ms)\n",
                  (unsigned)g_syncStage,
                  (unsigned long)(millis() - waitStart));
  }
  // A successful NTP/HTTP-Date set g_syncStampNtpDay — persist it now
  // from the main loop (savePersisted from the task itself would risk
  // concurrent NVS access).
  if (g_syncStampNtpDay) {
    g_pet.persisted.lastNtpDay = currentDate();
    savePersisted(g_pet.persisted);
    g_syncStampNtpDay = false;
  }

#if TARGET_HAS_WIFI
  // Pip-link companion listener — start the listener if the user opted in
  // via Settings → Pip-Modus. The treat handler must be registered BEFORE
  // begin() so the listener is wired up the moment the radio comes alive.
  pip_link::setWandHandler(
    [](uint8_t peakCount, uint8_t /*senderAnimal*/, uint32_t now_ms) {
      // Pip-side wand: each user up/down peak = one hop on the home pet.
      // Cap at 9 (mirrors kWandMaxPeaks on the Pip side).
      if (peakCount > 9) peakCount = 9;
      if (peakCount == 0) return;
      // Reuse the existing hop-chain animation — same one the rapid-
      // tap-burst feature uses. The renderer picks up hopChainCount /
      // hopChainStartMs and animates a parabolic hop per slot; tick-
      // HopChain fires per-hop effects (Laughing face + Tickle giggle
      // + Heart float + 2 happiness). Net result: pet visibly hops N
      // times in a row instead of "just looking excited".
      if (g_pet.hopChainCount == 0) {
        g_pet.hopChainCount   = peakCount;
        g_pet.hopChainStartMs = now_ms;
        g_pet.hopChainLastIdx = -1;
      }
      Serial.printf("[main] pip-link wand → %u hops queued\n",
                    (unsigned)peakCount);
    });
  pip_link::setTreatHandler(
    [](pip_link::PipTreatKind kind, uint8_t /*senderAnimal*/, uint32_t now_ms) {
      // Treat thrown by a paired Pip — small reward burst. Treat kind is
      // logged (apple/carrot/bone) but doesn't drive distinct animations
      // yet; future cosmetic-only refinement.
      changeHappiness(+5, now_ms);
      changeFullness(+8, now_ms);
      spawnFloatToBar(FloatType::Apple, petHeadX(), petHeadY(),
                      TargetBar::Fullness, now_ms);
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now_ms);
      // Apple / Carrot / Bone are all edible — match the existing
      // tryFeedFromInventory reaction (Eating face + Eat sound) so the
      // pet visibly chews instead of just looking excited.
      flashFace(Face::Eating, 1200, now_ms);
      playSound(Sound::Eat);
      Serial.printf("[main] pip-link treat → kind=%u happiness +5 fullness +8\n",
                    (unsigned)kind);
    });
  pip_link::begin(g_pet.persisted.pipMode != 0);
#endif

#if TARGET_HAS_LLM
  // Safety wait: if voice isn't ready yet, wait up to 30 s
  {
    uint32_t waitStart = millis();
    while (!g_voiceSetupDone && millis() - waitStart < 30000) delay(100);
    Serial.printf("[boot] voice ready=%d kws=%s vad=%s whisper=%s llm=%s\n",
                  (int)g_voiceSetupOk,
                  g_lastKwsId.c_str(), g_lastVadId.c_str(),
                  g_lastWhisperId.c_str(), g_lastLlmId.c_str());
  }
#endif  // TARGET_HAS_LLM

#if TARGET_HAS_CAMERA
  // Initialise the face-detect camera (best effort — pet runs without it too).
  if (!face_detect::begin()) {
    Serial.println("[boot] face_detect init failed — continuing without camera");
  }
  // Mount the photo storage — if the partition is empty it gets
  // formatted automatically on first mount.
  if (!photo_store::begin()) {
    Serial.println("[boot] photo_store init failed — gallery disabled");
  }
#endif

  // Brief "Hello from <city>!" — shown only when ip-api gave us a city.
  showCityGreeting();

  // Pet picker — runs after the splash on first boot.
  if (firstRun) {
    g_pet.persisted.animal         = runFirstRunPetPicker();
    g_pet.persisted.languageChosen = 1;
    savePersisted(g_pet.persisted);
  }

  initForage();

  uint32_t now = millis();
  g_motion.lastInteractionMs = now;
  g_pet.lastDecayMs = 0;
  g_pet.lastSavedMs = now;
  // 30-minute play session starts now (resets every fresh boot).
  g_pet.sessionStartMs = now;

  WallDate today = currentDate();
  int daysAway = daysBetween(g_pet.persisted.lastSeen, today);
  // Goo-Goo as the canonical "hello world" babble. If we've been away for
  // a long time the pet still gets the "missed you" Love clip.
  playSound(daysAway > 0 ? Sound::Love : Sound::GooGoo);
  flashFace(Face::Speaking, 2000, millis());
}

// (Old direct-shutdown helper removed — both manual PWR clicks and the
// auto 30-min limit now go through the same bedtime sequence in the loop.)
#if 0
static void shutdownAndSave() {
  g_pet.persisted.lastSeen = currentDate();
  syncSettingsToPersisted();
  savePersisted(g_pet.persisted);
  M5.Speaker.stop();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.drawString("Tschuess!", 160, 110);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_DARKGREY);
  M5.Display.drawString("Zustand gespeichert", 160, 150);
  delay(900);

  M5.Power.powerOff();
}
#endif

// Picks a short name for the currently rendered screen — used by
// screenshot mode as a filename hint. Order matches the modal priority
// in render().
static const char* currentScreenName() {
  if (g_settings.open)            return "settings";
  if (g_pet.cleaningMode)         return "cleaning";
  if (g_pet.foragingMode)         return "foraging";
  if (g_pet.toySelectMode)        return "toy-select";
  if (g_pet.mediaSelectMode)      return "media-select";
#if TARGET_HAS_CAMERA
  if (g_pet.cameraMode)           return "camera";
  if (g_pet.galleryMode)          return "gallery";
#endif
  if (g_pet.travelSelectMode)     return "travel-select";
  if (g_pet.activityMode)         return "activity";
  if (g_pet.timerScreenMode)      return "timer";
  if (g_pet.sportSelectMode)      return "sport-select";
  if (g_pet.sportWorkoutMode)     return "sport-workout";
  if (g_pet.sportDoneMode)        return "sport-done";
  if (g_pet.friendsMode)          return "friends";
  if (g_pet.travelTransitionMode) return "travel-transition";
  return "pet";
}

void loop() {
  M5.update();
  screenshot::tick(millis(), currentScreenName());
#if TARGET_HAS_LLM
  voice::update();
  dispatchPendingTag(millis());

  // Safety timeout for voice listening: if the LLM doesn't answer
  // (e.g. module not ready), clear the mode after 30 s so the pet
  // doesn't get stuck in the thinking pose forever.
  if (g_pet.voiceListening &&
      (int32_t)(millis() - g_pet.voiceListeningUntilMs) > 0) {
    Serial.println("[voice] listening timeout — clearing");
    g_pet.voiceListening = false;
  }
#endif  // TARGET_HAS_LLM

  // Power button (small red side button) → schedule the bedtime sequence
  // so the user always sees the sleep animation before shutdown. The
  // existing safe-shutdown logic below picks it up, waits for a safe
  // moment, plays the animation, then powers off. Manual presses do NOT
  // commit the 30-min lockout — long-press (>~6 s) of the AXP192 still
  // forces a hard off without animation if firmware is locked.
  if (M5.BtnPWR.wasClicked() && !g_pet.shutdownPending) {
    g_pet.shutdownPending        = true;
    g_pet.shutdownCommitsLockout = false;
  }

  uint32_t now = millis();

  int evt = updateMotion(now);

  // ── Upside-down complaint ──────────────────────────────────────────────
  // gz tracks the device's screen-axis: ≈ +1 face-up, ≈ -1 face-down.
  // Hysteresis: enter when clearly inverted, exit only when meaningfully
  // upright again. Sustained inversion for 1.2 s triggers an initial
  // complaint, with re-complaints every 6 s while still face-down.
  if (g_motion.gz < -0.7f) {
    if (g_pet.upsideDownSinceMs == 0) g_pet.upsideDownSinceMs = now;
  } else if (g_motion.gz > -0.3f) {
    g_pet.upsideDownSinceMs = 0;
  }
  if (g_pet.upsideDownSinceMs != 0 &&
      now - g_pet.upsideDownSinceMs >= 1200 &&
      now - g_pet.lastComplaintMs   >= 6000 &&
      !g_settings.open && !g_pet.listenMode) {
    g_pet.lastComplaintMs = now;
    changeHappiness(-1, now);
    flashFace(Face::Sad, 2000, now);
    playSound(Sound::UpsideDown);
  }

  // ── Standing orientation reactions ──────────────────────────────────────
  // 0=Other, 1=Upright, 2=UpsideDown, 3=LeftEdge, 4=RightEdge.
  // The Core2's IMU has +Y pointing toward the top of the screen, so when
  // the device is set down upright (USB jack on the table, screen facing
  // up at the user), the gravity reading along Y is ≈ +1. Inverted (USB
  // up) reads ≈ -1.
  uint8_t newStand = 0;
  if      (g_motion.gy > +0.78f) newStand = 1;     // Upright (USB jack down)
  else if (g_motion.gy < -0.78f) newStand = 2;     // UpsideDown (USB jack up)
  else if (g_motion.gx > +0.78f) newStand = 4;     // RightEdge
  else if (g_motion.gx < -0.78f) newStand = 3;     // LeftEdge

  if (newStand != g_pet.curStand) {
    g_pet.curStand           = newStand;
    g_pet.standStableSinceMs = now;
  }

  bool inModal = g_settings.open || g_pet.cleaningMode || g_pet.foragingMode ||
                 g_pet.cameraMode || g_pet.galleryMode ||
                 g_pet.toySelectMode || g_pet.mediaSelectMode ||
                 g_pet.travelSelectMode || g_pet.activityMode ||
                 g_pet.timerScreenMode || g_pet.travelTransitionMode ||
                 g_pet.shutdownAnnouncedAtMs != 0 ||
                 g_pet.timerExpiring;

#if TARGET_HAS_CAMERA
  // Face detection: when someone is in front of the camera and we're not
  // in a modal screen, the pet greets once per cooldown window.
  // Switched off while sleeping — otherwise every passer-by would wake
  // the pet up immediately, which makes sleep practically impossible.
  if (!inModal && !g_pet.listenMode && !g_pet.friendsMode &&
      g_pet.face != Face::Sleeping && !g_pet.forceSleep &&
      face_detect::tick(now)) {
      g_pet.forceSleep = false;
      g_motion.lastInteractionMs = now;
      flashFace(Face::Happy, 2000, now);
      playSound(Sound::GooGoo);
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now);
      changeHappiness(+3, now);
  }
#endif

  if (g_pet.curStand != 0 && !inModal && !g_pet.listenMode &&
      now - g_pet.standStableSinceMs >= 500) {
    static const uint32_t kStandCooldown[5] = { 0, 8000, 8000, 6000, 6000 };
    uint32_t cd = kStandCooldown[g_pet.curStand];
    if (now - g_pet.lastStandReactMs[g_pet.curStand] >= cd) {
      g_pet.lastStandReactMs[g_pet.curStand] = now;
      switch (g_pet.curStand) {
        case 1:   // Upright — happy + Goo-Goo
          g_pet.forceSleep = false;
          flashFace(Face::Happy, 2200, now);
          playSound(Sound::GooGoo);
          spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                          TargetBar::Happiness, now);
          changeHappiness(+2, now);
          break;
        case 2:   // UpsideDown standing — sad + jammern
          flashFace(Face::Sad, 2200, now);
          playSound(Sound::Sad);
          changeHappiness(-1, now);
          break;
        case 3:   // LeftEdge — giggle + wobble
        case 4:   // RightEdge — giggle + wobble
          flashFace(Face::Laughing, 1800, now);
          playSound(Sound::Tickle);
          g_pet.sideShakeStartMs = now;
          changeHappiness(+1, now);
          break;
      }
    }
  }

  // Settings overlay still lets pet logic tick in the background, but blocks
  // motion-based reactions so a user adjusting volume doesn't accidentally
  // pet/shake the pet.
  if (!g_settings.open) {
    if (evt == 1) {
      g_pet.forceSleep = false;
      changeHappiness(+5, now);
      changeEnergy(+1, now);                 // sustained petting energises
      rememberStroke(now);
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now);
      if (now - g_pet.lastPurrMs > 1300) {
        g_pet.lastPurrMs = now;
        playSound(Sound::Pet);
      }
    } else if (evt == 3) {
      // Vertical shake → rain shower! Pet gets surprised, drenched, then
      // shakes itself dry. Net effect on mood is mildly positive (it's
      // playful), but we don't pile on happiness because being soaked
      // isn't pure joy.
      g_pet.forceSleep = false;
      g_pet.rainStartMs = now;
      flashFace(Face::Startled, 1800, now);
      changeHappiness(+3, now);
      playSound(Sound::RainShake);
      vibratePulse(now, 350);    // longer rumble — water hitting the head
      // Water also washes the pet's body clean (but doesn't sweep piles).
      washPetBody();
    } else if (evt == 2) {
      // Shake = playtime! The selected toy flies across, the pet hops in
      // sync, and a love-boost lands. After the same toy is used 3+ times
      // in a row the pet gets bored: the toy still appears (so the user
      // sees what triggered) but the animation is short, listless, and no
      // happiness bonus lands. Picking a different toy in the toy-select
      // screen resets the counter.
      g_pet.forceSleep = false;
      Toy chosen = (Toy)(g_pet.persisted.selectedToy < kToyCount
                           ? g_pet.persisted.selectedToy : 0);

      // Repeat counter: increment on each play. The boredom kicks in once
      // the counter reaches 3, so the user feels diminishing returns on the
      // 3rd consecutive play with the same toy.
      uint8_t before = g_pet.persisted.toyRepeatCount;
      if (before < 255) g_pet.persisted.toyRepeatCount++;
      bool bored = (before >= 3);

      bool leftToRight = (now & 1) == 0;
      g_pet.toyPlayStartMs   = now;
      g_pet.toyPlayWhich     = chosen;
      g_pet.toyPlayBored     = bored;
      g_pet.toyPlayFromLeft  = leftToRight;
      g_pet.toyPlayDurMs     = bored ? 1200 : 2800;

      if (bored) {
        flashFace(Face::Sad, 700, now);
        // No happiness bump, no celebratory sound — just a subdued sad clip
        // throttled by the existing setFace cooldown system.
      } else {
        rememberStroke(now);
        changeHappiness(+10, now);
        flashFace(Face::Excited, 1000, now);
        playSound(Sound::Tickle);
        vibratePulse(now, 130);    // short burst — toy thrown
      }
    }

#if TARGET_HAS_HARD_BUTTONS
    // Core2: physische Buttons unter dem Display. Werden parallel zu den
    // Touch-Zonen am unteren Bildschirmrand ausgewertet (siehe
    // handleTouchPet); Hard-A/B/C → Greet/Feed/Sleep, Touch-Strip →
    // Greet/Tickle/Sleep. drawButtonHints zeichnet die Glyphen, die auf
    // Core2 sowohl Hardware-Beschriftung als auch Touch-Affordance sind.
    // While webradio plays, the pet is "in listen mode" — hard-buttons
    // are blocked just like touch (see handleTouchPet for the same lock).
    // We drain the click latches so they don't fire when the radio gets
    // toggled off via the Media touch button.
    if (g_pet.mediaActive == Media::Radio) {
      (void)M5.BtnA.wasClicked();
      (void)M5.BtnB.wasClicked();
      (void)M5.BtnC.wasClicked();
    } else {
    if (M5.BtnA.wasClicked()) {
      g_pet.forceSleep = false;
      g_motion.lastInteractionMs = now;
      changeHappiness(+3, now);
      changeEnergy(+2, now);                 // greeting perks the pet up
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now);
      playSound(g_pet.face == Face::Sleeping ? Sound::Wake : Sound::Greet);
    }
    if (M5.BtnB.wasClicked()) {
      // Feeding — same gated path as a direct mouth-tap.
      g_pet.forceSleep = false;
      if (!tryFeedFromInventory(now, petHeadX(), petHeadY())) {
        flashFace(Face::Sad, 600, now);
        g_pet.basketHintUntilMs = now + 2500;
      }
    }
    if (M5.BtnC.wasClicked()) {
      g_pet.forceSleep = true;
      playSound(Sound::Yawn);
    }
    }   // end of !mediaActive==Radio else-block
#endif
  }

  // Clear the rain state once the full rain → wet → shake-off cycle is done.
  if (g_pet.rainStartMs != 0 && now - g_pet.rainStartMs > 4200) {
    g_pet.rainStartMs = 0;
  }

  // Hygiene tick — dirt slowly accumulates while the landscape is messy.
  tickDirt(now);
  if (isDirty() && now - g_pet.lastDirtPenaltyMs > 5000) {
    g_pet.lastDirtPenaltyMs = now;
    changeHappiness(-1, now);   // dirty pet feels worse
  }

  // USB charging — small one-time happiness bump when plugged in, plus a
  // slow trickle while it's connected. Pet treats getting charged like a
  // "warm meal" / battery-cuddle.
  {
    bool charging = M5.Power.isCharging();
    if (charging && !g_pet.prevCharging) {
      changeHappiness(+5, now);
      flashFace(Face::Happy, 1500, now);
      playSound(Sound::Happy);
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now);
      g_pet.lastChargeBoostMs = now;
    }
    if (charging && now - g_pet.lastChargeBoostMs > 30000) {
      g_pet.lastChargeBoostMs = now;
      changeHappiness(+1, now);
    }
    g_pet.prevCharging = charging;
  }

  // Media consumption — accelerated decay + periodic babble while glued to
  // a screen. Stops the moment the user taps the TV button again. Three
  // tiers, from gentlest to worst:
  //   Movies        — relatively passive entertainment, mild decay
  //   Games / Surf  — engaged screen time, regular decay
  //   Social        — doomscrolling, fastest decay AND fullness drain
  if (g_pet.mediaActive != Media::None && g_pet.mediaActive != Media::Radio &&
      g_pet.mediaActive != Media::Camera && g_pet.mediaActive != Media::Gallery) {
    // Radio/Camera/Gallery sind neutral bzw. positiv: kein Decay, kein
    // MediaBabble. Camera+Gallery sind eigentlich keine echten „media-
    // states" (kein mediaActive == Camera setzt sich), aber die
    // Defensive schadet nicht.
    Media m = g_pet.mediaActive;
    uint32_t hapInterval, engInterval, fullInterval, soundInterval;
    if (m == Media::Social) {
      hapInterval   =  900;
      engInterval   = 1500;
      fullInterval  = 4000;
      soundInterval = 4000;
    } else if (m == Media::Movies) {
      hapInterval   = 3500;
      engInterval   = 5000;
      fullInterval  = 0;
      soundInterval = 7000;
    } else {            // Games / Internet
      hapInterval   = 2000;
      engInterval   = 3000;
      fullInterval  = 0;
      soundInterval = 6000;
    }

    if (now - g_pet.lastMediaHapDecayMs >= hapInterval) {
      g_pet.lastMediaHapDecayMs = now;
      changeHappiness(-1, now);
    }
    if (now - g_pet.lastMediaEngDecayMs >= engInterval) {
      g_pet.lastMediaEngDecayMs = now;
      changeEnergy(-1, now);
    }
    if (fullInterval > 0 &&
        now - g_pet.lastMediaFullDecayMs >= fullInterval) {
      g_pet.lastMediaFullDecayMs = now;
      changeFullness(-1, now);    // social-only: forgot to eat
    }
    if (now - g_pet.lastMediaSoundMs >= soundInterval) {
      g_pet.lastMediaSoundMs = now;
      playSound(Sound::MediaBabble);
    }
  }
#if TARGET_HAS_WIFI
  // Pip-link companion listener — drains incoming ESP-NOW packets from a
  // paired Pip and dispatches into pet-state callbacks. Phase 1: skeleton,
  // body is empty unless pipMode is enabled and Phase 2 has shipped.
  pip_link::tick(now);

  // Radio: tick the stream decoder per frame + auto-stop after 30 min +
  // spawn a floating note in a random direction every 1.5 s.
  if (g_pet.mediaActive == Media::Radio) {
    webradio::tick(now);
    constexpr uint32_t kRadioMaxSessionMs = 30UL * 60UL * 1000UL;
    if (g_pet.mediaStartMs != 0 &&
        now - g_pet.mediaStartMs >= kRadioMaxSessionMs) {
      Serial.println(F("[radio] auto-stop after 30 min"));
      webradio::stop();
      g_pet.mediaActive  = Media::None;
      g_pet.mediaStartMs = 0;
    }
    // Note spawn — two alternating drift directions for a livelier feel.
    if (g_pet.mediaActive == Media::Radio &&
        now - g_pet.lastMediaSoundMs >= 1500) {
      g_pet.lastMediaSoundMs = now;
      int sx = petHeadX() + ((now & 1) ? -16 : 16);
      int sy = petHeadY() - 20;
      int dx = sx + ((now & 2) ? -28 : 28);
      int dy = 30;   // floats upward and out
      spawnFloat(g_pet.floats, FloatType::Note, sx, sy, dx, dy, now);
    }
  }
#endif

  bool sleeping = (g_pet.face == Face::Sleeping);
  uint8_t energyBefore = g_pet.persisted.needs.energy;
  decayNeeds(g_pet.persisted.needs, now, g_pet.lastDecayMs, sleeping);
  if (g_pet.persisted.needs.energy != energyBefore) {
    g_pet.engLastChangeMs = now;
  }
  // happiness/fullness decay is silent; we don't pulse on every −1.

  // Sleep regen visualization: occasional Z float toward energy bar
  if (sleeping && now - g_pet.lastSleepRegenMs > SLEEP_REGEN_FLOAT_MS) {
    g_pet.lastSleepRegenMs = now;
    spawnFloatToBar(FloatType::Zzz, petHeadX(), petHeadY(),
                    TargetBar::Energy, now);
  }

  bool touchActive = M5.Touch.getCount() > 0;
  int  touchX = 0, touchY = 0;
  if (touchActive) {
    auto tt = M5.Touch.getDetail(0);
    touchX = tt.x; touchY = tt.y;
  }
  handleTouch(now);
  detectCircleGesture(touchActive, touchX, touchY, now);
  detectTwoFingerGesture(now);
  tickHopChain(now);
  tickSinging(now);

  // Sport-mode rep detection runs while a workout screen is up.
  tickSportDetection(now);

  // Modal auto-close — close non-active modal screens after 2 min of no
  // touch input so the display can dim/sleep instead of burning power
  // on a forgotten dialog. Workout / activity / friends-mode are
  // explicitly excluded — those run on motion/IMU/network, not touch.
  constexpr uint32_t kModalIdleCloseMs = 2UL * 60UL * 1000UL;
  if (g_pet.lastTouchMs != 0 && now - g_pet.lastTouchMs > kModalIdleCloseMs) {
    if (g_pet.foragingMode)     { g_pet.foragingMode = false;
                                  savePersisted(g_pet.persisted); }
    else if (g_pet.toySelectMode)    g_pet.toySelectMode    = false;
    else if (g_pet.mediaSelectMode)  g_pet.mediaSelectMode  = false;
    else if (g_pet.travelSelectMode) g_pet.travelSelectMode = false;
    else if (g_pet.cleaningMode)     g_pet.cleaningMode     = false;
    else if (g_pet.sportSelectMode)  g_pet.sportSelectMode  = false;
    else if (g_pet.sportDoneMode)    g_pet.sportDoneMode    = false;
    else if (g_settings.open) {
      g_settings.open = false;
      g_settings.page = 0;
      g_settings.parentsHelpOpen     = false;
      g_settings.parentServerPageOpen = false;
      g_settings.locationPageOpen    = false;
    }
  }

  // Service the captive-portal state machine while WiFi setup is open.
  if (g_settings.wifiSetupOpen) {
    wifiSetupTick();
  }

  // Standort-Refresh from the Location sub-page — runs synchronously the
  // first frame the flag is set; takes a few seconds because of the
  // WiFi + HTTP round-trip.
  if (g_settings.locationRefreshing &&
      now - g_settings.locationRefreshAtMs >= 100) {
    invalidateLocationCache();
    wifiBeginAsync();
    // 10 s Connect / 8 s NTP (siehe Kommentar bei connectAndSyncTime).
    bool ok = wifiSyncTimeKeepConnected(10000, 8000);
    if (ok) {
      uint32_t epoch = (uint32_t)time(nullptr);
      fetchWorldIfStale(epoch);
    }
    wifiPowerOff();
    g_settings.locationRefreshing = false;
  }

  // Parent web server — runs only while toggled on. Stats are pushed
  // every second so the auto-refreshing HTML stays fresh.
  parentServerTick();
  static uint32_t s_lastStatsPushMs = 0;
  static uint32_t s_lastTotalBumpMs = 0;
  if (now - s_lastStatsPushMs >= 1000) {
    s_lastStatsPushMs = now;
    ParentServerStats st{};
    st.batteryPct      = (uint8_t)M5.Power.getBatteryLevel();
    st.charging        = M5.Power.isCharging() ? 1 : 0;
    st.sessionMin      = (uint32_t)((now - g_pet.sessionStartMs) / 60000UL);
    st.totalMin        = g_pet.persisted.totalPlayMin;
    st.animal          = (uint8_t)(g_pet.persisted.animal & 3);
    st.language        = (uint8_t)g_pet.persisted.language;
    st.sessionLimitMin = g_pet.persisted.sessionLimitMin;
    parentServerSetStats(st);
  }
  // Apply any pending session-limit change submitted via the web form.
  if (parentServerHasPendingLimit()) {
    uint8_t lim = parentServerPendingLimit();
    if (lim < 5)   lim = 5;
    if (lim > 120) lim = 120;
    if (g_pet.persisted.sessionLimitMin != lim) {
      g_pet.persisted.sessionLimitMin = lim;
      savePersisted(g_pet.persisted);
    }
    parentServerClearPendingLimit();
  }
  // Lifetime play counter — bump once per minute on the regular pet view
  // (not during boot lockout / shutdown announcement). Persist hourly to
  // keep wear low.
  if (g_pet.shutdownAnnouncedAtMs == 0 &&
      now - s_lastTotalBumpMs >= 60000) {
    s_lastTotalBumpMs = now;
    g_pet.persisted.totalPlayMin++;
    if ((g_pet.persisted.totalPlayMin % 60) == 0) {
      savePersisted(g_pet.persisted);
    }
  }

  // Friends mode — ESP-NOW item-exchange. Sending phase ends on either
  // 5 taps, 60 s timeout, or back-X (sets sentCount=0xFF). Then either
  // the playback runs (rx queue non-empty) or NoFriend pose flashes.
  if (g_pet.friendsMode) {
    friendsTick(now);
    FriendsState fs = friendsGetState();
    constexpr uint32_t kFriendsSessionMs = 60000;
    // Both done: 5 items have been sent locally AND we've also seen the
    // partner's done confirmation. Only then may sending end, otherwise
    // the faster tapper would stop listening before the slower one even
    // sent their items → no rx items.
    bool bothDone       = friendsLocalDone() && friendsRemoteDone();
    bool sessionTimeout = (fs == FriendsState::Sending) &&
                          (now - g_pet.friendsModeStartMs >= kFriendsSessionMs);
    bool forcedExit     = (g_pet.friendsSentCount == 0xFF);
    // bothDone trailing window: hold the Sending state open for 2 s
    // after bothDone, so net.cpp keeps re-broadcasting our Done. Without
    // this, the *first* pet to reach bothDone would call friendsEnd()
    // and go silent, leaving the partner stuck in Sending until the
    // 60 s timeout (or a manual back-X) — which is the exact "Empfangen:
    // 3" wait-screen bug we hit. forcedExit / sessionTimeout bypass
    // the window because they're already terminal cases.
    // 3 s of trailing time after both-done. Combined with the 100 ms
    // round-robin re-broadcast in net.cpp (which also keeps running
    // during this window now), every outbox item gets ~30 extra
    // re-send chances on top of the in-session retries before the
    // session actually closes.
    // Bumped from 3000 ms when re-broadcast cadences were slowed (Item
    // 100→500 ms, Done 250→700 ms) to mitigate self-deafening TX on the
    // ESP32-S3. With slower cadences each outbox item only gets 5–10
    // re-send chances per second, so the trailing window needs more
    // headroom for late items to still squeeze through.
    constexpr uint32_t kFriendsDoneTrailingMs = 6000;
    if (bothDone && g_pet.friendsBothDoneAtMs == 0) {
      g_pet.friendsBothDoneAtMs = now;
      Serial.printf("[friends] bothDone reached @%lu — trailing %lu ms\n",
                    (unsigned long)now,
                    (unsigned long)kFriendsDoneTrailingMs);
    }
    bool trailingDone = bothDone &&
                        (now - g_pet.friendsBothDoneAtMs >= kFriendsDoneTrailingMs);
    if (fs == FriendsState::Sending &&
        (trailingDone || sessionTimeout || forcedExit)) {
      // Snapshot the rx queue before friendsEnd resets ESP-NOW state.
      uint8_t rxN = friendsRxItemCount();
      Serial.printf("[friends] sending phase done — rx=%u items, kicking "
                    "off playback (both=%d trail=%d to=%d forced=%d)\n",
                    rxN, bothDone, trailingDone, sessionTimeout, forcedExit);
      // friendsEnd would clear the queue indirectly — call it AFTER we
      // kick off playback. Stop ESP-NOW radio (it leaves the queue intact
      // because friendsClearRxQueue is what nukes it).
      friendsEnd();
      // Hand control back to the pet view; the post-session sequence runs
      // there.
      g_pet.friendsMode = false;
      if (rxN > 0) {
        g_pet.friendsPlaybackActive       = true;
        g_pet.friendsPlaybackIdx          = 0;
        g_pet.friendsPlaybackNextMs       = now + 600;   // brief pause first
        g_pet.friendsPlaybackPlayedAny    = false;
        g_pet.friendsCelebrationDone      = false;
        g_pet.friendsPlaybackKind         = 0;
        g_pet.friendsPlaybackAnimal       = 0;
        g_pet.friendsPlaybackItemStartMs  = 0;
      } else {
        // Nobody answered — small sad reaction in the pet view.
        flashFace(Face::Sad, 1800, now);
        playSound(Sound::Sad);
      }
    }
    // Force a NoFriend snapshot before friendsEnd() runs (if user opens
    // friends with no creds at all — covered in friendsBegin).
    if (fs == FriendsState::NoFriend &&
        (int32_t)(now - friendsStateStartedAt()) >= 2200) {
      friendsEnd();
      g_pet.friendsMode = false;
    }
  }

  // Post-session item playback in the pet view.
  if (g_pet.friendsPlaybackActive) {
    constexpr uint32_t kPerItemMs = 1100;
    uint8_t rxN = friendsRxItemCount();
    if (g_pet.friendsPlaybackIdx >= rxN) {
      // Playback queue done → final celebration (only if anything played).
      if (!g_pet.friendsCelebrationDone) {
        g_pet.friendsCelebrationDone = true;
        if (g_pet.friendsPlaybackPlayedAny) {
          flashFace(Face::Excited, 2200, now);
          playSound(Sound::GooGoo);
          changeHappiness(+12, now);
          for (int k = 0; k < 6; ++k) {
            spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                            TargetBar::Happiness, now);
          }
          // 700 ms of vibration as the user-tactile celebration cue.
          vibratePulse(now, 700);
        }
        g_pet.friendsPlaybackNextMs = now + 2200;
      } else if (now >= g_pet.friendsPlaybackNextMs) {
        // Cleanup and exit playback after the celebration window.
        g_pet.friendsPlaybackActive = false;
        g_pet.friendsPlaybackKind   = 0;
        friendsClearRxQueue();
      }
    } else if (now >= g_pet.friendsPlaybackNextMs) {
      const FriendsRxItem* it = friendsRxItem(g_pet.friendsPlaybackIdx);
      g_pet.friendsPlaybackIdx++;
      if (it) {
        g_pet.friendsPlaybackKind        = it->kind;
        g_pet.friendsPlaybackAnimal      = it->senderAnimal;
        g_pet.friendsPlaybackItemStartMs = now;
        g_pet.friendsPlaybackPlayedAny   = true;
        // Spawn the floater coming in from the side toward the pet head.
        bool fromLeft = ((g_pet.friendsPlaybackIdx & 1) == 0);
        int  fromX    = fromLeft ? -20 : 340;
        int  fromY    = 60 + ((int)(now / 7) % 40);
        switch (it->kind) {
          case FriendsItemGift:
            flashFace(Face::Excited, 800, now);
            playSound(Sound::Excited);
            spawnFloat(g_pet.floats, FloatType::Heart, fromX, fromY,
                       petHeadX(), petHeadY(), now, 700);
            changeHappiness(+5, now);
            break;
          case FriendsItemHeart:
            flashFace(Face::Happy, 800, now);
            playSound(Sound::Love);
            spawnFloat(g_pet.floats, FloatType::Heart, fromX, fromY,
                       petHeadX(), petHeadY(), now, 700);
            changeHappiness(+4, now);
            break;
          case FriendsItemFood:
            flashFace(Face::Eating, 800, now);
            playSound(Sound::Eat);
            spawnFloat(g_pet.floats, FloatType::Apple, fromX, fromY,
                       petHeadX(), petHeadY(), now, 700);
            changeHappiness(+3, now);
            if (g_pet.persisted.needs.fullness < 96)
              g_pet.persisted.needs.fullness += 4;
            break;
          case FriendsItemGame:
            flashFace(Face::Laughing, 800, now);
            playSound(Sound::Tickle);
            spawnFloat(g_pet.floats, FloatType::Ball, fromX, fromY,
                       petHeadX(), petHeadY(), now, 700);
            changeHappiness(+4, now);
            break;
        }
        g_pet.friendsPlaybackNextMs = now + kPerItemMs;
      }
    }
  }

  // Auto-clear somersault state once the animation has finished.
  if (g_pet.somersaultStartMs != 0 &&
      now - g_pet.somersaultStartMs >= 800) {
    g_pet.somersaultStartMs = 0;
  }
  // Auto-clear snap-back state once its damped wobble runs out.
  if (g_pet.snapStartMs != 0 && now - g_pet.snapStartMs >= 700) {
    g_pet.snapStartMs = 0;
  }

  // Vibration motor — auto-stops once the pulse duration is up.
  tickVibration(now);

  // Travel transition — auto-ends after 8 s.
  if (g_pet.travelTransitionMode &&
      now - g_pet.travelTransitionStartMs > 8000) {
    g_pet.travelTransitionMode = false;
  }

  // ── Parental play-time limit ────────────────────────────────────────
  // 1. Once the 30-minute session is up, mark a shutdown as pending.
  // 2. As soon as we hit a safe state (no mini-game, no foraging, etc.),
  //    commit the 30-minute lockout to NVS and start the bedtime sequence.
  // 3. After ~9 s of announcement + sleep animation, power the device off.
  if (!g_pet.shutdownPending &&
      now - g_pet.sessionStartMs >= kPlaySessionLimitMs()) {
    g_pet.shutdownPending        = true;
    g_pet.shutdownCommitsLockout = true;     // 30-min auto → enforce break
  }
  if (g_pet.shutdownPending && g_pet.shutdownAnnouncedAtMs == 0 &&
      isSafeToShutdown()) {
    g_pet.shutdownAnnouncedAtMs = now;
    // Auto shutdowns commit the lockout *now* — even if the user yanks
    // power during the animation the next boot respects the break.
    // Manual shutdowns via the PWR button skip this so adults can power
    // off freely.
    if (g_pet.shutdownCommitsLockout) {
      uint32_t epoch = nowEpoch();
      if (epoch > 0) {
        g_pet.persisted.playLockoutEndSec = epoch + kBreakDurationSec;
      }
    }
    g_pet.persisted.lastSeen = currentDate();
    savePersisted(g_pet.persisted);
    // Calm the pet visually + silence other audio.
    M5.Speaker.stop();
    if (g_pet.listenMode) exitListenMode();
    flashFace(Face::Sleepy, kBedtimeTotalMs, now);
  }
  if (g_pet.shutdownAnnouncedAtMs != 0 &&
      now - g_pet.shutdownAnnouncedAtMs >= kBedtimeTotalMs) {
    M5.Power.powerOff();
  }

  // Weather — re-rolls on scene change + on a 60-120 s cadence.
  tickWeather(now);

  // Timer — check expiry, drive expiry animation (sounds + vibration).
  // Loud, persistent alarm: many Goo-Goo plays, repeated vibration bursts,
  // 12-second window so it can't easily be missed even on the table.
  if (g_pet.timerActive && now >= g_pet.timerEndMs) {
    g_pet.timerActive       = false;
    g_pet.timerExpiring     = true;
    g_pet.timerExpiredAtMs  = now;
    g_pet.timerExpiredPlays = 0;
    g_pet.lastTimerExpiredSoundMs = now - 9999;   // play immediately on first tick
    vibratePulse(now, 600);                        // long initial buzz
    flashFace(Face::Speaking, 12000, now);
    g_pet.bubble.current  = Bubble::Question;
    g_pet.bubble.startMs  = now;
    g_pet.bubble.untilMs  = now + 12000;
  }
  if (g_pet.timerExpiring) {
    // Goo-Goo plays repeatedly every ~500 ms for the duration of the
    // alarm — much harder to ignore than the previous 3-tone sequence.
    if (now - g_pet.lastTimerExpiredSoundMs >= 500) {
      g_pet.lastTimerExpiredSoundMs = now;
      playSound(Sound::GooGoo);
      g_pet.timerExpiredPlays++;
      // Periodic vibration kicks every 4 plays (~2 s) so the rumble keeps
      // going for the whole alarm.
      if ((g_pet.timerExpiredPlays % 4) == 0) {
        vibratePulse(now, 350);
      }
    }
    // Auto-dismiss after 12 s if the user hasn't tapped.
    if (now - g_pet.timerExpiredAtMs > 12000) {
      dismissTimerExpiry();
    }
  }

  // Foraging-screen item update only runs while the screen is open. Outside
  // the screen the items are frozen — they spawn fresh next time the user
  // opens it (unaffected by clock time, unlike rain/dirt).
  if (g_pet.foragingMode) updateForage(now, touchActive);

  // Activity (mini-game) update — only runs while the activity screen is
  // active. Game state is per-session.
  if (g_pet.activityMode) updateActivity(now);

  updateGaze  (g_pet.gaze,   touchActive, touchX, touchY, now);
  updateTilt  (g_pet.tilt,   g_motion.gx, g_motion.gy);

  bool funEvent = false;
  bool lullabyEvent = false;
  updateWander(g_pet.wander, g_motion.gx, g_motion.gy, now,
               /*sleeping=*/sleeping, &funEvent, &lullabyEvent);

  // Sustained gentle rocking → rocked to sleep. The flash shows Sleepy
  // for a moment, the Yawn sample plays, and forceSleep then takes over
  // so the next render switches the face to Sleeping.
  if (lullabyEvent && !sleeping && !g_settings.open) {
    flashFace(Face::Sleepy, 800, now);
    playSound(Sound::Yawn);
    g_pet.forceSleep = true;
  }

  if (funEvent && !sleeping && !g_settings.open) {
    g_pet.forceSleep = false;
    // Rocking the pet feels playful — boosts mood, also a tiny fullness
    // and energy nudge so it counts toward the secondary needs too.
    changeHappiness(+5, now);
    changeFullness(+2, now);
    changeEnergy(+1, now);
    rememberStroke(now);
    spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                    TargetBar::Happiness, now);
    flashFace(Face::Happy, 700, now);
    if (now - g_pet.lastWanderFunSoundMs > 2500) {
      g_pet.lastWanderFunSoundMs = now;
      playSound(Sound::Happy);
    }
  }

  // Listen mode: poll the mic and steadily produce love while dancing.
  if (g_pet.listenMode) {
    updateMicLevel();
    if (now - g_pet.lastListenLoveMs > 2000) {
      g_pet.lastListenLoveMs = now;
      changeHappiness(+2, now);
      rememberStroke(now);
      g_motion.lastStrokeEvent = now;     // qualifies for the "recently petted" face branch
      g_motion.lastInteractionMs = now;
      spawnFloatToBar(FloatType::Heart, petHeadX(), petHeadY(),
                      TargetBar::Happiness, now);
    }
  }

  updateMicro (g_pet.micro,  now,
               g_pet.face == Face::Idle && !touchActive && !g_settings.open);

  bool recentlyPetted = (g_pet.recentStrokes >= 3 &&
                         now - g_pet.recentStrokesResetMs < 5000);
  bool justWokeUp = (now - g_pet.lastWakeMs < 1000) && g_pet.lastWakeMs > 0;
  updateBubble(g_pet.bubble, now, g_pet.persisted.needs,
               recentlyPetted, justWokeUp);

  updateFaceFromState(now);

  // Boredom babble — when the pet has been left in plain Idle for a while,
  // it occasionally talks to itself ("goo-goo"). Disabled in any modal/
  // sleep/listen state so we never overlap with other audio events.
  bool reallyIdle = (g_pet.face == Face::Idle &&
                     now - g_motion.lastInteractionMs > 25000 &&
                     !g_settings.open && !g_pet.cleaningMode &&
                     !g_pet.listenMode && !sleeping &&
                     g_pet.rainStartMs == 0);
  if (!reallyIdle) {
    g_pet.nextBabbleMs = 0;
  } else {
    if (g_pet.nextBabbleMs == 0) {
      // First scheduled babble: 15..45 s out
      uint32_t wait = 15000 + ((now ^ 0x9E3779B9u) % 30000);
      g_pet.nextBabbleMs = now + wait;
    } else if (now >= g_pet.nextBabbleMs) {
      playSound(Sound::GooGoo);
      flashFace(Face::Speaking, 2000, now);
      // Next babble: 30..90 s out
      uint32_t wait = 30000 + ((now * 7919u) % 60000);
      g_pet.nextBabbleMs = now + wait;
    }
  }

  persistIfDue(now);
  render(now);

  // Adaptive frame rate — when nothing is animating, drop the loop tick
  // to ~10 fps so the SoC can spend more time idle. Active animations
  // re-engage 1 ms ticks for smoothness. ~30 mA → ~10 mA difference on
  // truly idle pet view.
  bool animatingNow =
    g_pet.somersaultStartMs != 0 || g_pet.snapStartMs != 0 ||
    g_pet.snuggleStartMs != 0 || g_pet.singStartMs != 0 ||
    g_pet.applauseListenStartMs != 0 ||
    g_pet.hopChainCount != 0 ||
    g_pet.spreadActive || g_pet.holdLocked ||
    g_pet.friendsPlaybackActive ||
    g_pet.basketHintUntilMs > now ||
    g_pet.flashUntilMs > now ||
    (g_pet.face != Face::Idle && g_pet.face != Face::Sleeping) ||
    g_pet.toyPlayStartMs != 0 ||
    g_pet.travelTransitionMode || g_pet.timerExpiring ||
    g_pet.shutdownAnnouncedAtMs != 0 ||
    g_pet.activityMode || g_pet.cleaningMode || g_pet.foragingMode ||
    g_pet.sportWorkoutMode || g_pet.friendsMode ||
    g_pet.listenMode || g_pet.mediaActive != Media::None;
  bool displayActive = (g_displayPowerMode != 2);
  // Webradio MUST always run on the fast loop, regardless of display
  // sleep. With the slow 100 ms cadence the audio decode task on core 1
  // still gets CPU, but pre-task code paths that call audio.loop() from
  // the main loop (legacy / fallback) would underrun. Cheap insurance.
  bool audioActive = (g_pet.mediaActive != Media::None);
  uint32_t loopDelayMs = (audioActive || (animatingNow && displayActive)) ? 1u : 100u;
  M5.delay(loopDelayMs);
}
