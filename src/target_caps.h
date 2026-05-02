#pragma once

// Target capabilities — derives from the TARGET_* build flags (set in
// platformio.ini) which hardware features are compiled and which
// branding strings are used.
//
// Usage:
//   #include "target_caps.h"
//
//   #if TARGET_HAS_LLM
//     ... voice-pipeline-specific code ...
//   #endif
//
//   /* ... */ TARGET_NAME /* ... */                   ← string literal
//   /* ... */ "Mit " TARGET_AP_NAME /* ... */         ← concatenation
//
// Available macros per target:
//   TARGET_HAS_LLM           1 = voice pipeline (KWS/VAD/Whisper/Qwen3)
//   TARGET_HAS_CAMERA        1 = front camera + face detection
//   TARGET_HAS_HARD_BUTTONS  1 = physical BtnA/B(/C)
//   TARGET_HAS_TOUCH         1 = touchscreen
//   TARGET_HAS_WAV_AUDIO     1 = WAV speaker (otherwise PWM buzzer / tone only)
//   TARGET_HAS_WIFI          1 = active WiFi use (NTP, ip-api, web radio,
//                                   ESP-NOW friends, captive portal, parent UI).
//                                   At 0 (Pip) all WiFi features are mute.
//   TARGET_DISPLAY_W         display width in pixels
//   TARGET_DISPLAY_H         display height in pixels
//   TARGET_NAME              "Muffin" / "Goo-Goo" / "Visu" / "Pip"
//   TARGET_AP_NAME           captive portal WiFi name
//   TARGET_MDNS_NAME         mDNS hostname

#if defined(TARGET_CORES3)
#  define TARGET_HAS_LLM           1
#  define TARGET_HAS_CAMERA        1
#  define TARGET_HAS_HARD_BUTTONS  0
#  define TARGET_HAS_TOUCH         1
#  define TARGET_HAS_WAV_AUDIO     1
#  define TARGET_HAS_WIFI          1
#  define TARGET_DISPLAY_W         320
#  define TARGET_DISPLAY_H         240
#  define TARGET_NAME              "Muffin"
#  define TARGET_AP_NAME           "muffin-setup"
#  define TARGET_MDNS_NAME         "muffin"

#elif defined(TARGET_CORE2)
#  define TARGET_HAS_LLM           0
#  define TARGET_HAS_CAMERA        0
#  define TARGET_HAS_HARD_BUTTONS  1
#  define TARGET_HAS_TOUCH         1
#  define TARGET_HAS_WAV_AUDIO     1
#  define TARGET_HAS_WIFI          1
#  define TARGET_DISPLAY_W         320
#  define TARGET_DISPLAY_H         240
#  define TARGET_NAME              "Goo-Goo"
#  define TARGET_AP_NAME           "goo-goo-setup"
#  define TARGET_MDNS_NAME         "goo-goo"

#elif defined(TARGET_VISU)
// CoreS3 ohne angeschlossenes Module-LLM. Identisch zu Muffin, nur ohne
// Voice-Pipeline. Frontkamera + Touch-UI bleiben erhalten.
#  define TARGET_HAS_LLM           0
#  define TARGET_HAS_CAMERA        1
#  define TARGET_HAS_HARD_BUTTONS  0
#  define TARGET_HAS_TOUCH         1
#  define TARGET_HAS_WAV_AUDIO     1
#  define TARGET_HAS_WIFI          1
#  define TARGET_DISPLAY_W         320
#  define TARGET_DISPLAY_H         240
#  define TARGET_NAME              "Visu"
#  define TARGET_AP_NAME           "visu-setup"
#  define TARGET_MDNS_NAME         "visu"

#elif defined(TARGET_PIP)
// M5StickC Plus 2 — kleines mobiles Pet. 135×240-Display, MPU6886-IMU,
// PWM-Buzzer (kein WAV), BtnA/BtnB. Kein LLM, keine Kamera, kein Touch.
// Kein WiFi (Akku-Optimierung).
#  define TARGET_HAS_LLM           0
#  define TARGET_HAS_CAMERA        0
#  define TARGET_HAS_HARD_BUTTONS  1
#  define TARGET_HAS_TOUCH         0
#  define TARGET_HAS_WAV_AUDIO     0
#  define TARGET_HAS_WIFI          0
#  define TARGET_DISPLAY_W         135
#  define TARGET_DISPLAY_H         240
#  define TARGET_NAME              "Pip"
#  define TARGET_AP_NAME           "pip-setup"
#  define TARGET_MDNS_NAME         "pip"

#else
#  error "Unknown target — set TARGET_CORES3 / TARGET_CORE2 / TARGET_VISU / TARGET_PIP in platformio.ini build_flags"
#endif
