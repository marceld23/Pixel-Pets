#pragma once
#include <Arduino.h>

// Voice pipeline: encapsulates KWS + VAD + Whisper + LLM (tag classification)
// against the M5Stack Module LLM. Delivers the final pet tags
// (EAT, PET, TOY_BUTTERFLY, MEDIA_MOVIE, ...) via callback.
//
// Usage: open Serial2 in setup(), call voice::begin(Serial2, cfg),
// call voice::update() in loop(). Tags arrive via the onTag callback.

namespace voice {

struct Config {
    const char* wake_word        = "MUFFIN";
    const char* whisper_language = "de";
    const char* whisper_model    = "whisper-base";
    const char* llm_model        = "qwen3-0.6B-ax630c";
    const char* system_prompt    = nullptr;   // must be set
    // 64 is enough for an empty <think></think> (Qwen3 with /no_think) +
    // up to 3 tag words comma-separated. Larger would only hurt: without
    // a limit the 0.6B model likes to hallucinate several sentences of
    // code on unclear input. If /no_think is ignored and full reasoning
    // starts, the response runs into the limit here — the parser catches
    // the truncated <think> sequence without a closing tag as IDLE.
    int   llm_max_tokens         = 64;
    float mic_volume             = 0.7f;
    int   min_asr_chars          = 5;
    bool  llm_enable_kws         = false;
    bool  append_no_think        = true;       // Qwen3 soft switch
};

// Maximum number of tags per sequence the LLM is allowed to deliver in one response.
static constexpr int MAX_TAG_SEQUENCE = 3;

// Callbacks — all optional
typedef void (*OnWakeFn)();
typedef void (*OnSpeechEndFn)();
typedef void (*OnTranscribedFn)(const String& text);
// Tag sequence: 1..MAX_TAG_SEQUENCE tags in the order they appear in the
// LLM output. When the LLM returns only a single tag, count == 1.
typedef void (*OnTagsFn)(const String tags[], int count, const String& raw_response);
typedef void (*OnSetupFn)(const char* unit_name, const String& work_id);
typedef void (*OnRawMsgFn)(const String& work_id, const String& object, int err_code, const String& raw_msg);

// Lifecycle
bool begin(Stream& moduleSerial, const Config& cfg);
void update();
bool ready();

// Voice pause: halts processing without tearing down the Module LLM
// setups. update() keeps draining the message queue but ignores all
// callbacks (no cb_wake / cb_speech_end / cb_transcribed / cb_tags).
// Use pause() while web radio is running (otherwise Whisper transcribes
// the speaker playback and triggers tags). resume() on stop.
void pause();
void resume();
bool isPaused();

// Callback registration
void onSetup(OnSetupFn cb);
void onWake(OnWakeFn cb);
void onSpeechEnd(OnSpeechEndFn cb);   // VAD reports end of speech
void onTranscribed(OnTranscribedFn cb);
void onTags(OnTagsFn cb);
void onRawMsg(OnRawMsgFn cb);   // optional, for diagnostics

// Current tag list (read only)
const char* const* knownTags();   // null-terminated, all 22 tags

} // namespace voice
