#pragma once
#include <Arduino.h>

#include "target_caps.h"

#if !TARGET_HAS_CAMERA
// On targets without a camera (core2, pip) the header stays includable
// and the functions become no-ops or return empty results.
#endif

// Pet selfie storage: up to 5 JPEG photos on the LittleFS "photos"
// partition. Order is "newest first" (index 0 = newest photo).
// Slots are overwritten with the oldest entry as needed — if individual
// slots are empty (e.g. after a delete), save() uses those first; only
// then does it round-robin overwrite the oldest.
//
// File layout:  /photo_<slot>.jpg  with slot ∈ [0..4]
// Index in NVS: namespace "photos", keys s0..s4 (uint32 monotonic order),
//               ord (uint32 next-order counter)

namespace photo_store {

constexpr uint8_t kMaxPhotos = 5;

// Call at boot: mounts LittleFS, reads index from NVS.
// Returns true if the partition could be mounted (on targets without
// CAMERA always true with no effect).
bool begin();

// Number of filled slots (0..kMaxPhotos), in "newest first" order.
uint8_t count();

// JPEG bytes of the photo at display index `i` (0 = newest). Returns true
// and fills out_data/out_len on success. Caller must NOT free out_data;
// the buffer is static and valid until the next photo_store call.
bool readByDisplayIndex(uint8_t i, const uint8_t** out_data, size_t* out_len);

// Save photo. data → JPEG bytes. Picks a free slot or the oldest
// (if full). Updates the index, persists NVS. true on success.
bool save(const uint8_t* data, size_t len);

// Delete the photo at display index (file + index entry). The remaining
// photos do not physically shift — the NVS index continues to track
// position vs. age.
bool deleteByDisplayIndex(uint8_t i);

}  // namespace photo_store
