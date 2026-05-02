#pragma once
#include <Arduino.h>
#include <M5Unified.h>

#include "target_caps.h"
#include "pet_state.h"

enum class Face : uint8_t {
  Idle, Happy, Excited, Love, Sleepy, Sleeping, Startled, Sad, Laughing, Eating,
  Speaking,
};

enum class AnimalType : uint8_t { Bear = 0, Cat = 1, Dog = 2 };

enum class Toy : uint8_t {
  Ball = 0, Mouse = 1, Rattle = 2, Butterfly = 3, Plush = 4,
};
constexpr uint8_t kToyCount = 5;

// Media consumption (TV / PC games / Internet). None = the pet is not
// staring at a screen. While anything other than None is set, the renderer
// switches to square sunken eyes and the pet's needs decay rapidly.
//
// Exceptions: Friends (ESP-NOW item exchange), Radio (web radio stream),
// Camera (taking a selfie) and Gallery (looking at photos) cause NO
// negative effects — they are social / positive activities.
enum class Media : uint8_t {
  None = 0, Movies = 1, Games = 2, Internet = 3, Social = 4, Friends = 5,
  Radio = 6, Camera = 7, Gallery = 8,
};
// Number of cells in the media modal:
//   - cores3 + visu (HAS_WIFI && HAS_CAMERA): 8 (Movies/Games/Web/Social
//                                               + Friends/Radio/Camera/Gallery)
//   - core2          (HAS_WIFI && !HAS_CAMERA):  6 (+ Friends/Radio)
//   - pip            (!HAS_WIFI && !HAS_CAMERA): 5 (+ Friends)
#if TARGET_HAS_WIFI && TARGET_HAS_CAMERA
constexpr uint8_t kMediaChoiceCount = 8;
#elif TARGET_HAS_WIFI
constexpr uint8_t kMediaChoiceCount = 6;
#else
constexpr uint8_t kMediaChoiceCount = 5;
#endif

// Travel destination — picks the background on the pet view. The default
// Meadow is the original landscape; the rest are alternative settings the
// user can send the pet to via the travel button.
enum class Scene : uint8_t {
  Meadow = 0, Bedroom = 1, Forest = 2, Beach = 3, Desert = 4, Space = 5,
  City = 6,
};
constexpr uint8_t kSceneCount = 7;

// Random weather drawn over the scene background. Indoor (Bedroom) and
// Space scenes always use Sunny — they aren't sky-driven.
enum class Weather : uint8_t {
  Sunny = 0, Cloudy = 1, Rainy = 2, Foggy = 3, Sandstorm = 4,
};
constexpr uint8_t kWeatherCount = 5;

struct Rect { int16_t x, y, w, h; };

// ─── Need bars (top strip, replace the single mood bar) ─────────────────────

constexpr int  kNeedBarY     = 12;
constexpr int  kNeedBarH     = 10;
constexpr int  kNeedBarW     = 46;
constexpr int  kNeedIconW    = 12;
constexpr int  kNeedSlotGap  = 4;
constexpr int  kNeedSlotW    = kNeedIconW + kNeedBarW + 2;       // 60

constexpr int  kHappinessSlotX = 44;
constexpr int  kEnergySlotX    = kHappinessSlotX + kNeedSlotW + kNeedSlotGap;   // 138
constexpr int  kFullnessSlotX  = kEnergySlotX    + kNeedSlotW + kNeedSlotGap;   // 230

// Bar centers (where floats land). Each bar's left edge = slot_x + icon + gap.
constexpr int  kHappinessBarX  = kHappinessSlotX + kNeedIconW + 2;
constexpr int  kEnergyBarX     = kEnergySlotX    + kNeedIconW + 2;
constexpr int  kFullnessBarX   = kFullnessSlotX  + kNeedIconW + 2;

inline int needBarCenterX(int slotX) {
  return slotX + kNeedIconW + 2 + kNeedBarW / 2;
}
inline int needBarCenterY() { return kNeedBarY + kNeedBarH / 2; }

// ─── Corner-Button-Hit-Tolerance ────────────────────────────────────────────
//
// Touch hit rect for buttons in the upper display corners (settings gear,
// back-X). Extends the click area to the physical display edge —
// Fitts's law: corner-placed targets are "infinitely large" in the
// direction of the screen edge because the finger cannot go past it.
// Visually the rendered button stays at its position (drawXyz still
// uses the original rect), only the touch test is more lax.
//
// Noticeably relevant on CoreS3 (FT5x06 loses accuracy in the last
// 4–8 px toward the bezel); on Core2 (FT6336U) it's a slight comfort
// win with no downside.
inline Rect cornerHitRect(const Rect& r) {
  Rect h = r;
  // Top edge: only if the rect already sits at the top display edge
  // (otherwise side buttons would wrongly expand up to y=0).
  if (r.y < 20) {
    h.h += h.y;
    h.y = 0;
  }
  if (r.x < 20) {
    // Top-left corner: out to x=0 and a few px tolerance to the right.
    h.w += h.x;
    h.x = 0;
    h.w += 6;
  } else if (r.x + r.w > 270) {
    // Top-right corner: 6 px tolerance to the left, out to display edge.
    h.x -= 6;
    h.w = TARGET_DISPLAY_W - h.x;
  }
  return h;
}

// ─── Top-bar gear button ────────────────────────────────────────────────────

constexpr Rect kGearButtonRect = { 6, 4, 32, 32 };

// ─── Settings page rects ────────────────────────────────────────────────────

constexpr Rect kSettingsBackRect   = { 280,   4, 36, 32 };

// ── Settings is split across two pages so each option has comfortable
//    breathing room. Page 0 holds the daily-use controls (volume,
//    brightness, help, credits); page 1 holds the configuration
//    entries (language, time, animal, reset).

// Page 0: Volume slider, Brightness dots, Help row, Credits row.
constexpr Rect kSettingsVolumeRect   = {  40,  72, 240, 18 };
constexpr Rect kBrightDotRect[4] = {
  {  44, 122, 56, 26 },
  { 108, 122, 56, 26 },
  { 172, 122, 56, 26 },
  { 236, 122, 56, 26 },
};
constexpr Rect kSettingsHelpRect     = {  40, 154, 240, 28 };
constexpr Rect kSettingsCreditsRect  = {  40, 186, 240, 28 };

// Page 1: Language, Time, Animal, Reset.
// Reset is destructive — kept at the bottom and visually separated.
constexpr Rect kSettingsLangRect     = {  40,  56, 240, 28 };
constexpr Rect kSettingsTimeRect     = {  40,  90, 240, 28 };
constexpr Rect kSettingsAnimalRect   = {  40, 122, 240, 28 };
constexpr Rect kSettingsResetRect    = {  40, 174, 240, 30 };

// Language picker (used both on first boot and via Settings → Language).
constexpr Rect kLangSelectBackRect = { 280,   4, 36, 32 };
constexpr Rect kLangChoiceRect[2]  = {
  {  20, 60, 130, 140 },
  { 170, 60, 130, 140 },
};

// Page-flip controls at the bottom (visible on all pages).
constexpr Rect kSettingsPrevPageRect = {   8, 216, 56, 22 };
constexpr Rect kSettingsNextPageRect = { 256, 216, 56, 22 };

// Page 2 — network / system. WiFi setup, parents help, parent-server,
// location info, and a destructive "Reset WiFi" row at the bottom.
constexpr Rect kSettingsWifiRect       = { 40,  46, 240, 28 };
constexpr Rect kSettingsParentsRect    = { 40,  78, 240, 28 };
constexpr Rect kSettingsParentSrvRect  = { 40, 110, 240, 28 };
constexpr Rect kSettingsLocationRect   = { 40, 142, 240, 28 };
constexpr Rect kSettingsWifiResetRect  = { 40, 178, 240, 28 };

// Page 3 — companion devices. Currently just the Pip-Modus toggle.
// Title at the top, description block, then a wide on/off button.
constexpr Rect kSettingsPipModeRect    = { 40, 168, 240, 40 };

// Parent-server sub-page rects.
constexpr Rect kParentSrvBackRect      = { 280,   4, 36, 32 };
constexpr Rect kParentSrvToggleRect    = {  40, 174, 240, 40 };

// Location sub-page rects.
constexpr Rect kLocationBackRect       = { 280,   4, 36, 32 };
constexpr Rect kLocationRefreshRect    = {  40, 188, 240, 36 };

// WiFi captive-portal setup screen
constexpr Rect kWifiSetupBackRect    = { 280,   4, 36, 32 };
constexpr Rect kWifiSetupCancelRect  = {  40, 200, 240, 30 };

// Parents-help screen
constexpr Rect kParentsHelpBackRect  = { 280,   4, 36, 32 };

// Reset confirmation modal — drawn over the settings page when the user
// taps Reset. Yes (red, destructive) on the right; Cancel (grey) on the
// left so a hesitant user is biased toward the safe option.
constexpr Rect kSettingsResetConfirmNoRect  = {  30, 178, 130, 40 };
constexpr Rect kSettingsResetConfirmYesRect = { 160, 178, 130, 40 };

// Help screen navigation — small bottom strip so it doesn't overlap body.
constexpr Rect kHelpBackRect = { 280,   4, 36, 32 };
constexpr Rect kHelpPrevRect = {  10, 216, 90, 22 };
constexpr Rect kHelpNextRect = { 220, 216, 90, 22 };

// Animal-select screen
constexpr Rect kAnimalBackRect       = { 280,   4, 36, 32 };
constexpr Rect kAnimalChoiceRect[3] = {
  {  10, 50, 100, 160 },
  { 115, 50, 100, 160 },
  { 220, 50, 100, 160 },
};

// ─── Cleaning shortcut + screen rects ───────────────────────────────────────

// Pet-view button: only drawn when there are piles to clean.
constexpr Rect kCleaningButtonRect = { 280, 136, 36, 36 };
// Back button on the cleaning screen itself.
constexpr Rect kCleaningBackRect   = { 280,   4, 36, 32 };

// ─── Foraging shortcut + screen rects ───────────────────────────────────────

// Pet-view basket button — opens the food-collection mini-screen.
constexpr Rect kForagingButtonRect = { 280,  96, 36, 36 };
constexpr Rect kForagingBackRect   = { 280,   4, 36, 32 };

// ─── Media shortcut + select-screen rects ───────────────────────────────────

// Pet-view TV button — opens the media-select screen, or stops media if
// already running. Lives on the LEFT column under the gear so it doesn't
// crowd the right-hand stack.
constexpr Rect kMediaButtonRect     = {   6,  58, 36, 36 };
// Sport (dumbbell) button — bottom of the left column, below Activity.
// Stack: Gear (y=4) / Media (y=58) / Travel (y=96) / Activity (y=136) /
// Sport (y=176). All four lower slots are always-visible.
constexpr Rect kSportButtonRect     = {   6, 176, 36, 36 };

// Sport-Select screen rects.
constexpr Rect kSportSelectBackRect = { 280,   4, 36, 32 };
constexpr Rect kSportSelectChoice[3] = {
  {  10, 50, 100, 160 },
  { 110, 50, 100, 160 },
  { 210, 50, 100, 160 },
};
// Sport workout / done screens
constexpr Rect kSportWorkoutBackRect = { 280,   4, 36, 32 };
constexpr Rect kSportDoneOkRect      = {  60, 196, 200, 36 };
constexpr Rect kMediaSelectBackRect = { 280,   4, 36, 32 };
// Layout depending on cell count:
//   - 8 cells (cores3/visu): 4×2 grid, 76×80 px each
//   - 6 cells (core2):       6×1 row, 50×160 px each
//   - 5 cells (pip):         5×1 row (this isn't actually compiled,
//                                     pip excludes face.cpp)
// Cell order: Movies, Games, Internet, Social, Friends, Radio,
// Camera, Gallery.
#if TARGET_HAS_WIFI && TARGET_HAS_CAMERA
// 4×2 grid. 4×76 + 3×4 = 316 px → start x=2. Rows y=50..130 and 140..220.
constexpr Rect kMediaChoiceRect[8]  = {
  {   2,  50, 76, 80 },   // Movies
  {  82,  50, 76, 80 },   // Games
  { 162,  50, 76, 80 },   // Internet
  { 242,  50, 76, 80 },   // Social
  {   2, 140, 76, 80 },   // Friends
  {  82, 140, 76, 80 },   // Radio
  { 162, 140, 76, 80 },   // Camera
  { 242, 140, 76, 80 },   // Gallery
};
#else
// Core2 / pip: 4 cells in row 1, 2 cells centered in row 2.
// Same cell size as the cores3 layout (76×80) — but with only 6 instead of 8 cells.
constexpr Rect kMediaChoiceRect[6]  = {
  {   2,  50, 76, 80 },   // Movies      row 1 col 1
  {  82,  50, 76, 80 },   // Games       row 1 col 2
  { 162,  50, 76, 80 },   // Internet    row 1 col 3
  { 242,  50, 76, 80 },   // Social      row 1 col 4
  {  82, 140, 76, 80 },   // Friends     row 2 (centered)
  { 162, 140, 76, 80 },   // Radio       row 2 (centered)
};
#endif

// Friends-mode dedicated screen — modal that owns the canvas while the
// pet broadcasts and listens for peers.
constexpr Rect kFriendsBackRect    = { 280,   4, 36, 32 };
constexpr Rect kFriendsCancelRect  = {  40, 200, 240, 30 };

// Sending-screen 2x2 button grid (Gift / Heart / Food / Game).
constexpr Rect kFriendsBtnGift     = {  20,  46, 130, 76 };
constexpr Rect kFriendsBtnHeart    = { 170,  46, 130, 76 };
constexpr Rect kFriendsBtnFood     = {  20, 130, 130, 76 };
constexpr Rect kFriendsBtnGame     = { 170, 130, 130, 76 };
// Rendezvous phase: one single large button in the center — both pets
// must tap here simultaneously, then we move on to sending gifts.
constexpr Rect kFriendsRendezvousBtnRect = { 40, 100, 240, 80 };

// ─── Activity (mini-game) shortcut + screen rects ──────────────────────────

// Pet-view activity button — left column under the travel button. Icon is
// scene-dependent (rendered by drawActivityButton).
constexpr Rect kActivityButtonRect = {   6, 136, 36, 36 };
// Top-right back X on the activity screen.
constexpr Rect kActivityBackRect   = { 280,   4, 36, 32 };
// Game-over overlay — Retry button (primary, left) + OK button
// (secondary, right) + back X (top-right reuses kActivityBackRect).
// Both buttons share the bottom row in the modal panel.
constexpr Rect kActivityRetryRect  = {  36, 188, 140, 28 };
constexpr Rect kActivityOkRect     = { 188, 188,  88, 28 };

// Returns true when the given scene has a mini-game.
bool sceneHasActivity(Scene s);

// Mushroom minigame slot positions — kept in the header so the spawn /
// hit-test logic in main.cpp and the renderer in face.cpp agree on
// geometry.
constexpr int16_t kMushSlotX[6] = {  60, 110, 160, 210, 260, 300 };
constexpr int16_t kMushSlotY[6] = { 200, 215, 200, 215, 200, 215 };

// Frogger lane Y positions (goal is above row 4).
constexpr int16_t kCrossLaneY[5] = { 200, 172, 144, 116, 88 };
constexpr int16_t kCrossPetX = 160;

// ─── Timer (clock tap → countdown) ──────────────────────────────────────────
//
// The clock at the top-right of the pet view doubles as the timer entry
// affordance. While a timer is running, the same area shows the countdown
// in an accent color instead of the wall clock.

constexpr Rect kClockTouchRect    = { 248,  2, 70, 32 };
constexpr Rect kTimerBackRect     = { 280,  4, 36, 32 };

// Six preset cells (3×2). Last one is "Custom".
constexpr Rect kTimerPresetRect[6] = {
  {  10,  50, 100, 64 },
  { 115,  50, 100, 64 },
  { 220,  50, 100, 64 },
  {  10, 120, 100, 64 },
  { 115, 120, 100, 64 },
  { 220, 120, 100, 64 },
};
constexpr int kTimerPresetMinutes[5] = { 1, 5, 15, 30, 60 };

// "Stoppen" button shown in place of the preset grid while a timer runs.
constexpr Rect kTimerStopRect     = { 50, 138, 220, 56 };

// Custom-edit sub-screen (tap-to-increment).
constexpr Rect kTimerCustomBackRect  = { 280,   4, 36, 32 };
constexpr Rect kTimerCustomMinRect   = {  20,  60, 130, 110 };
constexpr Rect kTimerCustomSecRect   = { 170,  60, 130, 110 };
constexpr Rect kTimerCustomResetRect = {  30, 188, 100, 36 };
constexpr Rect kTimerCustomStartRect = { 180, 188, 110, 36 };

// ─── Travel shortcut + select-screen rects ──────────────────────────────────

// Pet-view travel button — opens the scene-select screen. Lives below the
// media button on the left column.
constexpr Rect kTravelButtonRect     = {   6,  96, 36, 36 };
constexpr Rect kTravelSelectBackRect = { 280,   4, 36, 32 };
// 3×3 grid of scene previews (100×60 cells, 4 px gaps). The 7th cell is
// centered in the bottom row; the two side slots in that row stay empty.
constexpr Rect kTravelChoiceRect[7] = {
  {   6,  46, 100, 60 },
  { 110,  46, 100, 60 },
  { 214,  46, 100, 60 },
  {   6, 110, 100, 60 },
  { 110, 110, 100, 60 },
  { 214, 110, 100, 60 },
  { 110, 174, 100, 60 },
};

// ─── Toy shortcut + select-screen rects ─────────────────────────────────────

// Pet-view toy button — opens the toy-select screen.
constexpr Rect kToyButtonRect      = { 280,  58, 36, 36 };
constexpr Rect kToySelectBackRect  = { 280,   4, 36, 32 };
// Five horizontal cells on the toy-select screen (Ball, Mouse, Rattle,
// Butterfly, Plush).
constexpr Rect kToyChoiceRect[5] = {
  {   4, 50, 60, 160 },
  {  68, 50, 60, 160 },
  { 132, 50, 60, 160 },
  { 196, 50, 60, 160 },
  { 256, 50, 60, 160 },
};

// ─── Time-edit screen rects ─────────────────────────────────────────────────

constexpr Rect kTimeBackRect = { 280,   4, 36, 32 };
constexpr Rect kHourUpRect   = {  60,  50, 80, 38 };
constexpr Rect kMinUpRect    = { 180,  50, 80, 38 };
constexpr Rect kHourDownRect = {  60, 168, 80, 38 };
constexpr Rect kMinDownRect  = { 180, 168, 80, 38 };

// ─── PetView for drawFace ───────────────────────────────────────────────────

struct PetView {
  Face      face;
  uint32_t  now_ms;
  Needs     needs;
  TimePhase phase;

  float     gazeX, gazeY;
  float     tiltX, tiltY;
  float     wanderX, wanderY;     // accumulated drift offset

  MicroAnim micro;
  uint32_t  microStartMs;
  uint32_t  microUntilMs;

  Bubble    bubble;
  uint32_t  bubbleStartMs;
  uint32_t  bubbleUntilMs;

  // Rain-shower phase. 0 = no rain; otherwise the timestamp it started.
  // The renderer derives raining / wet / shake-off from elapsed time.
  uint32_t  rainStartMs;

  // Listen mode: ears pulse, head dances based on mic level.
  bool      listenMode;
  int       micLevel;        // smoothed peak amplitude, 0..32768

  // Hygiene
  const int16_t* pileX;
  const int16_t* pileY;
  uint8_t   pileCount;
  bool      dirty;

  AnimalType animal;

  // Per-need recent change times (for bar pulse)
  uint32_t  happLastChangeMs;
  uint32_t  engLastChangeMs;
  uint32_t  fullLastChangeMs;

  const FloatPool* floats;

  // Toy play animation. toyPlayStartMs == 0 → no animation in flight.
  // toyPlayBored skips the celebratory hop and renders a shorter, listless
  // version of the toy moving across the screen.
  uint32_t  toyPlayStartMs;
  uint32_t  toyPlayDurMs;
  Toy       toyPlayWhich;
  bool      toyPlayBored;
  bool      toyPlayFromLeft;

  // Active media consumption. Media::None when the pet isn't glued to a
  // screen. Otherwise the renderer overrides the eye drawing with square,
  // sunken-eyed "screen-zombie" eyes regardless of v.face.
  Media     mediaActive;

  // Background scene — controls the landscape painted before the pet.
  Scene     scene;

  // Current weather effect drawn after the scene background.
  Weather   weather;

  // Activity-button gating for the current scene (drives dimmed render
  // and on-button cooldown countdown). 0 cooldown + canPlay=true → ready.
  uint32_t  activityCooldownSec;
  bool      activityCanPlay;

  // Travel-button gating — same idea as the activity button. canPlay false
  // when energy is below the travel cost.
  uint32_t  travelCooldownSec;
  bool      travelCanPlay;

  // Brief wobble triggered when the device is set down on its side — head
  // shakes back and forth for a moment as part of the giggle reaction.
  // 0 = no wobble in flight.
  uint32_t  sideShakeStartMs;

  // Moon phase for night-mode bedroom-window rendering. 0..1 along the
  // synodic cycle (0 = new, 0.5 = full). 0xFF = no data — renderer falls
  // back to a generic crescent.
  uint8_t   moonPhase255;

  // Pulsing glow on the basket button — set when a feed attempt failed
  // because the inventory is empty. Active while now_ms < basketHintUntilMs.
  uint32_t  basketHintUntilMs;

  // Somersault — set when the user draws a circle on the screen. Renderer
  // arches the head along a parabolic loop while spin lines fan out around
  // it. 0 = no animation in flight.
  uint32_t  somersaultStartMs;

  // Two-finger spread / snap-back. After release, `snapStartMs` (0 =
  // inactive) drives a damped horizontal wobble.
  bool      spreadActive;
  int16_t   spreadF0X, spreadF0Y;
  int16_t   spreadF1X, spreadF1Y;
  float     spreadAmount;        // 0..1
  uint32_t  snapStartMs;

  // Hand-warming hold. While `holdLocked` and not yet `snuggleActive`,
  // `warmPhase` runs 0..1 over the warm-up window — drives a continuous
  // breath slow-down and a small body lean toward the finger midpoint.
  // `snuggleActive` is true once the threshold is crossed (eyes closed,
  // smile, blush, slow breath); `snuggleSleeping` is set after the deep-
  // sleep phase kicks in.
  bool      warmHoldActive;       // hold locked, warming up
  float     warmPhase;            // 0..1 progress
  int16_t   warmMidX, warmMidY;   // finger midpoint, for body lean
  bool      snuggleActive;
  bool      snuggleSleeping;

  // Hop chain — pet hops `hopChainCount` times in a row after a rapid-tap
  // burst. Each hop is 400 ms. 0 = idle.
  uint8_t   hopChainCount;
  uint32_t  hopChainStartMs;

  // Friends-mode playback overlay — drawn on the pet view after the
  // sending screen closes, showing each received gift/heart/food/game in
  // sequence with the sender's animal next to it. 0 = no overlay.
  uint8_t   friendsPlaybackKind;       // FriendsItemKind value (0/2/3/4/5)
  uint8_t   friendsPlaybackAnimal;     // sender animal (0..2)
  uint32_t  friendsPlaybackItemStartMs; // ms when current item started

  // Gifts bar at the bottom of the screen: shows all received items as a
  // row in front of the pet during the entire playback + celebration
  // window. The currently playing slot is highlighted. 0 = no bar.
  uint8_t   friendsPlaybackTotal;      // 0..5 (kFriendsMaxRxItems)
  uint8_t   friendsPlaybackIdxView;    // 1..total during/after item N (0 = nothing yet)
  uint8_t   friendsPlaybackKinds[5];   // FriendsItemKind per slot

  // Singing — gives the pet a rhythmic side-to-side sway during the
  // singing.wav playback. 0 = not singing.
  uint32_t  singStartMs;

  // Applause-listen — pet poses with closed eyes + bright red cheeks
  // while the mic is sampling for clapping. 0 = not listening.
  uint32_t  applauseListenStartMs;
};

struct SettingsView {
  uint32_t now_ms;
  uint8_t  volume;            // 0..255
  uint8_t  brightnessLevel;   // 0..3
  AnimalType animal;
  uint8_t  page;              // 0 = daily, 1 = lang/help/credits/reset, 2 = wifi, 3 = companions
  uint8_t  language;          // 0 = DE, 1 = EN — shown next to Sprache row
  bool     resetConfirmOpen;  // when true, draw the pet-reset confirmation modal
  bool     wifiResetConfirmOpen;  // when true, draw the WiFi-reset confirmation modal
  uint8_t  parentSrvState;    // mirrors ParentServerState (0=off, 1=conn, 2=run, 3=fail)
  const char* parentSrvIp;    // valid string when state==Running
  bool     pipMode;           // page 3 — companion-listener toggle
};

struct WifiSetupView {
  uint32_t    now_ms;
  uint8_t     state;          // matches WifiSetupState enum order
  const char* apName;         // "goo-goo-setup"
  const char* apIp;           // "192.168.4.1"
  const char* pendingSsid;    // ssid being tested in Connecting/Success/Failed
};

struct ParentSrvView {
  uint32_t    now_ms;
  uint8_t     state;        // 0=Off, 1=Connecting, 2=Running, 3=Failed
  const char* ip;           // valid when Running
};

// Sport-mode views.
struct SportSelectView {
  uint32_t now_ms;
};

struct SportWorkoutView {
  uint32_t now_ms;
  uint8_t  exercise;        // 0=Squat, 1=Jump, 2=Yoga
  uint8_t  phase;           // 0=Intro/safety, 1=Active
  uint32_t phaseStartMs;
  uint8_t  repCount;        // squats/jumps reps OR yoga seconds elapsed
  uint8_t  targetReps;      // target reps OR yoga target seconds
  uint8_t  motionPhase;     // exercise-specific (squat: 0=up,1=down)
  uint8_t  myAnimal;
};

struct SportDoneView {
  uint32_t now_ms;
  uint8_t  exercise;
  uint8_t  reps;            // completed reps
  uint16_t streak;          // current streak in days
  uint8_t  myAnimal;
};

struct LocationView {
  uint32_t    now_ms;
  bool        valid;
  const char* city;
  const char* countryCode;
  float       lat;
  float       lon;
  uint32_t    lastUpdatedAgoSec;   // 0 if never; otherwise seconds-ago
  bool        refreshing;          // true while a fresh ip-api lookup is in flight
};

struct FriendsView {
  uint32_t    now_ms;
  uint32_t    stateStartedAt;  // ms when current state began
  uint8_t     state;           // mirrors FriendsState enum (Sending/NoFriend used)
  uint8_t     myAnimal;        // own animal type
  uint8_t     peerAnimal;      // legacy (Matched state)
  uint32_t    secondsLeft;     // session countdown (60s)
  uint8_t     sentCount;       // 0..5 — how many items the user has sent
  uint8_t     maxSends;        // = kFriendsMaxSends (5)
  uint32_t    lastSentAtMs;    // for the brief "Sent!" highlight
  uint8_t     lastSentKind;    // last kind tapped (for the highlight)
  bool        localReady;      // Rendezvous: local "Verabreden" tap active
  bool        remoteReady;     // Rendezvous: remote ready ack just seen
  bool        localDone;       // Sending: 5/5 locally, own done-burst sent
  bool        remoteDone;      // Sending: done ack received from partner
  uint8_t     rxItemCount;     // Sending: items received so far for the wait counter
};

struct AnimalSelectView {
  uint32_t now_ms;
  AnimalType current;
  bool       firstRun;       // hide back X + show welcome banner
};

struct LangSelectView {
  uint32_t now_ms;
  uint8_t  current;          // 0 = DE, 1 = EN, 0xFF = no current selection
  bool     firstRun;         // hide back X
};

struct ToySelectView {
  uint32_t now_ms;
  Toy      current;
  uint8_t  repeatCount;     // shows the boredom indicator on the current toy
};

struct MediaSelectView {
  uint32_t now_ms;
  Media    current;         // None = no active consumption (just opening the menu)
};

// Multi-page help / instructions screen accessed via Settings → Anleitung.
struct HelpView {
  uint32_t now_ms;
  uint8_t  page;            // 0..kHelpPageCount-1
};
// Help page count: 17 universal pages + 1 Voice (HAS_LLM) + 1 Camera
// (HAS_CAMERA) + 1 Web radio (HAS_WIFI) + 1 Photo+Gallery (HAS_CAMERA).
// On Core2/Pip the non-applicable pages are dropped — their i18n
// strings stay compiled but aren't rendered.
constexpr uint8_t kHelpPageCount =
    17
#if TARGET_HAS_LLM
    + 1
#endif
#if TARGET_HAS_CAMERA
    + 1
#endif
#if TARGET_HAS_WIFI
    + 1
#endif
#if TARGET_HAS_CAMERA
    + 1   // Photo + Gallery
#endif
    ;

struct TravelSelectView {
  uint32_t  now_ms;
  Scene     current;
  TimePhase phase;          // for previews that respect time-of-day
};

// Timer screens (preset picker / running screen / custom-edit sub-screen).
struct TimerView {
  uint32_t now_ms;
  bool     active;          // true → show "Stoppen" + countdown instead of presets
  uint32_t remainingMs;     // for live display while running
  uint32_t durationMs;      // initial duration (for the running screen header)
};

struct TimerCustomView {
  uint32_t now_ms;
  uint8_t  minutes;
  uint8_t  seconds;
};

// One view struct serves all mini-games — fields not used by the current
// game stay zero. The renderer dispatches on `scene` to pick the layout.
struct ActivityView {
  uint32_t   now_ms;
  Scene      scene;
  TimePhase  phase;
  AnimalType animal;
  uint16_t   score;
  uint16_t   best;
  uint32_t   timeLeftMs;     // 0 when not a timed game
  bool       gameOver;
  uint32_t   gameOverAtMs;

  // Reward summary shown on the game-over panel.
  uint8_t    stars;          // 0..3 — how many stars earned
  uint8_t    bonusHap;       // happiness reward applied
  uint8_t    bonusEng;       // energy reward applied
  bool       newBest;        // whether this run beat the persisted best

  // Replay limit — drives the look of the "Nochmal" button on the
  // game-over overlay. plays = current run number (1..max), max = total
  // allowed. When plays == max the button is rendered dimmed.
  uint8_t    playsInSession;
  uint8_t    maxPlaysPerSession;

  // ── Butterfly minigame (Meadow) ──────────────────────────────────────────
  static constexpr uint8_t kBfCount = 6;
  int16_t  bfX [kBfCount];
  int16_t  bfY [kBfCount];
  uint8_t  bfState[kBfCount];     // 0 alive  1 caught & flying up  2 gone
  uint32_t bfCaughtMs[kBfCount];
  uint8_t  bfColor[kBfCount];     // palette index

  // ── Stack minigame (Bedroom) ────────────────────────────────────────────
  static constexpr uint8_t kStackMax = 24;
  uint8_t stackCount;
  int16_t stackX[kStackMax];
  int16_t stackY[kStackMax];
  int16_t stackW[kStackMax];
  int16_t movingX, movingY;
  int16_t movingW;

  // ── Mushroom minigame (Forest) ──────────────────────────────────────────
  static constexpr uint8_t kMushSlots = 6;
  uint8_t  mushKind  [kMushSlots];     // 0 empty, 1 normal, 2 gold
  uint32_t mushSpawnedMs[kMushSlots];

  // ── Common: pet position + lives + obstacles (Surf / Scorpion /
  //    Asteroids / Cross). Each game uses the subset it needs. ────────────
  int16_t  petPosX;
  int16_t  petPosY;
  uint8_t  lives;
  static constexpr uint8_t kObsCount = 10;
  int16_t  obsX[kObsCount];
  int16_t  obsY[kObsCount];
  uint8_t  obsType[kObsCount];     // 0 empty, otherwise game-specific kind
  // Cross (Frogger): pet-row index (0 = bottom safe, top = goal).
  int8_t   petRow;
};

// ─── Renderers ──────────────────────────────────────────────────────────────

struct CleaningView {
  uint32_t       now_ms;
  TimePhase      phase;
  Scene          scene;
  const int16_t* pileX;
  const int16_t* pileY;
  uint8_t        pileCount;
  uint32_t       cleanedAtMs;        // 0 unless count just hit 0 (for celebration)
};

// ─── Foraging screen ────────────────────────────────────────────────────────
//
// One scene contains a tree (apples), a bush (berries) and water (fish). The
// renderer doesn't know how items move — the controller fills in `x`,`y`
// each frame. `flyStartMs != 0` means the item was just collected and is
// animating toward its inventory slot in the top bar.

constexpr uint8_t kForageMaxItems = 8;

enum class ForageItem : uint8_t { None = 0, Apple = 1, Berry = 2, Fish = 3 };

struct ForageItemView {
  ForageItem type;
  int16_t    x, y;
  uint32_t   flyStartMs;       // 0 = idle in scene; else collected, flying away
  int16_t    flyToX, flyToY;   // target = inventory slot center
  bool       facingLeft;       // for fish — flips the tail
};

struct ForagingView {
  uint32_t  now_ms;
  TimePhase phase;
  Scene     scene;
  uint16_t  apples;
  uint16_t  berries;
  uint16_t  fish;
  ForageItemView items[kForageMaxItems];
};

// Apple inventory icon center on the foraging screen — the controller uses
// these so collected items fly to the right slot.
constexpr int kForageInvApplePosX = 28;
constexpr int kForageInvBerryPosX = 110;
constexpr int kForageInvFishPosX  = 192;
constexpr int kForageInvY         = 18;

void drawFace          (M5Canvas& canvas, const PetView& view);
void drawSettingsScreen(M5Canvas& canvas, const SettingsView& s);
void drawWifiSetupScreen(M5Canvas& canvas, const WifiSetupView& v);
void drawParentsHelpScreen(M5Canvas& canvas, uint32_t now_ms);
void drawFriendsScreen(M5Canvas& canvas, const FriendsView& v);
void drawParentSrvScreen(M5Canvas& canvas, const ParentSrvView& v);
void drawLocationScreen(M5Canvas& canvas, const LocationView& v);
void drawSportSelectScreen(M5Canvas& canvas, const SportSelectView& v);
void drawSportWorkoutScreen(M5Canvas& canvas, const SportWorkoutView& v);
void drawSportDoneScreen(M5Canvas& canvas, const SportDoneView& v);
void drawTimeEditScreen(M5Canvas& canvas, uint32_t now_ms);
void drawCleaningScreen(M5Canvas& canvas, const CleaningView& v);
void drawAnimalSelectScreen(M5Canvas& canvas, const AnimalSelectView& v);
void drawForagingScreen(M5Canvas& canvas, const ForagingView& v);
void drawToySelectScreen(M5Canvas& canvas, const ToySelectView& v);
void drawMediaSelectScreen(M5Canvas& canvas, const MediaSelectView& v);
// "Connecting..." modal overlay for the radio start. Drawn synchronously
// before the (potentially blocking) WiFi reconnect so the user gets
// immediate feedback.
void drawRadioConnectingOverlay(M5Canvas& canvas);

// Boot sync progress: full-screen status while WiFi/NTP/World run in the
// background. stage matches the SyncStage enum in main.cpp:
//   0 Idle   → empty frame (should practically never be called)
//   1 Wifi   → "Checking WiFi"
//   2 Time   → "Fetching time"
//   3 World  → "Fetching location"
//   4 Done   → empty frame (caller exits)
void drawSyncProgressOverlay(M5Canvas& canvas, uint8_t stage);
void drawTravelSelectScreen(M5Canvas& canvas, const TravelSelectView& v);
void drawHelpScreen(M5Canvas& canvas, const HelpView& v);
void drawCreditsScreen(M5Canvas& canvas, uint32_t now_ms);
void drawLangSelectScreen(M5Canvas& canvas, const LangSelectView& v);
void drawActivityScreen(M5Canvas& canvas, const ActivityView& v);
void drawTimerScreen      (M5Canvas& canvas, const TimerView& v);
void drawTimerCustomScreen(M5Canvas& canvas, const TimerCustomView& v);

// 8-second cinematic between scenes — pet rides a scene-appropriate vehicle
// across a parallax-scrolling backdrop. The current time-of-day tints sky,
// ground and lighting (Space ignores phase since it's already night).
void drawTravelTransition(M5Canvas& canvas, Scene target,
                          AnimalType animal, uint32_t elapsedMs,
                          TimePhase phase = TimePhase::Day);

// Boot-time "still in your 30-min break" screen. Shown for 5 s, then the
// device powers off again. The little sleeping pet shown reflects the
// user's chosen animal type.
void drawLockoutScreen(M5Canvas& canvas, uint32_t remainingSec,
                       uint32_t now_ms, AnimalType animal);

// Bedtime announcement (~3 s) drawn over the live pet view as soon as the
// 30-minute play session ends.
void drawBedtimeAnnouncement(M5Canvas& canvas, uint32_t elapsedMs);

// Full-screen 6 s sleep animation that follows the announcement before the
// device powers off.
void drawBedtimeSleepAnim(M5Canvas& canvas, uint32_t elapsedMs,
                          AnimalType animal);

// Always-visible badge during minigames + modal screens. Centered at the
// top of the canvas so it doesn't conflict with per-screen HUDs that own
// the top-left and top-right corners.
void drawTimerBadge       (M5Canvas& canvas, uint32_t remainingMs,
                            uint32_t now_ms, bool topRight);

// Full-screen flash overlay drawn while the expiry animation runs.
void drawTimerExpiryOverlay(M5Canvas& canvas, uint32_t now_ms,
                             uint32_t expireStartMs);

// Draws "HH:MM:SS" at the bottom of the screen. `phase` only influences the
// text color (light text on dark phases / overlays). No-op when the RTC is
// not yet set.
void drawClock         (M5Canvas& canvas, TimePhase phase);

// Renders the bottom strip with three labels indicating what the physical
// chin buttons (A / B / C) currently do. Drawn only on the pet view.
void drawButtonHints   (M5Canvas& canvas, TimePhase phase);

// Small battery indicator. `level` 0..100, `charging` adds a bolt overlay.
// `dark` flips the outline colour for visibility against dark backgrounds.
void drawBatteryIcon   (M5Canvas& canvas, int x, int y,
                        int level, bool charging, bool dark);
