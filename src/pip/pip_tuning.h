#pragma once

// Pip-specific IMU and timing thresholds. Pip is now an accessory, not a
// pet — only the shake gesture (treat throw) and the mobile power
// strategy are relevant. Stroke / stand / happiness-decay constants from
// the previous bear UI are gone.

namespace pip {

// Shake detection (= throw a treat). Uses |a| - 1g as the shake metric,
// not "linear accel via gravity-EMA subtraction" — because the EMA tends
// to absorb the shake itself within a couple of samples and the linear
// accel falls below the threshold even for vigorous flicks. With raw
// magnitude deviation, gravity always stays at 1 g and any translational
// motion deviates the total acceleration from 1 g cleanly.
//
// 0.4 g threshold + single-peak trigger feels right for a casual wrist
// flick. The 1.5 s cooldown still prevents accidental double-throws if
// the motion has multiple peaks.
constexpr float    SHAKE_PEAK_G          = 0.4f;
constexpr float    SHAKE_LOW_HYS         = 0.15f;
constexpr uint32_t SHAKE_WINDOW_MS       = 700;
constexpr uint8_t  SHAKE_REQUIRED_PEAKS  = 1;
constexpr uint32_t SHAKE_COOLDOWN_MS     = 1500;

// Throw animation length (must match face_pip.cpp's drawThrowing kThrowMs).
constexpr uint32_t THROW_ANIM_MS         = 800;

// Idle → Sleeping: Pip dims to a Zzz screen after this many ms without input.
// 60 s is a reasonable balance between "always responsive" and "saves battery
// when forgotten in a pocket / on the desk".
constexpr uint32_t IDLE_TO_SLEEP_MS      = 60000;

// BtnA hold duration → forceSleep. Short press still cycles the treat;
// holding it down past this threshold puts Pip into the Zzz screen.
constexpr uint32_t BTN_HOLD_SLEEP_MS     = 600;

// Mobile power strategy.
constexpr uint32_t DIM_AFTER_MS          = 30000;   // 30 s → half brightness
constexpr uint32_t DARK_AFTER_MS         = 60000;   // 60 s → 5 % brightness
constexpr uint32_t DISPLAY_OFF_AFTER_MS  = 300000;  // 5 min → display off
constexpr uint8_t  BRIGHT_NORMAL         = 150;
constexpr uint8_t  BRIGHT_DIM            =  60;
constexpr uint8_t  BRIGHT_DARK           =  12;

// Light-sleep period while the display is off. Wake every X ms for an
// IMU check ("pet taken out of pocket" → display back on).
constexpr uint32_t LIGHT_SLEEP_MS        = 1500;

// Battery threshold at boot. Below this percent we don't go into full
// operation — show "Low Battery" briefly and deep-sleep, otherwise the
// brownout protection sends the device into a reboot loop.
constexpr int      LOW_BATTERY_PCT       = 5;

}  // namespace pip
