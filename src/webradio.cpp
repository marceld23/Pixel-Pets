#include "webradio.h"

#include "target_caps.h"

#if TARGET_HAS_WIFI

#include <M5Unified.h>
#include <WiFi.h>
#include <Audio.h>

#include "net.h"
#include "pip_link.h"
#include "sounds/sounds.h"

#if TARGET_HAS_LLM
#include "voice_pipeline.h"
#endif

namespace webradio {

namespace {

// ── Stream URLs (researched) ──────────────────────────────────────────────
// Picked depending on g_lang.
// WDR Maus: HTTP variant as primary — the HTTPS variant via icecastssl
// has reproducible TLS handshake problems in the ESP32-audioI2S library
// (socket closes before setSocketOption → errno 9 EBADF). HTTP works.
// 56 kbps variant: the URL the server *currently* redirects to a
// 128 kbps stream on rndfnk.com, but we keep the canonical Maus URL —
// the 302 hop is acceptable, the stutter we previously chased was
// caused by main-loop blocking, not bandwidth.
constexpr const char* kUrlDe =
    "http://wdr-diemaus-live.icecast.wdr.de/wdr/diemaus/live/mp3/56/stream.mp3";
constexpr const char* kUrlEn =
    "http://listen-funkids.sharp-stream.com/funkids.mp3";

constexpr const char* kKeepAliveReason = "webradio";

// Audio decoder from the schreibfaul1 package. Allocated on the first start().
Audio*    g_audio    = nullptr;
State     g_state    = State::Off;
uint8_t   g_volume0_255 = 200;
uint8_t   g_lang     = 0;

// Dedicated audio-decode task pinned to core 1. The schreibfaul1
// maintainer recommends this pattern (examples/separate_audiotask) for
// stutter-free streaming on ESP32: the main loop is full of TFT
// redraws, touch sampling, pet/friends ticks etc. — every long iteration
// starves audio.loop() and produces audible dropouts. A pinned task
// with vTaskDelay(1) yields cleanly to other tasks while keeping the
// MP3 decoder fed at near-100% duty cycle.
TaskHandle_t      g_audioTask        = nullptr;
volatile bool     g_audioTaskRunning = false;

// Reconnect logic: on stream loss we retry ≤3 times with a pause
// between attempts. Then Error state and the renderer can show it.
constexpr uint8_t  kMaxReconnects = 3;
constexpr uint32_t kReconnectGapMs = 5000;
uint8_t   g_reconnectsTried = 0;
uint32_t  g_lastReconnectMs = 0;

// WiFi connect timeout before stream open. After expiry → Error.
constexpr uint32_t kWifiConnectTimeoutMs = 10000;
uint32_t  g_wifiWaitStartMs = 0;

const char* selectUrl(uint8_t lang) {
    return (lang == 1) ? kUrlEn : kUrlDe;   // Default DE
}

// Volume 0..255 (settings) → 0..21 (audio lib). Audio lib clips internally,
// but we scale cleanly so the settings slider has a sensible effect.
uint8_t mapVolume(uint8_t v0_255) {
    return (uint8_t)((uint32_t)v0_255 * 21 / 255);
}

// Toggle speaker amp power explicitly. M5.Speaker.begin()/.end()
// internally triggers a callback that switches the amplifier power
// rail on/off (Core2: AXP192-GPIO2 to NS4168, Core2 v1.1: AXP2101-
// ALDO3, CoreS3: AW9523 bit to AW88298). After M5.Speaker.end() the
// amp is unpowered — then the audio lib busily decodes but the
// speaker stays silent. In claimI2sForAudio we have to bring the
// rail back up manually.
#if defined(TARGET_CORES3) || defined(TARGET_VISU)
// AW88298 register writer (big-endian value over I2C). Values taken
// from M5Unified::_speaker_enabled_cb_cores3 for 44.1 kHz — the
// AW88298 then tolerates sample-rate changes in the stream
// (44.1 kHz pet sounds, 48 kHz web-radio MP3).
constexpr uint8_t kAw88298Addr = 0x36;
constexpr uint8_t kAw9523Addr  = 0x58;
static void aw88298WriteReg(uint8_t reg, uint16_t value) {
    value = __builtin_bswap16(value);
    M5.In_I2C.writeRegister(kAw88298Addr, reg, (const uint8_t*)&value, 2, 400000);
}
#endif

void setSpeakerAmpPower(bool on) {
#if defined(TARGET_CORE2)
    // Core2 / Core2 v1.1: Amp power via AXP192-GPIO2 or AXP2101-ALDO3.
    switch (M5.Power.getType()) {
        case m5::Power_Class::pmic_axp192:
            M5.Power.Axp192.setGPIO2(on);
            break;
        case m5::Power_Class::pmic_axp2101:
            M5.Power.Axp2101.setALDO3(on ? 3300 : 0);
            break;
        default:
            break;
    }
#elif defined(TARGET_CORES3) || defined(TARGET_VISU)
    // CoreS3 / Visu: AW9523 bit 2 in reg 0x02 switches 5 V onto the AW88298.
    // The AW88298 also has to be re-initialized over I2C itself,
    // otherwise it stays in PWDN after M5.Speaker.end(). Values taken
    // 1:1 from M5Unified::_speaker_enabled_cb_cores3 (44.1 kHz default,
    // I2S slave mode, max volume).
    if (on) {
        M5.In_I2C.bitOn (kAw9523Addr, 0x02, 0b00000100, 400000);
        aw88298WriteReg(0x61, 0x0673);   // boost mode disabled
        aw88298WriteReg(0x04, 0x4040);   // I2SEN=1, AMPPD=0, PWDN=0
        aw88298WriteReg(0x05, 0x0008);   // RMSE/HAGCE/HDCCE/HMUTE = 0
        aw88298WriteReg(0x06, 0x14C7);   // I2SBCK=0 + rate idx for 44.1 kHz
        aw88298WriteReg(0x0C, 0x0064);   // Volume = full
    } else {
        aw88298WriteReg(0x04, 0x4000);   // I2SEN=0
        M5.In_I2C.bitOff(kAw9523Addr, 0x02, 0b00000100, 400000);
    }
#else
    (void)on;
#endif
}

// Audio task body — runs forever (until g_audioTaskRunning is cleared)
// and just keeps feeding the decoder. The library's loop() reads from
// the input buffer (PSRAM ringbuffer) and writes to the I2S DMA. With
// vTaskDelay(1) we yield 1 RTOS tick (= 1 ms at the default 1 kHz tick
// rate), giving the WiFi/lwIP task on core 0 room to fill the buffer.
static void audioTaskFn(void* /*arg*/) {
    while (g_audioTaskRunning) {
        if (g_audio) {
            g_audio->loop();
        }
        vTaskDelay(1);
    }
    g_audioTask = nullptr;
    vTaskDelete(nullptr);
}

static void startAudioTask() {
    if (g_audioTask != nullptr) return;
    g_audioTaskRunning = true;
    // Stack 8000 words = 32 KB — the maintainer's example uses 5000, we
    // give extra headroom for safety (MP3 decoder + reconnect path can
    // use ~6 KB peak). Priority 5 keeps the task above the Arduino loop
    // (default priority 1) so a long redraw / pet-tick can't starve
    // audio. portPRIVILEGE_BIT is the default for tasks that don't need
    // unprivileged-mode separation.
    BaseType_t r = xTaskCreatePinnedToCore(audioTaskFn, "webradio_audio",
                                           8000, nullptr,
                                           5 | portPRIVILEGE_BIT,
                                           &g_audioTask, /*core*/ 1);
    Serial.printf("[webradio] audio task created on core 1: %d\n", (int)r);
}

static void stopAudioTask() {
    if (g_audioTask == nullptr) return;
    g_audioTaskRunning = false;
    // Wait briefly for the task to exit on its own (the loop polls
    // the running flag at most every ~1 ms). If it doesn't exit within
    // ~200 ms something is very wrong and we just leak the handle.
    for (int i = 0; i < 20 && g_audioTask != nullptr; ++i) {
        delay(10);
    }
    if (g_audioTask != nullptr) {
        Serial.println(F("[webradio] audio task did not exit cleanly"));
        g_audioTask = nullptr;
    }
}

// Switch I2S peripheral: release M5.Speaker, audio lib takes over.
// Only one can hold the I2S bus at a time.
void claimI2sForAudio() {
    auto cfg = M5.Speaker.config();   // read pins *before* end() nulls the setup
    M5.Speaker.end();                 // releases I2S + switches amp off
    if (!g_audio) {
        g_audio = new Audio(/*internalDAC*/ false, /*channelEnabled*/ 3);
    }
    // ESP32-audioI2S uses the same pins M5.Speaker just used. For CoreS3
    // (NS4150) and Core2 (NS4168) the pins are target-specific — we take
    // them from the M5 speaker config instead of guessing.
    g_audio->setPinout(cfg.pin_bck, cfg.pin_ws, cfg.pin_data_out);
    g_audio->setVolume(mapVolume(g_volume0_255));
    // Bring amp back up — otherwise the audio lib plays into a dead
    // I2S bus.
    setSpeakerAmpPower(true);
    // Spin up the dedicated audio decoder task. From here on
    // audio.loop() is *not* called from the main loop — the task is
    // the sole owner.
    startAudioTask();
}

void releaseI2sFromAudio() {
    // Stop the audio task before touching g_audio.stopSong(), otherwise
    // we'd race with the task's audio.loop() call.
    stopAudioTask();
    if (g_audio) {
        g_audio->stopSong();
    }
    // Keep the audio object (re-init costs ~50 KB heap churn);
    // bring M5.Speaker back up for pet sounds.
    M5.Speaker.begin();
}

void tryConnectStream() {
    g_audio->setVolume(mapVolume(g_volume0_255));
    // Connect timeouts generous — defaults are ~250 ms HTTP / 2 s TLS,
    // which is tight over LTE / guest WiFi.
    g_audio->setConnectionTimeout(8000, 8000);
    bool ok = g_audio->connecttohost(selectUrl(g_lang));
    Serial.printf("[webradio] connecttohost(%s) → %d\n",
                  selectUrl(g_lang), (int)ok);
    g_lastReconnectMs = millis();
}

}  // namespace

}  // namespace webradio

// ── Audio library callbacks (weak symbols, global namespace) ────────────────
//
// The ESP32-audioI2S library calls these if they're defined. We
// route everything into the serial log to see where a stream connect
// tips over (DNS, HTTP status, codec detect, etc.).
void audio_info(const char* info) {
    Serial.printf("[audio_info] %s\n", info ? info : "");
}
void audio_showstation(const char* st) {
    Serial.printf("[audio_station] %s\n", st ? st : "");
}
void audio_lasthost(const char* host) {
    Serial.printf("[audio_lasthost] %s\n", host ? host : "");
}
void audio_eof_stream(const char* host) {
    Serial.printf("[audio_eof] %s\n", host ? host : "");
}

namespace webradio {

// ── Public API ────────────────────────────────────────────────────────────

void start(uint8_t lang) {
    g_lang = lang;
    g_reconnectsTried = 0;

    if (g_state == State::Playing || g_state == State::Connecting) {
        // Already active — language may have changed, reopen stream.
        if (g_audio) g_audio->stopSong();
        tryConnectStream();
        g_state = State::Connecting;
        return;
    }

#if TARGET_HAS_LLM
    // Pause voice pipeline — speaker would otherwise trigger Whisper
    // and we'd get ghost tags ("SLEEP" because someone on the radio
    // says "schlafen"). Resume only on stop().
    voice::pause();
#endif

    // Pause Pip-link listener — the AP's WiFi channel rarely matches the
    // ESP-NOW broadcast channel (6), so packets from a paired Pip wouldn't
    // arrive while the radio holds STA on the router's channel. Phase 1:
    // no-op skeleton.
    pip_link::pause();

    // Suppress all pet sound effects while the radio is playing.
    // playSound() goes through M5.Speaker which fights with the audio
    // library for the I2S bus — overlapping starts produce audible
    // crackling. Restored in stop().
    setSoundEnabled(false);

    // Take a WiFi reference — if currently off, the function in
    // net.cpp brings up the async connect.
    wifiKeepAlive(kKeepAliveReason);
    g_wifiWaitStartMs = millis();
    g_state = State::Connecting;
}

void stop() {
    if (g_state == State::Off) return;
    Serial.println(F("[webradio] stop"));
    releaseI2sFromAudio();
    wifiRelease(kKeepAliveReason);
#if TARGET_HAS_LLM
    voice::resume();
#endif
    pip_link::resume();
    setSoundEnabled(true);
    g_state = State::Off;
    g_reconnectsTried = 0;
}

void tick(uint32_t now_ms) {
    if (g_state == State::Off) return;

    if (g_state == State::Connecting) {
        // Wait for WiFi, then open stream.
        if (!wifiIsConnected()) {
            if (now_ms - g_wifiWaitStartMs >= kWifiConnectTimeoutMs) {
                Serial.println(F("[webradio] WiFi connect timeout"));
                g_state = State::Error;
            }
            return;
        }
        // Disable WiFi modem-sleep. The Arduino default leaves
        // power-save on, so RX is gated by the AP's DTIM beacon (~100
        // ms intervals). For continuous MP3 streaming that's far too
        // bursty — packets queue up at the AP, lwIP RX buffer overflows,
        // and the decoder underruns. setSleep(false) keeps the radio
        // listening at all times; battery cost is acceptable while
        // streaming actively.
        WiFi.setSleep(false);
        // WiFi is up — claim I2S every time. claimI2sForAudio() is
        // idempotent (only the Audio object itself is constructed once),
        // and we *must* call it on every start because stop() runs
        // M5.Speaker.begin(), which grabs the I2S bus back. Without
        // re-claiming here the decoder runs but plays into a bus M5
        // owns again — silent webradio while pet sounds keep working.
        claimI2sForAudio();
        // Open stream.
        tryConnectStream();
        // connecttohost returns true if the host was reached, but the
        // actual audio stream is established in audio.loop(). We move
        // to Playing for now and correct later if needed.
        g_state = State::Playing;
        return;
    }

    if (g_state == State::Playing) {
        // audio.loop() is now driven by the dedicated audio task on
        // core 1 (started in claimI2sForAudio). Don't call it from
        // here — calling loop() from two places is a known cause of
        // stutter (issue #253 of the schreibfaul1 lib).
        // Stream loss → reconnect attempt after kReconnectGapMs.
        if (g_audio && !g_audio->isRunning()) {
            if (now_ms - g_lastReconnectMs >= kReconnectGapMs) {
                if (g_reconnectsTried >= kMaxReconnects) {
                    Serial.println(F("[webradio] max reconnects reached"));
                    g_state = State::Error;
                    return;
                }
                g_reconnectsTried++;
                Serial.printf("[webradio] reconnect attempt %u/%u\n",
                              g_reconnectsTried, kMaxReconnects);
                tryConnectStream();
            }
        } else {
            // Stream running → reset reconnect counter.
            g_reconnectsTried = 0;
        }
    }
    // Error state: only stop() gets us out again.
}

State state() { return g_state; }

void setVolume(uint8_t vol) {
    g_volume0_255 = vol;
    if (g_audio && g_state == State::Playing) {
        g_audio->setVolume(mapVolume(vol));
    }
}

}  // namespace webradio

#else   // !TARGET_HAS_WIFI — Pip build, all functions are no-ops

namespace webradio {
void  start(uint8_t)       {}
void  stop()               {}
void  tick(uint32_t)       {}
State state()              { return State::Off; }
void  setVolume(uint8_t)   {}
}

#endif  // TARGET_HAS_WIFI
