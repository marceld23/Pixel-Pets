#include "photo_store.h"

#if TARGET_HAS_CAMERA

#include <LittleFS.h>
#include <Preferences.h>

namespace photo_store {

namespace {

// Slot order: monotonic counter; the highest value is the newest capture.
// 0 = empty.
constexpr char kNvsNs[]   = "photos";
constexpr char kKeyOrder0[] = "s0";
constexpr char kKeyOrder1[] = "s1";
constexpr char kKeyOrder2[] = "s2";
constexpr char kKeyOrder3[] = "s3";
constexpr char kKeyOrder4[] = "s4";
constexpr char kKeyOrder5[] = "ord";

constexpr const char* kSlotKey[kMaxPhotos] = {
    kKeyOrder0, kKeyOrder1, kKeyOrder2, kKeyOrder3, kKeyOrder4
};

uint32_t g_slotOrder[kMaxPhotos] = {0, 0, 0, 0, 0};
uint32_t g_nextOrder = 1;
bool     g_mounted = false;

// Read buffer for readByDisplayIndex(). Overwritten on the next call.
// Heap-allocated, kept static — size ~30 KB for a typical 320×240
// q=10 JPEG.
uint8_t* g_readBuf = nullptr;
size_t   g_readCap = 0;

void buildSlotPath(uint8_t slot, char* out, size_t cap) {
    snprintf(out, cap, "/photo_%u.jpg", (unsigned)slot);
}

// Sort slot indices by order descending (newest first).
// Returns the number of filled slots.
uint8_t collectSortedSlots(uint8_t out[kMaxPhotos]) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kMaxPhotos; ++i) {
        if (g_slotOrder[i] > 0) out[n++] = i;
    }
    // Insertion sort by order desc — n is small.
    for (uint8_t i = 1; i < n; ++i) {
        uint8_t cur = out[i];
        int j = i - 1;
        while (j >= 0 && g_slotOrder[out[j]] < g_slotOrder[cur]) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = cur;
    }
    return n;
}

uint8_t findSaveSlot() {
    // Prefer an empty slot (also the lowest empty index).
    for (uint8_t i = 0; i < kMaxPhotos; ++i) {
        if (g_slotOrder[i] == 0) return i;
    }
    // Otherwise: oldest slot (smallest order value).
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < kMaxPhotos; ++i) {
        if (g_slotOrder[i] < g_slotOrder[oldest]) oldest = i;
    }
    return oldest;
}

void persistIndex() {
    Preferences p;
    if (!p.begin(kNvsNs, false)) return;
    for (uint8_t i = 0; i < kMaxPhotos; ++i) {
        p.putUInt(kSlotKey[i], g_slotOrder[i]);
    }
    p.putUInt(kKeyOrder5, g_nextOrder);
    p.end();
}

void loadIndex() {
    Preferences p;
    if (!p.begin(kNvsNs, true)) {
        // First boot with an empty partition.
        for (uint8_t i = 0; i < kMaxPhotos; ++i) g_slotOrder[i] = 0;
        g_nextOrder = 1;
        return;
    }
    for (uint8_t i = 0; i < kMaxPhotos; ++i) {
        g_slotOrder[i] = p.getUInt(kSlotKey[i], 0);
    }
    g_nextOrder = p.getUInt(kKeyOrder5, 1);
    p.end();
    // Konsistenz-Check: nextOrder muss > max(slotOrder) sein, sonst Reset.
    uint32_t maxOrder = 0;
    for (uint8_t i = 0; i < kMaxPhotos; ++i) {
        if (g_slotOrder[i] > maxOrder) maxOrder = g_slotOrder[i];
    }
    if (g_nextOrder <= maxOrder) g_nextOrder = maxOrder + 1;
}

bool ensureReadBuf(size_t need) {
    if (need <= g_readCap) return true;
    free(g_readBuf);
    g_readBuf = (uint8_t*)malloc(need);
    if (!g_readBuf) {
        g_readCap = 0;
        return false;
    }
    g_readCap = need;
    return true;
}

}  // namespace

bool begin() {
    if (g_mounted) return true;
    // formatOnFail=true → leere Partition wird beim ersten Boot mit dem
    // neuen Layout automatisch formatiert.
    if (!LittleFS.begin(true, "/photos", 5, "photos")) {
        Serial.println(F("[photo_store] LittleFS mount failed"));
        return false;
    }
    g_mounted = true;
    loadIndex();
    Serial.printf("[photo_store] mounted, %u/%u slots in use, nextOrder=%u\n",
                  (unsigned)count(), (unsigned)kMaxPhotos,
                  (unsigned)g_nextOrder);
    return true;
}

uint8_t count() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kMaxPhotos; ++i) if (g_slotOrder[i] > 0) ++n;
    return n;
}

bool readByDisplayIndex(uint8_t i, const uint8_t** out_data, size_t* out_len) {
    if (!g_mounted || !out_data || !out_len) return false;
    uint8_t sorted[kMaxPhotos];
    uint8_t n = collectSortedSlots(sorted);
    if (i >= n) return false;
    uint8_t slot = sorted[i];

    char path[24];
    buildSlotPath(slot, path, sizeof(path));
    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[photo_store] open '%s' failed\n", path);
        return false;
    }
    size_t sz = f.size();
    if (!ensureReadBuf(sz)) {
        f.close();
        return false;
    }
    f.read(g_readBuf, sz);
    f.close();
    *out_data = g_readBuf;
    *out_len  = sz;
    return true;
}

bool save(const uint8_t* data, size_t len) {
    if (!g_mounted || !data || len == 0) return false;
    uint8_t slot = findSaveSlot();

    char path[24];
    buildSlotPath(slot, path, sizeof(path));
    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("[photo_store] save: open '%s' failed\n", path);
        return false;
    }
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        Serial.printf("[photo_store] save: short write %u/%u\n",
                      (unsigned)written, (unsigned)len);
        return false;
    }
    g_slotOrder[slot] = g_nextOrder++;
    persistIndex();
    Serial.printf("[photo_store] saved %u bytes to slot %u (order=%u)\n",
                  (unsigned)len, (unsigned)slot,
                  (unsigned)g_slotOrder[slot]);
    return true;
}

bool deleteByDisplayIndex(uint8_t i) {
    if (!g_mounted) return false;
    uint8_t sorted[kMaxPhotos];
    uint8_t n = collectSortedSlots(sorted);
    if (i >= n) return false;
    uint8_t slot = sorted[i];

    char path[24];
    buildSlotPath(slot, path, sizeof(path));
    LittleFS.remove(path);   // ignoriere Fehler — Slot wird sowieso freigegeben
    g_slotOrder[slot] = 0;
    persistIndex();
    Serial.printf("[photo_store] deleted slot %u (display idx %u)\n",
                  (unsigned)slot, (unsigned)i);
    return true;
}

}  // namespace photo_store

#else   // !TARGET_HAS_CAMERA — No-Ops

namespace photo_store {
bool   begin()                                                      { return true; }
uint8_t count()                                                     { return 0; }
bool   readByDisplayIndex(uint8_t, const uint8_t**, size_t*)        { return false; }
bool   save(const uint8_t*, size_t)                                 { return false; }
bool   deleteByDisplayIndex(uint8_t)                                { return false; }
}

#endif
