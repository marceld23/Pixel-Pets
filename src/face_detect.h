#pragma once
#include <Arduino.h>

// Simple face detection on the CoreS3 camera (GC0308) via skin-color
// heuristic (HSV/RGB threshold). No ML, ~5 ms per frame.
//
// Lifecycle:
//   begin()          — initialize camera (RGB565, QVGA)
//   setEnabled(bool) — capture on/off (e.g. when display sleeps)
//   tick(now)        — call every few hundred ms; returns true when
//                       a face detection has just triggered (earliest
//                       after cooldown). Otherwise false.

namespace face_detect {

bool begin();
bool ready();

void setEnabled(bool en);
bool isEnabled();

bool tick(uint32_t now_ms);

// ─── Live preview + JPEG capture for the photo feature ──────────────────
//
// The sensor normally runs in RGB565 mode for skin-detect. For the selfie
// capture we briefly switch to JPEG mode, pull a frame, and switch back.
// During Camera / Gallery mode the detect tick is paused (via
// setEnabled(false) by the caller).

// Get an RGB565 frame (for live preview in Camera mode). Returns nullptr
// if the camera isn't initialized or no frame is available.
// Caller MUST call releaseFrame() when done — otherwise the frame buffer
// is blocked on the next fb_get.
struct FrameView {
    const uint8_t* data;     // RGB565 pixels, host endianness
    int            width;
    int            height;
    size_t         len;
    void*          handle;   // internal (camera_fb_t*)
};
bool acquireFrame(FrameView* out);
void releaseFrame(const FrameView& fv);

// One-shot JPEG capture. Temporarily switches the sensor to JPEG mode,
// grabs a frame, switches back to RGB565. out_data points to a heap-
// allocated buffer that the caller must free() when done.
// Returns true on success.
bool captureJpeg(uint8_t** out_data, size_t* out_len);

}   // namespace face_detect
