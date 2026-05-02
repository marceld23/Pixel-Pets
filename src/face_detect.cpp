#include "face_detect.h"
#include "esp_camera.h"
#include "img_converters.h"   // frame2jpg() — Software-JPEG-Encoder, da
                              // der GC0308 PIXFORMAT_JPEG nicht beherrscht.

namespace face_detect {

namespace {

// CoreS3 GC0308-Pin-Mapping
constexpr int PIN_D0 = 39, PIN_D1 = 40, PIN_D2 = 41, PIN_D3 = 42;
constexpr int PIN_D4 = 15, PIN_D5 = 16, PIN_D6 = 48, PIN_D7 = 47;
constexpr int PIN_XCLK  = 2;
constexpr int PIN_PCLK  = 45;
constexpr int PIN_VSYNC = 46;
constexpr int PIN_HREF  = 38;
constexpr int PIN_SDA   = 12;
constexpr int PIN_SCL   = 11;

constexpr uint32_t SAMPLE_INTERVAL_MS = 400;     // ~2,5 FPS
constexpr uint32_t COOLDOWN_MS        = 30000;   // nach Detect 30 s stumm
constexpr uint32_t SKIN_PIXEL_THRESH  = 600;     // gelockert, war 1500 — zu strikt
constexpr uint32_t SKIN_DEBUG_LOG_MIN = 150;     // ab diesem Skin-Count loggen

bool     g_inited        = false;
bool     g_enabled       = true;
uint32_t g_lastSampleMs  = 0;
uint32_t g_lastDetectMs  = 0;

uint32_t countSkinPixels(camera_fb_t* fb) {
    if (!fb || fb->format != PIXFORMAT_RGB565) return 0;
    const uint16_t* px = (const uint16_t*)fb->buf;
    int w = fb->width, h = fb->height;
    // Zentraler Bereich: mittlere 60 % horizontal, 50 % vertikal
    int x0 = w * 20 / 100, x1 = w * 80 / 100;
    int y0 = h * 25 / 100, y1 = h * 75 / 100;
    uint32_t count = 0;
    for (int y = y0; y < y1; y += 2) {                 // Sub-Sampling 2× zur Beschleunigung
        for (int x = x0; x < x1; x += 2) {
            uint16_t p = px[y * w + x];
            // Camera endianness: usually host order (little-endian RGB565)
            // bytes: high=rrrrrggg, low=gggbbbbb
            // We read as uint16, the compiler sees the right order
            uint8_t r = ((p >> 11) & 0x1F) << 3;
            uint8_t g = ((p >>  5) & 0x3F) << 2;
            uint8_t b = ( p        & 0x1F) << 3;
            // Skin heuristic: broadened for more skin tones + lighting.
            // Criteria: R > G > B (warm tone), R not too dark, clear
            // red-blue color difference, but tolerance for very bright tones.
            bool warm  = (r > g && g >= b);
            bool bright = (r > 60);
            bool diff  = (r - b) > 15 && (r - g) >= 10;
            bool notRed = (r < 245 || g > 30 || b > 30);   // excludes pure red
            if (warm && bright && diff && notRed) count++;
        }
    }
    return count;
}

camera_config_t makeConfig(pixformat_t fmt = PIXFORMAT_RGB565) {
    camera_config_t c{};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0 = PIN_D0; c.pin_d1 = PIN_D1; c.pin_d2 = PIN_D2; c.pin_d3 = PIN_D3;
    c.pin_d4 = PIN_D4; c.pin_d5 = PIN_D5; c.pin_d6 = PIN_D6; c.pin_d7 = PIN_D7;
    c.pin_xclk     = PIN_XCLK;
    c.pin_pclk     = PIN_PCLK;
    c.pin_vsync    = PIN_VSYNC;
    c.pin_href     = PIN_HREF;
    // SCCB shares the I2C bus with M5Unified (PMIC/IMU/Touch on
    // GPIO 11/12). pin_sccb_sda = -1 signals "use existing I2C
    // port"; sccb_i2c_port=1 is the internal I2C of CoreS3.
    c.pin_sccb_sda = -1;
    c.pin_sccb_scl = -1;
    c.sccb_i2c_port = 1;
    c.pin_pwdn     = -1;
    c.pin_reset    = -1;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = fmt;
    c.frame_size   = FRAMESIZE_QVGA;        // 320×240
    c.jpeg_quality = 10;                    // only relevant for PIXFORMAT_JPEG
    c.fb_count     = 1;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    // WHEN_EMPTY: DMA pauses after one frame until we pick it up via
    // fb_get. With a 400 ms skin-detect tick that saves ~3.5 MB/s of
    // PSRAM bandwidth (otherwise continuous refill at ~25 fps) which
    // the audio-lib stream buffer would have to share with the camera
    // → eliminates stutter on web radio during live pet display.
    c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    return c;
}

}   // anon namespace

bool begin() {
    if (g_inited) return true;
    camera_config_t c = makeConfig();
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        Serial.printf("[face_detect] esp_camera_init FAILED: 0x%x\n", err);
        return false;
    }
    Serial.println("[face_detect] camera ready");
    g_inited = true;
    return true;
}

bool ready() { return g_inited; }

void setEnabled(bool en) {
    if (g_enabled == en) return;
    g_enabled = en;
    Serial.printf("[face_detect] %s\n", en ? "ENABLED" : "DISABLED");
}

bool isEnabled() { return g_enabled; }

bool tick(uint32_t now) {
    if (!g_inited || !g_enabled) return false;
    if (now - g_lastSampleMs < SAMPLE_INTERVAL_MS) return false;
    g_lastSampleMs = now;
    if (now - g_lastDetectMs < COOLDOWN_MS) return false;

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return false;
    uint32_t skin = countSkinPixels(fb);
    esp_camera_fb_return(fb);

    if (skin >= SKIN_PIXEL_THRESH) {
        Serial.printf("[face_detect] DETECT skin=%lu (thresh=%lu)\n",
                      (unsigned long)skin, (unsigned long)SKIN_PIXEL_THRESH);
        g_lastDetectMs = now;
        return true;
    }
    if (skin >= SKIN_DEBUG_LOG_MIN) {
        Serial.printf("[face_detect] near-miss skin=%lu (thresh=%lu)\n",
                      (unsigned long)skin, (unsigned long)SKIN_PIXEL_THRESH);
    }
    return false;
}

// ─── Live preview + JPEG capture ──────────────────────────────────────────

bool acquireFrame(FrameView* out) {
    if (!g_inited || !out) return false;
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return false;
    out->data   = fb->buf;
    out->width  = fb->width;
    out->height = fb->height;
    out->len    = fb->len;
    out->handle = fb;
    return true;
}

void releaseFrame(const FrameView& fv) {
    if (fv.handle) esp_camera_fb_return((camera_fb_t*)fv.handle);
}

bool captureJpeg(uint8_t** out_data, size_t* out_len) {
    if (!g_inited || !out_data || !out_len) return false;
    *out_data = nullptr;
    *out_len  = 0;

    // The GC0308 does not support PIXFORMAT_JPEG (sensor only delivers
    // YUV/RGB streams), so a driver reinit to JPEG fails with
    // ESP_ERR_NOT_SUPPORTED. Instead we grab the RGB565 frame that
    // already runs for live preview and encode it in software via
    // frame2jpg() — fast enough on the ESP32-S3 (~150 ms for QVGA).
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println(F("[face_detect] capture: fb_get returned null"));
        return false;
    }

    bool ok = frame2jpg(fb, 12, out_data, out_len);
    if (!ok) {
        Serial.printf("[face_detect] frame2jpg failed (fmt=%d w=%u h=%u len=%u)\n",
                      (int)fb->format, fb->width, fb->height, (unsigned)fb->len);
    } else {
        Serial.printf("[face_detect] captured %u bytes JPEG\n",
                      (unsigned)*out_len);
    }
    esp_camera_fb_return(fb);
    return ok;
}

}   // namespace face_detect
