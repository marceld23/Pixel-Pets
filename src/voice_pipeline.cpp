#include "voice_pipeline.h"
#include <M5ModuleLLM.h>
#include <ArduinoJson.h>

namespace voice {

namespace {

M5ModuleLLM g_llm;
String      g_kws_id, g_vad_id, g_whisper_id, g_llm_id;
String      g_buf;             // accumulates LLM stream output until finish:true
Config      g_cfg;
bool        g_ready = false;
bool        g_paused = false;  // suppresses all callbacks (web radio mode)

OnWakeFn        cb_wake        = nullptr;
OnSpeechEndFn   cb_speech_end  = nullptr;
OnTranscribedFn cb_transcribed = nullptr;
OnTagsFn        cb_tags        = nullptr;
OnSetupFn       cb_setup       = nullptr;
OnRawMsgFn      cb_raw         = nullptr;

void emitSetup(const char* unit, const String& id) {
    if (cb_setup) cb_setup(unit, id);
}

// LLM output words → pet action tags. The LLM is offered only simple
// English words (eat, pet, ball, movie, ...) to choose from; the mapping
// to the full tag names happens here.
struct WordMap { const char* word; const char* tag; };
static const WordMap KEYWORDS[] = {
    // English primary keywords (what Qwen3 ideally delivers)
    {"EAT",       "EAT"},
    {"PET",       "PET"},
    {"LOVE",      "LOVE"},
    {"LAUGH",     "LAUGH"},
    {"SLEEP",     "SLEEP"},
    {"WAKE",      "WAKE"},
    {"GREET",     "GREET"},
    {"SAD",       "SAD"},
    {"STARTLE",   "STARTLE"},
    {"SING",      "SING"},
    {"DANCE",     "DANCE"},
    {"BALL",      "TOY_BALL"},
    {"MOUSE",     "TOY_MOUSE"},
    {"RATTLE",    "TOY_RATTLE"},
    {"BUTTERFLY", "TOY_BUTTERFLY"},
    {"PLUSH",     "TOY_PLUSH"},
    {"MOVIE",     "MEDIA_MOVIE"},
    {"GAME",      "MEDIA_GAME"},
    {"INTERNET",  "MEDIA_INTERNET"},
    {"SOCIAL",    "MEDIA_SOCIAL"},
    {"FRIENDS",   "MEDIA_FRIENDS"},
    {"RADIO",     "MEDIA_RADIO"},
    {"IDLE",      "IDLE"},

    // German aliases — Qwen3 often echoes the German input word instead
    // of picking an English tag. We map directly in the parser.
    {"LIEB",          "LOVE"},
    {"HERZ",          "LOVE"},
    {"KUSS",          "LOVE"},
    {"FUTTER",        "EAT"},
    {"ESSEN",         "EAT"},
    {"FRESSEN",       "EAT"},
    {"STREICH",       "PET"},        // streicheln/streichel
    {"LACHEN",        "LAUGH"},
    {"KITZ",          "LAUGH"},      // kitzeln, kitzelig
    {"WITZIG",        "LAUGH"},
    {"SCHLAF",        "SLEEP"},
    {"MUEDE",         "SLEEP"},
    {"AUFSTEH",       "WAKE"},
    {"HALLO",         "GREET"},
    {"GUTEN MORGEN",  "GREET"},
    {"TRAURIG",       "SAD"},
    {"BOESE",         "SAD"},
    {"DOOF",          "SAD"},
    {"BUH",           "STARTLE"},
    {"SCHRECK",       "STARTLE"},
    {"SINGEN",        "SING"},
    {"LIED",          "SING"},
    {"TANZ",          "DANCE"},      // tanz, tanzen
    {"MUSIK",         "DANCE"},
    {"SCHMETTERLING", "TOY_BUTTERFLY"},
    {"MAUS",          "TOY_MOUSE"},
    {"RASSEL",        "TOY_RATTLE"},
    {"STOFFTIER",     "TOY_PLUSH"},
    {"PUPPE",         "TOY_PLUSH"},
    {"KUSCHELT",      "TOY_PLUSH"},
    {"FILM",          "MEDIA_MOVIE"},
    {"FERNSEH",       "MEDIA_MOVIE"},
    {"KINO",          "MEDIA_MOVIE"},
    {"COMPUTER",      "MEDIA_GAME"},
    {"KONSOLE",       "MEDIA_GAME"},
    {"TIKTOK",        "MEDIA_SOCIAL"},
    {"INSTAGRAM",     "MEDIA_SOCIAL"},
    {"INSTA",         "MEDIA_SOCIAL"},
    {"FREUNDE",       "MEDIA_FRIENDS"},
    {"FREUND",        "MEDIA_FRIENDS"},
    {"RADIO",         "MEDIA_RADIO"},
    {"SENDER",        "MEDIA_RADIO"},
    {"NACHRICHTEN",   "MEDIA_RADIO"},
    {nullptr, nullptr}
};

// Tag name list for consumers (all 22 incl. IDLE)
static const char* KNOWN_TAGS[] = {
    "EAT", "PET", "LOVE", "LAUGH", "SLEEP", "WAKE", "GREET",
    "SAD", "STARTLE", "SING", "DANCE",
    "TOY_BALL", "TOY_MOUSE", "TOY_RATTLE", "TOY_BUTTERFLY", "TOY_PLUSH",
    "MEDIA_MOVIE", "MEDIA_GAME", "MEDIA_INTERNET", "MEDIA_SOCIAL", "MEDIA_FRIENDS",
    "MEDIA_RADIO",
    "IDLE",
    nullptr
};

// Finds up to MAX_TAG_SEQUENCE tags in the order they appear in the
// raw LLM output. Consecutive duplicates are collapsed. If nothing is
// recognized the function returns a single IDLE tag.
int parseTags(const String& raw, String out[]) {
    // Qwen3 with /no_think typically emits an empty <think></think>
    // before the actual response. That has to come out completely;
    // otherwise the keyword matching below finds hits inside the
    // (possibly hallucinated) reasoning block. If /no_think gets
    // ignored and the response is cut off mid-thought (no closing
    // </think>), there's no real classification anyway → IDLE.
    String cleaned = raw;
    int tStart = cleaned.indexOf("<think>");
    if (tStart >= 0) {
        int tEnd = cleaned.indexOf("</think>", tStart);
        if (tEnd > tStart) {
            cleaned = cleaned.substring(0, tStart) +
                      cleaned.substring(tEnd + 8);   // 8 = strlen("</think>")
        } else {
            // <think> without </think> → response cut off mid-reasoning.
            out[0] = "IDLE";
            return 1;
        }
    }
    cleaned.trim();

    String upper = cleaned;
    upper.toUpperCase();

    // Garbage filter: catches obvious hallucinations (code, markdown,
    // multi-line structures). A legit response is shorter than ~30 chars,
    // single-line, no code markers. If the filter trips we ignore the
    // output completely — no trying to fish keywords out of hallucinated
    // code, that would be the worst case (sleep tag from "while sleep_count").
    bool looksLikeCode = upper.indexOf("IMPORT ") >= 0 ||
                         upper.indexOf("FROM SCIPY")  >= 0 ||
                         upper.indexOf("FROM NUMPY")  >= 0 ||
                         upper.indexOf("FROM MATH")   >= 0 ||
                         upper.indexOf("DEF ")        >= 0 ||
                         upper.indexOf("FUNCTION ")   >= 0 ||
                         upper.indexOf("RETURN ")     >= 0 ||
                         upper.indexOf("```")         >= 0 ||
                         upper.indexOf("{")           >= 0 ||
                         upper.indexOf("<DIV")        >= 0 ||
                         upper.indexOf("<P>")         >= 0;
    int newlineCount = 0;
    for (int i = 0; i < (int)cleaned.length(); ++i) {
        if (cleaned[i] == '\n') newlineCount++;
    }
    bool tooLong      = cleaned.length() > 120;   // legit max ~30 chars
    bool tooManyLines = newlineCount > 2;
    if (looksLikeCode || tooLong || tooManyLines) {
        out[0] = "IDLE";
        return 1;
    }

    struct Hit { int pos; const char* tag; };
    Hit hits[MAX_TAG_SEQUENCE * 4];
    int hitCount = 0;

    // Collect all keyword hits in the text (multiple per keyword possible
    // so e.g. "EAT, SLEEP, EAT" is recognized as three steps).
    for (int i = 0; KEYWORDS[i].word != nullptr; ++i) {
        int from = 0;
        while (true) {
            int p = upper.indexOf(KEYWORDS[i].word, from);
            if (p < 0) break;
            if (hitCount < (int)(sizeof(hits) / sizeof(hits[0]))) {
                hits[hitCount++] = {p, KEYWORDS[i].tag};
            }
            from = p + 1;
        }
    }

    // Sort by position (small insertion sort, hitCount is small)
    for (int i = 1; i < hitCount; ++i) {
        Hit cur = hits[i];
        int j = i - 1;
        while (j >= 0 && hits[j].pos > cur.pos) {
            hits[j + 1] = hits[j];
            --j;
        }
        hits[j + 1] = cur;
    }

    int count = 0;
    const char* lastTag = nullptr;
    for (int i = 0; i < hitCount && count < MAX_TAG_SEQUENCE; ++i) {
        if (lastTag && strcmp(lastTag, hits[i].tag) == 0) continue;
        out[count++] = hits[i].tag;
        lastTag = hits[i].tag;
    }

    if (count == 0) {
        out[0] = "IDLE";
        count = 1;
    }
    return count;
}

}  // anon namespace

bool begin(Stream& moduleSerial, const Config& cfg) {
    g_cfg = cfg;
    g_ready = false;
    g_buf = "";

    g_llm.begin(&moduleSerial);

    // Wait for module boot — Linux startup takes long on the first boot
    uint32_t deadline = millis() + 60000;
    while (!g_llm.checkConnection() && millis() < deadline) {
        delay(500);
    }
    if (!g_llm.checkConnection()) return false;

    g_llm.sys.reset();
    // sys.reset restarts the services on the LLM module — give them
    // time to come up, otherwise the first setup call times out.
    delay(3000);

    // Audio (mic gain) — best-effort, no retry since uncritical
    {
        m5_module_llm::ApiAudioSetupConfig_t c;
        c.capVolume = g_cfg.mic_volume;
        g_llm.audio.setup(c);
    }

    // KWS (wake word) — up to 3× retry, exits on success
    for (int i = 0; i < 3 && g_kws_id.isEmpty(); ++i) {
        m5_module_llm::ApiKwsSetupConfig_t c;
        c.kws = g_cfg.wake_word;
        g_kws_id = g_llm.kws.setup(c, "kws_setup", "en_US");
        if (g_kws_id.isEmpty()) delay(1500);
    }
    emitSetup("kws", g_kws_id);
    if (g_kws_id.isEmpty()) return false;

    // VAD
    for (int i = 0; i < 3 && g_vad_id.isEmpty(); ++i) {
        m5_module_llm::ApiVadSetupConfig_t c;
        c.input = {"sys.pcm", g_kws_id};
        g_vad_id = g_llm.vad.setup(c, "vad_setup");
        if (g_vad_id.isEmpty()) delay(1000);
    }
    emitSetup("vad", g_vad_id);
    if (g_vad_id.isEmpty()) return false;

    // Whisper
    for (int i = 0; i < 3 && g_whisper_id.isEmpty(); ++i) {
        m5_module_llm::ApiWhisperSetupConfig_t c;
        c.model    = g_cfg.whisper_model;
        c.input    = {"sys.pcm", g_kws_id, g_vad_id};
        c.language = g_cfg.whisper_language;
        g_whisper_id = g_llm.whisper.setup(c, "whisper_setup");
        if (g_whisper_id.isEmpty()) delay(1000);
    }
    emitSetup("whisper", g_whisper_id);
    if (g_whisper_id.isEmpty()) return false;

    // LLM
    for (int i = 0; i < 3 && g_llm_id.isEmpty(); ++i) {
        m5_module_llm::ApiLlmSetupConfig_t c;
        c.model         = g_cfg.llm_model;
        c.prompt        = g_cfg.system_prompt ? g_cfg.system_prompt : "";
        c.enkws         = g_cfg.llm_enable_kws;
        c.max_token_len = g_cfg.llm_max_tokens;
        g_llm_id = g_llm.llm.setup(c);
        if (g_llm_id.isEmpty()) delay(1000);
    }
    emitSetup("llm", g_llm_id);
    if (g_llm_id.isEmpty()) return false;

    g_ready = true;
    return true;
}

void update() {
    g_llm.update();
    if (g_paused) return;   // drain already happened, suppress callbacks

    for (auto& msg : g_llm.msg.responseMsgList) {
        if (cb_raw) cb_raw(msg.work_id, msg.object, msg.error.code, msg.raw_msg);

        // Wake word
        if (msg.work_id == g_kws_id) {
            if (cb_wake) cb_wake();
            continue;
        }

        // VAD: data=true speech starts, data=false speech ends
        if (msg.work_id == g_vad_id && msg.object == "vad.bool") {
            JsonDocument doc;
            deserializeJson(doc, msg.raw_msg);
            bool active = doc["data"];
            if (!active && cb_speech_end) cb_speech_end();
            continue;
        }

        // Whisper transcription → first keyword match, otherwise LLM fallback
        if (msg.work_id == g_whisper_id && msg.object == "asr.utf-8") {
            JsonDocument doc;
            deserializeJson(doc, msg.raw_msg);
            String text = doc["data"].as<String>();
            text.trim();
            if (cb_transcribed) cb_transcribed(text);

            if (text.length() < (unsigned)g_cfg.min_asr_chars) continue;

            // Whisper-first bypass: if the transcribed text already
            // contains a known keyword (e.g. "Futter", "streichel",
            // "schlaf"), we dispatch the tag directly — without an LLM
            // round-trip. Saves ~1-2 s of latency and bypasses 0.6B-model
            // hallucinations (date spam, code blocks, etc.) entirely,
            // since the LLM isn't even called.
            //
            // parseTags returns IDLE if nothing matches — then fall back
            // to the LLM for semantic cases like "ich bin müde" → SLEEP.
            {
                String tags[MAX_TAG_SEQUENCE];
                int n = parseTags(text, tags);
                bool hasNonIdle = false;
                for (int i = 0; i < n; ++i) {
                    if (tags[i] != "IDLE") { hasNonIdle = true; break; }
                }
                if (hasNonIdle) {
                    if (cb_tags) cb_tags(tags, n, text);
                    continue;
                }
            }

            // Whisper output contained no direct keyword → semantic LLM
            // fallback. Here the model can translate statements like
            // "ich bin müde" → SLEEP, which the direct keyword match
            // wouldn't catch (unless Whisper itself hears "MUEDE").
            if (!g_llm_id.isEmpty()) {
                String prompt_input = text;
                if (g_cfg.append_no_think) prompt_input += " /no_think";
                g_llm.llm.inference(g_llm_id, prompt_input);
            }
            continue;
        }

        // LLM stream → accumulate buffer until finish, then parse tag
        if (msg.work_id == g_llm_id && msg.object == "llm.utf-8.stream") {
            JsonDocument doc;
            deserializeJson(doc, msg.raw_msg);
            String delta = doc["data"]["delta"].as<String>();
            bool isFinish = doc["data"]["finish"];
            g_buf += delta;
            if (isFinish) {
                String tags[MAX_TAG_SEQUENCE];
                int n = parseTags(g_buf, tags);
                if (cb_tags) cb_tags(tags, n, g_buf);
                g_buf = "";
            }
            continue;
        }
    }
    g_llm.msg.responseMsgList.clear();
}

bool ready() { return g_ready; }

void onSetup(OnSetupFn cb)             { cb_setup       = cb; }
void onWake(OnWakeFn cb)               { cb_wake        = cb; }
void onSpeechEnd(OnSpeechEndFn cb)     { cb_speech_end  = cb; }
void onTranscribed(OnTranscribedFn cb) { cb_transcribed = cb; }
void onTags(OnTagsFn cb)               { cb_tags        = cb; }
void onRawMsg(OnRawMsgFn cb)           { cb_raw         = cb; }

const char* const* knownTags() { return KNOWN_TAGS; }

void pause()      { g_paused = true; }
void resume()     { g_paused = false; }
bool isPaused()   { return g_paused; }

} // namespace voice
