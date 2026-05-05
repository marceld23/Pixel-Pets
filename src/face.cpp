#include "face.h"
#include "i18n.h"
#include "screenshot.h"
#include <math.h>

// Forward decls for helpers used by drawFace() — definitions sit further
// down with the friends-screen renderer.
static void drawTinyPet(M5Canvas& c, int cx, int cy, uint8_t animal,
                        uint16_t /*tint*/, bool happy);
static void drawItemIcon(M5Canvas& c, uint8_t kind, int cx, int cy, int sz);

namespace {

// ─── Layout (must match pet_state.cpp's classifyTouchZone) ──────────────────

constexpr int CANVAS_W = 320;
constexpr int CANVAS_H = 240;
constexpr int CX = CANVAS_W / 2;
constexpr int CY = CANVAS_H / 2 + 8;
constexpr int HEAD_RX = 110;
constexpr int HEAD_RY = 92;
constexpr int EYE_DX  = 38;
constexpr int EYE_OFF_Y = -18;
constexpr int MOUTH_OFF_Y = 32;

// ─── Palette ─────────────────────────────────────────────────────────────────

// Per-animal palette. Filled in by setRenderAnimal at the start of drawFace
// and read by COL_BODY etc. — keeps the cascade simple.
struct AnimalStyle {
  uint16_t body;
  uint16_t bodyShade;
  uint16_t bodyHighlight;
  uint16_t muzzle;
  uint16_t nose;
  uint16_t innerEar;
};
static AnimalStyle g_renderStyle = {
  0,0,0,0,0,0      // populated at runtime — values can't be filled at static-init time
};

static AnimalStyle styleFor(AnimalType t, M5Canvas& c) {
  switch (t) {
    case AnimalType::Cat:
      return AnimalStyle{
        c.color565(210, 210, 215),    // body — light grey
        c.color565(170, 170, 180),    // shade
        c.color565(235, 235, 240),    // highlight
        c.color565(245, 240, 230),    // muzzle white
        c.color565(240, 130, 150),    // pink nose
        c.color565(255, 195, 210),    // pink inner ear
      };
    case AnimalType::Dog:
      return AnimalStyle{
        c.color565(170, 110,  75),    // body — chocolate brown
        c.color565(130,  85,  55),    // shade
        c.color565(195, 140, 100),    // highlight
        c.color565(240, 220, 190),    // cream muzzle
        c.color565( 30,  20,  15),    // black nose
        c.color565(225, 165, 145),    // warm inner ear
      };
    case AnimalType::Bear:
    default:
      return AnimalStyle{
        c.color565(200, 145,  95),    // teddy tan
        c.color565(170, 115,  75),
        c.color565(225, 175, 130),
        c.color565(245, 225, 195),
        c.color565( 50,  32,  22),
        c.color565(255, 195, 175),
      };
  }
}

static void setRenderAnimal(AnimalType t, M5Canvas& c) {
  g_renderStyle = styleFor(t, c);
}

inline uint16_t COL_BODY(M5Canvas&)       { return g_renderStyle.body; }
inline uint16_t COL_BODY_SHADE(M5Canvas&) { return g_renderStyle.bodyShade; }
inline uint16_t COL_MUZZLE(M5Canvas&)     { return g_renderStyle.muzzle; }
inline uint16_t COL_NOSE(M5Canvas&)       { return g_renderStyle.nose; }
inline uint16_t COL_DARK(M5Canvas& c)     { return c.color565( 45,  35,  30); }
inline uint16_t COL_BLUSH(M5Canvas& c)      { return c.color565(255, 130, 145); }
inline uint16_t COL_HEART(M5Canvas& c)      { return c.color565(240, 70, 110); }
inline uint16_t COL_TEAR(M5Canvas& c)       { return c.color565(120, 190, 255); }
inline uint16_t COL_GLINT(M5Canvas& c)      { return c.color565(255, 255, 255); }

uint16_t bgForPhase(M5Canvas& c, TimePhase p) {
  switch (p) {
    case TimePhase::Morning: return c.color565(180, 220, 245);   // soft pale blue
    case TimePhase::Day:     return c.color565(140, 200, 240);   // clear sky blue
    case TimePhase::Evening: return c.color565(160, 180, 220);   // muted dusk blue
    case TimePhase::Night:   return c.color565( 60,  70, 110);
  }
  return c.color565(140, 200, 240);
}
bool isDarkPhase(TimePhase p) { return p == TimePhase::Night; }

void drawNightStars(M5Canvas& c, uint32_t now_ms) {
  for (int i = 0; i < 10; ++i) {
    int x = ((i * 47) + 13) % CANVAS_W;
    int y = ((i * 23) + 7)  % 90;
    int phase = ((now_ms / 250) + i * 3) & 7;
    if (phase < 5) {
      c.drawPixel(x,     y,     COL_GLINT(c));
      if (phase < 3) {
        c.drawPixel(x + 1, y, COL_GLINT(c));
        c.drawPixel(x, y + 1, COL_GLINT(c));
      }
    }
  }
}

// Apply a dance offset to the head position based on current time and mic
// level. Cycles through five distinct moves every 2.6 seconds. Amplitudes
// are deliberately large — the pet should clearly *react* to the music.
void applyDanceOffset(int& hx, int& hy, uint32_t now_ms, int micLevel) {
  // Intensity from mic level — punchier scaling than before. The lower
  // sensitivity threshold makes quieter sounds also visibly move the pet,
  // and the upper bound goes past 1.0 so loud peaks really kick.
  float intensity = (float)micLevel / 8000.0f;
  if (intensity > 1.5f) intensity = 1.5f;
  if (intensity < 0.40f) intensity = 0.40f;

  uint8_t move = (now_ms / 2600) % 5;
  float p = (float)(now_ms % 2600) / 2600.0f;

  switch (move) {
    case 0: {  // Big bounce + horizontal jitter
      float t = p * 8.0f * (float)PI;
      hy -= (int)(fabsf(sinf(t)) * 24.0f * intensity);
      hx += (int)(sinf(t * 0.5f) * 8.0f * intensity);
      break;
    }
    case 1: {  // Wide hip-swing — bigger lateral throw with vertical bob
      float t = p * 4.0f * (float)PI;
      hx += (int)(sinf(t) * 32.0f * intensity);
      hy -= (int)(fabsf(cosf(t * 0.5f)) * 7.0f * intensity);
      break;
    }
    case 2: {  // Twist — fuller orbit
      float t = p * 6.0f * (float)PI;
      hx += (int)(sinf(t) * 20.0f * intensity);
      hy += (int)(cosf(t) * 14.0f * intensity);
      break;
    }
    case 3: {  // Aggressive headbang + lateral shake
      float t = p * 14.0f * (float)PI;
      hy -= (int)(fabsf(sinf(t)) * 26.0f * intensity);
      hx += (int)(sinf(t * 2.0f) * 10.0f * intensity);
      break;
    }
    case 4: {  // Figure-8 spin — both axes weave at different rates
      float t = p * 4.0f * (float)PI;
      hx += (int)(sinf(t)         * 24.0f * intensity);
      hy += (int)(sinf(t * 2.0f)  * 16.0f * intensity);
      break;
    }
  }
}

// Small poop pile — three stacked blobs in two browns.
void drawPile(M5Canvas& c, int x, int y) {
  uint16_t brown     = c.color565(120,  80,  50);
  uint16_t brownDark = c.color565( 85,  55,  35);
  c.fillEllipse(x,     y,     8, 4, brown);
  c.fillEllipse(x - 1, y - 3, 6, 3, brownDark);
  c.fillCircle (x + 1, y - 6, 2,    brown);
}

// Dirt smudges on the pet's body when its hygiene is low.
void drawDirt(M5Canvas& c, int hx, int hy) {
  uint16_t dirtCol     = c.color565(85, 60, 40);
  uint16_t dirtDarker  = c.color565(60, 40, 25);
  c.fillEllipse(hx - 28, hy + 18, 8, 5, dirtCol);
  c.fillCircle (hx - 26, hy + 17, 2,     dirtDarker);
  c.fillEllipse(hx + 32, hy + 28, 7, 4, dirtCol);
  c.fillCircle (hx + 34, hy + 28, 2,     dirtDarker);
  c.fillEllipse(hx -  8, hy + 38, 6, 3, dirtCol);
}

// Pet-view shortcut: small panel with a "pile + red slash" icon. Only
// drawn when piles exist.
void drawCleaningButton(M5Canvas& c, uint8_t pileCount, uint32_t now_ms) {
  if (pileCount == 0) return;
  const Rect& r = kCleaningButtonRect;
  // Subtle pulse to draw the eye when the pet really needs help.
  bool urgent = (pileCount >= 5);
  uint16_t panel = urgent ? c.color565(240, 200, 180) : c.color565(255, 240, 240);
  if (urgent) {
    int p = (now_ms / 600) & 1;
    if (p) panel = c.color565(255, 220, 200);
  }
  uint16_t glyph = c.color565(60, 50, 50);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);

  int cx = r.x + r.w/2, cy = r.y + r.h/2 + 2;
  // Small pile icon
  uint16_t brown = c.color565(120, 80, 50);
  c.fillEllipse(cx,     cy + 1, 7, 3, brown);
  c.fillEllipse(cx - 1, cy - 2, 5, 3, c.color565(85, 55, 35));
  // Red diagonal slash
  uint16_t red = c.color565(220, 60, 60);
  c.drawLine(cx - 11, cy + 8, cx + 11, cy - 8, red);
  c.drawLine(cx - 10, cy + 8, cx + 12, cy - 8, red);
  c.drawLine(cx - 11, cy + 7, cx + 11, cy - 9, red);

  // Tiny badge in the corner with the count
  char buf[4];
  snprintf(buf, sizeof(buf), "%u", (unsigned)pileCount);
  c.setTextDatum(top_right);
  c.setTextSize(1);
  c.setTextColor(glyph);
  c.drawString(buf, r.x + r.w - 4, r.y + 3);
  c.setTextDatum(top_left);
}

// Falling rain — stripes of blue raindrops at varying speeds.
void drawRain(M5Canvas& c, uint32_t age_ms) {
  uint16_t dropCol  = c.color565(150, 200, 255);
  uint16_t dropDark = c.color565( 90, 150, 220);
  // Density ramps up over the first 200 ms and down over the last 300 ms
  int density = 32;
  if (age_ms < 200)               density = (age_ms * 32) / 200;
  else if (age_ms > 1900)         density = ((2200 - (int)age_ms) * 32) / 300;
  if (density < 0) density = 0;
  if (density > 32) density = 32;
  for (int i = 0; i < density; ++i) {
    int x = (i * 73 + 17) % CANVAS_W;
    int speed = 200 + (i * 19) % 120;             // 200..320 px/sec
    int total = CANVAS_H + 30;                     // off-screen on both ends
    int yProgress = (int)(((uint64_t)age_ms * speed / 1000 + i * 31) % total);
    int y = yProgress - 20;
    if (y > -6 && y < CANVAS_H) {
      c.drawLine(x,     y, x,     y + 5, dropCol);
      c.drawLine(x + 1, y + 1, x + 1, y + 5, dropDark);
    }
  }
}

// Three little drops perched on the head — only during the "wet" phase.
void drawWetDroplets(M5Canvas& c, int hx, int hy, uint32_t phase_ms) {
  uint16_t dropCol  = c.color565(120, 180, 230);
  uint16_t dropDark = c.color565( 70, 130, 200);
  // Slight slide as the wet phase progresses
  int slide = (int)(phase_ms / 80) % 2;
  c.fillEllipse(hx - 32, hy - 50 + slide, 3, 5, dropCol);
  c.fillEllipse(hx + 24, hy - 60 + slide, 3, 5, dropCol);
  c.fillEllipse(hx +  4, hy - 66, 4, 6, dropCol);
  c.fillEllipse(hx +  4, hy - 67, 2, 2, dropDark);
}

// Tiny droplets flying outward when the pet shakes itself dry.
void drawShakeOffDroplets(M5Canvas& c, int hx, int hy, uint32_t phase_ms) {
  uint16_t dropCol = c.color565(160, 210, 240);
  // 8 droplets sprayed from around the head, expanding over 800ms
  float t = phase_ms / 800.0f;
  if (t > 1.0f) t = 1.0f;
  int reach = 30 + (int)(60.0f * t);
  for (int i = 0; i < 8; ++i) {
    float a = i * (PI / 4.0f);
    int dx = (int)(cosf(a) * reach);
    int dy = (int)(sinf(a) * reach * 0.7f) - 20;       // bias upward
    int sz = (t < 0.6f) ? 3 : (t < 0.9f) ? 2 : 1;
    c.fillCircle(hx + dx, hy + dy, sz, dropCol);
  }
}

// A small fluffy cloud — clusters of circles. `size`: 0 small, 1 normal.
void drawCloud(M5Canvas& c, int cx, int cy, int size, uint16_t col) {
  if (size <= 0) {
    c.fillCircle(cx - 5, cy + 1, 4, col);
    c.fillCircle(cx + 5, cy + 1, 4, col);
    c.fillCircle(cx,     cy - 2, 5, col);
  } else {
    c.fillCircle(cx - 9, cy + 2, 6, col);
    c.fillCircle(cx + 9, cy + 2, 6, col);
    c.fillCircle(cx,     cy - 2, 8, col);
    c.fillCircle(cx + 5, cy + 3, 5, col);
    c.fillCircle(cx - 4, cy + 3, 5, col);
  }
}

// Two clouds drifting slowly across the sky. Phase-tinted so night clouds
// sit naturally against the dark sky.
void drawClouds(M5Canvas& c, TimePhase phase, uint32_t now_ms) {
  uint16_t col;
  switch (phase) {
    case TimePhase::Night:   col = c.color565(135, 145, 175); break;
    case TimePhase::Evening: col = c.color565(255, 235, 220); break;
    case TimePhase::Morning: col = c.color565(255, 248, 235); break;
    case TimePhase::Day:
    default:                 col = c.color565(252, 252, 254);
  }

  struct Def { int y; int size; uint32_t period; uint32_t offset; };
  static const Def clouds[2] = {
    {  65, 1, 75000, 0     },
    { 105, 0, 55000, 22000 },
  };
  for (int i = 0; i < 2; ++i) {
    uint32_t t = (now_ms + clouds[i].offset) % clouds[i].period;
    int x = (int)(((int64_t)t * 380 / clouds[i].period) - 30);  // -30..350 drift
    drawCloud(c, x, clouds[i].y, clouds[i].size, col);
  }
}

// Sun (with gently pulsing rays) by day, crescent moon by night. Positioned
// in the upper-right "sky" — out of the way of bars/clock/battery, behind
// the pet so the head can occlude it slightly when wandering right.
void drawCelestial(M5Canvas& c, TimePhase phase, uint32_t now_ms,
                   uint8_t moonPhase255 = 0xFF) {
  const int cx = 275, cy = 55, r = 18;

  if (phase == TimePhase::Night) {
    uint16_t moonCol = c.color565(235, 235, 245);
    uint16_t skyBg   = c.color565( 60,  70, 110);   // matches Night bg
    if (moonPhase255 == 0xFF) {
      // Fallback crescent (no world data yet).
      c.fillCircle(cx, cy, r, moonCol);
      c.fillCircle(cx + 7, cy - 2, r - 1, skyBg);
    } else {
      // Phase-aware moon: full disc, then carve a shadow on the dark side.
      float p = moonPhase255 / 254.0f;
      c.fillCircle(cx, cy, r, moonCol);
      if (p < 0.02f || p > 0.98f) {
        c.fillCircle(cx, cy, r, skyBg);                     // new moon
      } else if (p < 0.5f) {
        int dx = (int)((0.5f - p) * 2.0f * (r * 2));         // waxing
        c.fillCircle(cx - dx, cy, r, skyBg);
      } else if (p > 0.5f) {
        int dx = (int)((p - 0.5f) * 2.0f * (r * 2));         // waning
        c.fillCircle(cx + dx, cy, r, skyBg);
      }
      c.drawCircle(cx, cy, r, c.color565(220, 215, 200));
    }
    // Two tiny stars hanging out near the moon
    c.drawPixel(cx - 24, cy -  8, c.color565(255, 255, 255));
    c.drawPixel(cx + 28, cy + 12, c.color565(220, 220, 240));
  } else {
    uint16_t sunCol, rayCol;
    switch (phase) {
      case TimePhase::Morning:
        sunCol = c.color565(255, 200, 130);
        rayCol = c.color565(255, 220, 150);
        break;
      case TimePhase::Day:
        sunCol = c.color565(255, 220,  80);
        rayCol = c.color565(255, 235, 130);
        break;
      case TimePhase::Evening:
        sunCol = c.color565(255, 145,  90);
        rayCol = c.color565(255, 175, 110);
        break;
      default:
        sunCol = c.color565(255, 220,  80);
        rayCol = c.color565(255, 235, 130);
    }
    // Rays — pulse length over a 3-second cycle
    float t = (float)(now_ms % 3000) / 3000.0f;
    int rayInner = r + 4;
    int rayOuter = r + 8 + (int)(2.0f * sinf(t * 2.0f * (float)PI));
    for (int i = 0; i < 8; ++i) {
      float a = i * (PI / 4.0f);
      int x1 = cx + (int)(cosf(a) * rayInner);
      int y1 = cy + (int)(sinf(a) * rayInner);
      int x2 = cx + (int)(cosf(a) * rayOuter);
      int y2 = cy + (int)(sinf(a) * rayOuter);
      c.drawLine(x1, y1, x2, y2, rayCol);
      // Slight thickening on the cardinal rays
      if (i % 2 == 0) c.drawLine(x1 + 1, y1, x2 + 1, y2, rayCol);
    }
    c.fillCircle(cx, cy, r, sunCol);
  }
}

// Two layers of soft rolling hills along the bottom of the canvas. They
// peek out left and right of the pet to suggest a stylised landscape.
void drawHills(M5Canvas& c, TimePhase phase) {
  uint16_t back, front;
  switch (phase) {
    case TimePhase::Morning:
      back  = c.color565(170, 210, 150);
      front = c.color565(130, 180, 110);
      break;
    case TimePhase::Day:
      back  = c.color565(150, 200, 130);
      front = c.color565(110, 175,  95);
      break;
    case TimePhase::Evening:
      back  = c.color565(185, 145, 105);
      front = c.color565(145, 110,  80);
      break;
    case TimePhase::Night:
      back  = c.color565( 45,  60,  90);
      front = c.color565( 22,  32,  62);
      break;
  }

  // Back hills (further, lighter)
  c.fillEllipse( 35, 210,  95, 28, back);
  c.fillEllipse(170, 205, 130, 32, back);
  c.fillEllipse(290, 210,  80, 28, back);

  // Front hills (closer, darker)
  c.fillEllipse(110, 220, 75, 22, front);
  c.fillEllipse(250, 220, 95, 24, front);
}

// ─── Scene backgrounds ──────────────────────────────────────────────────────
//
// Each scene renderer fills the whole canvas with its own background. The
// pet is drawn on top by drawFace afterwards, so each scene needs to leave
// the head/body area visually clean. Phase (time of day) is honored where
// it makes sense (Meadow / Forest / Beach / Desert); Bedroom and Space
// have their own fixed lighting.

void drawSceneMeadow(M5Canvas& c, TimePhase phase, uint32_t now_ms,
                     uint8_t moonPhase255 = 0xFF) {
  c.fillSprite(bgForPhase(c, phase));
  if (isDarkPhase(phase)) drawNightStars(c, now_ms);
  drawCelestial(c, phase, now_ms, moonPhase255);
  drawClouds   (c, phase, now_ms);
  drawHills    (c, phase);
}

void drawSceneBedroom(M5Canvas& c, TimePhase phase, uint32_t now_ms,
                      Weather weather = Weather::Sunny,
                      uint8_t moonPhase255 = 0xFF) {
  bool isMorning = (phase == TimePhase::Morning);
  bool isEvening = (phase == TimePhase::Evening);
  bool isNight   = (phase == TimePhase::Night);

  // Wall + floor colours dim toward evening / night so the room feels lit
  // by the window rather than the full daytime palette.
  uint16_t wallA      = isNight   ? c.color565(105,  70,  95) :
                         isEvening ? c.color565(195, 145, 170) :
                                    c.color565(255, 220, 230);
  uint16_t wallB      = isNight   ? c.color565( 90,  60,  82) :
                         isEvening ? c.color565(180, 132, 158) :
                                    c.color565(245, 200, 220);
  uint16_t floorMain  = isNight   ? c.color565( 85,  60,  42) :
                         isEvening ? c.color565(135,  98,  68) :
                                    c.color565(180, 130,  85);
  uint16_t floorPlank = isNight   ? c.color565( 65,  45,  32) :
                         isEvening ? c.color565(110,  78,  50) :
                                    c.color565(150, 100,  60);
  uint16_t skirt      = isNight   ? c.color565( 60,  42,  28) :
                         isEvening ? c.color565( 88,  60,  40) :
                                    c.color565(120,  80,  50);

  c.fillSprite(wallA);
  for (int x = 0; x < 320; x += 32) {
    c.fillRect(x + 16, 0, 16, 165, wallB);
  }
  // Picture frame upper-left — picture itself dims at night/evening.
  uint16_t frameC   = c.color565(70, 50, 30);
  uint16_t pictureC = isNight   ? c.color565(110, 130, 160) :
                       isEvening ? c.color565(195, 205, 220) :
                                  c.color565(220, 240, 255);
  uint16_t pictureSun = isNight ? c.color565(180, 160,  80)
                                 : c.color565(255, 220, 100);
  c.fillRect( 30, 30, 50, 35, frameC);
  c.fillRect( 34, 34, 42, 27, pictureC);
  c.fillTriangle(34, 61, 76, 61, 55, 42, pictureSun);

  // ── Window (220, 24, 70 × 60) — sky + sun/moon + weather inside ─────
  const int wx = 220, wy = 24, ww = 70, wh = 60;

  // Vertical-gradient sky tinted by phase.
  uint8_t topR, topG, topB, botR, botG, botB;
  if (isNight) {
    topR =  10; topG =  16; topB =  56;
    botR =  30; botG =  38; botB = 100;
  } else if (isEvening) {
    topR = 110; topG =  60; topB = 100;
    botR = 255; botG = 150; botB =  85;
  } else if (isMorning) {
    topR = 200; topG = 215; topB = 235;
    botR = 255; botG = 200; botB = 200;
  } else {  // Day
    topR = 120; topG = 180; topB = 230;
    botR = 175; botG = 220; botB = 245;
  }
  for (int y = 0; y < wh; ++y) {
    float t = (float)y / (float)(wh - 1);
    uint8_t r = (uint8_t)(topR * (1.0f - t) + botR * t);
    uint8_t g = (uint8_t)(topG * (1.0f - t) + botG * t);
    uint8_t b = (uint8_t)(topB * (1.0f - t) + botB * t);
    c.drawFastHLine(wx, wy + y, ww, c.color565(r, g, b));
  }

  // Sun, evening sun, or moon — chosen by phase.
  if (isNight) {
    // Moon — phase-aware. The shape is the lit portion of a disc; the
    // unlit side is masked with the night sky colour (skyTop). For the
    // night palette here that's roughly (10,16,56), which we approximate
    // with the local sky-top RGB.
    int mx = wx + ww - 18, my = wy + 14;
    int   r = 8;
    uint16_t lit  = c.color565(245, 240, 220);
    uint16_t dark = c.color565( 10,  16,  56);
    // Decode phase255 (0..254). 0xFF sentinel → render as crescent.
    uint8_t p255 = moonPhase255;
    if (p255 == 0xFF) {
      // Fallback: simple crescent like before.
      c.fillCircle(mx, my, r, lit);
      c.fillCircle(mx + 3, my - 1, r - 1, dark);
    } else {
      float p = p255 / 254.0f;     // 0=new, 0.5=full, 1=≈new
      // Full disc of "lit" first, then carve the shadow.
      c.fillCircle(mx, my, r, lit);
      // Shadow shift along x to model the terminator.
      // For phase 0 (new) shadow covers the entire disc → render as dark.
      // For phase 0.5 (full) shadow is gone.
      // Otherwise we draw a circle of "dark" offset horizontally.
      if (p < 0.02f || p > 0.98f) {
        c.fillCircle(mx, my, r, dark);
      } else if (p < 0.5f) {
        // Waxing — illuminated on the right; shadow is offset to the left.
        int dx = (int)((0.5f - p) * 2.0f * (r * 2));     // 0..2r
        c.fillCircle(mx - dx, my, r, dark);
      } else if (p > 0.5f) {
        // Waning — illuminated on the left; shadow offset to the right.
        int dx = (int)((p - 0.5f) * 2.0f * (r * 2));
        c.fillCircle(mx + dx, my, r, dark);
      }
      // Outline so the moon reads as a disc even at phases close to new.
      c.drawCircle(mx, my, r, c.color565(220, 215, 200));
    }
    // Stars (deterministic positions)
    c.drawPixel(wx + 12, wy + 10,  c.color565(240, 240, 220));
    c.drawPixel(wx + 22, wy + 22,  c.color565(240, 240, 220));
    c.drawPixel(wx + 36, wy + 14,  c.color565(240, 240, 220));
    c.drawPixel(wx + 48, wy + 28,  c.color565(240, 240, 220));
  } else if (isEvening) {
    // Setting sun — lower in the window with a deep orange glow.
    int sx = wx + ww / 2, sy = wy + wh - 14;
    c.fillCircle(sx, sy, 11, c.color565(255, 110,  50));
    c.fillCircle(sx, sy,  8, c.color565(255, 170,  80));
    c.fillCircle(sx, sy,  4, c.color565(255, 220, 130));
  } else {
    // Day / morning sun with short rays.
    int sx = wx + ww - 16, sy = wy + 14;
    uint16_t rayC = c.color565(255, 200,  80);
    for (int i = 0; i < 8; ++i) {
      float a = i * (float)PI / 4.0f;
      int ex = sx + (int)(cosf(a) * 12);
      int ey = sy + (int)(sinf(a) * 12);
      c.drawLine(sx, sy, ex, ey, rayC);
    }
    c.fillCircle(sx, sy, 7, c.color565(255, 230,  80));
    c.fillCircle(sx, sy, 4, c.color565(255, 245, 160));
  }

  // Weather inside the window pane. Clipped to the window rect so the
  // cloud's rounded edges don't bleed past the muntins onto the wall.
  c.setClipRect(wx, wy, ww, wh);
  if (weather == Weather::Cloudy) {
    int drift = (int)((now_ms / 70) % (ww + 40)) - 40;
    uint16_t cloudC = isNight ? c.color565(120, 130, 160) :
                                c.color565(245, 245, 250);
    int cx = wx + drift;
    if (cx + 36 > wx && cx < wx + ww + 4) {
      c.fillCircle(cx + 10, wy + 22, 6, cloudC);
      c.fillCircle(cx + 22, wy + 18, 8, cloudC);
      c.fillCircle(cx + 34, wy + 22, 6, cloudC);
    }
  } else if (weather == Weather::Rainy) {
    uint16_t rainC = c.color565(150, 200, 230);
    for (int i = 0; i < 10; ++i) {
      uint32_t r = (uint32_t)(i + 1) * 2654435761u;
      int rx = (int)((r >> 8) % ww);
      int ry = (int)(((r >> 16) % wh + (now_ms / 5) + i * 11) % (wh + 16)) - 8;
      if (ry < 0) continue;
      if (ry >= wh - 2) continue;
      c.drawFastVLine(wx + rx, wy + ry, 3, rainC);
    }
  } else if (weather == Weather::Foggy) {
    uint16_t fogC = c.color565(220, 220, 225);
    int by = (int)((now_ms / 35) % wh);
    c.drawFastHLine(wx, wy + by, ww, fogC);
    int by2 = (by + (int)(wh / 2)) % wh;
    c.drawFastHLine(wx, wy + by2, ww, fogC);
  } else if (weather == Weather::Sandstorm) {
    uint16_t sandC = c.color565(220, 180, 100);
    int drift = (int)((now_ms / 12) % 20);
    for (int y = 0; y < wh; y += 4) {
      int rx = (drift + y * 3) % ww;
      c.drawFastHLine(wx + rx, wy + y, ww - rx, sandC);
    }
  }
  c.clearClipRect();

  // Window frame (cross of muntins) and curtains — drawn last so they sit
  // on top of the sky/weather rendering.
  uint16_t frameLine = c.color565(80, 60, 40);
  c.drawRect(wx, wy, ww, wh, frameLine);
  c.drawFastVLine(wx + ww/2, wy, wh, frameLine);
  c.drawFastHLine(wx, wy + wh/2, ww, frameLine);
  uint16_t curtain = isNight   ? c.color565(140,  40,  60) :
                      isEvening ? c.color565(170,  50,  75) :
                                 c.color565(220,  60,  90);
  c.fillRect(wx - 6, wy, 8, wh, curtain);
  c.fillRect(wx + ww - 2, wy, 8, wh, curtain);

  // Skirting + floor
  c.fillRect(0, 165, 320, 6, skirt);
  c.fillRect(0, 171, 320, 240 - 171, floorMain);
  for (int y = 180; y < 240; y += 18) {
    c.drawFastHLine(0, y, 320, floorPlank);
  }
}

void drawSceneForest(M5Canvas& c, TimePhase phase, uint32_t now_ms,
                     uint8_t moonPhase255 = 0xFF) {
  // Dark green canopy sky + tree trunks + mossy floor.
  uint16_t sky;
  switch (phase) {
    case TimePhase::Morning: sky = c.color565( 90, 140, 105); break;
    case TimePhase::Day:     sky = c.color565( 65, 115,  85); break;
    case TimePhase::Evening: sky = c.color565( 80,  85,  70); break;
    case TimePhase::Night:   sky = c.color565( 25,  40,  35); break;
    default:                 sky = c.color565( 65, 115,  85);
  }
  c.fillSprite(sky);

  uint16_t canopy = c.color565( 35,  85,  55);
  uint16_t canHi  = c.color565( 65, 130,  85);
  uint16_t trunk  = c.color565( 80,  55,  35);
  uint16_t trunkSh = c.color565( 50,  30,  20);
  uint16_t moss   = c.color565( 70, 110,  55);
  uint16_t mossDk = c.color565( 45,  85,  45);

  // Canopy across the top
  for (int x = -20; x < 340; x += 35) {
    int yJit = 30 + ((x * 13) & 7);
    c.fillCircle(x, yJit, 30, canopy);
  }
  for (int x = -10; x < 340; x += 50) {
    c.fillCircle(x, 22, 14, canHi);
  }

  // A few thin tree trunks in the back
  static const int trunkX[5] = { 25, 95, 175, 255, 305 };
  for (int i = 0; i < 5; ++i) {
    int tx = trunkX[i];
    c.fillRect(tx, 60, 8, 130, trunk);
    c.drawFastVLine(tx, 60, 130, trunkSh);
    c.drawFastVLine(tx + 7, 60, 130, trunkSh);
  }

  // Floor
  c.fillRect(0, 188, 320, 52, moss);
  c.drawFastHLine(0, 188, 320, mossDk);
  // Ferns / grass tufts
  for (int x = 0; x < 320; x += 24) {
    int j = (x * 7) & 7;
    c.drawLine(x + 4, 200 + j, x + 6, 192, mossDk);
    c.drawLine(x + 7, 200 + j, x + 8, 193, mossDk);
    c.drawLine(x + 10, 200 + j, x + 11, 192, mossDk);
  }
  (void)now_ms;
}

void drawSceneBeach(M5Canvas& c, TimePhase phase, uint32_t now_ms) {
  uint16_t sky;
  switch (phase) {
    case TimePhase::Morning: sky = c.color565(255, 210, 180); break;
    case TimePhase::Day:     sky = c.color565(160, 215, 245); break;
    case TimePhase::Evening: sky = c.color565(255, 165, 130); break;
    case TimePhase::Night:   sky = c.color565( 35,  45,  85); break;
    default:                 sky = c.color565(160, 215, 245);
  }
  c.fillSprite(sky);
  if (phase == TimePhase::Night) drawNightStars(c, now_ms);
  drawCelestial(c, phase, now_ms);

  // Ocean (mid band)
  uint16_t seaA = c.color565( 60, 145, 200);
  uint16_t seaB = c.color565( 35, 110, 175);
  uint16_t foam = c.color565(220, 235, 245);
  c.fillRect(0, 130, 320, 60, seaA);
  c.drawFastHLine(0, 130, 320, c.color565(20, 80, 130));
  for (int i = 0; i < 6; ++i) {
    int phx = (i * 60 + (now_ms / 90) % 60);
    c.drawFastHLine(phx,      150, 12, foam);
    c.drawFastHLine(phx + 30, 170, 10, foam);
    c.drawFastHLine(phx + 18, 180, 16, seaB);
  }

  // Sand
  uint16_t sand     = c.color565(245, 220, 165);
  uint16_t sandDark = c.color565(210, 180, 130);
  c.fillRect(0, 190, 320, 50, sand);
  c.drawFastHLine(0, 190, 320, foam);    // wet line
  c.drawFastHLine(0, 192, 320, foam);
  // Subtle texture
  for (int x = 6; x < 320; x += 17) {
    c.drawPixel(x, 210 + ((x * 13) & 7), sandDark);
    c.drawPixel(x + 4, 224 + ((x * 7) & 5), sandDark);
  }

  // Palm tree on the left
  uint16_t bark = c.color565(110, 70, 35);
  uint16_t leaf = c.color565( 65, 145, 60);
  uint16_t leafHi = c.color565(115, 195, 90);
  c.fillRect(28, 110, 8, 90, bark);
  for (int i = 0; i < 4; ++i) {
    c.drawFastHLine(26, 120 + i * 18, 12, c.color565(80, 50, 25));
  }
  // Fronds — fan of curved triangles
  c.fillTriangle(32, 110,  -4, 100, 22, 90, leaf);
  c.fillTriangle(32, 110,  72, 105, 38, 88, leaf);
  c.fillTriangle(32, 110, -10, 130, 18, 95, leafHi);
  c.fillTriangle(32, 110,  78, 130, 40, 95, leafHi);
  c.fillCircle (32, 110, 5, c.color565(150, 100, 50));    // coconuts
  c.fillCircle (38, 113, 3, c.color565(80,  50,  25));
}

void drawSceneDesert(M5Canvas& c, TimePhase phase, uint32_t now_ms) {
  uint16_t sky;
  switch (phase) {
    case TimePhase::Morning: sky = c.color565(255, 195, 140); break;
    case TimePhase::Day:     sky = c.color565(255, 180, 110); break;
    case TimePhase::Evening: sky = c.color565(220, 110,  85); break;
    case TimePhase::Night:   sky = c.color565( 60,  50,  85); break;
    default:                 sky = c.color565(255, 180, 110);
  }
  c.fillSprite(sky);
  if (phase == TimePhase::Night) drawNightStars(c, now_ms);

  // Big hot sun (or moon)
  if (phase != TimePhase::Night) {
    uint16_t sun = c.color565(255, 240, 130);
    uint16_t glow = c.color565(255, 200,  90);
    c.fillCircle(255,  60, 32, glow);
    c.fillCircle(255,  60, 26, sun);
  } else {
    drawCelestial(c, phase, now_ms);
  }

  // Heat-haze line
  uint16_t haze = c.color565(255, 220, 170);
  c.drawFastHLine(0, 145, 320, haze);
  c.drawFastHLine(0, 148, 320, haze);

  // Layered dunes
  uint16_t duneFar  = c.color565(245, 195, 130);
  uint16_t duneMid  = c.color565(225, 165, 100);
  uint16_t duneNear = c.color565(200, 140,  85);
  c.fillEllipse( 60, 175, 130, 30, duneFar);
  c.fillEllipse(220, 175, 140, 28, duneFar);
  c.fillEllipse(140, 200, 170, 32, duneMid);
  c.fillEllipse(280, 205, 110, 30, duneMid);
  c.fillRect   (0, 215, 320,  25, duneNear);

  // Cactus on the right
  uint16_t cact  = c.color565( 70, 130,  70);
  uint16_t cactSh= c.color565( 45,  95,  50);
  int cx = 285, cy = 195;
  c.fillRoundRect(cx - 6, cy - 50, 12, 60, 4, cact);
  c.fillRoundRect(cx + 6, cy - 30, 8, 22, 3, cact);
  c.fillRoundRect(cx + 6, cy - 30, 14, 4, 2, cact);
  c.fillRoundRect(cx - 14, cy - 38, 8, 18, 3, cact);
  c.fillRoundRect(cx - 14, cy - 38, 14, 4, 2, cact);
  c.drawFastVLine(cx - 6, cy - 50, 60, cactSh);
  c.drawFastVLine(cx + 5, cy - 50, 60, cactSh);
  // Spines
  for (int s = 0; s < 6; ++s) {
    int sy = cy - 45 + s * 9;
    c.drawPixel(cx - 7, sy, cactSh);
    c.drawPixel(cx + 6, sy, cactSh);
  }
  (void)now_ms;
}

void drawSceneSpace(M5Canvas& c, TimePhase /*phase*/, uint32_t now_ms) {
  // Deep space: black + dense starfield + a couple of distant planets.
  c.fillSprite(c.color565(8, 5, 22));

  // Star field — denser than the night-stars overlay.
  for (int i = 0; i < 90; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8) % 320;
    int sy = (r >> 18) % 200;
    uint8_t bri = 140 + ((r >> 24) & 0x7F);
    bool twinkle = (((now_ms / 200) + i) & 7) == 0;
    if (twinkle) bri = 255;
    c.drawPixel(sx, sy, c.color565(bri, bri, bri));
    if ((r & 0x1F) == 0) {
      c.fillCircle(sx, sy, 1, c.color565(255, 255, 255));
    }
  }

  // Distant nebula tint (large soft circles)
  c.fillCircle( 60,  50, 36, c.color565(40, 25, 70));
  c.fillCircle( 60,  50, 22, c.color565(70, 35, 95));
  c.fillCircle(260,  90, 30, c.color565(60, 30, 75));

  // A planet on the right with rings
  uint16_t planet  = c.color565(200, 130,  80);
  uint16_t band    = c.color565(150,  90,  55);
  uint16_t shadow  = c.color565( 60,  35,  20);
  c.fillCircle(265, 175, 22, planet);
  c.fillEllipse(266, 178, 8, 4, band);
  c.fillEllipse(264, 173, 7, 3, band);
  c.drawEllipse(265, 175, 36, 10, c.color565(220, 180, 130));
  c.drawEllipse(265, 175, 35,  9, c.color565(180, 130,  90));
  // Crescent shadow
  c.fillCircle(272, 170, 19, shadow);
  c.fillCircle(265, 175, 22, planet);
  c.fillEllipse(266, 178, 8, 4, band);

  // Small moon on the left
  c.fillCircle(38, 195, 11, c.color565(220, 220, 230));
  c.fillCircle(34, 192, 2, c.color565(170, 170, 185));
  c.fillCircle(42, 198, 1, c.color565(170, 170, 185));
}

void drawSceneCity(M5Canvas& c, TimePhase phase, uint32_t now_ms,
                   uint8_t moonPhase255 = 0xFF) {
  uint16_t sky;
  switch (phase) {
    case TimePhase::Morning: sky = c.color565(255, 195, 175); break;
    case TimePhase::Day:     sky = c.color565(170, 200, 230); break;
    case TimePhase::Evening: sky = c.color565(255, 145, 110); break;
    case TimePhase::Night:   sky = c.color565( 25,  30,  60); break;
    default:                 sky = c.color565(170, 200, 230);
  }
  c.fillSprite(sky);
  if (phase == TimePhase::Night) drawNightStars(c, now_ms);
  drawCelestial(c, phase, now_ms, moonPhase255);

  // Distant skyline — thin pale buildings (further/smaller).
  uint16_t farTone = (phase == TimePhase::Night)
                       ? c.color565( 50,  55,  90)
                       : c.color565(150, 165, 195);
  static const struct { int16_t x, w, h; } far_b[] = {
    {  10, 18,  60 }, {  32, 22,  80 }, {  60, 16,  50 }, {  80, 24,  90 },
    { 110, 18,  70 }, { 132, 26, 100 }, { 162, 14,  55 }, { 180, 28,  85 },
    { 212, 20,  72 }, { 236, 22,  92 }, { 262, 16,  62 }, { 282, 26, 105 },
  };
  for (auto b : far_b) {
    c.fillRect(b.x, 175 - b.h, b.w, b.h, farTone);
  }

  // Mid-ground skyline — taller, darker. Skyscrapers with window grids.
  uint16_t midTone   = (phase == TimePhase::Night)
                         ? c.color565( 30,  35,  55)
                         : c.color565( 90,  90, 110);
  uint16_t midDetail = (phase == TimePhase::Night)
                         ? c.color565( 22,  25,  40)
                         : c.color565( 70,  70,  90);
  uint16_t winLit    = c.color565(255, 220,  90);  // night windows
  uint16_t winDark   = c.color565(180, 200, 220);  // day windows

  // 5 skyscrapers across, varied widths/heights
  static const struct { int16_t x, w, h; uint8_t cols; uint8_t rows; } mid_b[] = {
    {   0,  44, 130, 3, 9 },
    {  44,  56, 165, 4, 12 },
    { 100,  68, 190, 4, 14 },   // tallest, center
    { 168,  52, 150, 3, 11 },
    { 220,  48, 120, 3, 8 },
    { 268,  52, 175, 3, 13 },
  };
  for (auto b : mid_b) {
    int top = 195 - b.h;
    c.fillRect(b.x, top, b.w, b.h, midTone);
    c.drawRect(b.x, top, b.w, b.h, midDetail);
    // Roof detail — small box on top
    c.fillRect(b.x + b.w/2 - 3, top - 5, 6, 5, midDetail);
    c.drawFastVLine(b.x + b.w/2, top - 9, 4, midDetail);

    // Window grid
    int gapX = (b.w - 6) / b.cols;
    int gapY = (b.h - 10) / b.rows;
    for (int cx = 0; cx < b.cols; ++cx) {
      for (int rrow = 0; rrow < b.rows; ++rrow) {
        int wx = b.x + 4 + cx * gapX;
        int wy = top + 6 + rrow * gapY;
        // Pseudo-random "is this window lit" based on cell index + slow time
        uint32_t r = (uint32_t)(b.x ^ (cx * 17) ^ (rrow * 91));
        bool lit;
        if (phase == TimePhase::Night) {
          // Most windows lit at night, with slow flicker on a few
          uint32_t flick = (r ^ (now_ms / 800)) & 0xF;
          lit = (r & 3) != 0 && flick != 0;
        } else {
          // A few highlights in daytime
          lit = (r & 7) == 0;
        }
        uint16_t col = (phase == TimePhase::Night && lit) ? winLit : winDark;
        if (phase != TimePhase::Night && !lit) col = midDetail;
        c.fillRect(wx, wy, gapX - 3, gapY - 3, col);
      }
    }
  }

  // Sidewalk / street
  uint16_t sidewalk = (phase == TimePhase::Night)
                        ? c.color565( 50,  55,  70)
                        : c.color565(170, 170, 180);
  uint16_t road     = (phase == TimePhase::Night)
                        ? c.color565( 25,  25,  35)
                        : c.color565( 80,  80,  90);
  c.fillRect(0, 195, 320,  8, sidewalk);
  c.fillRect(0, 203, 320, 37, road);
  c.drawFastHLine(0, 195, 320, midDetail);
  // Center yellow road markings
  uint16_t marking = c.color565(230, 200,  60);
  for (int x = 10; x < 320; x += 36) {
    c.fillRect(x, 220, 18, 3, marking);
  }
}

void drawSceneBackground(M5Canvas& c, Scene s, TimePhase phase, uint32_t now_ms,
                         Weather weather = Weather::Sunny,
                         uint8_t moonPhase255 = 0xFF) {
  switch (s) {
    case Scene::Meadow:  drawSceneMeadow (c, phase, now_ms, moonPhase255); break;
    case Scene::Bedroom: drawSceneBedroom(c, phase, now_ms, weather,
                                          moonPhase255); break;
    case Scene::Forest:  drawSceneForest (c, phase, now_ms, moonPhase255); break;
    case Scene::Beach:   drawSceneBeach  (c, phase, now_ms); break;
    case Scene::Desert:  drawSceneDesert (c, phase, now_ms); break;
    case Scene::Space:   drawSceneSpace  (c, phase, now_ms); break;
    case Scene::City:    drawSceneCity   (c, phase, now_ms, moonPhase255); break;
  }
}

// ─── Weather overlays ───────────────────────────────────────────────────────

void drawWeatherCloudy(M5Canvas& c, uint32_t now_ms) {
  // A few extra opaque clouds drifting on top of whatever the scene drew.
  uint16_t cl  = c.color565(230, 230, 240);
  uint16_t shd = c.color565(180, 180, 200);
  int drift = (now_ms / 60) % 360;
  static const struct { int16_t y; int8_t scale; } extras[3] = {
    { 35, 18 }, { 60, 24 }, { 25, 14 },
  };
  for (int i = 0; i < 3; ++i) {
    int x = ((i * 130 + drift) % 400) - 40;
    int y = extras[i].y;
    int s = extras[i].scale;
    c.fillEllipse(x,        y, s,     s/2 + 4, cl);
    c.fillEllipse(x + s,    y - 3, s - 4, s/2 + 2, cl);
    c.fillEllipse(x - s/2,  y + 2, s - 6, s/2 + 1, shd);
  }
}

void drawWeatherRainy(M5Canvas& c, uint32_t now_ms) {
  // Continuous diagonal rain streaks across the upper screen.
  uint16_t rain = c.color565(180, 200, 230);
  uint16_t hi   = c.color565(220, 235, 250);
  int t = (int)(now_ms / 40);
  for (int i = 0; i < 36; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int xBase = (int)(r >> 8) % 320;
    int yBase = (int)(r >> 18) % 200;
    int phase = (t + (int)(r >> 20)) % 80;
    int x = xBase - phase / 2;
    int y = yBase + phase * 2;
    if (y > 230) y -= 230;
    c.drawLine(x, y, x - 2, y + 5, rain);
    c.drawLine(x + 1, y, x - 1, y + 5, hi);
  }
  // Subtle puddle highlights on the ground
  uint16_t puddle = c.color565(120, 150, 200);
  for (int i = 0; i < 5; ++i) {
    int px = (i * 71 + (int)(now_ms / 1000) * 17) % 320;
    c.drawFastHLine(px, 232, 6, puddle);
  }
}

void drawWeatherFoggy(M5Canvas& c, uint32_t now_ms) {
  // Horizontal grey bands at varying heights — fakes haze without alpha.
  uint16_t fog  = c.color565(200, 200, 210);
  uint16_t fog2 = c.color565(180, 180, 195);
  int drift = (int)(now_ms / 30);
  for (int band = 0; band < 8; ++band) {
    int y = 30 + band * 22;
    int xOff = (band * 41 + drift) % 60;
    for (int x = 0; x < 320; x += 8) {
      int sx = x + (xOff & 7);
      uint16_t col = (band & 1) ? fog2 : fog;
      c.drawFastHLine(sx, y, 4, col);
      c.drawFastHLine(sx, y + 1, 3, col);
    }
  }
}

void drawWeatherSandstorm(M5Canvas& c, uint32_t now_ms) {
  // Tan particles streaking left → right with a yellow haze tint.
  uint16_t haze    = c.color565(220, 180, 130);
  uint16_t hazeHi  = c.color565(255, 220, 180);
  int t = (int)(now_ms / 25);
  for (int i = 0; i < 60; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int yBase = (int)(r >> 8) % 220;
    int speed = 6 + ((r >> 16) & 7);
    int x = ((int)(r >> 18) + t * speed) % 360 - 20;
    uint16_t col = ((r >> 24) & 1) ? hazeHi : haze;
    c.drawFastHLine(x, yBase, 6 + (int)((r >> 26) & 3), col);
  }
  // Translucent overall yellow tint via sparse pixel pattern
  for (int y = 0; y < 240; y += 4) {
    for (int x = (y / 2) & 3; x < 320; x += 7) {
      c.drawPixel(x, y, haze);
    }
  }
}

void drawWeatherOverlay(M5Canvas& c, Weather w, uint32_t now_ms) {
  switch (w) {
    case Weather::Sunny:     break;
    case Weather::Cloudy:    drawWeatherCloudy   (c, now_ms); break;
    case Weather::Rainy:     drawWeatherRainy    (c, now_ms); break;
    case Weather::Foggy:     drawWeatherFoggy    (c, now_ms); break;
    case Weather::Sandstorm: drawWeatherSandstorm(c, now_ms); break;
  }
}

// ─── Tiny icons (used by both bar slots and floating icons) ─────────────────

void drawIconHeart(M5Canvas& c, int cx, int cy, int sz, uint16_t col) {
  c.fillCircle(cx - sz/2, cy - sz/4, sz/2, col);
  c.fillCircle(cx + sz/2, cy - sz/4, sz/2, col);
  c.fillTriangle(cx - sz, cy - sz/4, cx + sz, cy - sz/4, cx, cy + sz, col);
}

void drawIconApple(M5Canvas& c, int cx, int cy, int sz, uint16_t colBody) {
  c.fillCircle(cx, cy + 1, sz / 2, colBody);
  c.fillRect(cx - 1, cy - sz/2, 2, sz/3, c.color565(120, 80, 40));
  c.fillTriangle(cx, cy - sz/3, cx + sz/2, cy - sz/2, cx + sz/2, cy - sz/4,
                 c.color565(80, 160, 80));
}

void drawIconZ(M5Canvas& c, int cx, int cy, int /*sz*/, uint16_t col) {
  c.setTextDatum(middle_center);
  c.setTextSize(2);
  c.setTextColor(col);
  c.drawString("Z", cx, cy);
  c.setTextDatum(top_left);
}

// Note glyph — small eighth note. Uses a filled notehead and a vertical
// stem; on larger sz a hint of a flag is added too. Spawned during the
// web radio.
void drawIconNote(M5Canvas& c, int cx, int cy, int sz, uint16_t col) {
  int r = (sz < 2) ? 2 : sz - 1;
  // Head — slightly slanted ellipse feels "musical".
  c.fillEllipse(cx - r, cy + r, r + 1, r, col);
  // Stem going up-right from the head.
  c.drawFastVLine(cx, cy + r - 1, -((sz + 4)), col);
  // On larger floats: flag to the right of the stem.
  if (sz >= 4) {
    c.drawLine(cx + 1, cy - sz - 2, cx + 4, cy - sz + 1, col);
    c.drawLine(cx + 1, cy - sz, cx + 4, cy - sz + 3, col);
  }
}

// ─── Need bars ──────────────────────────────────────────────────────────────

uint16_t needFillColor(M5Canvas& c, int slot, uint8_t value, uint32_t now,
                       uint32_t lastChangeMs) {
  // Color per slot: 0=happiness pink, 1=energy yellow, 2=fullness green→red
  uint16_t base;
  switch (slot) {
    case 0: base = c.color565(240, 90, 120); break;
    case 1: base = c.color565(255, 200,  60); break;
    case 2: {
      if (value < 30) base = c.color565(220,  90,  90);
      else            base = c.color565(110, 200,  90);
      break;
    }
    default: base = c.color565(180, 180, 180); break;
  }
  // Critical pulsing: when value<25, fade between bright and dim.
  if (value < 25) {
    float p = 0.5f + 0.5f * sinf((now % 1000) / 1000.0f * 2.0f * (float)PI);
    uint8_t r = ((base >> 11) & 0x1F) << 3;
    uint8_t g = ((base >>  5) & 0x3F) << 2;
    uint8_t b = ( base        & 0x1F) << 3;
    r = (uint8_t)(r * (0.55f + 0.45f * p));
    g = (uint8_t)(g * (0.55f + 0.45f * p));
    b = (uint8_t)(b * (0.55f + 0.45f * p));
    base = c.color565(r, g, b);
  }
  (void)lastChangeMs;
  return base;
}

void drawNeedSlot(M5Canvas& c, int slotX, int slotIdx, uint8_t value,
                  uint32_t now, uint32_t lastChangeMs) {
  uint16_t glyph  = c.color565(60, 50, 50);
  uint16_t bg     = c.color565(220, 200, 200);
  uint16_t fill   = needFillColor(c, slotIdx, value, now, lastChangeMs);

  int barX = slotX + kNeedIconW + 2;
  int barY = kNeedBarY;
  int barW = kNeedBarW;
  int barH = kNeedBarH;

  // Icon (left of bar)
  int icx = slotX + kNeedIconW / 2;
  int icy = barY + barH / 2;
  switch (slotIdx) {
    case 0: drawIconHeart      (c, icx, icy, 4, fill); break;
    case 1: { c.setTextDatum(middle_center); c.setTextSize(1);
              c.setTextColor(fill); c.drawString("Z", icx, icy);
              c.setTextDatum(top_left); break; }
    case 2: drawIconApple      (c, icx, icy, 7, fill); break;
  }

  // Bar background
  c.fillRoundRect(barX, barY, barW, barH, barH/2, bg);

  // Filled portion
  int fillW = (uint32_t)value * barW / 100;
  if (fillW > 0) {
    c.fillRoundRect(barX, barY, fillW, barH, barH/2, fill);
  }

  // Border (default subtle, thicker/white when recently changed)
  bool recently = (lastChangeMs != 0 && now - lastChangeMs < 250);
  uint16_t border = recently ? c.color565(255, 255, 255) : glyph;
  c.drawRoundRect(barX, barY, barW, barH, barH/2, border);
}

void drawNeedBars(M5Canvas& c, const PetView& v) {
  drawNeedSlot(c, kHappinessSlotX, 0, v.needs.happiness, v.now_ms, v.happLastChangeMs);
  drawNeedSlot(c, kEnergySlotX,    1, v.needs.energy,    v.now_ms, v.engLastChangeMs);
  drawNeedSlot(c, kFullnessSlotX,  2, v.needs.fullness,  v.now_ms, v.fullLastChangeMs);
}

// ─── Floating icons ─────────────────────────────────────────────────────────

void drawFloatingIcons(M5Canvas& c, const FloatPool* p, uint32_t now) {
  if (!p) return;
  for (int i = 0; i < FloatPool::N; ++i) {
    const FloatingIcon& f = p->items[i];
    if (f.type == FloatType::None) continue;
    uint32_t age = now - f.startMs;
    if (age >= f.durMs) continue;

    float t = (float)age / (float)f.durMs;

    if (f.type == FloatType::Ball) {
      // Linear trajectory, no shrinking. Big-bouncing ball that crosses
      // the screen in the foreground (drawn after the pet but before the
      // top-strip UI).
      int bx = f.fromX + (int)((f.toX - f.fromX) * t);
      int by = f.fromY + (int)((f.toY - f.fromY) * t);
      float bouncePhase = t * 3.0f * (float)PI;
      int yOff = (int)(28.0f * fabsf(sinf(bouncePhase)));

      uint16_t col;
      switch ((f.startMs / 100) % 3) {
        case 0:  col = c.color565(225,  85,  85); break;   // red
        case 1:  col = c.color565( 90, 145, 230); break;   // blue
        default: col = c.color565(120, 200, 100); break;   // green
      }
      const int r = 15;
      int cx = bx, cy = by - yOff;
      c.fillCircle(cx, cy, r, col);
      // Crisp outline for definition over the busy pet background.
      c.drawCircle(cx, cy, r, c.color565(35, 25, 25));
      // Big upper-left highlight + tiny secondary glint = "glossy" look
      c.fillCircle(cx - 5, cy - 5, 5, c.color565(255, 255, 255));
      c.fillCircle(cx + 4, cy + 4, 2, c.color565(230, 230, 235));
      continue;
    }

    // Other float types: ease-out + shrink toward the target bar
    float te = 1.0f - (1.0f - t) * (1.0f - t);
    int x = f.fromX + (int)((f.toX - f.fromX) * te);
    int y = f.fromY + (int)((f.toY - f.fromY) * te);
    int sz = max(2, 7 - (int)(t * 4));

    switch (f.type) {
      case FloatType::Heart:
        drawIconHeart(c, x, y, sz, c.color565(240, 90, 120));
        break;
      case FloatType::Apple:
        drawIconApple(c, x, y, sz, c.color565(220, 80, 80));
        break;
      case FloatType::Zzz:
        drawIconZ(c, x, y, sz, c.color565(80, 110, 200));
        break;
      case FloatType::Note:
        drawIconNote(c, x, y, sz, c.color565(180, 120, 255));
        break;
      default: break;
    }
  }
}

// ─── Gear button ────────────────────────────────────────────────────────────

void drawGear(M5Canvas& c, int cx, int cy, int outerR, uint16_t col) {
  // 8 small teeth — short rectangles at angles
  for (int i = 0; i < 8; ++i) {
    float a = i * (PI / 4.0f);
    int tx = cx + (int)(cosf(a) * (outerR - 1));
    int ty = cy + (int)(sinf(a) * (outerR - 1));
    int tx2 = cx + (int)(cosf(a) * (outerR + 3));
    int ty2 = cy + (int)(sinf(a) * (outerR + 3));
    c.drawLine(tx, ty, tx2, ty2, col);
    // Thicker by drawing offset twice
    c.drawLine(tx + 1, ty, tx2 + 1, ty2, col);
    c.drawLine(tx, ty + 1, tx2, ty2 + 1, col);
  }
  c.fillCircle(cx, cy, outerR, col);
  c.fillCircle(cx, cy, outerR - 4, c.color565(255, 240, 240));
}

void drawGearButton(M5Canvas& c) {
  const Rect& r = kGearButtonRect;
  uint16_t glyph = c.color565(60, 50, 50);
  uint16_t panel = c.color565(255, 240, 240);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
  drawGear(c, r.x + r.w/2, r.y + r.h/2, 9, glyph);
}

// ─── Pet building blocks (unchanged from previous version) ──────────────────

// Listen-mode-aware "ear pulse" multiplier — returns the additional growth
// amount in pixels for an ear dimension. 0 when not in listen mode.
static int listenEarPulse(bool listenMode, int micLevel, uint32_t now_ms,
                          int amplitude) {
  if (!listenMode) return 0;
  float base = (sinf((now_ms % 600) / 600.0f * 2.0f * (float)PI) + 1.0f) * 0.5f;
  float micI = (float)micLevel / 12000.0f;
  if (micI > 1.0f) micI = 1.0f;
  return (int)((amplitude * 0.4f) * base + (amplitude * 0.6f) * micI);
}

static void drawEarsRound(M5Canvas& c, int hx, int hy, int leftEarDx,
                          bool listenMode, int micLevel, uint32_t now_ms) {
  int lex = hx - 70 + leftEarDx;
  int rex = hx + 70;
  int ey  = hy - HEAD_RY + 6;
  int extra = listenEarPulse(listenMode, micLevel, now_ms, 6);
  int earR = 15 + extra;
  int innerR = 7 + extra / 2;
  c.fillCircle(lex, ey, earR, COL_BODY(c));
  c.fillCircle(rex, ey, earR, COL_BODY(c));
  c.fillCircle(lex, ey + 2, innerR, g_renderStyle.innerEar);
  c.fillCircle(rex, ey + 2, innerR, g_renderStyle.innerEar);
}

static void drawEarsTriangle(M5Canvas& c, int hx, int hy, int leftEarDx,
                             bool listenMode, int micLevel, uint32_t now_ms) {
  int lex = hx - 60 + leftEarDx;
  int rex = hx + 60;
  int baseY = hy - HEAD_RY + 8;
  int apexY = hy - HEAD_RY - 18 - listenEarPulse(listenMode, micLevel, now_ms, 8);
  // Outer triangles
  c.fillTriangle(lex - 14, baseY, lex + 14, baseY, lex - 4, apexY, COL_BODY(c));
  c.fillTriangle(rex - 14, baseY, rex + 14, baseY, rex + 4, apexY, COL_BODY(c));
  // Inner pink triangles
  c.fillTriangle(lex - 8, baseY - 1, lex + 8, baseY - 1, lex - 2, apexY + 8,
                 g_renderStyle.innerEar);
  c.fillTriangle(rex - 8, baseY - 1, rex + 8, baseY - 1, rex + 2, apexY + 8,
                 g_renderStyle.innerEar);
}

static void drawEarsFloppy(M5Canvas& c, int hx, int hy, int leftEarDx,
                           bool listenMode, int micLevel, uint32_t now_ms) {
  int lex = hx - 86 + leftEarDx;
  int rex = hx + 86;
  int ey  = hy - HEAD_RY + 32;
  int extraH = listenEarPulse(listenMode, micLevel, now_ms, 6);
  int earH = 26 + extraH;
  int earW = 11;
  c.fillEllipse(lex, ey + extraH/2, earW, earH, COL_BODY_SHADE(c));
  c.fillEllipse(rex, ey + extraH/2, earW, earH, COL_BODY_SHADE(c));
  // Slightly lighter inner band
  c.fillEllipse(lex - 1, ey + 2 + extraH/2, earW - 4, earH - 8,
                g_renderStyle.innerEar);
  c.fillEllipse(rex + 1, ey + 2 + extraH/2, earW - 4, earH - 8,
                g_renderStyle.innerEar);
}

static void drawWhiskers(M5Canvas& c, int hx, int hy) {
  uint16_t col = c.color565(80, 70, 60);
  int wy = hy + 22;
  // Left side, three whiskers fanning out
  c.drawLine(hx - 22, wy - 4, hx - 38, wy - 8, col);
  c.drawLine(hx - 22, wy,     hx - 40, wy,     col);
  c.drawLine(hx - 22, wy + 4, hx - 38, wy + 8, col);
  // Right side
  c.drawLine(hx + 22, wy - 4, hx + 38, wy - 8, col);
  c.drawLine(hx + 22, wy,     hx + 40, wy,     col);
  c.drawLine(hx + 22, wy + 4, hx + 38, wy + 8, col);
}

void drawHead(M5Canvas& c, int hx, int hy, int breath,
              MicroAnim micro, uint32_t microAge,
              bool listenMode, int micLevel, uint32_t now_ms,
              AnimalType animal) {
  // Body
  c.fillEllipse(hx, hy, HEAD_RX, HEAD_RY + breath, COL_BODY(c));

  // Highlight on the upper forehead
  c.fillEllipse(hx, hy - HEAD_RY + 14 - breath, HEAD_RX - 40, 9,
                g_renderStyle.bodyHighlight);

  // Ear-twitch micro-anim shifts the left ear horizontally
  int leftEarDx = 0;
  if (micro == MicroAnim::EarTwitch && microAge < 250) {
    float t = (float)microAge / 250.0f;
    leftEarDx = (int)(sinf(t * 3.0f * (float)PI) * 3.0f);
  }

  // Dispatch ear style by animal
  switch (animal) {
    case AnimalType::Cat:
      drawEarsTriangle(c, hx, hy, leftEarDx, listenMode, micLevel, now_ms);
      break;
    case AnimalType::Dog:
      drawEarsFloppy  (c, hx, hy, leftEarDx, listenMode, micLevel, now_ms);
      break;
    case AnimalType::Bear:
    default:
      drawEarsRound   (c, hx, hy, leftEarDx, listenMode, micLevel, now_ms);
  }

  // Cream muzzle — common to all three.
  c.fillEllipse(hx, hy + 24, 42, 22, COL_MUZZLE(c));

  // Nose: cat gets a small upturned pink triangle, dog/bear a chunkier dark one
  int nx = hx;
  if (animal == AnimalType::Cat) {
    int ny = hy + 12;
    c.fillTriangle(nx - 4, ny - 2, nx + 4, ny - 2, nx, ny + 4, COL_NOSE(c));
    c.fillCircle(nx - 1, ny - 1, 1, c.color565(255, 220, 230));
  } else {
    int ny = hy + 11;
    c.fillTriangle(nx - 6, ny - 2, nx + 6, ny - 2, nx, ny + 5, COL_NOSE(c));
    c.fillEllipse(nx, ny - 1, 6, 3, COL_NOSE(c));
    c.fillCircle(nx - 2, ny - 2, 1, c.color565(220, 200, 200));
  }

  // Cat-only whiskers
  if (animal == AnimalType::Cat) drawWhiskers(c, hx, hy);
}

void drawBlush(M5Canvas& c, int hx, int hy, int strength = 16) {
  c.fillEllipse(hx - 60, hy + 14, strength, 9, COL_BLUSH(c));
  c.fillEllipse(hx + 60, hy + 14, strength, 9, COL_BLUSH(c));
}

// Soft baseline blush — subtle pink, drawn for every face so the pet
// always looks slightly rosy. Stronger blush from drawHappy/Excited/Love
// overdraws it for a "gets really excited" effect.
void drawSoftBlush(M5Canvas& c, int hx, int hy) {
  uint16_t col = c.color565(255, 195, 210);
  c.fillEllipse(hx - 60, hy + 14, 9, 6, col);
  c.fillEllipse(hx + 60, hy + 14, 9, 6, col);
}

void drawEyeOpen(M5Canvas& c, int cx, int cy, int gx, int gy) {
  // Bigger sclera + slightly oval pupil + two highlights = kawaii eye.
  c.fillEllipse(cx, cy, 15, 13, COL_GLINT(c));
  int px = cx + gx, py = cy + gy;
  c.fillEllipse(px, py, 6, 7, COL_DARK(c));        // taller-than-wide pupil
  c.fillCircle(px - 2, py - 3, 3, COL_GLINT(c));   // big upper-left glint
  c.fillCircle(px + 2, py + 2, 1, COL_GLINT(c));   // tiny accent glint
}
void drawEyeBlink(M5Canvas& c, int cx, int cy) {
  c.fillRoundRect(cx - 13, cy - 2, 26, 4, 2, COL_DARK(c));
}
void drawEyeHappy(M5Canvas& c, int cx, int cy) {
  c.fillEllipse(cx, cy, 16, 11, COL_DARK(c));
  c.fillEllipse(cx, cy + 6, 18, 11, COL_BODY(c));
}
void drawEyeSleepy(M5Canvas& c, int cx, int cy) {
  c.fillEllipse(cx, cy + 2, 13, 6, COL_DARK(c));
  c.fillEllipse(cx, cy - 2, 15, 6, COL_BODY(c));
}
void drawEyeWide(M5Canvas& c, int cx, int cy, int gx, int gy) {
  c.fillCircle(cx, cy, 17, COL_GLINT(c));
  c.drawCircle(cx, cy, 17, COL_DARK(c));
  c.fillEllipse(cx + gx, cy + gy, 7, 9, COL_DARK(c));
  c.fillCircle(cx + gx - 3, cy + gy - 3, 3, COL_GLINT(c));
  c.fillCircle(cx + gx + 3, cy + gy + 2, 1, COL_GLINT(c));
}
void drawEyeStartled(M5Canvas& c, int cx, int cy, int gx, int gy) {
  c.fillCircle(cx, cy, 18, COL_GLINT(c));
  c.drawCircle(cx, cy, 18, COL_DARK(c));
  c.fillEllipse(cx + gx, cy + gy, 4, 6, COL_DARK(c));
  c.fillCircle(cx + gx - 1, cy + gy - 1, 1, COL_GLINT(c));
}
void drawEyeHeart(M5Canvas& c, int cx, int cy) {
  uint16_t pink = COL_HEART(c);
  c.fillCircle(cx - 6, cy - 4, 7, pink);
  c.fillCircle(cx + 6, cy - 4, 7, pink);
  c.fillTriangle(cx - 13, cy - 1, cx + 13, cy - 1, cx, cy + 12, pink);
  c.fillCircle(cx - 7, cy - 6, 2, COL_GLINT(c));
}
void drawEyeSad(M5Canvas& c, int cx, int cy, int gx, int gy) {
  c.fillEllipse(cx, cy + 2, 13, 9, COL_GLINT(c));
  c.fillEllipse(cx + gx, cy + 4 + gy, 5, 6, COL_DARK(c));
  c.fillCircle(cx + gx - 1, cy + 3 + gy, 1, COL_GLINT(c));
  c.fillTriangle(cx - 14, cy - 8, cx + 14, cy - 4, cx + 14, cy + 2, COL_BODY(c));
}
void drawEyeSquint(M5Canvas& c, int cx, int cy) {
  c.fillTriangle(cx - 12, cy + 2, cx + 12, cy + 2, cx, cy - 4, COL_DARK(c));
  c.fillTriangle(cx - 11, cy + 3, cx + 11, cy + 3, cx, cy - 1, COL_BODY(c));
}

void drawMouthSmile(M5Canvas& c, int cx, int cy, int radius = 18) {
  c.fillEllipse(cx, cy, radius, radius/2 + 2, COL_DARK(c));
  // Mouth sits on the cream muzzle — erase with muzzle colour.
  c.fillEllipse(cx, cy - 4, radius + 1, radius/2 + 1, COL_MUZZLE(c));
}
void drawMouthFlat(M5Canvas& c, int cx, int cy, int width = 10) {
  c.fillRoundRect(cx - width, cy + 4, 2*width, 4, 2, COL_DARK(c));
}
void drawMouthO(M5Canvas& c, int cx, int cy, int radius = 8) {
  c.fillCircle(cx, cy + 2, radius, COL_DARK(c));
}
void drawMouthFrown(M5Canvas& c, int cx, int cy, int radius = 14) {
  c.fillEllipse(cx, cy + 8, radius, radius/2 + 2, COL_DARK(c));
  c.fillEllipse(cx, cy + 12, radius + 1, radius/2 + 1, COL_MUZZLE(c));
}

void drawZzz(M5Canvas& c, int hx, int hy, uint32_t now_ms) {
  int baseX = hx + HEAD_RX - 6;
  int baseY = hy - HEAD_RY - 6;
  c.setTextColor(COL_DARK(c));
  for (int i = 0; i < 3; ++i) {
    int phase = ((now_ms / 50) + i * 30) % 90;
    int x = baseX + i * 14 + phase / 6;
    int y = baseY - phase;
    c.setTextSize(2 + i);
    c.setCursor(x, y);
    c.print('Z');
  }
}

void drawTear(M5Canvas& c, int hx, int hy, uint32_t now_ms) {
  int phase = (now_ms / 30) % 50;
  int x = hx - EYE_DX - 10;
  int y = hy + EYE_OFF_Y + 8 + phase;
  c.fillCircle(x, y, 4, COL_TEAR(c));
  c.fillTriangle(x - 4, y, x + 4, y, x, y - 8, COL_TEAR(c));
}

void drawSparkle(M5Canvas& c, int x, int y, int size, uint16_t col) {
  c.fillTriangle(x, y - size, x - 2, y, x + 2, y, col);
  c.fillTriangle(x, y + size, x - 2, y, x + 2, y, col);
  c.fillTriangle(x - size, y, x, y - 2, x, y + 2, col);
  c.fillTriangle(x + size, y, x, y - 2, x, y + 2, col);
}

void drawSneezePuff(M5Canvas& c, int hx, int hy, uint32_t age) {
  if (age > 500) return;
  float t = (float)age / 500.0f;
  int spread = (int)(t * 14);
  int alpha_phase = (int)(t * 5);
  int radius = max(0, 5 - alpha_phase);
  uint16_t cloud = c.color565(220, 220, 230);
  c.fillCircle(hx - 6 - spread, hy + MOUTH_OFF_Y + 22, radius,     cloud);
  c.fillCircle(hx + 6 + spread, hy + MOUTH_OFF_Y + 22, radius,     cloud);
  c.fillCircle(hx,              hy + MOUTH_OFF_Y + 26, radius - 1, cloud);
}

// ─── Thought bubble (unchanged) ─────────────────────────────────────────────

void drawBubble(M5Canvas& c, const PetView& v, int hx, int hy) {
  if (v.bubble == Bubble::None) return;
  uint32_t age = v.now_ms - v.bubbleStartMs;
  if (age < 50 || v.now_ms + 100 > v.bubbleUntilMs) return;

  int bw = 44, bh = 36;
  int bx = hx + HEAD_RX - 30;
  int by = hy - HEAD_RY - 6;

  uint16_t fill = c.color565(255, 255, 255);
  uint16_t border = c.color565(60, 50, 50);

  c.fillRoundRect(bx, by, bw, bh, 12, fill);
  c.drawRoundRect(bx, by, bw, bh, 12, border);
  c.fillTriangle(bx + 6, by + bh - 1, bx, by + bh + 8, bx + 18, by + bh - 1, fill);
  c.drawLine(bx + 6, by + bh,    bx, by + bh + 8, border);
  c.drawLine(bx + 18, by + bh,   bx, by + bh + 8, border);

  int icx = bx + bw / 2, icy = by + bh / 2;
  switch (v.bubble) {
    case Bubble::Hungry:
      c.fillCircle(icx, icy + 2, 8, c.color565(220, 80, 80));
      c.fillRect(icx - 1, icy - 8, 2, 5, c.color565(120, 80, 40));
      c.fillTriangle(icx - 1, icy - 4, icx + 6, icy - 9, icx + 6, icy - 4,
                     c.color565(80, 160, 80));
      break;
    case Bubble::Sleepy:
      c.setTextColor(c.color565(40, 40, 100));
      c.setTextSize(2);
      c.setTextDatum(middle_center);
      c.drawString("Z", icx, icy);
      c.setTextDatum(top_left);
      break;
    case Bubble::Loved: {
      uint16_t pink = COL_HEART(c);
      c.fillCircle(icx - 4, icy - 2, 5, pink);
      c.fillCircle(icx + 4, icy - 2, 5, pink);
      c.fillTriangle(icx - 9, icy, icx + 9, icy, icx, icy + 9, pink);
      break;
    }
    case Bubble::Question:
      c.setTextColor(c.color565(60, 60, 100));
      c.setTextSize(3);
      c.setTextDatum(middle_center);
      c.drawString("?", icx, icy);
      c.setTextDatum(top_left);
      break;
    default: break;
  }
}

// ─── Per-face renderers (unchanged from previous version) ──────────────────

void drawIdle(M5Canvas& c, int hx, int hy, const PetView& v) {
  int gx = (int)v.gazeX, gy = (int)v.gazeY;
  uint32_t period = (v.needs.energy < 30) ? 2000 :
                    (v.needs.energy < 70) ? 3000 : 4000;
  bool blinking = ((v.now_ms / 100) % (period / 100)) < 2;
#if SCREENSHOT_MODE
  // During the animals-cycle the screenshot module forces eyes open so the
  // dumped frame doesn't accidentally land on a blink.
  if (screenshot::g_forceEyesOpen) blinking = false;
#endif
  if (blinking) {
    drawEyeBlink(c, hx - EYE_DX, hy + EYE_OFF_Y);
    drawEyeBlink(c, hx + EYE_DX, hy + EYE_OFF_Y);
  } else {
    drawEyeOpen(c, hx - EYE_DX, hy + EYE_OFF_Y, gx, gy);
    drawEyeOpen(c, hx + EYE_DX, hy + EYE_OFF_Y, gx, gy);
  }
  drawMouthSmile(c, hx, hy + MOUTH_OFF_Y, 14);
}
void drawHappy(M5Canvas& c, int hx, int hy, const PetView&) {
  drawEyeHappy(c, hx - EYE_DX, hy + EYE_OFF_Y);
  drawEyeHappy(c, hx + EYE_DX, hy + EYE_OFF_Y);
  drawBlush(c, hx, hy);
  drawMouthSmile(c, hx, hy + MOUTH_OFF_Y, 22);
}
void drawExcited(M5Canvas& c, int hx, int hy, const PetView& v) {
  int gx = (int)v.gazeX, gy = (int)v.gazeY;
  drawEyeWide(c, hx - EYE_DX, hy + EYE_OFF_Y, gx, gy);
  drawEyeWide(c, hx + EYE_DX, hy + EYE_OFF_Y, gx, gy);
  drawBlush(c, hx, hy);
  drawMouthO(c, hx, hy + MOUTH_OFF_Y, 10);
  int phase = (v.now_ms / 100) % 10;
  drawSparkle(c, hx - HEAD_RX + 6,  hy - HEAD_RY + 6,  3 + phase / 3, COL_GLINT(c));
  drawSparkle(c, hx + HEAD_RX - 6,  hy - HEAD_RY + 6,  3 + (10 - phase) / 3, COL_GLINT(c));
  drawSparkle(c, hx, hy + HEAD_RY - 5, 4, COL_GLINT(c));
}
void drawLove(M5Canvas& c, int hx, int hy, const PetView& v) {
  drawEyeHeart(c, hx - EYE_DX, hy + EYE_OFF_Y);
  drawEyeHeart(c, hx + EYE_DX, hy + EYE_OFF_Y);
  drawBlush(c, hx, hy);
  drawMouthSmile(c, hx, hy + MOUTH_OFF_Y, 18);
  int phase = (v.now_ms / 80) % 60;
  for (int i = 0; i < 2; ++i) {
    int p = (phase + i * 30) % 60;
    int hxh = hx + (i ? HEAD_RX - 10 : -HEAD_RX + 10);
    int hyh = hy - HEAD_RY + 5 - p;
    uint16_t pink = COL_HEART(c);
    c.fillCircle(hxh - 3, hyh - 2, 4, pink);
    c.fillCircle(hxh + 3, hyh - 2, 4, pink);
    c.fillTriangle(hxh - 6, hyh, hxh + 6, hyh, hxh, hyh + 6, pink);
  }
}
void drawSleepy(M5Canvas& c, int hx, int hy, const PetView&) {
  drawEyeSleepy(c, hx - EYE_DX, hy + EYE_OFF_Y);
  drawEyeSleepy(c, hx + EYE_DX, hy + EYE_OFF_Y);
  drawMouthFlat(c, hx, hy + MOUTH_OFF_Y, 10);
}
void drawSleeping(M5Canvas& c, int hx, int hy, const PetView& v) {
  drawEyeBlink(c, hx - EYE_DX, hy + EYE_OFF_Y + 4);
  drawEyeBlink(c, hx + EYE_DX, hy + EYE_OFF_Y + 4);
  drawMouthFlat(c, hx, hy + MOUTH_OFF_Y, 8);
  drawZzz(c, hx, hy, v.now_ms);
}
void drawStartled(M5Canvas& c, int hx, int hy, const PetView& v) {
  int gx = (int)v.gazeX, gy = (int)v.gazeY;
  drawEyeStartled(c, hx - EYE_DX, hy + EYE_OFF_Y, gx, gy);
  drawEyeStartled(c, hx + EYE_DX, hy + EYE_OFF_Y, gx, gy);
  drawMouthO(c, hx, hy + MOUTH_OFF_Y, 12);
}
// Babbling-mouth animation for the "Goo-Goo" speech moments.
void drawSpeaking(M5Canvas& c, int hx, int hy, const PetView& v) {
  int gx = (int)v.gazeX, gy = (int)v.gazeY;
  // Blinking-friendly idle eyes that follow gaze
  drawEyeOpen(c, hx - EYE_DX, hy + EYE_OFF_Y, gx, gy);
  drawEyeOpen(c, hx + EYE_DX, hy + EYE_OFF_Y, gx, gy);

  // Mouth alternates open / closed at ~3 Hz to mimic vocalising.
  int my = hy + MOUTH_OFF_Y;
  uint32_t phase = (v.now_ms / 160) & 3;
  if (phase < 2) {
    // Open small "o" mouth
    c.fillEllipse(hx, my + 3, 8, 6, COL_DARK(c));
    // Tiny inner highlight that gives the mouth depth
    c.fillEllipse(hx, my + 5, 5, 2, c.color565(180, 80, 90));
  } else {
    // Briefly closed: small content smile
    drawMouthSmile(c, hx, my, 11);
  }

  // Two faint sound-wave arcs floating off to the side, animated outwards.
  uint32_t age = (v.now_ms / 80) % 12;
  uint16_t waveCol = c.color565(140, 130, 130);
  int sx = hx + 70 - (int)age;
  int sy = hy + MOUTH_OFF_Y;
  c.drawLine(sx, sy - 4, sx + 6, sy - 8, waveCol);
  c.drawLine(sx, sy,     sx + 8, sy,     waveCol);
  c.drawLine(sx, sy + 4, sx + 6, sy + 8, waveCol);
}

void drawEating(M5Canvas& c, int hx, int hy, const PetView& v) {
  // Closed crescent eyes — content while munching.
  drawEyeHappy(c, hx - EYE_DX, hy + EYE_OFF_Y);
  drawEyeHappy(c, hx + EYE_DX, hy + EYE_OFF_Y);
  drawBlush(c, hx, hy);

  // Chomp cycle ~4 chews/second
  uint32_t cycle = (v.now_ms / 130) & 1;
  int my = hy + MOUTH_OFF_Y;
  if (cycle == 0) {
    // Open mouth — chomp down, tongue glimpse
    c.fillEllipse(hx, my + 3, 13, 8, COL_DARK(c));
    c.fillEllipse(hx, my + 7,  9, 3, c.color565(225,  95, 110));
  } else {
    // Closed mouth — small content smile
    drawMouthSmile(c, hx, my, 13);
  }
}

void drawLaughing(M5Canvas& c, int hx, int hy, const PetView& v) {
  // Closed crescent eyes ^_^
  drawEyeHappy(c, hx - EYE_DX, hy + EYE_OFF_Y);
  drawEyeHappy(c, hx + EYE_DX, hy + EYE_OFF_Y);
  // Stronger blush
  drawBlush(c, hx, hy, 19);
  // Wide-open laughing mouth on the muzzle
  int my = hy + MOUTH_OFF_Y;
  c.fillEllipse(hx, my + 3, 18, 10, COL_DARK(c));
  // Carve the upper lip back into the muzzle for a smile-like top edge
  c.fillEllipse(hx, my - 2, 19, 6, COL_MUZZLE(c));
  // Tongue inside
  c.fillEllipse(hx, my + 9, 11, 4, c.color565(225, 95, 110));
  // Two short laughter "shake" arcs on the temples
  uint16_t lineCol = COL_DARK(c);
  int lx = hx - HEAD_RX + 30;
  int rx = hx + HEAD_RX - 30;
  uint32_t age = v.now_ms / 80;
  int wob = (age & 1) ? 0 : 2;
  c.drawLine(lx - 8, hy - 30 + wob, lx - 14, hy - 24 + wob, lineCol);
  c.drawLine(lx - 8, hy - 22 + wob, lx - 14, hy - 16 + wob, lineCol);
  c.drawLine(rx + 8, hy - 30 - wob, rx + 14, hy - 24 - wob, lineCol);
  c.drawLine(rx + 8, hy - 22 - wob, rx + 14, hy - 16 - wob, lineCol);
}

void drawSad(M5Canvas& c, int hx, int hy, const PetView& v) {
  int gx = (int)v.gazeX, gy = (int)v.gazeY;
  drawEyeSad(c, hx - EYE_DX, hy + EYE_OFF_Y, gx, gy);
  drawEyeSad(c, hx + EYE_DX, hy + EYE_OFF_Y, gx, gy);
  drawMouthFrown(c, hx, hy + MOUTH_OFF_Y, 14);
  drawTear(c, hx, hy, v.now_ms);
}

// ─── Toy visuals ────────────────────────────────────────────────────────────
//
// Each toy has its own little sprite + its own position curve so the play
// animation has visual variety. Coords are screen-space; sprite is drawn
// centered at (x,y).

void drawToyBall(M5Canvas& c, int x, int y) {
  uint16_t red = c.color565(220,  60,  60);
  uint16_t hi  = c.color565(255, 160, 160);
  uint16_t sh  = c.color565(140,  30,  30);
  c.fillCircle(x, y, 11, red);
  c.fillCircle(x - 4, y - 4, 4, hi);
  c.drawCircle(x, y, 11, sh);
  // Stitch line across the middle
  c.drawFastHLine(x - 8, y, 17, sh);
}

void drawToyMouse(M5Canvas& c, int x, int y, bool faceLeft) {
  uint16_t fur  = c.color565(170, 165, 175);
  uint16_t pink = c.color565(245, 175, 180);
  uint16_t dark = c.color565( 35,  30,  30);
  // Body
  c.fillEllipse(x, y, 13, 8, fur);
  // Ears (round)
  c.fillCircle(x - 6, y - 6, 4, fur);
  c.fillCircle(x + 2, y - 7, 4, fur);
  c.fillCircle(x - 6, y - 6, 2, pink);
  c.fillCircle(x + 2, y - 7, 2, pink);
  // Eye + nose, plus tail trailing the direction of motion
  if (faceLeft) {
    c.fillCircle(x - 9, y - 1, 1, dark);
    c.fillCircle(x - 12, y + 1, 2, pink);  // nose
    // tail to the right (trailing)
    for (int i = 0; i < 8; ++i)
      c.drawPixel(x + 11 + i, y + 2 - (i & 1), fur);
  } else {
    c.fillCircle(x + 9, y - 1, 1, dark);
    c.fillCircle(x + 12, y + 1, 2, pink);
    for (int i = 0; i < 8; ++i)
      c.drawPixel(x - 11 - i, y + 2 - (i & 1), fur);
  }
}

void drawToyRattle(M5Canvas& c, int x, int y) {
  uint16_t handle = c.color565(220, 180, 140);
  uint16_t head   = c.color565(255, 140, 180);
  uint16_t headHi = c.color565(255, 200, 220);
  uint16_t dot    = c.color565(255, 255, 255);
  uint16_t outline= c.color565( 90,  60,  40);
  // Handle
  c.fillRoundRect(x - 3, y, 6, 22, 3, handle);
  c.drawRoundRect(x - 3, y, 6, 22, 3, outline);
  c.fillCircle(x, y + 22, 4, handle);
  c.drawCircle(x, y + 22, 4, outline);
  // Head
  c.fillCircle(x, y - 4, 11, head);
  c.fillCircle(x - 3, y - 7, 4, headHi);
  c.drawCircle(x, y - 4, 11, outline);
  // Polka dots
  c.fillCircle(x - 5, y - 1, 1, dot);
  c.fillCircle(x + 4, y - 7, 1, dot);
  c.fillCircle(x + 2, y + 1, 1, dot);
}

void drawToyButterfly(M5Canvas& c, int x, int y, uint32_t now_ms) {
  // Wings flap — height oscillates with time so the silhouette pulses.
  float flap = 0.6f + 0.4f * sinf(now_ms / 80.0f);
  int wH = (int)(10 * flap);
  uint16_t wing  = c.color565(255, 130, 100);
  uint16_t wing2 = c.color565(255, 200, 130);
  uint16_t body  = c.color565( 50,  40,  60);
  // Upper wings
  c.fillEllipse(x - 8, y - 4, 9, wH + 2, wing);
  c.fillEllipse(x + 8, y - 4, 9, wH + 2, wing);
  // Lower wings (smaller)
  c.fillEllipse(x - 6, y + 4, 6, wH - 1, wing2);
  c.fillEllipse(x + 6, y + 4, 6, wH - 1, wing2);
  // Body
  c.fillRoundRect(x - 1, y - 6, 3, 13, 1, body);
  // Antennae
  c.drawLine(x - 1, y - 6, x - 4, y - 11, body);
  c.drawLine(x + 1, y - 6, x + 4, y - 11, body);
}

void drawToyPlush(M5Canvas& c, int x, int y) {
  uint16_t body  = c.color565(200, 145,  95);
  uint16_t shade = c.color565(160, 110,  70);
  uint16_t muz   = c.color565(245, 225, 195);
  uint16_t nose  = c.color565( 60,  40,  25);
  uint16_t inEar = c.color565(255, 195, 175);
  // Body (circle)
  c.fillCircle(x, y + 4, 11, body);
  // Head
  c.fillCircle(x, y - 5, 9, body);
  // Ears
  c.fillCircle(x - 6, y - 11, 4, body);
  c.fillCircle(x + 6, y - 11, 4, body);
  c.fillCircle(x - 6, y - 11, 2, inEar);
  c.fillCircle(x + 6, y - 11, 2, inEar);
  // Muzzle + nose + tiny eyes
  c.fillEllipse(x, y - 3, 4, 3, muz);
  c.fillCircle(x, y - 4, 1, nose);
  c.fillCircle(x - 3, y - 6, 1, nose);
  c.fillCircle(x + 3, y - 6, 1, nose);
  // Belly highlight
  c.fillEllipse(x, y + 6, 5, 3, muz);
  c.drawCircle(x, y + 4, 11, shade);
  c.drawCircle(x, y - 5, 9, shade);
}

void drawToy(M5Canvas& c, Toy t, int x, int y, bool faceLeft, uint32_t now_ms) {
  switch (t) {
    case Toy::Ball:      drawToyBall(c, x, y); break;
    case Toy::Mouse:     drawToyMouse(c, x, y, faceLeft); break;
    case Toy::Rattle:    drawToyRattle(c, x, y); break;
    case Toy::Butterfly: drawToyButterfly(c, x, y, now_ms); break;
    case Toy::Plush:     drawToyPlush(c, x, y); break;
  }
  (void)now_ms;
}

// Position of the toy at normalized time `t` ∈ [0,1] of the play animation.
// Each toy has its own motion curve so the action feels distinct.
struct ToyPos { float x, y; };

ToyPos toyPositionAt(Toy toy, float t, bool fromLeft, bool bored) {
  float x0 = fromLeft ? -20.0f : 340.0f;
  float x1 = fromLeft ? 340.0f : -20.0f;
  float xL = x0 + (x1 - x0) * t;

  if (bored) {
    // Listless: just slide across the bottom with no bounce or flutter.
    return { xL, 215.0f };
  }

  switch (toy) {
    case Toy::Ball: {
      // Four parabolic bounces.
      float bt = t * 4.0f;
      int   b  = (int)bt;
      float l  = bt - b;
      float h  = 4.0f * l * (1.0f - l);          // 0..1, peak at 0.5
      return { xL, 200.0f - 75.0f * h };
    }
    case Toy::Mouse: {
      // Quick low scurry with a couple of small hops.
      float bt = t * 6.0f;
      int   b  = (int)bt;
      float l  = bt - b;
      float h  = 4.0f * l * (1.0f - l);
      return { xL, 215.0f - 18.0f * h };
    }
    case Toy::Rattle: {
      // Rattle stays on screen and jitters around the center.
      float wobX = sinf(t * 18.0f) * 50.0f;
      float wobY = sinf(t * 14.0f + 1.2f) * 14.0f;
      return { 160.0f + wobX, 130.0f + wobY };
    }
    case Toy::Butterfly: {
      // Sinusoidal fluttering flight.
      float wobY = sinf(t * 9.0f) * 30.0f;
      return { xL, 130.0f + wobY };
    }
    case Toy::Plush: {
      // Slow gentle drift with a soft up-down bob.
      float wobY = sinf(t * 4.0f) * 8.0f;
      return { xL, 175.0f + wobY };
    }
  }
  return { xL, 200.0f };
}

// Vertical hop offset for the pet head during a happy play animation. 4
// hops over the duration; bored animations skip this entirely.
int petHopOffsetAt(float t) {
  float ht = t * 4.0f;
  int   hb = (int)ht;
  float hl = ht - hb;
  float h  = 4.0f * hl * (1.0f - hl);   // 0..1
  return (int)(-10.0f * h);
}

// ─── Media (TV / games / internet) visuals ──────────────────────────────────

void drawMediaTV(M5Canvas& c, int x, int y) {
  uint16_t case_   = c.color565( 60,  50,  60);
  uint16_t screen  = c.color565(120, 200, 220);
  uint16_t glow    = c.color565(220, 255, 255);
  uint16_t leg     = c.color565( 35,  30,  35);
  c.fillRoundRect(x - 24, y - 16, 48, 32, 5, case_);
  c.fillRoundRect(x - 20, y - 12, 40, 24, 3, screen);
  // Reflections
  c.fillRect(x - 17, y - 9, 6, 4, glow);
  c.fillRect(x +  4, y - 4, 4, 8, glow);
  // Legs / stand
  c.fillRect(x - 14, y + 16, 4, 4, leg);
  c.fillRect(x + 10, y + 16, 4, 4, leg);
}

void drawMediaGamepad(M5Canvas& c, int x, int y) {
  uint16_t body  = c.color565( 80,  60, 110);
  uint16_t btn   = c.color565(220,  80,  80);
  uint16_t btn2  = c.color565( 90, 200, 130);
  uint16_t pad   = c.color565( 40,  30,  60);
  uint16_t white = c.color565(240, 240, 240);
  // Body
  c.fillRoundRect(x - 28, y - 9, 56, 22, 9, body);
  c.fillCircle(x - 22, y + 2, 9, body);
  c.fillCircle(x + 22, y + 2, 9, body);
  // D-pad
  c.fillRect(x - 22, y - 3, 10, 4, pad);
  c.fillRect(x - 19, y - 6, 4, 10, pad);
  // Action buttons
  c.fillCircle(x + 18, y - 2, 3, btn);
  c.fillCircle(x + 24, y + 2, 3, btn2);
  c.fillCircle(x + 12, y + 2, 3, white);
  c.fillCircle(x + 18, y + 6, 3, white);
}

void drawMediaGlobe(M5Canvas& c, int x, int y) {
  uint16_t globe   = c.color565( 90, 165, 220);
  uint16_t land    = c.color565(110, 180, 110);
  uint16_t outline = c.color565( 30,  60, 100);
  c.fillCircle(x, y, 22, globe);
  c.drawCircle(x, y, 22, outline);
  // Continents (rough)
  c.fillEllipse(x - 8, y - 4, 6, 8, land);
  c.fillEllipse(x + 8, y + 6, 7, 5, land);
  c.fillCircle(x - 12, y + 8, 3, land);
  // Meridian / equator
  c.drawEllipse(x, y, 8, 22, outline);
  c.drawFastHLine(x - 22, y, 44, outline);
}

void drawMediaSocial(M5Canvas& c, int x, int y) {
  uint16_t body  = c.color565( 30,  30,  40);
  uint16_t bezel = c.color565( 60,  60,  75);
  uint16_t screen= c.color565(255, 255, 255);
  uint16_t heart = c.color565(230,  60, 100);
  uint16_t notify= c.color565(255,  80,  80);
  uint16_t green = c.color565( 90, 200, 130);
  // Phone body
  c.fillRoundRect(x - 14, y - 22, 28, 44, 5, body);
  c.drawRoundRect(x - 14, y - 22, 28, 44, 5, bezel);
  // Screen
  c.fillRoundRect(x - 11, y - 17, 22, 30, 2, screen);
  // Heart in middle of screen (a la "like" feed)
  c.fillCircle(x - 3, y - 5, 3, heart);
  c.fillCircle(x + 3, y - 5, 3, heart);
  c.fillTriangle(x - 6, y - 4, x + 6, y - 4, x, y + 4, heart);
  // Comment / notification dots above the heart
  c.fillRect(x - 8, y - 14, 4, 2, c.color565(120, 130, 200));
  c.fillRect(x - 2, y - 14, 8, 2, c.color565(120, 130, 200));
  // Bottom: small thumbs-up bar (green)
  c.fillRect(x - 7, y + 7, 14, 4, green);
  // Speaker slit at top
  c.fillRect(x - 4, y - 20, 8, 1, bezel);
  // Home button
  c.drawCircle(x, y + 18, 2, bezel);
  // Notification badge — red dot top-right
  c.fillCircle(x + 12, y - 20, 4, notify);
  c.drawCircle(x + 12, y - 20, 4, body);
}

// Webradio-Icon: kleines Vintage-Radio mit Antenne, Lautsprecher-Grille
// und Frequenz-Skala. Klar abgesetzt von TV/Phone-Icons.
void drawMediaRadio(M5Canvas& c, int x, int y) {
  uint16_t body    = c.color565(180, 90,  40);    // warmer Holz-Ton
  uint16_t bodyHi  = c.color565(220, 140, 80);
  uint16_t panel   = c.color565( 35,  30,  25);
  uint16_t mesh    = c.color565( 80,  70,  60);
  uint16_t glow    = c.color565(255, 220, 100);
  uint16_t metal   = c.color565(180, 180, 200);
  // Body
  c.fillRoundRect(x - 16, y - 14, 32, 26, 4, body);
  c.drawRoundRect(x - 16, y - 14, 32, 26, 4, panel);
  c.drawFastHLine(x - 14, y - 13, 28, bodyHi);
  // Lautsprecher-Grille links (Punktraster)
  c.fillRoundRect(x - 13, y - 9, 13, 16, 2, panel);
  for (int gy = 0; gy < 5; ++gy) {
    for (int gx = 0; gx < 5; ++gx) {
      c.drawPixel(x - 12 + gx * 3, y - 8 + gy * 3, mesh);
    }
  }
  // Frequenz-Skala rechts
  c.fillRoundRect(x + 1, y - 9, 13, 8, 1, c.color565(245, 230, 180));
  c.drawFastHLine(x + 2, y - 5, 11, panel);
  c.drawFastVLine(x + 6, y - 9, 8, c.color565(220, 60, 60));
  // Drehknopf darunter
  c.fillCircle(x + 7, y + 5, 3, metal);
  c.drawCircle(x + 7, y + 5, 3, panel);
  // Antenne nach oben/rechts
  c.drawLine(x + 12, y - 14, x + 18, y - 22, metal);
  c.fillCircle(x + 18, y - 22, 1, glow);
  // Highlight accent
  c.drawPixel(x - 10, y - 11, c.color565(255, 240, 200));
}

// Photo icon: stylised camera with lens + shutter button + flash dot.
void drawMediaCamera(M5Canvas& c, int x, int y) {
  uint16_t body    = c.color565( 60,  55,  75);
  uint16_t bodyHi  = c.color565(110, 100, 140);
  uint16_t lens    = c.color565( 30,  30,  40);
  uint16_t glass   = c.color565(150, 200, 230);
  uint16_t shutter = c.color565(220,  80,  80);
  uint16_t flash   = c.color565(255, 240, 100);
  // Body
  c.fillRoundRect(x - 14, y - 9, 28, 20, 3, body);
  c.fillRoundRect(x - 14, y - 9, 28, 4, 2, bodyHi);
  // Viewfinder bump on top
  c.fillRect(x - 5, y - 12, 10, 4, body);
  c.drawRect(x - 5, y - 12, 10, 4, bodyHi);
  // Lens
  c.fillCircle(x, y + 2, 7, lens);
  c.fillCircle(x, y + 2, 5, glass);
  c.drawPixel(x - 2, y, c.color565(255, 255, 255));
  // Shutter
  c.fillCircle(x + 11, y - 6, 2, shutter);
  // Flash LED
  c.fillCircle(x - 11, y - 6, 1, flash);
}

// Gallery icon: three stacked photo frames with little pictures inside.
void drawMediaGallery(M5Canvas& c, int x, int y) {
  uint16_t paper   = c.color565(245, 245, 250);
  uint16_t paperSh = c.color565(180, 180, 195);
  uint16_t outline = c.color565( 60,  50,  70);
  uint16_t sky     = c.color565(180, 210, 240);
  uint16_t hill    = c.color565(120, 180, 100);
  uint16_t sun     = c.color565(255, 220, 100);
  // Hinteres Bild (leicht versetzt)
  c.fillRoundRect(x - 11, y - 8, 22, 18, 2, paperSh);
  c.drawRoundRect(x - 11, y - 8, 22, 18, 2, outline);
  // Mittleres Bild
  c.fillRoundRect(x - 13, y - 6, 22, 18, 2, paper);
  c.drawRoundRect(x - 13, y - 6, 22, 18, 2, outline);
  // Vorderes Bild mit Mini-Landschaft
  c.fillRoundRect(x - 15, y - 4, 22, 18, 2, paper);
  c.drawRoundRect(x - 15, y - 4, 22, 18, 2, outline);
  // Inhalt: kleine Sonne + Berg
  c.fillRect(x - 13, y - 2, 18, 9, sky);
  c.fillCircle(x +  3, y    , 2, sun);
  c.fillTriangle(x - 13, y + 8, x - 4, y - 1, x + 7, y + 8, hill);
}

void drawMediaIcon(M5Canvas& c, Media m, int x, int y) {
  switch (m) {
    case Media::Movies:   drawMediaTV     (c, x, y); break;
    case Media::Games:    drawMediaGamepad(c, x, y); break;
    case Media::Internet: drawMediaGlobe  (c, x, y); break;
    case Media::Social:   drawMediaSocial (c, x, y); break;
    case Media::Radio:    drawMediaRadio  (c, x, y); break;
    case Media::Camera:   drawMediaCamera (c, x, y); break;
    case Media::Gallery:  drawMediaGallery(c, x, y); break;
    case Media::Friends:  break;   // Friends hat eigene Render-Logik im Modal
    case Media::None:     break;
  }
}

// Pet-view travel button — opens the scene-select screen. Compass-y icon
// (a stylised arrow inside a circle). Dims + shows a cooldown counter or
// energy glyph when the user can't currently travel.
void drawTravelButton(M5Canvas& c, uint32_t cooldownSec, bool canPlay) {
  const Rect& r = kTravelButtonRect;
  bool ready = (cooldownSec == 0 && canPlay);
  uint16_t panel   = ready ? c.color565(220, 240, 220)
                           : c.color565(150, 160, 150);
  uint16_t outline = ready ? c.color565( 50,  90,  60)
                           : c.color565( 60,  70,  60);
  uint16_t needle  = ready ? c.color565(200,  60,  60)
                           : c.color565(140,  60,  60);
  uint16_t needle2 = ready ? c.color565(240, 240, 240)
                           : c.color565(190, 190, 190);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 8, outline);

  int cx = r.x + r.w/2, cy = r.y + r.h/2;
  c.drawCircle(cx, cy, 12, outline);
  c.drawCircle(cx, cy, 11, outline);
  // Needle (north red, south white)
  c.fillTriangle(cx, cy - 10, cx - 4, cy, cx + 4, cy, needle);
  c.fillTriangle(cx, cy + 10, cx - 4, cy, cx + 4, cy, needle2);
  c.drawTriangle(cx, cy + 10, cx - 4, cy, cx + 4, cy, outline);
  c.fillCircle(cx, cy, 2, outline);
  c.drawPixel(cx, cy - 14, outline);

  // Locked-state overlay — same pattern as the activity button so the two
  // gated buttons read consistently.
  if (!ready) {
    for (int yy = r.y + 1; yy < r.y + r.h - 1; yy += 2) {
      for (int xx = r.x + 1 + ((yy & 1) ? 0 : 1); xx < r.x + r.w - 1; xx += 2) {
        c.drawPixel(xx, yy, c.color565(20, 20, 30));
      }
    }
    if (cooldownSec > 0) {
      char buf[8];
      uint32_t mins = cooldownSec / 60;
      uint32_t secs = cooldownSec % 60;
      snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)mins, (unsigned)secs);
      c.setTextDatum(middle_center);
      c.setTextSize(1);
      c.setTextColor(c.color565(255, 230, 100));
      c.drawString(buf, r.x + r.w / 2, r.y + r.h / 2 + 1);
      c.setTextDatum(top_left);
    } else if (!canPlay) {
      uint16_t bolt = c.color565(255, 230, 100);
      uint16_t boltSh = c.color565(180, 130, 30);
      int bx = r.x + r.w / 2, by = r.y + r.h / 2;
      c.fillTriangle(bx - 3, by - 7, bx + 4, by - 1, bx + 1, by - 1, bolt);
      c.fillTriangle(bx + 1, by - 1, bx + 4, by - 1, bx - 1, by + 7, bolt);
      c.drawTriangle(bx - 3, by - 7, bx + 4, by - 1, bx + 1, by - 1, boltSh);
      c.drawTriangle(bx + 1, by - 1, bx + 4, by - 1, bx - 1, by + 7, boltSh);
    }
  }
}

// Pet-view media button — small TV. Tapping while inactive opens the
// media-select screen; tapping while active stops media (handled by the
// dispatcher in main.cpp). The icon shows a red dot when active, like a
// recording indicator, so the button reads as a stop button.
void drawMediaButton(M5Canvas& c, bool active) {
  const Rect& r = kMediaButtonRect;
  uint16_t panel   = active ? c.color565(255, 200, 200)
                            : c.color565(220, 230, 245);
  uint16_t outline = c.color565( 60,  50,  90);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 8, outline);
  // Tiny TV (manual draw — drawMediaTV is too large for a 36px button)
  int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  c.fillRoundRect(cx - 12, cy - 8, 24, 16, 3, c.color565(50, 40, 50));
  c.fillRoundRect(cx - 10, cy - 6, 20, 12, 2, c.color565(120, 200, 220));
  c.drawFastVLine(cx - 6, cy - 4, 8, c.color565(220, 255, 255));
  c.fillRect(cx - 6, cy + 8, 3, 2, c.color565(40, 30, 40));
  c.fillRect(cx + 3, cy + 8, 3, 2, c.color565(40, 30, 40));
  if (active) {
    // Red recording dot
    c.fillCircle(cx + 11, cy - 11, 3, c.color565(220, 60, 60));
    c.drawCircle(cx + 11, cy - 11, 3, c.color565(120, 30, 30));
  }
}

}  // namespace (close anon so drawSport*Screen functions get external linkage)

void drawSportButton(M5Canvas& c) {
  const Rect& r = kSportButtonRect;
  uint16_t panel   = c.color565(255, 230, 180);
  uint16_t outline = c.color565( 90,  60,  20);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 8, outline);
  // Mini dumbbell: bar + two end weights.
  int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  uint16_t bar    = c.color565( 50,  40,  30);
  uint16_t weight = c.color565( 70,  55,  35);
  c.fillRect(cx - 12, cy - 1, 24, 3, bar);
  c.fillRoundRect(cx - 14, cy - 6, 5, 13, 1, weight);
  c.fillRoundRect(cx +  9, cy - 6, 5, 13, 1, weight);
  c.drawRoundRect(cx - 14, cy - 6, 5, 13, 1, c.color565(40, 25, 10));
  c.drawRoundRect(cx +  9, cy - 6, 5, 13, 1, c.color565(40, 25, 10));
}

// ─── Sport-Select screen ───────────────────────────────────────────────

static void drawSportIconLarge(M5Canvas& c, uint8_t exercise, int cx, int cy) {
  switch (exercise) {
    case 0: { // Squat — stick figure crouching
      uint16_t fg = c.color565( 80,  50,  20);
      c.fillCircle(cx, cy - 22, 8, fg);              // head
      c.fillRect(cx - 2, cy - 14, 4, 14, fg);        // body
      c.fillRect(cx - 9, cy - 4, 4, 10, fg);         // left leg bent
      c.fillRect(cx + 5, cy - 4, 4, 10, fg);         // right leg bent
      c.drawLine(cx - 2, cy - 8, cx - 12, cy + 2, fg);  // arm
      c.drawLine(cx + 2, cy - 8, cx + 12, cy + 2, fg);
      break;
    }
    case 1: { // Jump — stick figure mid-air with arms up
      uint16_t fg = c.color565(220,  80,  60);
      c.fillCircle(cx, cy - 24, 8, fg);
      c.fillRect(cx - 2, cy - 16, 4, 14, fg);
      c.drawLine(cx - 2, cy - 14, cx - 12, cy - 24, fg);
      c.drawLine(cx + 2, cy - 14, cx + 12, cy - 24, fg);
      c.drawLine(cx - 2, cy - 2, cx - 8, cy + 8, fg);
      c.drawLine(cx + 2, cy - 2, cx + 8, cy + 8, fg);
      // ground line dashed
      c.drawLine(cx - 14, cy + 14, cx + 14, cy + 14, c.color565(150, 150, 150));
      break;
    }
    case 2: { // Yoga — stick figure standing tall
      uint16_t fg = c.color565( 80, 130,  90);
      c.fillCircle(cx, cy - 22, 8, fg);
      c.fillRect(cx - 2, cy - 14, 4, 18, fg);
      c.fillRect(cx - 2, cy + 4, 4, 10, fg);
      c.drawLine(cx - 2, cy - 12, cx - 12, cy - 4, fg);
      c.drawLine(cx + 2, cy - 12, cx + 12, cy - 4, fg);
      // Calm stars
      c.drawPixel(cx - 18, cy - 28, c.color565(255, 255, 200));
      c.drawPixel(cx + 18, cy - 22, c.color565(255, 255, 200));
      break;
    }
  }
}

void drawSportSelectScreen(M5Canvas& c, const SportSelectView& v) {
  c.fillSprite(c.color565(28, 32, 50));
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  // Title
  c.setTextDatum(top_left);
  c.setTextSize(2);
  c.setTextColor(c.color565(255, 200, 100));
  c.setCursor(20, 14);
  c.print(tr(Str::SportTitle));
  c.setTextDatum(top_center);
  c.setTextSize(2);
  c.setTextColor(c.color565(230, 235, 245));
  c.drawString(tr(Str::SportSelectTitle), 160, 30);
  c.setTextDatum(top_left);

  // Back X
  {
    const Rect& r = kSportSelectBackRect;
    uint16_t panel = c.color565(255, 240, 240);
    uint16_t glyph = c.color565( 60,  50,  50);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // 3 exercise cards
  static const Str labels[3] = {
    Str::SportExSquat, Str::SportExJump, Str::SportExYoga,
  };
  static const uint16_t cardCol[3] = { 0, 0, 0 };
  for (int i = 0; i < 3; ++i) {
    const Rect& r = kSportSelectChoice[i];
    uint16_t bg, border;
    switch (i) {
      case 0: bg = c.color565(220, 225, 245); border = c.color565( 60,  80, 130); break;
      case 1: bg = c.color565(255, 220, 210); border = c.color565(170,  60,  40); break;
      default: bg = c.color565(220, 245, 220); border = c.color565( 60, 130,  70); break;
    }
    c.fillRoundRect(r.x, r.y, r.w, r.h, 12, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 12, border);

    int icx = r.x + r.w / 2, icy = r.y + 60;
    drawSportIconLarge(c, (uint8_t)i, icx, icy);

    // Label at textSize 1 — tighter than the previous size 2 so even the
    // longest German word ("Kniebeugen", "Stillhalten") fits in the
    // 100-px-wide card without clipping. The icon already conveys the
    // gist visually, the label just confirms it.
    c.setTextDatum(top_center);
    c.setTextSize(1);
    c.setTextColor(c.color565( 30,  30,  40));
    c.drawString(tr(labels[i]), r.x + r.w / 2, r.y + r.h - 18);
    c.setTextDatum(top_left);
  }
  (void)cardCol; (void)v.now_ms;
}

// ─── Workout screen (text-driven) ─────────────────────────────────────

void drawSportWorkoutScreen(M5Canvas& c, const SportWorkoutView& v) {
  c.fillSprite(c.color565(28, 32, 50));
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  // Exercise name top
  Str nameStr;
  switch (v.exercise) {
    case 0: nameStr = Str::SportExSquat; break;
    case 1: nameStr = Str::SportExJump;  break;
    default: nameStr = Str::SportExYoga; break;
  }
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(c.color565(255, 200, 100));
  c.drawString(tr(nameStr), 160, 6);

  // Back X
  {
    const Rect& r = kSportWorkoutBackRect;
    uint16_t panel = c.color565(255, 240, 240);
    uint16_t glyph = c.color565( 60,  50,  50);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // Phase 0 = safety hint + Action-Prompt was gleich kommt.
  // Auf 320 px breitem Display passt "TARGET_NAME festhalten!" bei size 3
  // nicht in eine Zeile, deshalb 2-zeilig (TARGET_NAME oben, "festhalten!"
  // drunter). Action-Prompt ("Mach Kniebeugen!" etc.) kommt darunter
  // einzeilig in size 2.
  if (v.phase == 0) {
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(255, 220, 120));
    c.drawString(TARGET_NAME, 160, 70);
    c.drawString(tr(Str::SportSafetyHoldEnd), 160, 102);

    Str promptStr;
    switch (v.exercise) {
      case 0:  promptStr = Str::SportPromptSquat; break;
      case 1:  promptStr = Str::SportPromptJump;  break;
      default: promptStr = Str::SportPromptYoga;  break;
    }
    c.setTextSize(2);
    c.setTextColor(c.color565(180, 230, 255));
    c.drawString(tr(promptStr), 160, 168);
    c.setTextDatum(top_left);
    return;
  }

  // Phase 1 = active exercise
  // Big arrow / icon depending on exercise + motion phase
  uint16_t arrowCol = c.color565(255, 220, 100);
  if (v.exercise == 0) {
    // Squat: alternating up/down arrow with text
    bool downPhase = (v.motionPhase == 1);
    int cx = 160, cy = 100;
    int sz = 32 + (int)(sinf(v.now_ms / 180.0f) * 4.0f);
    if (downPhase) {
      c.fillTriangle(cx - sz, cy - sz/2, cx + sz, cy - sz/2,
                     cx, cy + sz/2, arrowCol);
    } else {
      c.fillTriangle(cx - sz, cy + sz/2, cx + sz, cy + sz/2,
                     cx, cy - sz/2, arrowCol);
    }
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(230, 235, 245));
    c.drawString(tr(downPhase ? Str::SportSquatDown : Str::SportSquatUp),
                 160, 142);
  } else if (v.exercise == 1) {
    // Jump: big up-arrow + "Spring!"
    int cx = 160, cy = 100;
    int sz = 36 + (int)(sinf(v.now_ms / 140.0f) * 5.0f);
    c.fillTriangle(cx - sz, cy + sz/2, cx + sz, cy + sz/2,
                   cx, cy - sz/2, arrowCol);
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(230, 235, 245));
    c.drawString(tr(Str::SportJumpGo), 160, 142);
  } else {
    // Yoga: peaceful pulsing circle
    int cx = 160, cy = 100;
    float t = sinf(v.now_ms / 600.0f) * 0.5f + 0.5f;
    int sz = 30 + (int)(t * 6.0f);
    uint16_t inner = c.color565(120, 200, 160);
    uint16_t outer = c.color565( 60, 130,  90);
    c.fillCircle(cx, cy, sz, inner);
    c.drawCircle(cx, cy, sz, outer);
    c.drawCircle(cx, cy, sz + 4, outer);
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(230, 235, 245));
    c.drawString(tr(Str::SportYogaStill), 160, 142);
  }

  // Counter
  char counterBuf[16];
  if (v.exercise == 2) {
    snprintf(counterBuf, sizeof(counterBuf), "%u s", (unsigned)v.repCount);
  } else {
    snprintf(counterBuf, sizeof(counterBuf), "%u / %u",
             (unsigned)v.repCount, (unsigned)v.targetReps);
  }
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(c.color565(160, 220, 255));
  c.drawString(counterBuf, 160, 178);
  c.setTextDatum(top_left);
}

// ─── Done celebration ────────────────────────────────────────────────

void drawSportDoneScreen(M5Canvas& c, const SportDoneView& v) {
  c.fillSprite(c.color565(28, 32, 50));
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  // Trophy icon
  int cx = 160, cy = 78;
  uint16_t gold = c.color565(255, 200,  60);
  uint16_t goldDk = c.color565(180, 130,  20);
  c.fillRoundRect(cx - 26, cy - 22, 52, 36, 8, gold);
  c.drawRoundRect(cx - 26, cy - 22, 52, 36, 8, goldDk);
  c.fillEllipse(cx - 30, cy - 4, 6, 14, gold);
  c.fillEllipse(cx + 30, cy - 4, 6, 14, gold);
  c.fillRect(cx - 8, cy + 14, 16, 6, gold);
  c.fillRect(cx - 18, cy + 18, 36, 6, goldDk);

  // Title
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(c.color565(255, 220, 120));
  c.drawString(tr(Str::SportDoneTitle), 160, 110);

  // Streak
  char streakBuf[40];
  snprintf(streakBuf, sizeof(streakBuf), "%s%u",
           tr(Str::SportDoneStreak), (unsigned)v.streak);
  c.setTextSize(2);
  c.setTextColor(c.color565(230, 235, 245));
  c.drawString(streakBuf, 160, 144);

  // Rewards
  c.setTextSize(2);
  c.setTextColor(c.color565(120, 220, 140));
  c.drawString(tr(Str::SportDoneEnergy), 160, 166);
  c.setTextColor(c.color565(255, 130, 170));
  c.drawString(tr(Str::SportDoneJoy), 160, 184);
  c.setTextDatum(top_left);

  // OK button
  {
    const Rect& r = kSportDoneOkRect;
    uint16_t bg = c.color565( 90, 180, 110);
    uint16_t bd = c.color565(220, 255, 220);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 10, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 10, bd);
    c.setTextDatum(middle_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(255, 255, 255));
    c.drawString("OK", r.x + r.w / 2, r.y + r.h / 2);
    c.setTextDatum(top_left);
  }
  (void)v.exercise; (void)v.reps; (void)v.now_ms; (void)v.myAnimal;
}

namespace {  // reopen anon namespace for the rest of the file

// Square, sunken eyes — the screen-zombie look. Drawn on top of the head
// when v.mediaActive is set, replacing whatever per-face eye renderer
// would normally run. Includes dark "bags" underneath.
void drawMediaEyes(M5Canvas& c, int hx, int hy, uint32_t now_ms) {
  uint16_t bag    = c.color565(110,  80, 130);   // purple under-eye shadow
  uint16_t white  = c.color565(220, 230, 235);
  uint16_t edge   = c.color565( 40,  30,  60);
  uint16_t pupil  = c.color565( 20,  20,  30);
  uint16_t glow   = c.color565( 90, 230, 255);   // screen reflection

  int eyeY = hy + EYE_OFF_Y;
  for (int side = -1; side <= 1; side += 2) {
    int ex = hx + side * EYE_DX;
    // Under-eye bag (sunken)
    c.fillEllipse(ex, eyeY + 9, 10, 3, bag);
    c.fillEllipse(ex - 1, eyeY + 8, 7, 2, c.color565(80, 55, 100));
    // Square eye
    c.fillRoundRect(ex - 9, eyeY - 7, 18, 14, 2, white);
    c.drawRoundRect(ex - 9, eyeY - 7, 18, 14, 2, edge);
    c.drawRoundRect(ex - 8, eyeY - 6, 16, 12, 2, edge);
    // Empty stare — pupil locked in place, slightly bigger
    c.fillRect(ex - 3, eyeY - 4, 6, 8, pupil);
    // Flickering screen reflection — alternates over time so the eyes look
    // glazed by changing TV light.
    int flick = ((int)(now_ms / 80)) & 3;
    if (flick == 0) c.fillRect(ex - 7, eyeY - 5, 3, 3, glow);
    if (flick == 1) c.fillRect(ex + 4, eyeY - 5, 3, 3, glow);
    if (flick == 2) c.fillRect(ex - 7, eyeY + 1, 3, 3, glow);
  }
}

// Pet-view toy button — opens the toy-select screen. Always visible.
void drawToyButton(M5Canvas& c) {
  const Rect& r = kToyButtonRect;
  uint16_t panel   = c.color565(255, 230, 240);
  uint16_t outline = c.color565(150,  60, 100);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 8, outline);
  // Mini ball icon, hinting the play action.
  int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  uint16_t red = c.color565(220, 60, 60);
  uint16_t hi  = c.color565(255, 160, 160);
  c.fillCircle(cx, cy, 11, red);
  c.fillCircle(cx - 4, cy - 4, 3, hi);
  c.drawCircle(cx, cy, 11, outline);
  c.drawFastHLine(cx - 8, cy, 17, outline);
}

// ─── Foraging visuals ───────────────────────────────────────────────────────

// Pet-view basket button — opens the foraging screen.
// `hint` enables a pulsing golden glow that draws attention to the button
// when the user just tried to feed an empty inventory.
void drawForagingButton(M5Canvas& c, bool hint, uint32_t now_ms) {
  const Rect& r = kForagingButtonRect;

  if (hint) {
    // Pulsing glow: brightness oscillates with sin(now/120). Three rings
    // stepping outward from the button, alpha-faked via dimmer colors.
    float t = sinf((float)now_ms / 120.0f) * 0.5f + 0.5f;   // 0..1
    uint8_t base = 140 + (uint8_t)(t * 115.0f);              // 140..255
    uint16_t glow1 = c.color565(base, base * 5 / 6, 40);
    uint16_t glow2 = c.color565(base * 3 / 4, base / 2, 30);
    uint16_t glow3 = c.color565(base / 2, base / 3, 20);
    c.drawRoundRect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 9,  glow1);
    c.drawRoundRect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 10, glow2);
    c.drawRoundRect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 11, glow3);
  }

  uint16_t panel   = c.color565(245, 220, 180);
  uint16_t outline = c.color565(110,  70,  35);
  uint16_t weave   = c.color565(180, 130,  60);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 8, outline);

  int cx = r.x + r.w / 2;
  int cy = r.y + r.h / 2;
  // Handle: two arcs (use thin half circles approximated with drawCircle)
  for (int i = 0; i < 2; ++i) {
    c.drawCircle(cx, cy - 1 + i, 9 - i, outline);
  }
  // Body trapezoid
  c.fillTriangle(cx - 12, cy + 1, cx + 12, cy + 1, cx +  9, cy + 11, weave);
  c.fillTriangle(cx - 12, cy + 1, cx +  9, cy + 11, cx -  9, cy + 11, weave);
  c.drawLine(cx - 12, cy + 1, cx -  9, cy + 11, outline);
  c.drawLine(cx + 12, cy + 1, cx +  9, cy + 11, outline);
  c.drawLine(cx - 12, cy + 1, cx + 12, cy + 1, outline);
  c.drawLine(cx -  9, cy + 11, cx +  9, cy + 11, outline);
  // Apple peeking out of the basket
  c.fillCircle(cx - 3, cy - 1, 4, c.color565(220, 60, 60));
  c.fillCircle(cx - 4, cy - 2, 1, c.color565(255, 140, 140));
  c.drawFastVLine(cx - 3, cy - 6, 2, c.color565(80, 50, 30));
}

// Tree, bush, water — the static foraging-scene props. Items are drawn on
// top by drawForageItem(). The hill/grass strip is shared with drawHills.
void drawForageScene(M5Canvas& c, uint32_t now_ms) {
  // Grass strip
  c.fillRect(0, 165, 320, 30, c.color565(95, 155, 75));
  c.drawFastHLine(0, 165, 320, c.color565(60, 110, 50));

  // Water — bottom 45 px
  c.fillRect(0, 195, 320, 45, c.color565(60, 130, 200));
  c.drawFastHLine(0, 195, 320, c.color565(35,  90, 160));
  uint16_t wave = c.color565(170, 210, 235);
  for (int i = 0; i < 6; ++i) {
    int phase = (now_ms / 80) % 60;
    int x1 = (i * 60 + phase) % 320 - 30;
    c.drawFastHLine(x1,      210, 10, wave);
    c.drawFastHLine(x1 + 30, 224, 10, wave);
  }

  // Tree (left side)
  uint16_t trunk = c.color565(110, 70, 40);
  uint16_t trunkSh = c.color565(75, 45, 25);
  c.fillRect(58, 105, 14, 60, trunk);
  c.drawFastVLine(58, 105, 60, trunkSh);
  c.drawFastVLine(71, 105, 60, trunkSh);
  // Foliage — three overlapping circles
  uint16_t foliage = c.color565(70, 140, 65);
  uint16_t folHi   = c.color565(100, 175, 90);
  c.fillCircle(50,  95, 28, foliage);
  c.fillCircle(80,  82, 30, foliage);
  c.fillCircle(105,100, 26, foliage);
  c.fillCircle(75,  72, 14, folHi);
  c.fillCircle(48,  85,  8, folHi);

  // Bush (right side)
  c.fillCircle(220, 162, 18, foliage);
  c.fillCircle(248, 156, 24, foliage);
  c.fillCircle(275, 165, 19, foliage);
  c.fillCircle(248, 148, 10, folHi);
}

void drawForageItem(M5Canvas& c, ForageItem t, int x, int y, bool faceLeft) {
  switch (t) {
    case ForageItem::Apple: {
      uint16_t red    = c.color565(220,  50,  50);
      uint16_t hi     = c.color565(255, 140, 140);
      uint16_t stem   = c.color565( 80,  50,  30);
      uint16_t leaf   = c.color565( 90, 155,  70);
      c.fillCircle(x, y, 7, red);
      c.fillCircle(x - 2, y - 2, 2, hi);
      c.drawFastVLine(x, y - 9, 3, stem);
      c.fillTriangle(x + 1, y - 8, x + 5, y - 9, x + 4, y - 6, leaf);
      break;
    }
    case ForageItem::Berry: {
      uint16_t a = c.color565(140,  50, 170);
      uint16_t b = c.color565(165,  65, 190);
      uint16_t cberry = c.color565(190,  85, 215);
      c.fillCircle(x - 3, y,     4, a);
      c.fillCircle(x + 3, y - 1, 4, b);
      c.fillCircle(x,     y + 3, 4, cberry);
      c.fillCircle(x - 4, y - 1, 1, c.color565(230, 180, 240));   // tiny highlight
      break;
    }
    case ForageItem::Fish: {
      uint16_t body = c.color565(255, 165,  60);
      uint16_t fin  = c.color565(230, 130,  40);
      if (faceLeft) {
        c.fillEllipse(x, y, 9, 5, body);
        c.fillTriangle(x - 8, y, x - 14, y - 4, x - 14, y + 4, fin);
        c.fillCircle(x + 5, y - 1, 1, c.color565(20, 20, 20));
      } else {
        c.fillEllipse(x, y, 9, 5, body);
        c.fillTriangle(x + 8, y, x + 14, y - 4, x + 14, y + 4, fin);
        c.fillCircle(x - 5, y - 1, 1, c.color565(20, 20, 20));
      }
      break;
    }
    case ForageItem::None: break;
  }
}

// Top inventory strip on the foraging screen. Drawn last so collected items
// flying into it visibly disappear under the panel rim.
void drawForageInventoryBar(M5Canvas& c, const ForagingView& v) {
  uint16_t panel  = c.color565( 40,  30,  20);
  uint16_t border = c.color565(110,  80,  50);
  uint16_t white  = c.color565(255, 250, 240);
  c.fillRect(0, 0, 320, 36, panel);
  c.drawFastHLine(0, 36, 320, border);

  // Apple slot
  drawForageItem(c, ForageItem::Apple, kForageInvApplePosX, kForageInvY, true);
  // Berry slot
  drawForageItem(c, ForageItem::Berry, kForageInvBerryPosX, kForageInvY, true);
  // Fish slot
  drawForageItem(c, ForageItem::Fish,  kForageInvFishPosX,  kForageInvY, true);

  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(white);
  char buf[8];
  snprintf(buf, sizeof(buf), "x%u", (unsigned)v.apples);
  c.drawString(buf, kForageInvApplePosX + 14, kForageInvY);
  snprintf(buf, sizeof(buf), "x%u", (unsigned)v.berries);
  c.drawString(buf, kForageInvBerryPosX + 14, kForageInvY);
  snprintf(buf, sizeof(buf), "x%u", (unsigned)v.fish);
  c.drawString(buf, kForageInvFishPosX  + 14, kForageInvY);
  c.setTextDatum(top_left);

  // Back X
  const Rect& r = kForagingBackRect;
  uint16_t bp = c.color565(255, 240, 240);
  uint16_t bg = c.color565( 60,  50,  50);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 6, bp);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 6, bg);
  int cx = r.x + r.w/2, cy = r.y + r.h/2;
  c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, bg);
  c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, bg);
  c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, bg);
  c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, bg);
}

// ─── Mini-pet sprite (used by activity mini-games) ──────────────────────────
//
// A chibi version of the animal head. ~26 px tall, suitable for placing in
// minigames where the full-size head would crowd the play area.

void drawPetMini(M5Canvas& c, int x, int y, AnimalType animal) {
  AnimalStyle s = styleFor(animal, c);
  uint16_t outline = s.bodyShade;

  // Ears
  if (animal == AnimalType::Cat) {
    c.fillTriangle(x - 11, y -  6, x -  4, y -  6, x -  9, y - 13, s.body);
    c.fillTriangle(x + 11, y -  6, x +  4, y -  6, x +  9, y - 13, s.body);
    c.fillTriangle(x -  9, y -  7, x -  6, y -  7, x -  9, y - 11, s.innerEar);
    c.fillTriangle(x +  9, y -  7, x +  6, y -  7, x +  9, y - 11, s.innerEar);
  } else if (animal == AnimalType::Dog) {
    c.fillEllipse(x - 11, y -  2, 5, 8, s.bodyShade);
    c.fillEllipse(x + 11, y -  2, 5, 8, s.bodyShade);
  } else {  // Bear / default
    c.fillCircle(x - 10, y -  9, 5, s.body);
    c.fillCircle(x + 10, y -  9, 5, s.body);
    c.fillCircle(x - 10, y -  9, 2, s.innerEar);
    c.fillCircle(x + 10, y -  9, 2, s.innerEar);
  }

  // Head
  c.fillEllipse(x, y, 14, 13, s.body);
  c.drawEllipse(x, y, 14, 13, outline);

  // Cheeks
  c.fillCircle(x - 8, y + 3, 2, c.color565(255, 170, 180));
  c.fillCircle(x + 8, y + 3, 2, c.color565(255, 170, 180));

  // Muzzle / nose
  c.fillEllipse(x, y + 3, 5, 3, s.muzzle);
  c.fillCircle(x, y + 1, 1, s.nose);

  // Eyes
  c.fillCircle(x - 5, y - 2, 2, c.color565(20, 20, 30));
  c.fillCircle(x + 5, y - 2, 2, c.color565(20, 20, 30));
  c.drawPixel (x - 5, y - 3, c.color565(255, 255, 255));
  c.drawPixel (x + 5, y - 3, c.color565(255, 255, 255));

  // Whiskers (cat only)
  if (animal == AnimalType::Cat) {
    c.drawFastHLine(x - 14, y + 3, 3, outline);
    c.drawFastHLine(x + 12, y + 3, 3, outline);
  }
}

// ─── Activity (mini-game) visuals ───────────────────────────────────────────

void drawActivityButton(M5Canvas& c, Scene s, uint32_t cooldownSec,
                        bool canPlay) {
  if (!sceneHasActivity(s)) return;
  const Rect& r = kActivityButtonRect;
  bool ready = (cooldownSec == 0 && canPlay);
  uint16_t panel   = ready ? c.color565(255, 245, 220)
                           : c.color565(150, 145, 130);
  uint16_t outline = ready ? c.color565(120,  80,  40)
                           : c.color565( 80,  70,  60);
  c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 8, outline);
  int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  switch (s) {
    case Scene::Meadow: {
      // Butterfly net: short stick + circle hoop with mesh cross
      uint16_t stick = c.color565(110, 70, 35);
      uint16_t hoop  = c.color565( 60,  50,  40);
      uint16_t mesh  = c.color565(255, 255, 255);
      c.drawLine(cx + 6, cy + 10, cx - 4, cy - 6, stick);
      c.drawLine(cx + 7, cy + 10, cx - 3, cy - 6, stick);
      c.drawCircle(cx - 5, cy - 7, 8, hoop);
      c.drawCircle(cx - 5, cy - 7, 7, hoop);
      c.drawLine(cx - 12, cy - 7, cx + 2, cy - 7, mesh);
      c.drawLine(cx -  5, cy - 14, cx - 5, cy, mesh);
      // Tiny butterfly above the net
      c.fillCircle(cx + 8, cy - 11, 2, c.color565(255, 130, 100));
      c.fillCircle(cx + 12, cy - 11, 2, c.color565(255, 130, 100));
      break;
    }
    case Scene::Bedroom: {
      // Stack of blocks
      c.fillRect(cx - 10, cy +  4, 20, 7, c.color565(220, 100, 100));
      c.drawRect(cx - 10, cy +  4, 20, 7, outline);
      c.fillRect(cx -  7, cy -  3, 16, 7, c.color565(100, 180, 220));
      c.drawRect(cx -  7, cy -  3, 16, 7, outline);
      c.fillRect(cx -  4, cy - 10, 12, 7, c.color565(220, 200,  80));
      c.drawRect(cx -  4, cy - 10, 12, 7, outline);
      c.drawPixel(cx, cy + 7, c.color565(80, 30, 30));
      c.drawPixel(cx + 2, cy, c.color565(20, 60, 100));
      break;
    }
    case Scene::Forest: {
      // Mushroom — red cap with white spots, white stem
      uint16_t cap = c.color565(220,  60,  60);
      uint16_t stem = c.color565(245, 230, 200);
      c.fillRect(cx - 4, cy + 3, 8, 8, stem);
      c.drawRect(cx - 4, cy + 3, 8, 8, outline);
      c.fillEllipse(cx, cy + 1, 11, 7, cap);
      c.drawEllipse(cx, cy + 1, 11, 7, outline);
      c.fillCircle(cx - 4, cy - 1, 1, c.color565(255, 255, 255));
      c.fillCircle(cx + 3, cy + 2, 1, c.color565(255, 255, 255));
      c.fillCircle(cx,     cy - 2, 1, c.color565(255, 255, 255));
      break;
    }
    case Scene::Beach: {
      // Surfboard with wave
      uint16_t board = c.color565(245, 200, 100);
      uint16_t stripe = c.color565(220, 60, 60);
      c.fillEllipse(cx + 3, cy + 4, 14, 4, board);
      c.drawEllipse(cx + 3, cy + 4, 14, 4, outline);
      c.drawFastHLine(cx - 9, cy + 4, 24, stripe);
      // Wave curl
      uint16_t wave = c.color565(100, 180, 230);
      c.fillCircle(cx - 8, cy - 4, 6, wave);
      c.fillCircle(cx - 4, cy - 6, 4, c.color565(220, 240, 255));
      c.drawCircle(cx - 8, cy - 4, 6, c.color565(40, 100, 160));
      break;
    }
    case Scene::Desert: {
      // Stylised scorpion silhouette + a "jump" arrow
      uint16_t scorp = c.color565(120,  70,  35);
      c.fillEllipse(cx + 1, cy + 6, 7, 3, scorp);   // body
      c.drawLine(cx + 7, cy + 6, cx + 13, cy + 4, scorp);   // tail seg
      c.drawLine(cx + 13, cy + 4, cx + 11, cy + 1, scorp);  // tail tip
      c.fillCircle(cx + 11, cy + 1, 1, scorp);
      c.drawLine(cx - 4, cy + 6, cx - 9, cy + 4, scorp);
      c.drawLine(cx - 4, cy + 6, cx - 9, cy + 8, scorp);
      // Jump arrow above
      uint16_t arr = c.color565(200, 140, 60);
      c.drawLine(cx - 6, cy - 2, cx, cy - 8, arr);
      c.drawLine(cx + 6, cy - 2, cx, cy - 8, arr);
      c.drawFastVLine(cx, cy - 8, 4, arr);
      break;
    }
    case Scene::Space: {
      // Asteroid + dodging arrows
      uint16_t aster = c.color565(150, 130, 110);
      uint16_t crater = c.color565( 90,  75,  65);
      c.fillCircle(cx, cy - 1, 9, aster);
      c.drawCircle(cx, cy - 1, 9, outline);
      c.fillCircle(cx - 3, cy - 3, 2, crater);
      c.fillCircle(cx + 3, cy + 1, 2, crater);
      c.fillCircle(cx + 1, cy + 4, 1, crater);
      // Two side arrows suggesting "dodge"
      uint16_t arr = c.color565( 90, 200, 240);
      c.drawLine(cx - 14, cy + 8, cx - 10, cy + 4, arr);
      c.drawLine(cx - 14, cy + 8, cx - 10, cy + 12, arr);
      c.drawLine(cx + 14, cy + 8, cx + 10, cy + 4, arr);
      c.drawLine(cx + 14, cy + 8, cx + 10, cy + 12, arr);
      break;
    }
    case Scene::City: {
      // Traffic light
      uint16_t case_ = c.color565( 40,  40,  50);
      c.fillRoundRect(cx - 7, cy - 12, 14, 24, 3, case_);
      c.drawRoundRect(cx - 7, cy - 12, 14, 24, 3, outline);
      c.fillCircle(cx, cy -  6, 3, c.color565(220,  60,  60));
      c.fillCircle(cx, cy +  0, 3, c.color565(230, 200,  60));
      c.fillCircle(cx, cy +  6, 3, c.color565(110, 200, 130));
      c.drawFastVLine(cx, cy + 11, 3, case_);
      break;
    }
    default: break;
  }

  // Locked-state overlay — semi-transparent darkening + countdown / energy
  // glyph. Uses an alternating-pixel shade (M5GFX has no alpha).
  if (!ready) {
    for (int yy = r.y + 1; yy < r.y + r.h - 1; yy += 2) {
      for (int xx = r.x + 1 + ((yy & 1) ? 0 : 1); xx < r.x + r.w - 1; xx += 2) {
        c.drawPixel(xx, yy, c.color565(20, 20, 30));
      }
    }
    if (cooldownSec > 0) {
      char buf[8];
      uint32_t mins = cooldownSec / 60;
      uint32_t secs = cooldownSec % 60;
      snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)mins, (unsigned)secs);
      c.setTextDatum(middle_center);
      c.setTextSize(1);
      c.setTextColor(c.color565(255, 230, 100));
      c.drawString(buf, r.x + r.w / 2, r.y + r.h / 2 + 1);
      c.setTextDatum(top_left);
    } else if (!canPlay) {
      // Lightning bolt = "needs energy"
      uint16_t bolt = c.color565(255, 230, 100);
      uint16_t boltSh = c.color565(180, 130, 30);
      int bx = r.x + r.w / 2, by = r.y + r.h / 2;
      c.fillTriangle(bx - 3, by - 7, bx + 4, by - 1, bx + 1, by - 1, bolt);
      c.fillTriangle(bx + 1, by - 1, bx + 4, by - 1, bx - 1, by + 7, bolt);
      c.drawTriangle(bx - 3, by - 7, bx + 4, by - 1, bx + 1, by - 1, boltSh);
      c.drawTriangle(bx + 1, by - 1, bx + 4, by - 1, bx - 1, by + 7, boltSh);
    }
  }
}

// Single butterfly sprite (top + body) — reused by drawActivityButterflyGame.
void drawButterflySprite(M5Canvas& c, int x, int y, uint8_t colorIdx,
                         uint32_t now_ms, bool caughtFly) {
  static const uint16_t palette[6][2] = {
    { 0xF180, 0xFCE0 },   // orange + warm yellow (overrides per-runtime)
    { 0,      0      },
  };
  // Build colors at runtime (palette table above is too coarse with 565)
  uint16_t wing, wing2, body;
  switch (colorIdx % 5) {
    case 0: wing = c.color565(255, 130, 100); wing2 = c.color565(255, 200, 130); break;
    case 1: wing = c.color565(180, 130, 220); wing2 = c.color565(220, 180, 240); break;
    case 2: wing = c.color565(255, 200,  80); wing2 = c.color565(255, 230, 160); break;
    case 3: wing = c.color565(120, 200, 240); wing2 = c.color565(180, 220, 250); break;
    default:wing = c.color565(255, 160, 200); wing2 = c.color565(255, 210, 230); break;
  }
  body = c.color565(50, 40, 60);

  float flap = 0.6f + 0.4f * sinf(now_ms / 70.0f);
  if (caughtFly) flap = 1.2f;     // wings spread when "caught" rising
  int wH = (int)(7 * flap);
  c.fillEllipse(x - 6, y - 3, 7, wH + 1, wing);
  c.fillEllipse(x + 6, y - 3, 7, wH + 1, wing);
  c.fillEllipse(x - 4, y + 3, 4, wH - 1, wing2);
  c.fillEllipse(x + 4, y + 3, 4, wH - 1, wing2);
  c.fillRoundRect(x - 1, y - 4, 3,  9, 1, body);
  c.drawLine(x - 1, y - 4, x - 3, y - 8, body);
  c.drawLine(x + 1, y - 4, x + 3, y - 8, body);
  (void)palette;
}

void drawActivityButterflyGame(M5Canvas& c, const ActivityView& v) {
  // Meadow background as the play field.
  drawSceneMeadow(c, v.phase, v.now_ms);

  // Mini-pet at bottom-center, "watching"
  drawPetMini(c, 160, 220, v.animal);

  // Butterflies
  for (int i = 0; i < ActivityView::kBfCount; ++i) {
    if (v.bfState[i] == 2) continue;       // gone
    bool caught = v.bfState[i] == 1;
    drawButterflySprite(c, v.bfX[i], v.bfY[i], v.bfColor[i],
                        v.now_ms, caught);
    if (caught) {
      // "+1" tag rising with the butterfly
      uint32_t age = v.now_ms - v.bfCaughtMs[i];
      uint8_t alpha = (age < 600) ? 255 : 0;
      if (alpha > 0) {
        c.setTextDatum(middle_center);
        c.setTextSize(2);
        c.setTextColor(c.color565(255, 255, 100));
        c.drawString("+1", v.bfX[i] + 14, v.bfY[i] - 8);
      }
    }
  }

  // HUD — score + countdown
  uint16_t hud = c.color565(50, 40, 30);
  uint16_t hudBg = c.color565(255, 250, 220);
  c.fillRoundRect(8, 8, 100, 24, 6, hudBg);
  c.drawRoundRect(8, 8, 100, 24, 6, hud);
  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(hud);
  char buf[24];
  snprintf(buf, sizeof(buf), "x%u", (unsigned)v.score);
  c.drawString(buf, 16, 20);
  // Tiny butterfly icon at right of score
  drawButterflySprite(c, 92, 20, 0, v.now_ms, false);

  c.fillRoundRect(212, 8, 64, 24, 6, hudBg);
  c.drawRoundRect(212, 8, 64, 24, 6, hud);
  c.setTextDatum(middle_center);
  c.setTextSize(2);
  c.setTextColor(hud);
  unsigned secs = (v.timeLeftMs + 999) / 1000;
  snprintf(buf, sizeof(buf), "%us", secs);
  c.drawString(buf, 244, 20);
  c.setTextDatum(top_left);
}

void drawActivityStackGame(M5Canvas& c, const ActivityView& v) {
  // Bedroom backdrop
  drawSceneBedroom(c, v.phase, v.now_ms);

  // Stack of placed blocks
  static const uint16_t palette[6] = {
    0xE925, 0x4B5F, 0xFFC0, 0x6E5C, 0xFB31, 0x8E5A,
  };
  // Build runtime colors (the literals above are imprecise) — recompute:
  uint16_t cols[6] = {
    c.color565(220, 100, 100),
    c.color565( 80, 170, 230),
    c.color565(255, 200,  80),
    c.color565(110, 200, 130),
    c.color565(255, 140, 200),
    c.color565(170, 130, 230),
  };
  uint16_t outline = c.color565( 50,  35,  20);

  for (int i = 0; i < v.stackCount; ++i) {
    uint16_t fill = cols[i % 6];
    c.fillRect(v.stackX[i], v.stackY[i], v.stackW[i], 14, fill);
    c.drawRect(v.stackX[i], v.stackY[i], v.stackW[i], 14, outline);
    // Highlight stripe
    c.drawFastHLine(v.stackX[i] + 1, v.stackY[i] + 1, v.stackW[i] - 2,
                    c.color565(255, 255, 255));
  }

  // Currently-moving block
  if (!v.gameOver && v.movingW > 0) {
    uint16_t fill = cols[v.stackCount % 6];
    c.fillRect(v.movingX, v.movingY, v.movingW, 14, fill);
    c.drawRect(v.movingX, v.movingY, v.movingW, 14, outline);
  }

  // Mini pet at far-right, watching
  drawPetMini(c, 290, 200, v.animal);

  // HUD — block count
  uint16_t hud = c.color565(50, 40, 30);
  uint16_t hudBg = c.color565(255, 250, 240);
  c.fillRoundRect(8, 8, 100, 24, 6, hudBg);
  c.drawRoundRect(8, 8, 100, 24, 6, hud);
  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(hud);
  char buf[24];
  snprintf(buf, sizeof(buf), "x%u", (unsigned)v.score);
  c.drawString(buf, 16, 20);
  c.fillRect(80, 14, 18, 10, c.color565(220, 100, 100));
  c.drawRect(80, 14, 18, 10, outline);
  c.setTextDatum(top_left);
  (void)palette;
}

void drawMushroomSprite(M5Canvas& c, int x, int y, uint8_t kind, uint32_t age) {
  // Pop-in / pop-out scale based on age
  float scale = 1.0f;
  if (age < 120) scale = age / 120.0f;
  // No pop-out animation here — death is handled by parent
  uint16_t cap   = (kind == 2) ? c.color565(255, 200,  60)
                               : c.color565(220,  60,  60);
  uint16_t spots = c.color565(255, 255, 255);
  uint16_t stem  = c.color565(245, 230, 200);
  uint16_t outline = c.color565(60, 40, 30);
  int capW = (int)(13 * scale);
  int capH = (int)( 9 * scale);
  int stemW = (int)( 8 * scale);
  int stemH = (int)( 8 * scale);
  if (capW < 2) capW = 2;
  if (capH < 2) capH = 2;
  if (stemW < 2) stemW = 2;
  if (stemH < 2) stemH = 2;
  c.fillRect(x - stemW/2, y, stemW, stemH, stem);
  c.drawRect(x - stemW/2, y, stemW, stemH, outline);
  c.fillEllipse(x, y, capW, capH, cap);
  c.drawEllipse(x, y, capW, capH, outline);
  if (scale > 0.7f) {
    c.fillCircle(x - 4, y - 2, 1, spots);
    c.fillCircle(x + 3, y + 1, 1, spots);
    c.fillCircle(x,     y - 4, 1, spots);
  }
  if (kind == 2) {
    c.drawEllipse(x, y, capW + 1, capH + 1, c.color565(255, 230, 100));
  }
}

void drawActivityMushroomGame(M5Canvas& c, const ActivityView& v) {
  drawSceneForest(c, v.phase, v.now_ms);
  // Floor strip subtly highlighted to show the mushroom area
  c.fillRect(0, 192, 320, 4, c.color565(45, 80, 35));

  // Slots
  for (int i = 0; i < ActivityView::kMushSlots; ++i) {
    if (v.mushKind[i] == 0) continue;
    uint32_t age = v.now_ms - v.mushSpawnedMs[i];
    drawMushroomSprite(c, kMushSlotX[i], kMushSlotY[i], v.mushKind[i], age);
  }

  // Mini-pet on the left, watching
  drawPetMini(c, 30, 200, v.animal);

  // HUD
  uint16_t hud = c.color565(50, 40, 30);
  uint16_t hudBg = c.color565(255, 250, 220);
  c.fillRoundRect(8, 8, 100, 24, 6, hudBg);
  c.drawRoundRect(8, 8, 100, 24, 6, hud);
  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(hud);
  char buf[24];
  snprintf(buf, sizeof(buf), "x%u", (unsigned)v.score);
  c.drawString(buf, 16, 20);
  drawMushroomSprite(c, 92, 20, 1, 200);

  c.fillRoundRect(212, 8, 64, 24, 6, hudBg);
  c.drawRoundRect(212, 8, 64, 24, 6, hud);
  c.setTextDatum(middle_center);
  c.setTextColor(hud);
  unsigned secs = (v.timeLeftMs + 999) / 1000;
  snprintf(buf, sizeof(buf), "%us", secs);
  c.drawString(buf, 244, 20);
  c.setTextDatum(top_left);
}

void drawSurfboard(M5Canvas& c, int x, int y) {
  uint16_t board = c.color565(245, 200, 100);
  uint16_t stripe = c.color565(220, 60, 60);
  uint16_t shade = c.color565(180, 130,  60);
  c.fillEllipse(x, y, 22, 5, board);
  c.drawEllipse(x, y, 22, 5, shade);
  c.drawFastHLine(x - 18, y, 36, stripe);
  // Foam
  uint16_t foam = c.color565(220, 235, 245);
  c.drawFastHLine(x - 26, y + 5, 8, foam);
  c.drawFastHLine(x + 16, y + 5, 12, foam);
}

void drawActivitySurfGame(M5Canvas& c, const ActivityView& v) {
  drawSceneBeach(c, v.phase, v.now_ms);
  // Falling obstacles
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    if (v.obsType[i] == 0) continue;
    int x = v.obsX[i], y = v.obsY[i];
    if (v.obsType[i] == 1) {
      // Rock
      c.fillCircle(x, y, 9, c.color565(95, 90, 85));
      c.fillCircle(x - 3, y - 3, 2, c.color565(140, 135, 130));
      c.drawCircle(x, y, 9, c.color565(50, 45, 40));
    } else if (v.obsType[i] == 2) {
      // Jellyfish
      uint16_t jel = c.color565(220, 180, 240);
      c.fillEllipse(x, y, 9, 6, jel);
      for (int t = 0; t < 4; ++t) {
        int tx = x - 6 + t * 4;
        c.drawFastVLine(tx, y + 3, 8, jel);
      }
      c.fillCircle(x - 3, y - 1, 1, c.color565(80, 50, 110));
      c.fillCircle(x + 3, y - 1, 1, c.color565(80, 50, 110));
    } else if (v.obsType[i] == 3) {
      // Shell (collectible)
      uint16_t sh = c.color565(255, 200, 220);
      c.fillTriangle(x, y - 7, x - 7, y + 5, x + 7, y + 5, sh);
      c.drawTriangle(x, y - 7, x - 7, y + 5, x + 7, y + 5, c.color565(180, 100, 130));
      c.drawLine(x, y - 7, x - 4, y + 5, c.color565(180, 100, 130));
      c.drawLine(x, y - 7, x + 4, y + 5, c.color565(180, 100, 130));
    }
  }
  // Pet on surfboard
  drawSurfboard(c, v.petPosX, v.petPosY + 10);
  drawPetMini(c, v.petPosX, v.petPosY, v.animal);

  // HUD: score + lives
  uint16_t hud = c.color565(40, 40, 60);
  uint16_t hudBg = c.color565(255, 250, 240);
  c.fillRoundRect(8, 8, 100, 24, 6, hudBg);
  c.drawRoundRect(8, 8, 100, 24, 6, hud);
  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(hud);
  char buf[24];
  snprintf(buf, sizeof(buf), "%u", (unsigned)v.score);
  c.drawString(buf, 16, 20);

  // Lives = small surfboards on the right
  for (int i = 0; i < v.lives; ++i) {
    drawSurfboard(c, 240 + i * 24, 20);
  }
}

void drawActivityScorpionGame(M5Canvas& c, const ActivityView& v) {
  drawSceneDesert(c, v.phase, v.now_ms);
  // Ground floor visualisation
  c.fillRect(0, 218, 320, 22, c.color565(200, 140, 85));

  // Obstacles scrolling right→left
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    if (v.obsType[i] == 0) continue;
    int x = v.obsX[i], y = v.obsY[i];
    if (v.obsType[i] == 1) {
      // Scorpion
      uint16_t scorp = c.color565( 90,  55,  30);
      c.fillEllipse(x, y, 10, 4, scorp);
      c.fillEllipse(x - 6, y - 1, 3, 2, scorp);
      c.drawLine(x + 9, y, x + 14, y - 4, scorp);
      c.drawLine(x + 14, y - 4, x + 12, y - 7, scorp);
      c.fillCircle(x + 12, y - 7, 1, scorp);
      c.drawLine(x - 4, y, x - 7, y - 3, scorp);
      c.drawLine(x - 4, y, x - 7, y + 3, scorp);
    } else if (v.obsType[i] == 2) {
      // Cactus
      uint16_t cact = c.color565( 70, 130,  70);
      uint16_t cactSh = c.color565( 45,  95,  50);
      c.fillRoundRect(x - 4, y - 22, 8, 24, 2, cact);
      c.fillRoundRect(x + 4, y - 14, 5, 12, 2, cact);
      c.fillRoundRect(x + 4, y - 14, 8, 3, 1, cact);
      c.drawFastVLine(x - 4, y - 22, 24, cactSh);
    }
  }

  // Mini-pet (running)
  drawPetMini(c, v.petPosX, v.petPosY, v.animal);

  // HUD
  uint16_t hud = c.color565(80, 40, 20);
  uint16_t hudBg = c.color565(255, 240, 200);
  c.fillRoundRect(8, 8, 100, 24, 6, hudBg);
  c.drawRoundRect(8, 8, 100, 24, 6, hud);
  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(hud);
  char buf[24];
  snprintf(buf, sizeof(buf), "%u", (unsigned)v.score);
  c.drawString(buf, 16, 20);
}

void drawActivityAsteroidsGame(M5Canvas& c, const ActivityView& v) {
  drawSceneSpace(c, v.phase, v.now_ms);

  // Falling objects
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    if (v.obsType[i] == 0) continue;
    int x = v.obsX[i], y = v.obsY[i];
    if (v.obsType[i] == 1) {
      // Asteroid
      c.fillCircle(x, y, 11, c.color565(155, 130, 105));
      c.drawCircle(x, y, 11, c.color565( 80,  65,  50));
      c.fillCircle(x - 4, y - 3, 2, c.color565(95, 75, 60));
      c.fillCircle(x + 3, y + 2, 2, c.color565(95, 75, 60));
      c.fillCircle(x + 1, y + 5, 1, c.color565(95, 75, 60));
    } else if (v.obsType[i] == 2) {
      // Star (collectible)
      uint16_t st = c.color565(255, 240, 100);
      c.fillTriangle(x, y - 8, x - 3, y + 1, x + 3, y + 1, st);
      c.fillTriangle(x, y + 8, x - 3, y - 1, x + 3, y - 1, st);
      c.fillTriangle(x - 8, y, x - 1, y - 3, x - 1, y + 3, st);
      c.fillTriangle(x + 8, y, x + 1, y - 3, x + 1, y + 3, st);
      c.fillCircle(x, y, 2, c.color565(255, 220, 60));
    }
  }

  // Pet in helmet (already wearing space helmet via drawAccessoryForScene
  // for the main pet; mini-pet shows just the head)
  drawPetMini(c, v.petPosX, v.petPosY, v.animal);
  // Tiny helmet outline around mini-pet
  c.drawCircle(v.petPosX, v.petPosY - 2, 18, c.color565(180, 220, 240));
  c.drawCircle(v.petPosX, v.petPosY - 2, 17, c.color565(120, 180, 220));

  // HUD
  uint16_t hud = c.color565(220, 220, 240);
  uint16_t hudBg = c.color565( 30,  35,  60);
  c.fillRoundRect(8, 8, 100, 24, 6, hudBg);
  c.drawRoundRect(8, 8, 100, 24, 6, hud);
  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(hud);
  char buf[24];
  snprintf(buf, sizeof(buf), "%u", (unsigned)v.score);
  c.drawString(buf, 16, 20);
  // Lives as small stars on the right
  for (int i = 0; i < v.lives; ++i) {
    int x = 244 + i * 18;
    c.fillCircle(x, 20, 4, c.color565(255, 240, 100));
  }
}

// Tiny driver pet drawn inside a Frogger car's windshield. ~9×5 head with
// 1–2 px ears poking above the roof so the animal type is recognisable
// at this scale. Sized to fit the 14×6 light-blue glass — does not extend
// into adjacent lanes.
static void drawCarDriver(M5Canvas& c, int cx, int car_y, uint8_t animal) {
  if (animal == 0) return;
  AnimalType t = (animal == 1) ? AnimalType::Bear :
                 (animal == 2) ? AnimalType::Cat  :
                                 AnimalType::Dog;
  AnimalStyle s = styleFor(t, c);
  uint16_t eye = c.color565(20, 20, 30);
  int cy = car_y - 2;                  // windshield centre

  // Ears first, so the head ellipse covers their inner edges.
  if (t == AnimalType::Cat) {
    c.fillTriangle(cx - 5, cy - 3, cx - 2, cy - 3, cx - 4, cy - 6, s.body);
    c.fillTriangle(cx + 5, cy - 3, cx + 2, cy - 3, cx + 4, cy - 6, s.body);
  } else if (t == AnimalType::Dog) {
    c.fillEllipse(cx - 5, cy + 1, 1, 2, s.bodyShade);
    c.fillEllipse(cx + 5, cy + 1, 1, 2, s.bodyShade);
  } else {                              // Bear
    c.fillCircle(cx - 4, cy - 4, 1, s.body);
    c.fillCircle(cx + 4, cy - 4, 1, s.body);
  }

  // Head — fits inside the windshield (radii 4×2 → 9×5 footprint).
  c.fillEllipse(cx, cy, 4, 2, s.body);

  // Eyes + tiny muzzle pixel — gives a focal point at this scale.
  c.drawPixel(cx - 2, cy - 1, eye);
  c.drawPixel(cx + 2, cy - 1, eye);
  c.drawPixel(cx,     cy + 1, s.nose);
}

void drawActivityCrossGame(M5Canvas& c, const ActivityView& v) {
  drawSceneCity(c, v.phase, v.now_ms);

  // Lane markings — clearer than the default city-scene ones
  uint16_t lane = c.color565(250, 220,  90);
  for (int row = 0; row < 5; ++row) {
    int y = kCrossLaneY[row] + 14;
    for (int x = 0; x < 320; x += 22) {
      c.fillRect(x, y, 14, 2, lane);
    }
  }
  // Goal banner at the top
  c.fillRect(0, 60, 320, 4, c.color565(110, 200, 130));
  c.fillRect(0, 64, 320, 16, c.color565( 70, 160,  90));
  c.setTextDatum(middle_center);
  c.setTextSize(2);
  c.setTextColor(c.color565(255, 255, 255));
  c.drawString(tr(Str::CrossGoal), 160, 72);

  // Cars
  for (int i = 0; i < ActivityView::kObsCount; ++i) {
    if (v.obsType[i] == 0) continue;
    int x = v.obsX[i], y = v.obsY[i];
    uint8_t k = v.obsType[i];
    uint16_t carCols[4] = {
      c.color565(220,  60,  60),
      c.color565(100, 180, 220),
      c.color565(255, 200,  80),
      c.color565(150, 100, 200),
    };
    uint16_t body = carCols[(k - 1) & 3];
    bool flip = (k & 0x80) != 0;
    int w = 36, h = 16;
    c.fillRoundRect(x - w/2, y, w, h, 3, body);
    c.drawRoundRect(x - w/2, y, w, h, 3, c.color565(30, 30, 35));
    // Roof
    c.fillRoundRect(x - 8, y - 6, 16, 8, 2, body);
    c.drawRoundRect(x - 8, y - 6, 16, 8, 2, c.color565(30, 30, 35));
    c.fillRoundRect(x - 7, y - 5, 14, 6, 1, c.color565(180, 220, 240));
    // Optional driver pet — encoded in bits 3-4 of obsType
    // (0 = no driver, 1 = Bear, 2 = Cat, 3 = Dog). Drawn over the windshield.
    uint8_t driver = (k >> 3) & 0x03;
    if (driver != 0) drawCarDriver(c, x, y, driver);
    // Wheels
    c.fillCircle(x - 11, y + h, 3, c.color565(20, 20, 25));
    c.fillCircle(x + 11, y + h, 3, c.color565(20, 20, 25));
    // Headlights pointing the way it's going
    uint16_t hl = c.color565(255, 240, 180);
    if (flip) c.fillCircle(x - 16, y + 4, 2, hl);
    else      c.fillCircle(x + 16, y + 4, 2, hl);
  }

  // Pet at its current row
  int row = v.petRow;
  if (row < 0) row = 0;
  if (row > 5) row = 5;
  int py = (row == 0) ? 222 : kCrossLaneY[row - 1] - 2;
  if (row >= 5) py = 72;
  drawPetMini(c, kCrossPetX, py, v.animal);

  // HUD
  uint16_t hud = c.color565(40, 40, 60);
  uint16_t hudBg = c.color565(255, 250, 240);
  c.fillRoundRect(8, 8, 100, 24, 6, hudBg);
  c.drawRoundRect(8, 8, 100, 24, 6, hud);
  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(hud);
  char buf[24];
  snprintf(buf, sizeof(buf), "%u", (unsigned)v.score);
  c.drawString(buf, 16, 20);
}

static void drawStarRow(M5Canvas& c, int cx, int cy, uint8_t starsEarned) {
  // Five-pointed star using two overlapping triangles. 3 stars total.
  const int spacing = 36;
  for (int i = 0; i < 3; ++i) {
    int sx = cx + (i - 1) * spacing;
    bool earned = (i < starsEarned);
    uint16_t fill = earned ? c.color565(255, 220,  60)
                           : c.color565( 90,  85,  90);
    uint16_t edge = earned ? c.color565(180, 120,  20)
                           : c.color565( 60,  55,  60);
    // Body
    c.fillTriangle(sx, cy - 12, sx - 11, cy + 7, sx + 11, cy + 7, fill);
    c.fillTriangle(sx - 11, cy - 3, sx + 11, cy - 3, sx, cy + 12, fill);
    c.drawTriangle(sx, cy - 12, sx - 11, cy + 7, sx + 11, cy + 7, edge);
    c.drawTriangle(sx - 11, cy - 3, sx + 11, cy - 3, sx, cy + 12, edge);
    if (earned) {
      // Tiny inner highlight
      c.fillCircle(sx - 2, cy - 2, 2, c.color565(255, 255, 200));
    }
  }
}

// Game-over overlay — score, stars, reward chips, retry. Bigger panel than
// the previous design to fit the reward summary.
void drawActivityGameOver(M5Canvas& c, const ActivityView& v) {
  // Subtle scanline dim of the underlying game
  for (int y = 0; y < 240; y += 2) {
    c.drawFastHLine(0, y, 320, c.color565(0, 0, 0));
  }
  // Bigger panel
  uint16_t panel  = c.color565(255, 250, 240);
  uint16_t glyph  = c.color565( 60,  50,  50);
  uint16_t accent = c.color565(255, 150,  60);
  uint16_t soft   = c.color565(110, 100, 100);
  uint16_t hapBg  = c.color565(255, 220, 230);
  uint16_t engBg  = c.color565(220, 240, 220);
  uint16_t newCol = c.color565(255,  90, 110);

  c.fillRoundRect(28, 26, 264, 192, 12, panel);
  c.drawRoundRect(28, 26, 264, 192, 12, glyph);
  c.drawRoundRect(29, 27, 262, 190, 11, glyph);

  // Title
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(accent);
  c.drawString(tr(Str::ActivityGameOver), 160, 38);

  // Stars row
  drawStarRow(c, 160, 80, v.stars);

  // Score + Best — single line each
  c.setTextSize(2);
  c.setTextColor(glyph);
  char buf[40];
  snprintf(buf, sizeof(buf), tr(Str::ActivityScoreFormat), (unsigned)v.score);
  c.drawString(buf, 160, 102);
  snprintf(buf, sizeof(buf), tr(Str::ActivityBestFormat),  (unsigned)v.best);
  c.setTextColor(soft);
  c.drawString(buf, 160, 122);

  if (v.newBest && v.score > 0) {
    c.setTextSize(2);
    c.setTextColor(newCol);
    c.drawString(tr(Str::ActivityNewRecord), 160, 142);
  }

  // Reward chips — Happy + Energy. Centered side-by-side.
  if (v.bonusHap > 0 || v.bonusEng > 0) {
    int chipY = v.newBest ? 162 : 146;
    int chipH = 22;
    int hapW = 70;
    int engW = 70;
    int gap  = 14;
    int totalW = (v.bonusEng > 0 && v.bonusHap > 0) ? (hapW + gap + engW)
                                                    : (v.bonusEng > 0 ? engW : hapW);
    int chipX = 160 - totalW / 2;
    if (v.bonusHap > 0) {
      c.fillRoundRect(chipX, chipY, hapW, chipH, 6, hapBg);
      c.drawRoundRect(chipX, chipY, hapW, chipH, 6, glyph);
      // Heart icon
      uint16_t heart = c.color565(220,  60, 110);
      int hx = chipX + 12, hy = chipY + chipH / 2;
      c.fillCircle(hx - 3, hy - 1, 3, heart);
      c.fillCircle(hx + 3, hy - 1, 3, heart);
      c.fillTriangle(hx - 5, hy, hx + 5, hy, hx, hy + 6, heart);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.setTextDatum(middle_center);
      snprintf(buf, sizeof(buf), "+%u", (unsigned)v.bonusHap);
      c.drawString(buf, chipX + 12 + 24, hy);
      chipX += hapW + gap;
    }
    if (v.bonusEng > 0) {
      c.fillRoundRect(chipX, chipY, engW, chipH, 6, engBg);
      c.drawRoundRect(chipX, chipY, engW, chipH, 6, glyph);
      // Lightning icon
      uint16_t bolt = c.color565(220, 180,  40);
      int bx = chipX + 12, by = chipY + chipH / 2;
      c.fillTriangle(bx - 2, by - 6, bx + 4, by, bx + 1, by, bolt);
      c.fillTriangle(bx + 1, by, bx + 4, by, bx - 2, by + 6, bolt);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.setTextDatum(middle_center);
      snprintf(buf, sizeof(buf), "+%u", (unsigned)v.bonusEng);
      c.drawString(buf, chipX + 12 + 24, by);
    }
    c.setTextDatum(top_center);
  }

  // Retry button — dimmed once the per-session replay cap is reached.
  {
    const Rect& r = kActivityRetryRect;
    bool exhausted = (v.maxPlaysPerSession > 0 &&
                      v.playsInSession >= v.maxPlaysPerSession);
    uint16_t bg = exhausted ? c.color565( 90,  90, 100) : accent;
    uint16_t fg = exhausted ? c.color565(170, 170, 185) : c.color565(255, 255, 255);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(fg);
    c.drawString(tr(Str::ActivityAgain), r.x + r.w / 2, r.y + r.h / 2 - 6);
    // Run counter underneath the label so the user sees how many tries
    // are left without extra UI clutter.
    if (v.maxPlaysPerSession > 0) {
      char playsBuf[12];
      snprintf(playsBuf, sizeof(playsBuf), "%u / %u",
               (unsigned)v.playsInSession, (unsigned)v.maxPlaysPerSession);
      c.setTextSize(1);
      c.setTextColor(fg);
      c.drawString(playsBuf, r.x + r.w / 2, r.y + r.h / 2 + 10);
    }
  }
  // OK button — secondary (neutral), ends the game and returns to the
  // pet view. Uses "Fertig"/"Done" from the WiFi setup.
  {
    const Rect& r = kActivityOkRect;
    uint16_t bg = c.color565(220, 220, 230);
    uint16_t fg = c.color565( 40,  40,  50);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(fg);
    c.drawString(tr(Str::WifiSetupDoneBtn), r.x + r.w / 2, r.y + r.h / 2);
  }
  c.setTextDatum(top_left);
}

// ─── Accessories (auto-derived from scene) ──────────────────────────────────
//
// Each accessory is drawn as an overlay on the pet's head after the per-face
// renderer (and after the media-eyes override). They use the same hx/hy as
// the head so they wander/tilt with the pet.

void drawAccessoryFlowerCrown(M5Canvas& c, int hx, int hy) {
  uint16_t petalCols[5] = {
    c.color565(255, 195, 220),    // pink
    c.color565(220, 200, 255),    // lavender
    c.color565(255, 240, 180),    // pale yellow
    c.color565(255, 175, 200),    // peach
    c.color565(195, 225, 255),    // pale blue
  };
  uint16_t leaf      = c.color565(110, 175,  90);
  uint16_t centerCol = c.color565(255, 215,  70);

  // Five blooms arched along the top of the head.
  for (int i = 0; i < 5; ++i) {
    float a = -1.05f + i * 0.525f;          // -1.05 .. +1.05 rad
    int fx = hx + (int)(sinf(a) * 78.0f);
    int fy = hy - 92 + (int)((1.0f - cosf(a)) * 14.0f) - 4;
    // Five small petals arranged around the center
    for (int p = 0; p < 5; ++p) {
      float pa = p * (2.0f * (float)PI / 5.0f);
      int px = fx + (int)(cosf(pa) * 4.0f);
      int py = fy + (int)(sinf(pa) * 4.0f);
      c.fillCircle(px, py, 3, petalCols[i]);
    }
    c.fillCircle(fx, fy, 2, centerCol);
  }
  // Two tiny leaves to fill the gaps
  c.fillEllipse(hx - 50, hy - 80, 4, 2, leaf);
  c.fillEllipse(hx + 50, hy - 80, 4, 2, leaf);
}

void drawAccessoryNightcap(M5Canvas& c, int hx, int hy) {
  uint16_t red       = c.color565(220,  60,  60);
  uint16_t redDark   = c.color565(150,  35,  35);
  uint16_t white     = c.color565(245, 245, 245);
  uint16_t whiteSh   = c.color565(195, 195, 205);
  // Slumped triangle, base across the top of the head, tip falling to right.
  int baseY     = hy - 86;
  int baseLeft  = hx - 32;
  int baseRight = hx + 30;
  int tipX      = hx + 56;
  int tipY      = hy - 130;
  c.fillTriangle(baseLeft, baseY, baseRight, baseY, tipX, tipY, red);
  // Mid-shading to suggest fold
  c.fillTriangle(hx, baseY, baseRight, baseY, tipX, tipY, redDark);
  c.drawLine(baseLeft, baseY, tipX, tipY, redDark);
  c.drawLine(baseRight, baseY, tipX, tipY, redDark);
  // Fluffy white band at the base
  c.fillRoundRect(baseLeft - 3, baseY - 6, 68, 10, 4, white);
  c.drawRoundRect(baseLeft - 3, baseY - 6, 68, 10, 4, whiteSh);
  for (int x = baseLeft - 1; x < baseLeft + 64; x += 6) {
    c.fillCircle(x, baseY - 1, 2, white);
  }
  // Pompon at the tip
  c.fillCircle(tipX, tipY, 7, white);
  c.fillCircle(tipX - 3, tipY - 3, 2, whiteSh);
  c.drawCircle(tipX, tipY, 7, whiteSh);
}

void drawAccessoryRangerHat(M5Canvas& c, int hx, int hy) {
  uint16_t green   = c.color565( 85, 115,  65);
  uint16_t greenSh = c.color565( 50,  75,  40);
  uint16_t band    = c.color565(110,  75,  45);
  uint16_t bandSh  = c.color565( 70,  45,  25);
  // Wide brim
  c.fillEllipse(hx, hy - 80, 80, 8, greenSh);
  c.fillEllipse(hx, hy - 81, 76, 6, green);
  // Crown (rounded)
  c.fillRoundRect(hx - 32, hy - 110, 64, 30, 10, green);
  c.drawRoundRect(hx - 32, hy - 110, 64, 30, 10, greenSh);
  // Center crease
  c.drawFastVLine(hx, hy - 108, 22, greenSh);
  c.drawFastVLine(hx + 1, hy - 108, 22, greenSh);
  // Hat band
  c.fillRect(hx - 32, hy - 92, 64, 7, band);
  c.drawRect(hx - 32, hy - 92, 64, 7, bandSh);
  // Tiny gold buckle
  c.fillRect(hx - 4, hy - 92, 8, 7, c.color565(220, 180, 70));
  c.drawRect(hx - 4, hy - 92, 8, 7, c.color565(150, 110, 30));
}

void drawAccessorySunglasses(M5Canvas& c, int hx, int hy) {
  uint16_t frame   = c.color565( 30,  30,  40);
  uint16_t lens    = c.color565( 18,  20,  35);
  uint16_t lensHi  = c.color565( 80, 110, 150);
  uint16_t glint   = c.color565(220, 235, 255);
  int eyeY = hy + EYE_OFF_Y;
  for (int side = -1; side <= 1; side += 2) {
    int ex = hx + side * EYE_DX;
    // Lens (oval)
    c.fillEllipse(ex, eyeY, 18, 13, lens);
    c.fillEllipse(ex, eyeY,  9,  6, lensHi);     // glow blob
    c.drawEllipse(ex, eyeY, 18, 13, frame);
    c.drawEllipse(ex, eyeY, 17, 12, frame);
    // Glint
    c.fillRoundRect(ex - 13, eyeY - 9, 6, 3, 1, glint);
  }
  // Bridge (over the nose) + temple arms
  c.drawFastHLine(hx - 22, eyeY - 1, 44, frame);
  c.drawFastHLine(hx - 22, eyeY,     44, frame);
  c.drawLine(hx - 56, eyeY - 4, hx - EYE_DX - 16, eyeY, frame);
  c.drawLine(hx + 56, eyeY - 4, hx + EYE_DX + 16, eyeY, frame);
}

void drawAccessoryCowboyHat(M5Canvas& c, int hx, int hy) {
  uint16_t tan     = c.color565(195, 145,  90);
  uint16_t tanSh   = c.color565(140,  95,  55);
  uint16_t band    = c.color565( 80,  50,  30);
  // Wide brim with curled ends
  c.fillEllipse(hx, hy - 80, 92, 8, tanSh);
  c.fillEllipse(hx, hy - 82, 88, 6, tan);
  // Curl up at the brim ends (small upward bulge)
  c.fillEllipse(hx - 82, hy - 84, 12, 5, tanSh);
  c.fillEllipse(hx + 82, hy - 84, 12, 5, tanSh);
  c.fillEllipse(hx - 82, hy - 85, 10, 3, tan);
  c.fillEllipse(hx + 82, hy - 85, 10, 3, tan);
  // Crown
  c.fillRoundRect(hx - 28, hy - 112, 56, 32, 8, tan);
  c.drawRoundRect(hx - 28, hy - 112, 56, 32, 8, tanSh);
  // Center crease (front pinch)
  c.drawFastVLine(hx, hy - 110, 28, tanSh);
  c.drawFastVLine(hx + 1, hy - 110, 28, tanSh);
  // Side dimples
  c.fillCircle(hx - 16, hy - 100, 3, tanSh);
  c.fillCircle(hx + 16, hy - 100, 3, tanSh);
  // Band
  c.fillRect(hx - 28, hy - 90, 56, 5, band);
  // Small bronze star buckle
  uint16_t star = c.color565(220, 180,  60);
  c.fillCircle(hx, hy - 88, 3, star);
  c.fillCircle(hx, hy - 88, 2, c.color565(255, 230, 130));
}

void drawAccessorySpaceHelmet(M5Canvas& c, int hx, int hy) {
  uint16_t glass     = c.color565(160, 210, 235);
  uint16_t glassDeep = c.color565(100, 165, 210);
  uint16_t metal     = c.color565(190, 195, 210);
  uint16_t metalSh   = c.color565(110, 115, 135);
  uint16_t antenna   = c.color565(210,  60,  60);

  // Helmet bubble — outline only so the face shows through.
  c.drawEllipse(hx, hy - 4, HEAD_RX +  6, HEAD_RY +  9, glassDeep);
  c.drawEllipse(hx, hy - 4, HEAD_RX +  7, HEAD_RY + 10, glass);
  c.drawEllipse(hx, hy - 4, HEAD_RX +  8, HEAD_RY + 11, glassDeep);

  // Reflection arc on the upper-left of the dome
  for (int i = 0; i < 14; ++i) {
    float a = -2.4f + i * 0.06f;
    int rx = hx + (int)(cosf(a) * (HEAD_RX + 7));
    int ry = hy - 4 + (int)(sinf(a) * (HEAD_RY + 10));
    c.drawPixel(rx, ry, c.color565(255, 255, 255));
  }
  for (int i = 0; i < 8; ++i) {
    float a = -2.0f + i * 0.04f;
    int rx = hx + (int)(cosf(a) * (HEAD_RX + 5));
    int ry = hy - 4 + (int)(sinf(a) * (HEAD_RY + 8));
    c.drawPixel(rx, ry, c.color565(220, 240, 255));
  }

  // Metallic neck ring
  c.fillRoundRect(hx - 70, hy + 80, 140, 14, 4, metal);
  c.drawRoundRect(hx - 70, hy + 80, 140, 14, 4, metalSh);
  c.drawFastHLine(hx - 68, hy + 84, 136, metalSh);
  // Latches
  c.fillCircle(hx - 60, hy + 87, 2, metalSh);
  c.fillCircle(hx + 60, hy + 87, 2, metalSh);

  // Antenna
  int ax = hx + 48;
  int ay = hy - HEAD_RY - 14;
  c.drawFastVLine(ax,     ay, 14, metalSh);
  c.drawFastVLine(ax + 1, ay, 14, metalSh);
  c.fillCircle(ax,     ay - 2, 4, antenna);
  c.fillCircle(ax - 1, ay - 3, 1, c.color565(255, 200, 200));
}

void drawAccessoryTopHat(M5Canvas& c, int hx, int hy) {
  uint16_t black     = c.color565( 22,  22,  28);
  uint16_t deepBlack = c.color565(  5,   5,  10);
  uint16_t hi        = c.color565( 65,  65,  80);
  uint16_t band      = c.color565(160,  35,  45);    // dark red ribbon
  uint16_t bandSh    = c.color565( 90,  20,  25);
  // Brim
  c.fillEllipse(hx, hy - 80, 60, 6, black);
  c.drawEllipse(hx, hy - 80, 60, 6, deepBlack);
  c.drawEllipse(hx, hy - 80, 59, 5, hi);
  // Crown body
  c.fillRect(hx - 26, hy - 122, 52, 42, black);
  c.drawRect(hx - 26, hy - 122, 52, 42, deepBlack);
  // Top of crown (subtle ellipse)
  c.fillEllipse(hx, hy - 122, 26, 4, black);
  c.drawEllipse(hx, hy - 122, 26, 4, hi);
  // Side highlight
  c.drawFastVLine(hx - 22, hy - 116, 34, hi);
  // Ribbon band near the brim
  c.fillRect(hx - 26, hy - 88, 52, 7, band);
  c.drawRect(hx - 26, hy - 88, 52, 7, bandSh);
  // Tiny buckle / pin
  c.fillRect(hx - 4, hy - 87, 8, 5, c.color565(220, 180, 70));
  c.drawRect(hx - 4, hy - 87, 8, 5, c.color565(150, 110, 30));
}

void drawAccessoryForScene(M5Canvas& c, Scene s, int hx, int hy) {
  switch (s) {
    case Scene::Meadow:  drawAccessoryFlowerCrown (c, hx, hy); break;
    case Scene::Bedroom: drawAccessoryNightcap    (c, hx, hy); break;
    case Scene::Forest:  drawAccessoryRangerHat   (c, hx, hy); break;
    case Scene::Beach:   drawAccessorySunglasses  (c, hx, hy); break;
    case Scene::Desert:  drawAccessoryCowboyHat   (c, hx, hy); break;
    case Scene::Space:   drawAccessorySpaceHelmet (c, hx, hy); break;
    case Scene::City:    drawAccessoryTopHat      (c, hx, hy); break;
  }
}

}  // namespace

// ─── Parental lockout / bedtime visuals ─────────────────────────────────────

void drawLockoutScreen(M5Canvas& c, uint32_t remainingSec, uint32_t now_ms,
                       AnimalType animal) {
  c.fillSprite(c.color565(15, 18, 50));     // deep night blue

  // Twinkling stars
  for (int i = 0; i < 36; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 175;
    uint32_t phase = ((now_ms / 90) + (r >> 24)) & 0x7F;
    uint8_t bri = 90 + (phase < 64 ? phase : (127 - phase)) * 3;
    c.drawPixel(sx, sy, c.color565(bri, bri, (bri + 60) > 255 ? 255 : bri + 60));
    if ((r & 0x1F) == 0) {
      c.fillCircle(sx, sy, 1, c.color565(255, 255, 255));
    }
  }

  // Crescent moon top-right
  c.fillCircle(260, 50, 22, c.color565(255, 240, 200));
  c.fillCircle(252, 46, 20, c.color565(15, 18, 50));

  // Title
  c.setTextDatum(top_center);
  c.setTextSize(4);
  c.setTextColor(c.color565(255, 220, 180));
  c.drawString(tr(Str::LockoutTitle), 160, 30);
  c.setTextSize(2);
  c.setTextColor(c.color565(200, 200, 220));
  c.drawString(tr(Str::LockoutSubtitle), 160, 80);

  // Pet head (small, sleeping) — uses the saved animal style + ear shape.
  int px = 86, py = 145;
  AnimalStyle st = styleFor(animal, c);
  uint16_t body  = st.body;
  uint16_t shade = st.bodyShade;
  uint16_t inEar = st.innerEar;
  if (animal == AnimalType::Cat) {
    c.fillTriangle(px - 22, py - 8, px - 10, py - 8, px - 18, py - 24, body);
    c.fillTriangle(px + 10, py - 8, px + 22, py - 8, px + 18, py - 24, body);
    c.drawLine    (px - 22, py - 8, px - 18, py - 24, shade);
    c.drawLine    (px - 10, py - 8, px - 18, py - 24, shade);
    c.drawLine    (px + 10, py - 8, px + 18, py - 24, shade);
    c.drawLine    (px + 22, py - 8, px + 18, py - 24, shade);
    c.fillTriangle(px - 19, py - 10, px - 13, py - 10, px - 18, py - 19, inEar);
    c.fillTriangle(px + 13, py - 10, px + 19, py - 10, px + 18, py - 19, inEar);
  } else if (animal == AnimalType::Dog) {
    // Floppy ears hanging beside the head
    c.fillEllipse(px - 18, py + 4, 7, 14, body);
    c.fillEllipse(px + 18, py + 4, 7, 14, body);
    c.drawEllipse(px - 18, py + 4, 7, 14, shade);
    c.drawEllipse(px + 18, py + 4, 7, 14, shade);
    c.fillEllipse(px - 18, py + 6, 4, 9, inEar);
    c.fillEllipse(px + 18, py + 6, 4, 9, inEar);
  } else {
    // Bear — round teddy ears
    c.fillCircle(px - 14, py - 14, 8, body);
    c.fillCircle(px + 14, py - 14, 8, body);
    c.fillCircle(px - 14, py - 14, 4, inEar);
    c.fillCircle(px + 14, py - 14, 4, inEar);
  }
  c.fillEllipse(px, py, 28, 24, body);
  c.drawEllipse(px, py, 28, 24, shade);
  // Closed eyes (curves)
  c.drawLine(px - 12, py - 4, px - 5, py - 4, shade);
  c.drawLine(px - 11, py - 5, px - 6, py - 5, shade);
  c.drawLine(px +  5, py - 4, px + 12, py - 4, shade);
  c.drawLine(px +  6, py - 5, px + 11, py - 5, shade);
  // Sleepy mouth
  c.fillEllipse(px, py + 8, 4, 2, st.muzzle);
  c.fillCircle (px, py + 6, 1, st.nose);
  // Cat whiskers
  if (animal == AnimalType::Cat) {
    c.drawFastHLine(px - 32, py + 1, 8, shade);
    c.drawFastHLine(px - 32, py + 5, 8, shade);
    c.drawFastHLine(px + 24, py + 1, 8, shade);
    c.drawFastHLine(px + 24, py + 5, 8, shade);
  }

  // Drifting Z's
  for (int i = 0; i < 3; ++i) {
    int phase = (now_ms / 30 + i * 600) % 1800;
    int zy = py - 20 - phase / 14;
    int zx = px + 28 + i * 9 + (int)(sinf(phase / 200.0f) * 4.0f);
    if (zy < 80) continue;
    uint8_t a = (phase > 1300) ? (uint8_t)((1800 - phase) * 255 / 500) : 255;
    c.setTextSize(i == 0 ? 3 : (i == 1 ? 2 : 1));
    c.setTextColor(c.color565((180 * a) / 255, (200 * a) / 255, (240 * a) / 255));
    c.setTextDatum(middle_center);
    c.drawString("Z", zx, zy);
  }
  c.setTextDatum(top_center);

  // Remaining time — large + accent
  char buf[32];
  uint32_t mins = (remainingSec + 59) / 60;     // round up so "1 sec" still says "1 min"
  if (remainingSec < 60) {
    snprintf(buf, sizeof(buf), tr(Str::LockoutRemainingSec),
             (unsigned)remainingSec);
  } else {
    snprintf(buf, sizeof(buf), tr(Str::LockoutRemainingMin),
             (unsigned)mins);
  }
  c.setTextSize(3);
  c.setTextColor(c.color565(255, 200, 100));
  c.drawString(buf, 160, 175);

  c.setTextSize(1);
  c.setTextColor(c.color565(150, 160, 200));
  c.drawString(tr(Str::LockoutHint), 160, 220);
  c.setTextDatum(top_left);
}

void drawBedtimeAnnouncement(M5Canvas& c, uint32_t elapsedMs) {
  // Centered translucent panel — uses full background dim (alternating
  // pixels) so the live pet view shows through.
  for (int y = 0; y < 240; y += 2) {
    c.drawFastHLine(0, y, 320, c.color565(0, 0, 0));
  }
  uint16_t panel  = c.color565( 35,  30,  60);
  uint16_t border = c.color565(150, 130, 200);
  c.fillRoundRect(20, 50, 280, 140, 14, panel);
  c.drawRoundRect(20, 50, 280, 140, 14, border);
  c.drawRoundRect(21, 51, 278, 138, 13, border);

  // Title fades in
  uint8_t a = elapsedMs < 700 ? (uint8_t)(elapsedMs * 255 / 700) : 255;
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(c.color565((255 * a) / 255,
                             (220 * a) / 255,
                             (180 * a) / 255));
  c.drawString(tr(Str::BedtimeTitle), 160, 70);

  // Subtitle (delayed fade-in)
  if (elapsedMs > 800) {
    uint32_t e2 = elapsedMs - 800;
    uint8_t a2 = e2 < 700 ? (uint8_t)(e2 * 255 / 700) : 255;
    c.setTextSize(2);
    c.setTextColor(c.color565((220 * a2) / 255,
                               (210 * a2) / 255,
                               (240 * a2) / 255));
    c.drawString(tr(Str::BedtimeSubtitle), 160, 115);
    c.drawString("...", 160, 142);
  }

  // Soft Z floating up on the right edge of the panel
  int zPhase = (elapsedMs / 25) % 90;
  c.setTextSize(2);
  c.setTextColor(c.color565(180, 200, 240));
  c.drawString("Z", 250 + (int)(sinf(zPhase / 14.0f) * 4),
                 130 - zPhase);
  c.setTextDatum(top_left);
}

void drawBedtimeSleepAnim(M5Canvas& c, uint32_t elapsedMs, AnimalType animal) {
  // Background gets darker over the 6 seconds.
  float t = (float)elapsedMs / 6000.0f;
  if (t > 1.0f) t = 1.0f;
  uint8_t r = (uint8_t)(40 - t * 35);
  uint8_t g = (uint8_t)(35 - t * 30);
  uint8_t b = (uint8_t)(75 - t * 65);
  c.fillSprite(c.color565(r, g, b));

  // Stars come in as the screen darkens
  if (t > 0.3f) {
    int starCount = (int)(t * 50);
    for (int i = 0; i < starCount; ++i) {
      uint32_t rr = (uint32_t)i * 2654435761u;
      int sx = (rr >> 8)  % 320;
      int sy = (rr >> 18) % 200;
      uint8_t bri = 100 + (((elapsedMs / 100) + i) & 0x3F) * 3;
      c.drawPixel(sx, sy, c.color565(bri, bri, bri));
    }
  }

  // Sleeping pet centered, gentle bob
  int px = 160, py = 130;
  int bob = (int)(sinf(elapsedMs / 800.0f) * 3.0f);
  int hy = py + bob;

  AnimalStyle s = styleFor(animal, c);
  uint16_t bodyCol = s.body;
  uint16_t shade   = s.bodyShade;
  uint16_t inEar   = s.innerEar;

  // Per-animal ears so each pet looks distinct even while sleeping.
  if (animal == AnimalType::Cat) {
    // Tall triangular ears
    c.fillTriangle(px - 36, hy - 8, px - 18, hy - 8, px - 30, hy - 36, bodyCol);
    c.fillTriangle(px + 18, hy - 8, px + 36, hy - 8, px + 30, hy - 36, bodyCol);
    c.drawLine    (px - 36, hy - 8, px - 30, hy - 36, shade);
    c.drawLine    (px - 18, hy - 8, px - 30, hy - 36, shade);
    c.drawLine    (px + 18, hy - 8, px + 30, hy - 36, shade);
    c.drawLine    (px + 36, hy - 8, px + 30, hy - 36, shade);
    c.fillTriangle(px - 32, hy - 11, px - 22, hy - 11, px - 30, hy - 28, inEar);
    c.fillTriangle(px + 22, hy - 11, px + 32, hy - 11, px + 30, hy - 28, inEar);
  } else if (animal == AnimalType::Dog) {
    // Floppy droopy ears hanging down past the head
    c.fillEllipse(px - 32, hy + 4, 11, 22, bodyCol);
    c.fillEllipse(px + 32, hy + 4, 11, 22, bodyCol);
    c.drawEllipse(px - 32, hy + 4, 11, 22, shade);
    c.drawEllipse(px + 32, hy + 4, 11, 22, shade);
    c.fillEllipse(px - 32, hy + 8, 6, 14, inEar);
    c.fillEllipse(px + 32, hy + 8, 6, 14, inEar);
  } else {
    // Bear — round teddy ears (default)
    c.fillCircle(px - 24, hy - 24, 12, bodyCol);
    c.fillCircle(px + 24, hy - 24, 12, bodyCol);
    c.fillCircle(px - 24, hy - 24,  6, inEar);
    c.fillCircle(px + 24, hy - 24,  6, inEar);
  }

  c.fillEllipse(px, hy, 50, 42, bodyCol);
  c.drawEllipse(px, hy, 50, 42, shade);

  // Whiskers (cat only) flickering with the bob
  if (animal == AnimalType::Cat) {
    c.drawFastHLine(px - 60, hy + 2, 12, shade);
    c.drawFastHLine(px - 60, hy + 8, 12, shade);
    c.drawFastHLine(px + 48, hy + 2, 12, shade);
    c.drawFastHLine(px + 48, hy + 8, 12, shade);
  }

  // Closed-eye lines
  c.drawLine(px - 22, hy - 10, px - 8, hy - 10, shade);
  c.drawLine(px - 21, hy - 11, px - 9, hy - 11, shade);
  c.drawLine(px +  8, hy - 10, px + 22, hy - 10, shade);
  c.drawLine(px +  9, hy - 11, px + 21, hy - 11, shade);
  // Soft mouth
  c.fillEllipse(px, hy + 18, 6, 4, s.muzzle);
  c.fillCircle(px, hy + 16, 1, s.nose);

  // Cascade of Z's
  for (int i = 0; i < 6; ++i) {
    int phase = (elapsedMs / 22 + i * 280) % 1800;
    int zy = hy - 44 - phase / 18;
    int zx = px + 30 + i * 7 + (int)(sinf(phase / 220.0f) * 5);
    if (zy < 10) continue;
    uint8_t a = (phase > 1300) ? (uint8_t)((1800 - phase) * 255 / 500) : 255;
    int sz = (i < 2) ? 3 : (i < 4 ? 2 : 1);
    c.setTextSize(sz);
    c.setTextColor(c.color565((180 * a) / 255,
                               (200 * a) / 255,
                               (240 * a) / 255));
    c.setTextDatum(middle_center);
    c.drawString("Z", zx, zy);
  }

  // "Gute Nacht!" appears mid-animation
  if (elapsedMs > 2000) {
    uint32_t e2 = elapsedMs - 2000;
    uint8_t a = e2 < 800 ? (uint8_t)(e2 * 255 / 800) : 255;
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565((255 * a) / 255,
                               (220 * a) / 255,
                               (160 * a) / 255));
    c.drawString(tr(Str::BedtimeGoodnight), 160, 30);
  }
  c.setTextDatum(top_left);
}

// Public — referenced from main.cpp for the activity-button hit-test.
bool sceneHasActivity(Scene s) {
  // All seven scenes have a mini-game.
  (void)s;
  return true;
}

// ─── Public renderer ────────────────────────────────────────────────────────

void drawFace(M5Canvas& canvas, const PetView& v) {
  setRenderAnimal(v.animal, canvas);
  drawSceneBackground(canvas, v.scene, v.phase, v.now_ms, v.weather,
                      v.moonPhase255);
  // Weather is rendered through the window for indoor scenes — skip the
  // full-screen overlay there so rain doesn't fall on the bedroom floor.
  if (v.scene != Scene::Bedroom) {
    drawWeatherOverlay(canvas, v.weather, v.now_ms);
  }

  int hx = CX + (int)v.tiltX + (int)v.wanderX;
  int hy = CY + (int)v.tiltY + (int)v.wanderY;

  // Listen-mode dance offset — adds rhythm on top of any tilt/wander.
  if (v.listenMode) {
    applyDanceOffset(hx, hy, v.now_ms, v.micLevel);
  }

  // Webradio body-sway — sanftes Schwingen ~0,5 Hz (~120 BPM-ish), ±3 px
  // horizontal. Spielt sich im Hintergrund parallel zu allen anderen
  // Tilt-/Wander-Effekten ab.
  if (v.mediaActive == Media::Radio) {
    float ph = (float)(v.now_ms % 2000) / 2000.0f;
    hx += (int)(sinf(ph * 6.283f) * 3.0f);
  }

  // Shake-off wobble — kicks in during the third phase of the rain cycle.
  // Computed before the head clamp so it stays within bounds.
  if (v.rainStartMs != 0) {
    uint32_t rage = v.now_ms - v.rainStartMs;
    if (rage >= 2800 && rage < 3700) {
      float t = (rage - 2800) / 900.0f;
      // Rapid 5-cycle horizontal vibration that decays toward the end.
      float decay = 1.0f - t;
      int wobble = (int)(10.0f * sinf(t * 10.0f * (float)PI) * decay);
      hx += wobble;
    }
  }

  // Side-shake giggle wobble — fast decaying horizontal sin-wave triggered
  // when the device is propped up on its side.
  if (v.sideShakeStartMs != 0) {
    uint32_t age = v.now_ms - v.sideShakeStartMs;
    if (age < 1500) {
      float t = (float)age / 1500.0f;
      float decay = 1.0f - t;
      hx += (int)(sinf(age / 60.0f) * 9.0f * decay);
      hy += (int)(cosf(age / 50.0f) * 4.0f * decay);
    }
  }

  // Somersault — pet hops along a tight loop and spin lines fan out around
  // it. Triggered by the user drawing a circle on the screen.
  bool somersaulting = false;
  float somersaultPhase = 0.0f;
  if (v.somersaultStartMs != 0) {
    uint32_t age = v.now_ms - v.somersaultStartMs;
    if (age < 800) {
      somersaulting   = true;
      somersaultPhase = (float)age / 800.0f;     // 0..1
      // Vertical parabola — peaks ~35 px above resting position.
      hy -= (int)(sinf(somersaultPhase * (float)PI) * 35.0f);
      // Loop sway — one full sideways cycle.
      hx += (int)(sinf(somersaultPhase * 2.0f * (float)PI) * 16.0f);
    }
  }

  // Snap-back wobble after the user releases a stretched pinch — damped
  // horizontal sin like a rubber band springing back to rest.
  if (v.snapStartMs != 0) {
    uint32_t age = v.now_ms - v.snapStartMs;
    if (age < 700) {
      float t = (float)age / 700.0f;
      float decay = 1.0f - t;
      hx += (int)(sinf(age / 35.0f) * 18.0f * decay);
      hy += (int)(cosf(age / 50.0f) *  4.0f * decay);
    }
  }

  // Singing — rhythmic sway during the 8 s singing animation. Combines a
  // wide horizontal sin with a smaller vertical bob to read as performing.
  if (v.singStartMs != 0) {
    uint32_t age = v.now_ms - v.singStartMs;
    if (age < 8000) {
      hx += (int)(sinf(age / 220.0f) * 11.0f);
      hy += (int)(cosf(age / 320.0f) *  4.0f);
    }
  }

  // (Friends-Playback-Overlays werden weiter unten gezeichnet — nach
  // drawFloatingIcons — damit sie sicher VOR dem Pet sitzen, nicht
  // dahinter. Vorher waren sie hier oben und vom Pet-Body verdeckt.)

  // Hop chain — `hopChainCount` parabolic hops, each 400 ms, after a
  // rapid-tap burst. Pure visual; sounds & hearts are dispatched in the
  // update loop on each hop transition.
  if (v.hopChainCount > 0 && v.hopChainStartMs != 0) {
    uint32_t elapsed = v.now_ms - v.hopChainStartMs;
    uint32_t total   = (uint32_t)v.hopChainCount * 400u;
    if (elapsed < total) {
      float t = (float)(elapsed % 400u) / 400.0f;
      hy -= (int)(sinf(t * (float)PI) * 26.0f);
    }
  }

  // Hand-warming — small body lean toward the finger midpoint while
  // warming up. Snuggle phase keeps the same lean (so the pet stays
  // cuddled in) plus a slow gentle sway.
  if (v.warmHoldActive || v.snuggleActive) {
    float pull = v.warmHoldActive ? v.warmPhase : 1.0f;
    hx += (int)((float)(v.warmMidX - hx) * pull * 0.06f);
    hy += (int)((float)(v.warmMidY - hy) * pull * 0.04f);
    if (v.snuggleActive) {
      hx += (int)(sinf(v.now_ms / 400.0f) * 1.5f);
    }
  }

  // Toy play — pet hops in time with the toy bouncing. Skipped when bored.
  bool toyPlaying = false;
  float toyT = 0.0f;
  if (v.toyPlayStartMs != 0 && v.toyPlayDurMs > 0) {
    uint32_t age = v.now_ms - v.toyPlayStartMs;
    if (age < v.toyPlayDurMs) {
      toyPlaying = true;
      toyT = (float)age / (float)v.toyPlayDurMs;
      if (!v.toyPlayBored) hy += petHopOffsetAt(toyT);
    }
  }
  // Hard clamp so the head ellipse + ears never leave the canvas — wander
  // shouldn't, but stacking with the tilt wobble could nudge it past the
  // edge by a few pixels.
  if (hx < HEAD_RX)              hx = HEAD_RX;
  if (hx > CANVAS_W - HEAD_RX)   hx = CANVAS_W - HEAD_RX;
  if (hy < HEAD_RY + 12)         hy = HEAD_RY + 12;
  if (hy > CANVAS_H - HEAD_RY)   hy = CANVAS_H - HEAD_RY;

  uint32_t microAge = v.now_ms - v.microStartMs;
  if (v.micro == MicroAnim::Hiccup && microAge < 300) {
    float t = (float)microAge / 300.0f;
    hy -= (int)(sinf(t * (float)PI) * 6.0f);
  }
  if (v.micro == MicroAnim::Sneeze && microAge < 500) {
    float t = (float)microAge / 500.0f;
    hy += (int)(sinf(t * (float)PI) * 4.0f);
  }

  uint32_t period = (v.face == Face::Sleeping) ? 3000 : 2000;
  float phase = (float)(v.now_ms % period) / (float)period;
  int breath = (int)(2.0f * sinf(phase * 2.0f * (float)PI));

  // Piles in the landscape — drawn BEFORE the pet so the pet body covers
  // any pile that's directly behind it. Lateral piles peek out beside.
  if (v.pileCount > 0 && v.pileX && v.pileY) {
    for (int i = 0; i < v.pileCount; ++i) {
      drawPile(canvas, v.pileX[i], v.pileY[i]);
    }
  }

  drawHead(canvas, hx, hy, breath, v.micro, microAge,
           v.listenMode, v.micLevel, v.now_ms, v.animal);

  if (v.dirty) drawDirt(canvas, hx, hy);
  // Subtle, always-visible cheeks; per-face renderers add a stronger
  // blush on top when the pet gets actually happy/excited.
  drawSoftBlush(canvas, hx, hy);

  if (v.micro == MicroAnim::Sneeze && microAge < 350) {
    drawEyeSquint(canvas, hx - EYE_DX, hy + EYE_OFF_Y);
    drawEyeSquint(canvas, hx + EYE_DX, hy + EYE_OFF_Y);
    drawMouthO(canvas, hx, hy + MOUTH_OFF_Y, 10);
    drawSneezePuff(canvas, hx, hy, microAge);
  } else {
    Face f = v.face;
#if SCREENSHOT_MODE
    // The animals cycle wants neutral, eyes-open frames for every animal
    // regardless of the pet's current mood. Forcing Idle routes through
    // drawIdle, which checks g_forceEyesOpen to suppress blinking too.
    if (screenshot::g_forceEyesOpen) f = Face::Idle;
#endif
    switch (f) {
      case Face::Idle:     drawIdle    (canvas, hx, hy, v); break;
      case Face::Happy:    drawHappy   (canvas, hx, hy, v); break;
      case Face::Excited:  drawExcited (canvas, hx, hy, v); break;
      case Face::Love:     drawLove    (canvas, hx, hy, v); break;
      case Face::Sleepy:   drawSleepy  (canvas, hx, hy, v); break;
      case Face::Sleeping: drawSleeping(canvas, hx, hy, v); break;
      case Face::Startled: drawStartled(canvas, hx, hy, v); break;
      case Face::Sad:      drawSad     (canvas, hx, hy, v); break;
      case Face::Laughing: drawLaughing(canvas, hx, hy, v); break;
      case Face::Eating:   drawEating  (canvas, hx, hy, v); break;
      case Face::Speaking: drawSpeaking(canvas, hx, hy, v); break;
    }
  }

  // Media override — square sunken eyes painted on top of whatever the
  // per-face renderer drew. Mouth stays whatever the mood/face dictates so
  // a tired pet still has its sad mouth, etc.
  // Radio is excluded — neutral / positive activity, pet stays on its
  // current mood face (Excited during the stream via the
  // applyVoiceTag / decay path).
  if (v.mediaActive != Media::None && v.mediaActive != Media::Radio &&
      v.face != Face::Sleeping) {
    drawMediaEyes(canvas, hx, hy, v.now_ms);
  }

  // Singing — overdraw a pulsing open-mouth oval at the muzzle so the pet
  // appears to be singing along with the audio.
  if (v.singStartMs != 0) {
    uint32_t age = v.now_ms - v.singStartMs;
    if (age < 8000) {
      // Open-close cycle ~480 ms (~125 BPM).
      float ph = (float)(age % 480) / 480.0f;
      float open = 0.5f - 0.5f * cosf(ph * 2.0f * (float)PI);   // 0..1..0
      int   ry   = 1 + (int)(open * 6.0f);
      int   rx   = 4 + (int)(open * 3.0f);
      uint16_t mouth   = canvas.color565( 90, 30, 50);
      uint16_t tongue  = canvas.color565(220, 90, 110);
      canvas.fillEllipse(hx, hy + 22, rx, ry, mouth);
      if (ry >= 4) {
        canvas.fillEllipse(hx, hy + 22 + ry/2, rx - 2, ry / 2, tongue);
      }
    }
  }

  // Applause listening — bright red cheeks (shy "did I do well?" pose).
  // The face is already overridden to Sleepy in main.cpp so eyes are shut.
  if (v.applauseListenStartMs != 0) {
    uint32_t age = v.now_ms - v.applauseListenStartMs;
    if (age < 5000) {
      // Cheeks pulse subtly to feel alive.
      float pulse  = 0.85f + 0.15f * sinf(v.now_ms / 240.0f);
      uint8_t r = (uint8_t)(255 * pulse);
      uint8_t g = (uint8_t)( 80 * pulse);
      uint8_t b = (uint8_t)(110 * pulse);
      uint16_t blush = canvas.color565(r, g, b);
      canvas.fillEllipse(hx - 56, hy + 14, 18, 10, blush);
      canvas.fillEllipse(hx + 56, hy + 14, 18, 10, blush);
      // Subtle inner highlight to give the cheek some volume.
      uint16_t hi = canvas.color565(255, 180, 200);
      canvas.fillEllipse(hx - 60, hy + 11,  6, 3, hi);
      canvas.fillEllipse(hx + 52, hy + 11,  6, 3, hi);
    }
  }

  // Scene-derived accessory (auto-mode: outfit changes when the user sends
  // the pet to a different scene). Drawn over face/media so it sits on top
  // of everything the head shows.
  drawAccessoryForScene(canvas, v.scene, hx, hy);

  drawBubble(canvas, v, hx, hy);

  // Floating action icons fly between pet and bars/gear, drawn just below them.
  drawFloatingIcons(canvas, v.floats, v.now_ms);

  // ── Friends mode playback overlays ──────────────────────────────────────
  // Drawn AFTER the pet on purpose so the sender->item panel on top and
  // the gift bar below sit visibly IN FRONT of the pet. Previously they
  // were drawn at the top of drawFace and got covered by the pet's body.

  // Sender->item panel: small modal over the pet, shows
  // "<sender-pet> -> <item-icon>" for the playback item currently shown.
  if (v.friendsPlaybackKind != 0) {
    uint32_t age = v.now_ms - v.friendsPlaybackItemStartMs;
    constexpr uint32_t kFadeIn = 200, kHold = 700, kFadeOut = 200;
    if (age < kFadeIn + kHold + kFadeOut) {
      float alpha = 1.0f;
      if (age < kFadeIn)               alpha = (float)age / kFadeIn;
      else if (age > kFadeIn + kHold)  alpha = 1.0f -
        (float)(age - kFadeIn - kHold) / kFadeOut;
      uint8_t a = (uint8_t)(alpha * 255.0f);
      uint16_t panel  = canvas.color565(255 * a / 255,
                                        245 * a / 255,
                                        220 * a / 255);
      uint16_t border = canvas.color565(120 * a / 255,
                                         90 * a / 255,
                                         60 * a / 255);
      int px = 80, py = 30, pw = 160, ph = 46;
      canvas.fillRoundRect(px, py, pw, ph, 10, panel);
      canvas.drawRoundRect(px, py, pw, ph, 10, border);
      drawTinyPet(canvas, px + 26, py + ph / 2 + 4,
                  v.friendsPlaybackAnimal, 0, true);
      canvas.fillTriangle(px + 60, py + ph/2 - 6,
                          px + 60, py + ph/2 + 6,
                          px + 70, py + ph/2, border);
      canvas.fillRect(px + 56, py + ph/2 - 2, 6, 4, border);
      drawItemIcon(canvas, v.friendsPlaybackKind,
                   px + pw - 30, py + ph/2 + 2, 14);
    }
  }

  // Geschenk-Leiste am unteren Bildschirmrand: alle empfangenen Items
  // als Reihe vor dem Pet, der aktuell abgespielte Slot heller umrandet
  // und gepulst.
  if (v.friendsPlaybackTotal > 0) {
    constexpr int kSlot = 36;
    constexpr int kGap  = 4;
    int total = v.friendsPlaybackTotal;
    int barW  = total * kSlot + (total - 1) * kGap;
    int barX  = (320 - barW) / 2;
    int barY  = 200;
    int barH  = 34;

    uint16_t panelBg     = canvas.color565(245, 230, 200);
    uint16_t panelBorder = canvas.color565(150, 110,  60);
    canvas.fillRoundRect(barX - 6, barY - 4, barW + 12, barH + 4, 9, panelBg);
    canvas.drawRoundRect(barX - 6, barY - 4, barW + 12, barH + 4, 9, panelBorder);

    int activeSlot = -1;
    if (v.friendsPlaybackKind != 0 && v.friendsPlaybackIdxView >= 1 &&
        v.friendsPlaybackIdxView <= total) {
      activeSlot = v.friendsPlaybackIdxView - 1;
    }

    for (int i = 0; i < total; ++i) {
      int sx = barX + i * (kSlot + kGap);
      int sy = barY;
      bool isActive = (i == activeSlot);
      uint16_t slotBg     = isActive ? canvas.color565(255, 250, 220)
                                     : canvas.color565(225, 210, 175);
      uint16_t slotBorder = isActive ? canvas.color565(255, 180,  60)
                                     : canvas.color565(180, 150, 100);
      canvas.fillRoundRect(sx, sy, kSlot, kSlot - 4, 6, slotBg);
      canvas.drawRoundRect(sx, sy, kSlot, kSlot - 4, 6, slotBorder);
      if (isActive) {
        float pulse = 0.5f + 0.5f * sinf(v.now_ms / 180.0f);
        if (pulse > 0.4f) {
          canvas.drawRoundRect(sx - 2, sy - 2, kSlot + 4, kSlot, 7,
                               slotBorder);
        }
      }
      drawItemIcon(canvas, v.friendsPlaybackKinds[i],
                   sx + kSlot / 2, sy + (kSlot - 4) / 2, 10);
    }
  }

  // ── Rain shower phases ────────────────────────────────────────────────
  if (v.rainStartMs != 0) {
    uint32_t rage = v.now_ms - v.rainStartMs;
    // Phase 1: rain falling (0..2200 ms). Drawn over the pet so droplets
    // visually pass in front of the head — like real rain.
    if (rage < 2200) drawRain(canvas, rage);
    // Phase 2: wet droplets perched on the head (1500..3000 ms).
    if (rage > 1500 && rage < 3000) {
      drawWetDroplets(canvas, hx, hy, rage - 1500);
    }
    // Phase 3: spray flying outward from the shake-off (2800..3700 ms).
    if (rage >= 2800 && rage < 3700) {
      drawShakeOffDroplets(canvas, hx, hy, rage - 2800);
    }
  }

  // Toy sprite — drawn over the pet so it visibly flies past.
  if (toyPlaying) {
    ToyPos tp = toyPositionAt(v.toyPlayWhich, toyT, v.toyPlayFromLeft,
                              v.toyPlayBored);
    drawToy(canvas, v.toyPlayWhich, (int)tp.x, (int)tp.y,
            v.toyPlayFromLeft, v.now_ms);
  }

  // Somersault spin lines — three orbiting arcs that suggest rotation.
  // Drawn over the pet so the spin reads clearly even when hearts overlap.
  if (somersaulting) {
    uint16_t spinA = canvas.color565(255, 235, 120);
    uint16_t spinB = canvas.color565(255, 180,  80);
    float baseAng = somersaultPhase * 6.28318f * 2.0f;   // 2 full turns
    for (int i = 0; i < 3; ++i) {
      float ang = baseAng + i * 2.094f;                  // 120° spacing
      int rx = (int)(cosf(ang) * 38.0f);
      int ry = (int)(sinf(ang) * 28.0f);
      uint16_t col = (i & 1) ? spinB : spinA;
      canvas.fillCircle(hx + rx, hy + ry, 3, col);
      // Trailing dot (a fraction earlier in the rotation) for motion feel.
      float trail = ang - 0.6f;
      int tx = (int)(cosf(trail) * 38.0f);
      int ty = (int)(sinf(trail) * 28.0f);
      canvas.fillCircle(hx + tx, hy + ty, 2, col);
    }
  }

  // Cleaning shortcut — only visible when there's something to clean.
  drawCleaningButton(canvas, v.pileCount, v.now_ms);

  // Foraging shortcut — always available.
  bool basketHint = (int32_t)(v.basketHintUntilMs - v.now_ms) > 0;
  drawForagingButton(canvas, basketHint, v.now_ms);

  // Toy shortcut — opens the toy-select screen.
  drawToyButton(canvas);

  // Media (TV) shortcut — left column. Highlighted while a media session
  // is active, doubling as a stop button.
  drawMediaButton(canvas, v.mediaActive != Media::None);
  drawSportButton(canvas);

  // Travel (compass) shortcut — left column.
  drawTravelButton(canvas, v.travelCooldownSec, v.travelCanPlay);

  // Activity (mini-game) shortcut — only when current scene has a game.
  drawActivityButton(canvas, v.scene, v.activityCooldownSec, v.activityCanPlay);

  // Top strip: bars + gear (drawn last so they sit on top of everything).
  drawNeedBars(canvas, v);
  drawGearButton(canvas);
}

// ─── Settings screen ────────────────────────────────────────────────────────

void drawSettingsScreen(M5Canvas& c, const SettingsView& s) {
  c.fillSprite(c.color565(30, 30, 40));

  uint16_t white  = c.color565(240, 240, 240);
  uint16_t panel  = c.color565(255, 240, 240);
  uint16_t glyph  = c.color565(60, 50, 50);
  uint16_t accent = c.color565(120, 200, 120);

  // Title
  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(white);
  c.setCursor(20, 10);
  c.print(tr(Str::SettingsTitle));

  // Back button (X)
  {
    const Rect& r = kSettingsBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  if (s.page == 0) {
    // ── Page 0: daily controls ─────────────────────────────────────────
    // Volume label + slider
    c.setTextSize(2);
    c.setTextColor(white);
    c.setCursor(40, 50);
    c.print(tr(Str::SettingsVolume));
    {
      const Rect& r = kSettingsVolumeRect;
      int knobX = r.x + (uint32_t)s.volume * r.w / 255;
      c.fillRoundRect(r.x, r.y, r.w, r.h, r.h/2, c.color565(220, 200, 200));
      int fillW = (uint32_t)s.volume * r.w / 255;
      if (fillW > 0) {
        uint16_t bar = (s.volume < 30)  ? c.color565(220, 100, 100)
                     : (s.volume < 100) ? c.color565(230, 180, 90)
                                        : accent;
        c.fillRoundRect(r.x, r.y, fillW, r.h, r.h/2, bar);
      }
      c.drawRoundRect(r.x, r.y, r.w, r.h, r.h/2, glyph);
      int knobR = r.h / 2 + 3;
      c.fillCircle(knobX, r.y + r.h/2, knobR, panel);
      c.drawCircle(knobX, r.y + r.h/2, knobR, glyph);
    }

    // Brightness label + 4 dots
    c.setTextSize(2);
    c.setTextColor(white);
    c.setCursor(40, 102);
    c.print(tr(Str::SettingsBrightness));

    static const char* labels[4] = { "25%", "50%", "75%", "100%" };
    for (int i = 0; i < 4; ++i) {
      const Rect& r = kBrightDotRect[i];
      bool active = ((int)s.brightnessLevel == i);
      uint16_t bg = active ? c.color565(255, 200, 60) : panel;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
      c.setTextDatum(middle_center);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(labels[i], r.x + r.w/2, r.y + r.h/2);
      c.setTextDatum(top_left);
    }

    // Anleitung row
    {
      const Rect& r = kSettingsHelpRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
      int icx = r.x + 22;
      int icy = r.y + r.h / 2;
      uint16_t a = c.color565(255, 150, 60);
      c.fillCircle(icx, icy, 9, a);
      c.setTextDatum(middle_center);
      c.setTextSize(2);
      c.setTextColor(c.color565(255, 255, 255));
      c.drawString("?", icx, icy + 1);
      c.setTextDatum(middle_left);
      c.setTextColor(glyph);
      c.drawString(tr(Str::SettingsHelp), r.x + 44, icy);
      c.setTextDatum(top_left);
      int ax = r.x + r.w - 18;
      c.fillTriangle(ax, icy - 8, ax, icy + 8, ax + 10, icy, glyph);
    }

    // Credits row
    {
      const Rect& r = kSettingsCreditsRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
      int icx = r.x + 22;
      int icy = r.y + r.h / 2;
      uint16_t heart = c.color565(220, 60, 110);
      c.fillCircle(icx - 3, icy - 1, 4, heart);
      c.fillCircle(icx + 3, icy - 1, 4, heart);
      c.fillTriangle(icx - 7, icy, icx + 7, icy, icx, icy + 7, heart);
      c.setTextDatum(middle_left);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(tr(Str::SettingsCredits), r.x + 44, icy);
      c.setTextDatum(top_left);
      int ax = r.x + r.w - 18;
      c.fillTriangle(ax, icy - 8, ax, icy + 8, ax + 10, icy, glyph);
    }
  } else if (s.page == 1) {
    // ── Page 1: Sprache / Uhrzeit / Tier / Reset ──────────────────────
    // Sprache row
    {
      const Rect& r = kSettingsLangRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
      int icx = r.x + 22;
      int icy = r.y + r.h / 2;
      // Mini globe icon
      uint16_t blue = c.color565( 60, 130, 200);
      c.fillCircle(icx, icy, 9, blue);
      c.drawCircle(icx, icy, 9, glyph);
      c.drawFastHLine(icx - 9, icy, 19, glyph);
      c.drawEllipse(icx, icy, 4, 9, glyph);
      // The globe icon already communicates "language", so we only show the
      // current value (in its own writing) — keeps the label inside the
      // 240-px row even with the longer "Language:" prefix in EN.
      static const char* langNames[2] = { "Deutsch", "English" };
      const char* langValue = (s.language < 2) ? langNames[s.language] : "Deutsch";
      c.setTextDatum(middle_left);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(langValue, r.x + 44, icy);
      c.setTextDatum(top_left);
      int ax = r.x + r.w - 18;
      c.fillTriangle(ax, icy - 8, ax, icy + 8, ax + 10, icy, glyph);
    }

    // Uhrzeit row
    {
      const Rect& r = kSettingsTimeRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
      int icx = r.x + 22;
      int icy = r.y + r.h / 2;
      c.drawCircle(icx, icy, 11, glyph);
      c.drawCircle(icx, icy, 10, glyph);
      c.drawLine(icx, icy, icx, icy - 7, glyph);
      c.drawLine(icx, icy, icx + 6, icy + 2, glyph);

      m5::rtc_datetime_t dt;
      char buf[32];
      if (M5.Rtc.getDateTime(&dt) && dt.date.year >= 2024) {
        snprintf(buf, sizeof(buf), "%s  %02d:%02d",
                 tr(Str::SettingsTimePrefix),
                 (int)dt.time.hours, (int)dt.time.minutes);
      } else {
        snprintf(buf, sizeof(buf), "%s", tr(Str::SettingsTimeUnset));
      }
      c.setTextDatum(middle_left);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(buf, r.x + 44, icy);
      c.setTextDatum(top_left);
      int ax = r.x + r.w - 18;
      c.fillTriangle(ax, icy - 8, ax, icy + 8, ax + 10, icy, glyph);
    }

    // Tier row
    {
      const Rect& r = kSettingsAnimalRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
      int icx = r.x + 22;
      int icy = r.y + r.h / 2;
      c.fillCircle(icx, icy + 2, 5, glyph);
      c.fillCircle(icx - 5, icy - 4, 2, glyph);
      c.fillCircle(icx - 2, icy - 7, 2, glyph);
      c.fillCircle(icx + 2, icy - 7, 2, glyph);
      c.fillCircle(icx + 5, icy - 4, 2, glyph);

      static const Str names[3] = {
        Str::AnimalBear, Str::AnimalCat, Str::AnimalDog
      };
      char buf[32];
      snprintf(buf, sizeof(buf), "%s  %s",
               tr(Str::SettingsAnimalPrefix),
               tr(names[(int)s.animal & 3]));
      c.setTextDatum(middle_left);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(buf, r.x + 44, icy);
      c.setTextDatum(top_left);
      int ax = r.x + r.w - 18;
      c.fillTriangle(ax, icy - 8, ax, icy + 8, ax + 10, icy, glyph);
    }

    // Reset row — visually separated, red, destructive.
    {
      const Rect& r = kSettingsResetRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, c.color565(180, 60, 60));
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, c.color565(255, 220, 220));
      c.setTextDatum(middle_center);
      c.setTextSize(2);
      c.setTextColor(c.color565(255, 255, 255));
      c.drawString(tr(Str::SettingsReset), r.x + r.w / 2, r.y + r.h / 2);
      c.setTextDatum(top_left);
    }
  }
  if (s.page == 2) {
    // ── Page 2: Network / WiFi + Parents help ──────────────────────────
    {
      const Rect& r = kSettingsWifiRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, glyph);
      int icx = r.x + 26, icy = r.y + r.h / 2;
      uint16_t wcol = c.color565( 70, 110, 200);
      c.fillCircle(icx, icy + 8, 3, wcol);
      c.drawCircle(icx, icy + 8, 8, wcol);
      c.drawCircle(icx, icy + 8, 13, wcol);
      c.fillRect(icx - 14, icy + 9, 28, 8, panel);
      c.setTextDatum(middle_left);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(tr(Str::SettingsWifi), r.x + 50, icy);
      c.setTextDatum(top_left);
    }
    // Parents-help row
    {
      const Rect& r = kSettingsParentsRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, glyph);
      int icx = r.x + 26, icy = r.y + r.h / 2;
      uint16_t pCol = c.color565( 90, 140,  90);
      c.fillCircle(icx - 5, icy - 6, 4, pCol);
      c.fillCircle(icx + 5, icy - 4, 4, pCol);
      c.fillTriangle(icx - 12, icy + 8, icx + 2, icy + 8, icx - 5, icy - 1, pCol);
      c.fillTriangle(icx -  2, icy + 8, icx + 12, icy + 8, icx + 5, icy + 1, pCol);
      c.setTextDatum(middle_left);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(tr(Str::SettingsParentsHelp), r.x + 50, icy);
      c.setTextDatum(top_left);
    }
    // Standort row — opens the Location sub-page.
    {
      const Rect& r = kSettingsLocationRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, glyph);
      // Mini globe icon
      int icx = r.x + 22, icy = r.y + r.h / 2;
      uint16_t blue  = c.color565( 80, 140, 220);
      uint16_t green = c.color565( 80, 170,  90);
      c.fillCircle(icx, icy, 9, blue);
      c.fillEllipse(icx - 2, icy - 1, 5, 3, green);
      c.fillEllipse(icx + 3, icy + 2, 4, 2, green);
      c.drawCircle(icx, icy, 9, glyph);
      c.setTextDatum(middle_left);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(tr(Str::SettingsLocation), r.x + 44, icy);
      // Chevron
      int ax = r.x + r.w - 14;
      c.fillTriangle(ax, icy - 7, ax, icy + 7, ax + 8, icy, glyph);
      c.setTextDatum(top_left);
    }
    // Parent-server row — opens a dedicated sub-page (toggle, status, IP
    // and battery hint live there so the label here doesn't get cramped).
    {
      const Rect& r = kSettingsParentSrvRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, glyph);
      int icx = r.x + 22, icy = r.y + r.h / 2;
      uint16_t iCol = c.color565( 70, 110, 200);
      c.fillRect(icx - 1, icy - 6, 3, 12, iCol);
      c.drawCircle(icx, icy - 6, 4, iCol);
      c.drawCircle(icx, icy - 6, 7, iCol);
      c.setTextDatum(middle_left);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(tr(Str::SettingsParentSrv), r.x + 44, icy);

      // Tiny status dot on the right side so the user gets a hint without
      // opening the sub-page.
      uint16_t dotCol;
      switch (s.parentSrvState) {
        case 1: dotCol = c.color565(230, 180, 60); break;
        case 2: dotCol = c.color565( 80, 180, 90); break;
        case 3: dotCol = c.color565(220,  80, 80); break;
        default: dotCol = c.color565(150, 150, 160); break;
      }
      c.fillCircle(r.x + r.w - 26, icy, 5, dotCol);
      // Chevron pointing right (sub-page affordance).
      int ax = r.x + r.w - 14;
      c.fillTriangle(ax, icy - 8, ax, icy + 8, ax + 8, icy, glyph);
      c.setTextDatum(top_left);
    }
    // Reset-WiFi row — destructive, red, separated.
    {
      const Rect& r = kSettingsWifiResetRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, c.color565(180, 60, 60));
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, c.color565(255, 220, 220));
      c.setTextDatum(middle_center);
      c.setTextSize(2);
      c.setTextColor(c.color565(255, 255, 255));
      c.drawString(tr(Str::SettingsWifiReset), r.x + r.w / 2, r.y + r.h / 2);
      c.setTextDatum(top_left);
    }
  }
  if (s.page == 3) {
    // ── Page 3: Companion devices (Pip-Modus toggle) ───────────────────
    // Layout (top to bottom):
    //   y=46   Title        size 3   "Pip-Modus"
    //   y=88   Body line 1  size 2   "Empfangt Geschenke"
    //   y=110  Body line 2  size 2   "von einem Pip."
    //   y=140  Battery hint size 2   "Kostet ~20% Akku."
    //   y=168  Toggle (40 high)
    // Earlier copy was a single 60-char DE sentence at size 1 — too small
    // and overflowed the 240 px content width.
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(glyph);
    c.drawString(tr(Str::PipModeTitle), 160, 46);

    c.setTextSize(2);
    c.setTextColor(c.color565(220, 220, 235));
    c.drawString(tr(Str::PipModeBody1), 160, 88);
    c.drawString(tr(Str::PipModeBody2), 160, 110);

    c.setTextColor(c.color565(170, 170, 195));
    c.drawString(tr(Str::PipModeBatteryHint), 160, 140);
    c.setTextDatum(top_left);

    // Big toggle button — green when ON, neutral when OFF.
    {
      const Rect& r = kSettingsPipModeRect;
      uint16_t btnBg = s.pipMode ? c.color565( 80, 170,  90)
                                  : c.color565( 80,  85, 110);
      uint16_t btnBd = s.pipMode ? c.color565(220, 255, 220)
                                  : c.color565(180, 180, 200);
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, btnBg);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, btnBd);
      c.setTextDatum(middle_center);
      c.setTextSize(2);
      c.setTextColor(c.color565(255, 255, 255));
      c.drawString(s.pipMode ? tr(Str::PipModeOn) : tr(Str::PipModeOff),
                   r.x + r.w / 2, r.y + r.h / 2);
      c.setTextDatum(top_left);
    }
  }

  // ── Page navigation strip at the bottom ───────────────────────────────
  uint16_t pageBg     = c.color565(80,  85, 110);
  uint16_t pageBgDis  = c.color565(50,  50,  60);
  uint16_t pageGlyph  = c.color565(60,  50,  50);

  bool prevOn = (s.page > 0);
  bool nextOn = (s.page < 3);
  // Prev button
  {
    const Rect& r = kSettingsPrevPageRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, prevOn ? pageBg : pageBgDis);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, pageGlyph);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(white);
    c.drawString("<", r.x + r.w/2, r.y + r.h/2);
    c.setTextDatum(top_left);
  }
  // Next button
  {
    const Rect& r = kSettingsNextPageRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, nextOn ? pageBg : pageBgDis);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, pageGlyph);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(white);
    c.drawString(">", r.x + r.w/2, r.y + r.h/2);
    c.setTextDatum(top_left);
  }
  // Page indicator centered between
  c.setTextDatum(middle_center);
  c.setTextSize(2);
  c.setTextColor(c.color565(180, 180, 200));
  char buf[8];
  snprintf(buf, sizeof(buf), "%u / 4", (unsigned)(s.page + 1));
  c.drawString(buf, 160, 216 + 11);
  c.setTextDatum(top_left);

  // ── Reset confirmation modal ───────────────────────────────────────────
  // Two flags share the same modal layout: pet-reset and WiFi-reset. The
  // title swaps; the buttons keep their generic JA / NEIN labels.
  if (s.resetConfirmOpen || s.wifiResetConfirmOpen) {
    // Dim the underlying page so it reads as inactive.
    c.fillRect(0, 0, 320, 240, c.color565(0, 0, 0));

    // Panel
    int px = 20, py = 50, pw = 280, ph = 150;
    c.fillRoundRect(px, py, pw, ph, 12, c.color565(40, 45, 60));
    c.drawRoundRect(px, py, pw, ph, 12, c.color565(255, 220, 220));

    // Warning icon (triangle with !)
    {
      int cx = 160, cy = py + 30;
      uint16_t warn = c.color565(255, 200, 80);
      uint16_t outl = c.color565(120, 80, 20);
      c.fillTriangle(cx - 18, cy + 14, cx + 18, cy + 14, cx, cy - 14, warn);
      c.drawTriangle(cx - 18, cy + 14, cx + 18, cy + 14, cx, cy - 14, outl);
      c.fillRect(cx - 1, cy - 7, 3, 12, outl);
      c.fillRect(cx - 1, cy + 8, 3, 3,  outl);
    }

    // Title — depends on which flag opened the modal.
    Str titleId = s.wifiResetConfirmOpen ? Str::WifiResetConfirmTitle
                                          : Str::SettingsResetConfirmTitle;
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(255, 255, 255));
    c.drawString(tr(titleId), 160, py + 70);
    c.setTextDatum(top_left);

    // Cancel button (grey, safe default)
    {
      const Rect& r = kSettingsResetConfirmNoRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, c.color565(90, 95, 115));
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, c.color565(220, 220, 235));
      c.setTextDatum(middle_center);
      c.setTextSize(2);
      c.setTextColor(c.color565(255, 255, 255));
      c.drawString(tr(Str::SettingsResetConfirmNo), r.x + r.w / 2, r.y + r.h / 2);
      c.setTextDatum(top_left);
    }

    // Confirm button (red, destructive)
    {
      const Rect& r = kSettingsResetConfirmYesRect;
      c.fillRoundRect(r.x, r.y, r.w, r.h, 10, c.color565(180, 60, 60));
      c.drawRoundRect(r.x, r.y, r.w, r.h, 10, c.color565(255, 220, 220));
      c.setTextDatum(middle_center);
      c.setTextSize(2);
      c.setTextColor(c.color565(255, 255, 255));
      c.drawString(tr(Str::SettingsResetConfirmYes), r.x + r.w / 2, r.y + r.h / 2);
      c.setTextDatum(top_left);
    }
  }
}

// ─── WiFi captive-portal setup screen ───────────────────────────────────

void drawWifiSetupScreen(M5Canvas& c, const WifiSetupView& v) {
  c.fillSprite(c.color565(28, 32, 50));
  // Star tint at the top, like the help/credits screens.
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  // Title
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(c.color565(255, 200, 100));
  c.drawString(tr(Str::WifiSetupTitle), 160, 8);
  c.setTextDatum(top_left);

  // Pulsing WiFi arc icon, centered around y=70
  {
    int icx = 160, icy = 76;
    float pulse = 0.6f + 0.4f * sinf(v.now_ms / 280.0f);
    uint8_t bri = (uint8_t)(140 + 100 * pulse);
    uint16_t arcA = c.color565(bri, bri, 255);
    uint16_t arcB = c.color565(bri * 3 / 4, bri * 3 / 4, 230);
    c.fillCircle(icx, icy + 12, 3, arcA);
    for (int rad = 8; rad <= 22; rad += 7) {
      uint16_t col = (rad < 16) ? arcA : arcB;
      c.drawCircle(icx, icy + 12, rad, col);
    }
    // Mask the bottom half so they read as arcs.
    c.fillRect(icx - 24, icy + 13, 48, 14, c.color565(28, 32, 50));
  }

  // Status block
  c.setTextDatum(top_center);
  c.setTextSize(2);

  uint16_t fg = c.color565(230, 230, 245);
  uint16_t accent = c.color565(160, 220, 255);
  uint16_t ok  = c.color565(100, 220, 130);
  uint16_t err = c.color565(240,  90, 100);

  // State enum values mirror WifiSetupState in net.h: Idle=0, Active=1,
  // Submitted=2, Connecting=3, Success=4, Failed=5.
  if (v.state == 1) {
    c.setTextColor(fg);
    c.drawString(tr(Str::WifiSetupHint1), 160, 110);
    c.setTextColor(accent);
    c.drawString(v.apName ? v.apName : TARGET_AP_NAME, 160, 130);
    c.setTextColor(fg);
    c.drawString(tr(Str::WifiSetupHint2), 160, 154);
    c.setTextColor(accent);
    c.drawString(v.apIp ? v.apIp : "192.168.4.1", 160, 174);
  } else if (v.state == 2 || v.state == 3) {
    c.setTextColor(fg);
    c.drawString(tr(Str::WifiSetupConnecting), 160, 130);
    if (v.pendingSsid && v.pendingSsid[0]) {
      c.setTextColor(accent);
      c.drawString(v.pendingSsid, 160, 156);
    }
  } else if (v.state == 4) {
    c.setTextColor(ok);
    c.setTextSize(3);
    c.drawString(tr(Str::WifiSetupSuccess), 160, 124);
    c.setTextSize(2);
    if (v.pendingSsid && v.pendingSsid[0]) {
      c.setTextColor(accent);
      c.drawString(v.pendingSsid, 160, 162);
    }
  } else if (v.state == 5) {
    c.setTextColor(err);
    c.setTextSize(3);
    c.drawString("!", 160, 116);
    c.setTextSize(2);
    c.drawString(tr(Str::WifiSetupFailed), 160, 156);
  }
  c.setTextDatum(top_left);

  // Bottom button — Cancel while Active/Submitted/Connecting, Done after Success/Failed.
  bool finished = (v.state == 4 || v.state == 5);
  {
    const Rect& r = kWifiSetupCancelRect;
    uint16_t bg = finished ? c.color565( 90, 160,  90) : c.color565(120,  80,  90);
    uint16_t bd = finished ? c.color565(200, 240, 200) : c.color565(255, 200, 200);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 8, bd);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(255, 255, 255));
    Str label = finished ? Str::WifiSetupDoneBtn : Str::WifiSetupCancelBtn;
    c.drawString(tr(label), r.x + r.w / 2, r.y + r.h / 2);
    c.setTextDatum(top_left);
  }
}

// ─── Parents-help screen ────────────────────────────────────────────────

void drawParentsHelpScreen(M5Canvas& c, uint32_t now_ms) {
  c.fillSprite(c.color565(28, 32, 50));
  // Soft star tint at the top.
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  // Title
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(c.color565(255, 200, 100));
  c.drawString(tr(Str::ParentsHelpTitle), 160, 8);
  c.setTextDatum(top_left);

  // Body — eight numbered lines, tighter spacing so the IP step fits.
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  c.setTextDatum(top_left);
  c.setTextSize(2);
  int y = 56;
  const Str lines[8] = {
    Str::ParentsHelpL1, Str::ParentsHelpL2,
    Str::ParentsHelpL3, Str::ParentsHelpL4,
    Str::ParentsHelpL5, Str::ParentsHelpL6,
    Str::ParentsHelpL7, Str::ParentsHelpL8,
  };
  for (int i = 0; i < 8; ++i) {
    // Step header lines (every other) are highlighted.
    c.setTextColor(((i & 1) == 0) ? accent : body);
    c.setCursor(24, y);
    c.print(tr(lines[i]));
    y += 18;
  }

  // Back X (top-right)
  uint16_t panel = c.color565(255, 240, 240);
  uint16_t glyph = c.color565( 60,  50,  50);
  const Rect& r = kParentsHelpBackRect;
  c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
  int cx = r.x + r.w/2, cy = r.y + r.h/2;
  c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
  c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
  c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
  c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  (void)now_ms;
}

// ─── Parent-server sub-page ─────────────────────────────────────────────

void drawParentSrvScreen(M5Canvas& c, const ParentSrvView& v) {
  c.fillSprite(c.color565(28, 32, 50));
  // Soft star tint at the top.
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  // Title
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(c.color565(255, 200, 100));
  c.drawString(tr(Str::SettingsParentSrv), 160, 8);
  c.setTextDatum(top_left);

  // Back X (top-right)
  uint16_t panel = c.color565(255, 240, 240);
  uint16_t glyph = c.color565( 60,  50,  50);
  {
    const Rect& r = kParentSrvBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // Big status block — tells the user what's happening + the URL when up.
  const char* stateLabel;
  uint16_t   stateCol;
  switch (v.state) {
    case 1: stateLabel = tr(Str::SettingsParentSrvConn);
            stateCol   = c.color565(230, 180,  60); break;
    case 2: stateLabel = tr(Str::SettingsParentSrvOn);
            stateCol   = c.color565(120, 220, 140); break;
    case 3: stateLabel = tr(Str::SettingsParentSrvFail);
            stateCol   = c.color565(240, 100, 100); break;
    default: stateLabel = tr(Str::SettingsParentSrvOff);
             stateCol   = c.color565(180, 180, 200); break;
  }
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(stateCol);
  c.drawString(stateLabel, 160, 60);

  // URL info — visible only when Running.
  c.setTextSize(2);
  if (v.state == 2) {
    c.setTextColor(c.color565(230, 235, 245));
    c.drawString("http://" TARGET_MDNS_NAME ".local", 160, 100);
    if (v.ip && v.ip[0]) {
      c.setTextColor(c.color565(160, 220, 255));
      c.drawString(v.ip, 160, 124);
    }
  } else {
    // Battery warning subtitle when off / connecting.
    c.setTextColor(c.color565(220, 180, 180));
    c.drawString(tr(Str::SettingsParentSrvHint), 160, 110);
  }
  c.setTextDatum(top_left);

  // Toggle button at the bottom — color shifts with state.
  {
    const Rect& r = kParentSrvToggleRect;
    bool on = (v.state == 1 || v.state == 2);
    uint16_t btnBg = on ? c.color565(180,  60,  60) : c.color565( 80, 160,  90);
    uint16_t btnBd = on ? c.color565(255, 220, 220) : c.color565(220, 255, 220);
    Str btnLabel = on ? Str::SettingsParentSrvOff : Str::SettingsParentSrvOn;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 10, btnBg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 10, btnBd);
    c.setTextDatum(middle_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(255, 255, 255));
    c.drawString(tr(btnLabel), r.x + r.w / 2, r.y + r.h / 2);
    c.setTextDatum(top_left);
  }
  (void)v.now_ms;
}

// ─── Location sub-page ─────────────────────────────────────────────────

void drawLocationScreen(M5Canvas& c, const LocationView& v) {
  c.fillSprite(c.color565(28, 32, 50));
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  // Title
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(c.color565(255, 200, 100));
  c.drawString(tr(Str::LocationTitle), 160, 8);
  c.setTextDatum(top_left);

  // Back X
  uint16_t panel = c.color565(255, 240, 240);
  uint16_t glyph = c.color565( 60,  50,  50);
  {
    const Rect& r = kLocationBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  uint16_t fg     = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);

  if (!v.valid) {
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(220, 180, 180));
    c.drawString(tr(Str::LocationNoData), 160, 100);
    c.setTextDatum(top_left);
  } else {
    // Two lines per entry — small label on top, value below — so long
    // labels (e.g. "Aktualisiert:") never run into the value column.
    c.setTextDatum(top_left);
    int y = 50;
    auto row = [&](Str label, const char* val) {
      c.setTextSize(1);
      c.setTextColor(c.color565(170, 180, 200));
      c.setCursor(22, y);
      c.print(tr(label));
      c.setTextSize(2);
      c.setTextColor(accent);
      c.setCursor(22, y + 11);
      c.print(val);
      y += 32;
    };
    row(Str::LocationCity,    v.city ? v.city : "?");
    row(Str::LocationCountry, v.countryCode ? v.countryCode : "?");
    char coordBuf[32];
    snprintf(coordBuf, sizeof(coordBuf), "%.3f, %.3f", v.lat, v.lon);
    row(Str::LocationCoords,  coordBuf);
    char ageBuf[24];
    if (v.lastUpdatedAgoSec == 0) {
      snprintf(ageBuf, sizeof(ageBuf), "-");
    } else if (v.lastUpdatedAgoSec < 60 * 60) {
      snprintf(ageBuf, sizeof(ageBuf), "%lu min",
               (unsigned long)(v.lastUpdatedAgoSec / 60));
    } else if (v.lastUpdatedAgoSec < 24UL * 3600UL) {
      snprintf(ageBuf, sizeof(ageBuf), "%lu h",
               (unsigned long)(v.lastUpdatedAgoSec / 3600));
    } else {
      snprintf(ageBuf, sizeof(ageBuf), "%lu d",
               (unsigned long)(v.lastUpdatedAgoSec / 86400UL));
    }
    row(Str::LocationLastSeen, ageBuf);
    (void)fg;
  }

  // Refresh button
  {
    const Rect& r = kLocationRefreshRect;
    uint16_t bg = v.refreshing ? c.color565(120, 110,  60)
                                : c.color565( 80, 130, 200);
    uint16_t bd = v.refreshing ? c.color565(220, 200, 130)
                                : c.color565(200, 220, 255);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 10, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 10, bd);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(255, 255, 255));
    Str label = v.refreshing ? Str::LocationRefreshing : Str::LocationRefreshBtn;
    c.drawString(tr(label), r.x + r.w / 2, r.y + r.h / 2);
    c.setTextDatum(top_left);
  }
  (void)v.now_ms;
}

// ─── Friends screen (UDP discovery + match animation) ──────────────────

static void drawTinyPet(M5Canvas& c, int cx, int cy, uint8_t animal,
                        uint16_t /*tint*/, bool happy) {
  // Cute mini-rendering of a Bear/Cat/Dog. ~36 px tall.
  uint16_t body, ear, nose, eye = c.color565(40, 25, 25);
  switch (animal) {
    case 1: body = c.color565(220, 215, 200); ear = c.color565(200, 195, 180);
            nose = c.color565(255, 150, 170); break;       // Cat
    case 2: body = c.color565(225, 195, 150); ear = c.color565(190, 155, 110);
            nose = c.color565( 60,  50,  50); break;       // Dog
    default: body = c.color565(200, 165, 130); ear = c.color565(170, 135, 100);
             nose = c.color565( 60,  50,  50);             // Bear
  }
  // Ears
  if (animal == 1) {
    c.fillTriangle(cx - 16, cy - 14, cx -  6, cy - 18, cx -  4, cy -  6, ear);
    c.fillTriangle(cx + 16, cy - 14, cx +  6, cy - 18, cx +  4, cy -  6, ear);
  } else if (animal == 2) {
    c.fillEllipse(cx - 14, cy - 6, 6, 10, ear);
    c.fillEllipse(cx + 14, cy - 6, 6, 10, ear);
  } else {
    c.fillCircle(cx - 14, cy - 12, 6, ear);
    c.fillCircle(cx + 14, cy - 12, 6, ear);
  }
  // Head
  c.fillEllipse(cx, cy, 18, 16, body);
  c.drawEllipse(cx, cy, 18, 16, c.color565(120,  90,  70));
  // Eyes
  c.fillCircle(cx - 6, cy - 2, 2, eye);
  c.fillCircle(cx + 6, cy - 2, 2, eye);
  // Nose
  c.fillTriangle(cx - 3, cy + 4, cx + 3, cy + 4, cx, cy + 8, nose);
  // Mouth
  if (happy) {
    for (int dx = -5; dx <= 5; ++dx) {
      int dy = (5 * 5 - dx * dx) / 14;
      c.drawPixel(cx + dx, cy + 11 - dy, eye);
    }
  } else {
    c.drawFastHLine(cx - 4, cy + 10, 8, eye);
  }
}

// ── Item icons used both on the sending buttons and the playback overlay
static void drawItemIconGift(M5Canvas& c, int cx, int cy, int sz) {
  uint16_t box = c.color565(220,  80,  80);
  uint16_t lid = c.color565(240, 130, 130);
  uint16_t bow = c.color565(255, 220,  90);
  c.fillRoundRect(cx - sz, cy - sz/2, sz * 2, sz, 3, box);
  c.fillRoundRect(cx - sz - 2, cy - sz/2 - 4, sz * 2 + 4, 6, 2, lid);
  c.fillRect(cx - 1, cy - sz/2, 3, sz, bow);
  c.fillCircle(cx - 4, cy - sz/2 - 4, 4, bow);
  c.fillCircle(cx + 4, cy - sz/2 - 4, 4, bow);
}
static void drawItemIconHeart(M5Canvas& c, int cx, int cy, int sz) {
  uint16_t hr = c.color565(240,  90, 120);
  c.fillCircle(cx - sz/2, cy - sz/4, sz/2, hr);
  c.fillCircle(cx + sz/2, cy - sz/4, sz/2, hr);
  c.fillTriangle(cx - sz, cy, cx + sz, cy, cx, cy + sz, hr);
}
static void drawItemIconFood(M5Canvas& c, int cx, int cy, int sz) {
  uint16_t skin = c.color565(220,  70,  70);
  uint16_t hi   = c.color565(255, 180, 180);
  uint16_t leaf = c.color565( 90, 160,  60);
  c.fillCircle(cx, cy + 2, sz, skin);
  c.fillCircle(cx - sz/3, cy - 2, sz/3, hi);
  c.fillTriangle(cx, cy - sz, cx + 4, cy - sz/2, cx - 4, cy - sz/2, leaf);
}
static void drawItemIconGame(M5Canvas& c, int cx, int cy, int sz) {
  // Yellow controller: rounded body + cross + two dots.
  uint16_t body = c.color565(255, 210,  80);
  uint16_t dark = c.color565( 70,  50,  20);
  c.fillRoundRect(cx - sz, cy - sz/2, sz * 2, sz, 5, body);
  c.fillRect(cx - sz/2 - 4, cy - 1, 9, 3, dark);
  c.fillRect(cx - sz/2 + 1, cy - 4, 3, 9, dark);
  c.fillCircle(cx + sz/2 + 3, cy - 2, 2, dark);
  c.fillCircle(cx + sz/2 - 3, cy + 2, 2, dark);
}

static void drawItemIcon(M5Canvas& c, uint8_t kind, int cx, int cy, int sz) {
  switch (kind) {
    case 2: drawItemIconGift (c, cx, cy, sz); break;
    case 3: drawItemIconHeart(c, cx, cy, sz); break;
    case 4: drawItemIconFood (c, cx, cy, sz); break;
    case 5: drawItemIconGame (c, cx, cy, sz); break;
    default: break;
  }
}

void drawFriendsScreen(M5Canvas& c, const FriendsView& v) {
  c.fillSprite(c.color565(28, 32, 50));
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  uint16_t fg     = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  uint16_t err    = c.color565(240, 130, 130);

  // FriendsState values: Idle=0, Connecting=1, Searching=2, Sending=3,
  // Matched=4, NoFriend=5, Rendezvous=6. face.cpp does not pull in net.h,
  // hence the literals.

  // ── Rendezvous: large "Verabreden" button centered ──────────────────────
  if (v.state == 6 /* Rendezvous */) {
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(255, 200, 100));
    c.drawString(tr(Str::FriendsTitle), 160, 12);

    // Hint about what to do.
    c.setTextSize(2);
    c.setTextColor(accent);
    c.drawString(tr(Str::FriendsRendezvousHint), 160, 60);

    // Large button: bright panel look with a strong border. When the
    // local user has already tapped, button dimmed + "Warte auf Freund..."
    // over it, making clear it's the other side's turn now.
    const Rect& r = kFriendsRendezvousBtnRect;
    bool waiting = v.localReady;
    uint16_t panel  = waiting ? c.color565(200, 200, 220)
                              : c.color565(255, 220, 180);
    uint16_t border = waiting ? c.color565( 90,  90, 130)
                              : c.color565(180, 110,  50);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 16, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 16, border);
    c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 15, border);

    // Verabreden CTA at size 3 (10 chars fit). Waiting text is 19+
    // chars long and would overflow the button at size 3 → size 2.
    c.setTextSize(waiting ? 2 : 3);
    c.setTextColor(waiting ? c.color565( 80,  80, 110)
                           : c.color565( 90,  55,  20));
    c.drawString(tr(waiting ? Str::FriendsRendezvousWaiting
                            : Str::FriendsRendezvousBtn),
                 r.x + r.w / 2, r.y + r.h / 2 - (waiting ? 6 : 12));

    // Back-X
    {
      const Rect& bx = kFriendsBackRect;
      uint16_t bp = c.color565(255, 240, 240);
      uint16_t bg2 = c.color565( 60,  50,  50);
      c.fillRoundRect(bx.x, bx.y, bx.w, bx.h, 6, bp);
      c.drawRoundRect(bx.x, bx.y, bx.w, bx.h, 6, bg2);
      int cx = bx.x + bx.w/2, cy2 = bx.y + bx.h/2;
      c.drawLine(cx - 7, cy2 - 7, cx + 7, cy2 + 7, bg2);
      c.drawLine(cx - 7, cy2 + 7, cx + 7, cy2 - 7, bg2);
    }
    c.setTextDatum(top_left);
    return;
  }

  // ── NoFriend: sad pet + text + brief auto-exit ─────────────────────────
  if (v.state == 5 /* NoFriend */) {
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(255, 200, 100));
    c.drawString(tr(Str::FriendsTitle), 160, 8);
    c.setTextDatum(top_left);

    int bob = (int)(sinf(v.now_ms / 700.0f) * 2.0f);
    drawTinyPet(c, 160, 120 + bob, v.myAnimal, 0, false);
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(err);
    c.drawString(tr(Str::FriendsNoFriend), 160, 170);
    c.setTextDatum(top_left);
    return;
  }

  // ── Wait screen: 5/5 done locally, waiting for partner ─────────────────
  // Shown as soon as we've locally sent all 5 items and the partner is
  // still tapping. Gives the user clear feedback "I'm done, the other is
  // still going" instead of staying stuck on the 4-button screen whose
  // buttons silently no longer have any effect.
  if (v.localDone) {
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(255, 200, 100));
    c.drawString(tr(Str::FriendsTitle), 160, 12);

    c.setTextSize(2);
    c.setTextColor(accent);
    c.drawString(tr(Str::FriendsWaitTitle), 160, 60);

    // Animated 3-dot sequence below the text — unambiguous
    // "still running" signal.
    {
      uint8_t step = (uint8_t)((v.now_ms / 350) % 3);
      uint16_t dotOn  = c.color565(180, 230, 255);
      uint16_t dotOff = c.color565( 60,  90, 130);
      for (int i = 0; i < 3; ++i) {
        int dx = 160 + (i - 1) * 18;
        c.fillCircle(dx, 100, i == step ? 7 : 5,
                     i == step ? dotOn : dotOff);
      }
    }

    // Receive counter — gives reassurance gifts are arriving.
    {
      char buf[32];
      snprintf(buf, sizeof(buf), tr(Str::FriendsWaitReceived),
               (unsigned)v.rxItemCount);
      c.setTextSize(2);
      c.setTextColor(fg);
      c.drawString(buf, 160, 138);
    }

    // Mini-pet bobber as a visual accompaniment.
    int bob = (int)(sinf(v.now_ms / 600.0f) * 2.0f);
    drawTinyPet(c, 160, 188 + bob, v.myAnimal, 0, false);

    // Back-X stays available — user can cancel.
    {
      const Rect& r = kFriendsBackRect;
      uint16_t panel = c.color565(255, 240, 240);
      uint16_t glyph = c.color565( 60,  50,  50);
      c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
      c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
      int cx = r.x + r.w/2, cy = r.y + r.h/2;
      c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
      c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    }
    c.setTextDatum(top_left);
    return;
  }

  // ── Sending state: title + action prompt + counter + 4-button grid ──
  // Small title top-left, below it the prominent action prompt
  // (state-aware: during search "Searching for pets...", once a peer
  // is there "Send gifts"). Counter top-right is more compact (size 2)
  // so it doesn't overlap the prompt.
  c.setTextDatum(top_left);
  c.setTextSize(1);
  c.setTextColor(c.color565(255, 200, 100));
  c.setCursor(20, 8);
  c.print(tr(Str::FriendsTitle));

  // Action prompt — the actual "what do I do?" hint.
  c.setTextSize(2);
  c.setTextColor(accent);
  c.setCursor(20, 22);
  // state 3 (Sending) and 4 (Matched) → send prompt; otherwise search hint.
  c.print(tr(v.state >= 3 ? Str::FriendsPrompt : Str::FriendsSearching));

  // Counter top-right (size 2, smaller than the previous size 3 so the
  // prompt has room + the battery icon would still fit alongside).
  char counterBuf[16];
  snprintf(counterBuf, sizeof(counterBuf), "%u/%u",
           (unsigned)v.sentCount, (unsigned)v.maxSends);
  c.setTextDatum(top_right);
  c.setTextSize(2);
  c.setTextColor(accent);
  c.drawString(counterBuf, 268, 22);
  c.setTextDatum(top_left);

  // Back X (cancel = exit early — the playback runs with whatever was rx'd)
  {
    const Rect& r = kFriendsBackRect;
    uint16_t panel = c.color565(255, 240, 240);
    uint16_t glyph = c.color565( 60,  50,  50);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // 4-button grid. Each button: rounded panel + icon + label. Last-tapped
  // button briefly pulses to confirm the send.
  struct Btn {
    const Rect& rect;
    uint8_t     kind;
    uint16_t    panelCol;
    Str         label;
  };
  uint16_t panelGift  = c.color565(255, 220, 180);
  uint16_t panelHeart = c.color565(255, 200, 220);
  uint16_t panelFood  = c.color565(220, 240, 200);
  uint16_t panelGame  = c.color565(220, 220, 255);
  Btn btns[4] = {
    { kFriendsBtnGift,  2, panelGift,  Str::FriendsItemGift  },
    { kFriendsBtnHeart, 3, panelHeart, Str::FriendsItemHeart },
    { kFriendsBtnFood,  4, panelFood,  Str::FriendsItemFood  },
    { kFriendsBtnGame,  5, panelGame,  Str::FriendsItemGame  },
  };
  uint16_t border = c.color565( 60,  50,  50);
  uint16_t txt    = c.color565( 40,  30,  30);

  for (int i = 0; i < 4; ++i) {
    const Btn& b = btns[i];
    const Rect& r = b.rect;
    // Pulse the just-pressed button for 250 ms.
    bool pulse = (v.lastSentKind == b.kind) &&
                 (v.now_ms - v.lastSentAtMs < 250);
    int pad = pulse ? -2 : 0;
    c.fillRoundRect(r.x + pad, r.y + pad,
                    r.w - 2*pad, r.h - 2*pad, 12, b.panelCol);
    c.drawRoundRect(r.x + pad, r.y + pad,
                    r.w - 2*pad, r.h - 2*pad, 12, border);
    if (pulse) {
      c.drawRoundRect(r.x + pad - 1, r.y + pad - 1,
                      r.w - 2*pad + 2, r.h - 2*pad + 2, 13,
                      c.color565(255, 200,  80));
    }
    int icx = r.x + r.w / 2;
    int icy = r.y + r.h / 2 - 8;
    drawItemIcon(c, b.kind, icx, icy, 12);
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(txt);
    c.drawString(tr(b.label), icx, r.y + r.h - 22);
    c.setTextDatum(top_left);
  }

  // Tiny session timer at bottom (so the user sees how long they have)
  if (v.secondsLeft > 0) {
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%lus", (unsigned long)v.secondsLeft);
    c.setTextDatum(top_center);
    c.setTextSize(1);
    c.setTextColor(fg);
    c.drawString(tbuf, 160, 222);
    c.setTextDatum(top_left);
  }
  (void)v.peerAnimal;
}

// ─── Time-edit screen ───────────────────────────────────────────────────────

// ─── Help (Anleitung) screen — 11 pages ────────────────────────────────────

namespace help_layout {
  constexpr int kBodyTop  = 50;
  constexpr int kBodyLeft = 24;
  constexpr int kLineH    = 18;     // textSize 2 line spacing
}

// Tiny inline icons for page 7 (~16 px wide, drawn at given center).
static void drawHelpIconGear(M5Canvas& c, int cx, int cy) {
  uint16_t panel = c.color565(180, 180, 195);
  uint16_t edge  = c.color565( 90,  90, 100);
  c.fillCircle(cx, cy,  9, panel);
  c.fillCircle(cx, cy,  4, edge);
  c.fillCircle(cx, cy,  3, c.color565(40, 40, 50));
  for (int t = 0; t < 8; ++t) {
    float a = t * (float)PI / 4.0f;
    int tx = cx + (int)(cosf(a) * 11);
    int ty = cy + (int)(sinf(a) * 11);
    c.fillCircle(tx, ty, 2, panel);
  }
}

static void drawHelpIconTV(M5Canvas& c, int cx, int cy) {
  uint16_t case_  = c.color565( 50,  40,  50);
  uint16_t screen = c.color565(120, 200, 220);
  c.fillRoundRect(cx - 12, cy - 8, 24, 16, 3, case_);
  c.fillRoundRect(cx - 10, cy - 6, 20, 12, 2, screen);
  c.drawFastVLine(cx - 6, cy - 4, 8, c.color565(220, 255, 255));
  c.fillRect(cx - 6, cy + 8, 3, 2, case_);
  c.fillRect(cx + 3, cy + 8, 3, 2, case_);
}

static void drawHelpIconCompass(M5Canvas& c, int cx, int cy) {
  uint16_t outline = c.color565( 50,  90,  60);
  c.drawCircle(cx, cy, 12, outline);
  c.fillTriangle(cx, cy - 10, cx - 4, cy, cx + 4, cy,
                 c.color565(200, 60, 60));
  c.fillTriangle(cx, cy + 10, cx - 4, cy, cx + 4, cy,
                 c.color565(240, 240, 240));
  c.drawTriangle(cx, cy + 10, cx - 4, cy, cx + 4, cy, outline);
  c.fillCircle(cx, cy, 2, outline);
}

static void drawHelpIconStar(M5Canvas& c, int cx, int cy) {
  uint16_t fill = c.color565(255, 220, 60);
  uint16_t edge = c.color565(180, 120, 20);
  c.fillTriangle(cx,      cy - 12, cx - 11, cy + 7, cx + 11, cy + 7, fill);
  c.fillTriangle(cx - 11, cy -  3, cx + 11, cy - 3, cx,      cy + 12, fill);
  c.drawTriangle(cx,      cy - 12, cx - 11, cy + 7, cx + 11, cy + 7, edge);
  c.drawTriangle(cx - 11, cy -  3, cx + 11, cy - 3, cx,      cy + 12, edge);
}

static void drawHelpIconBall(M5Canvas& c, int cx, int cy) {
  uint16_t red = c.color565(220, 60, 60);
  uint16_t hi  = c.color565(255, 160, 160);
  uint16_t sh  = c.color565(140, 30, 30);
  c.fillCircle(cx, cy, 11, red);
  c.fillCircle(cx - 4, cy - 4, 4, hi);
  c.drawCircle(cx, cy, 11, sh);
  c.drawFastHLine(cx - 8, cy, 17, sh);
}

static void drawHelpIconBasket(M5Canvas& c, int cx, int cy) {
  uint16_t weave   = c.color565(180, 130,  60);
  uint16_t outline = c.color565(110,  70,  35);
  for (int i = 0; i < 2; ++i) c.drawCircle(cx, cy - 1 + i, 9 - i, outline);
  c.fillTriangle(cx - 12, cy + 1, cx + 12, cy + 1, cx +  9, cy + 11, weave);
  c.fillTriangle(cx - 12, cy + 1, cx +  9, cy + 11, cx -  9, cy + 11, weave);
  c.drawLine(cx - 12, cy + 1, cx -  9, cy + 11, outline);
  c.drawLine(cx + 12, cy + 1, cx +  9, cy + 11, outline);
  c.drawLine(cx - 12, cy + 1, cx + 12, cy + 1, outline);
  c.fillCircle(cx - 3, cy - 1, 4, c.color565(220, 60, 60));
}

static void drawHelpIconClean(M5Canvas& c, int cx, int cy) {
  // Pile + diagonal slash
  uint16_t brown = c.color565(120, 80, 50);
  uint16_t red   = c.color565(220, 60, 60);
  c.fillEllipse(cx,     cy + 2, 7, 3, brown);
  c.fillEllipse(cx - 1, cy - 1, 5, 3, c.color565(85, 55, 35));
  c.fillCircle (cx + 1, cy - 4, 2,    brown);
  c.drawLine(cx - 11, cy + 8, cx + 11, cy - 8, red);
  c.drawLine(cx - 10, cy + 8, cx + 12, cy - 8, red);
}

static void drawHelpIconHeart(M5Canvas& c, int cx, int cy) {
  uint16_t red = c.color565(230,  60, 110);
  c.fillCircle(cx - 5, cy - 2, 5, red);
  c.fillCircle(cx + 5, cy - 2, 5, red);
  c.fillTriangle(cx - 9, cy, cx + 9, cy, cx, cy + 10, red);
}

static void drawHelpIconBolt(M5Canvas& c, int cx, int cy) {
  uint16_t bolt = c.color565(255, 220, 60);
  uint16_t edge = c.color565(180, 130,  20);
  c.fillTriangle(cx - 4, cy - 10, cx + 5, cy, cx,     cy, bolt);
  c.fillTriangle(cx,     cy,      cx + 5, cy, cx - 2, cy + 10, bolt);
  c.drawTriangle(cx - 4, cy - 10, cx + 5, cy, cx,     cy, edge);
  c.drawTriangle(cx,     cy,      cx + 5, cy, cx - 2, cy + 10, edge);
}

static void drawHelpIconApple(M5Canvas& c, int cx, int cy) {
  uint16_t red    = c.color565(220,  50,  50);
  uint16_t hi     = c.color565(255, 140, 140);
  uint16_t leaf   = c.color565( 90, 155,  70);
  c.fillCircle(cx, cy + 1, 8, red);
  c.fillCircle(cx - 2, cy - 2, 2, hi);
  c.drawFastVLine(cx, cy - 9, 3, c.color565(80, 50, 30));
  c.fillTriangle(cx + 1, cy - 8, cx + 5, cy - 9, cx + 4, cy - 6, leaf);
}

// Header (title + page count + back X) shared across all pages.
static void drawHelpChrome(M5Canvas& c, uint8_t page) {
  uint16_t title = c.color565(220, 230, 245);
  uint16_t soft  = c.color565(150, 165, 200);
  uint16_t panel = c.color565(255, 240, 240);
  uint16_t glyph = c.color565( 60,  50,  50);

  c.setTextDatum(top_left);
  c.setTextSize(2);
  c.setTextColor(title);
  c.setCursor(20, 12);
  c.print(tr(Str::HelpHeader));

  char buf[16];
  snprintf(buf, sizeof(buf), "%u / %u",
           (unsigned)(page + 1), (unsigned)kHelpPageCount);
  // Right-align the page indicator into the gap between the title and the
  // back X (back X starts at x=280). top_right keeps a stable right edge
  // even if the digit count changes (e.g. "10 / 12" vs "1 / 12").
  c.setTextDatum(top_right);
  c.setTextSize(2);
  c.setTextColor(soft);
  c.drawString(buf, 268, 12);

  // Back X (top-right)
  const Rect& r = kHelpBackRect;
  c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
  c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
  int cx = r.x + r.w/2, cy = r.y + r.h/2;
  c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
  c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
  c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
  c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  c.setTextDatum(top_left);
}

static void drawHelpNav(M5Canvas& c, uint8_t page) {
  uint16_t enabled  = c.color565(255, 150,  60);
  uint16_t disabled = c.color565( 80,  85, 100);
  uint16_t glyph    = c.color565( 50,  45,  45);
  uint16_t white    = c.color565(255, 255, 255);
  bool prevOn = (page > 0);
  bool nextOn = (page + 1 < kHelpPageCount);

  // Prev
  {
    const Rect& r = kHelpPrevRect;
    uint16_t bg = prevOn ? enabled : disabled;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 5, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 5, glyph);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(white);
    c.drawString(tr(Str::HelpNavBack), r.x + r.w/2, r.y + r.h/2);
  }
  // Next
  {
    const Rect& r = kHelpNextRect;
    uint16_t bg = nextOn ? enabled : disabled;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 5, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 5, glyph);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(white);
    c.drawString(tr(Str::HelpNavNext), r.x + r.w/2, r.y + r.h/2);
  }
  c.setTextDatum(top_left);
}

// Helper to print one body line at a given Y. All help pages now use a
// single body size (2) for visual consistency — earlier mixed sizes 1/2
// were inconsistent and harder to read.
static void helpLine(M5Canvas& c, int y, const char* s, uint16_t col) {
  c.setTextDatum(top_left);
  c.setTextSize(2);
  c.setTextColor(col);
  c.setCursor(help_layout::kBodyLeft, y);
  c.print(s);
}

static void helpTitle(M5Canvas& c, const char* s) {
  uint16_t title = c.color565(255, 200, 100);
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(title);
  c.drawString(s, 160, 44);
  c.setTextDatum(top_left);
}

static void drawHelpPage1(M5Canvas& c) {
  uint16_t body = c.color565(230, 235, 245);
  helpTitle(c, tr(Str::HelpTitleHello));
  helpLine(c,  88, tr(Str::HelpP1L1), body);
  helpLine(c, 108, tr(Str::HelpP1L2), body);
  helpLine(c, 144, tr(Str::HelpP1L3), body);
  helpLine(c, 164, tr(Str::HelpP1L4), body);
  helpLine(c, 184, tr(Str::HelpP1L5), body);
}

static void drawHelpPage2(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleTap));
  helpLine(c,  84, tr(Str::HelpP2L1), accent);
  helpLine(c, 104, tr(Str::HelpP2L2), body);
  helpLine(c, 132, tr(Str::HelpP2L3), accent);
  helpLine(c, 152, tr(Str::HelpP2L4), body);
  helpLine(c, 180, tr(Str::HelpP2L5), accent);
  helpLine(c, 200, tr(Str::HelpP2L6), body);
}

static void drawHelpPage3(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleMouthEars));
  helpLine(c,  84, tr(Str::HelpP3L1), accent);
  helpLine(c, 104, tr(Str::HelpP3L2), body);
  helpLine(c, 140, tr(Str::HelpP3L3), accent);
  helpLine(c, 160, tr(Str::HelpP3L4), body);
  helpLine(c, 180, tr(Str::HelpP3L5), body);
}

static void drawHelpPage4(M5Canvas& c) {
  uint16_t body = c.color565(230, 235, 245);
  helpTitle(c, tr(Str::HelpTitlePet));
  helpLine(c,  92, tr(Str::HelpP4L1), body);
  helpLine(c, 112, tr(Str::HelpP4L2), body);
  helpLine(c, 152, tr(Str::HelpP4L3), body);
  helpLine(c, 172, tr(Str::HelpP4L4), body);
}

// Page 5 — Finger gestures (circle → somersault, two-finger pull → wobble)
static void drawHelpPageGestures(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleGestures));
  helpLine(c,  84, tr(Str::HelpPGL1), accent);
  helpLine(c, 104, tr(Str::HelpPGL2), body);
  helpLine(c, 124, tr(Str::HelpPGL3), body);
  helpLine(c, 156, tr(Str::HelpPGL4), accent);
  helpLine(c, 176, tr(Str::HelpPGL5), body);
}

// Page 6 — Hand warming (two fingers held still → snuggle / sleep)
static void drawHelpPageWarming(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleWarming));
  helpLine(c,  84, tr(Str::HelpPWL1), accent);
  helpLine(c, 104, tr(Str::HelpPWL2), accent);
  helpLine(c, 132, tr(Str::HelpPWL3), body);
  helpLine(c, 152, tr(Str::HelpPWL4), body);
  helpLine(c, 184, tr(Str::HelpPWL5), body);
}

// Page 7 — Hop chain (rapid taps → pet hops N times)
static void drawHelpPageHopChain(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleHopChain));
  helpLine(c,  84, tr(Str::HelpPHL1), accent);
  helpLine(c, 112, tr(Str::HelpPHL2), body);
  helpLine(c, 132, tr(Str::HelpPHL3), body);
  helpLine(c, 152, tr(Str::HelpPHL4), body);
  helpLine(c, 184, tr(Str::HelpPHL5), accent);
}

// Page 8 — Singing (upright + L/R tilt → 8 s singing → 5 s applause window)
static void drawHelpPageSinging(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleSinging));
  helpLine(c,  84, tr(Str::HelpPSL1), accent);
  helpLine(c, 104, tr(Str::HelpPSL2), accent);
  helpLine(c, 132, tr(Str::HelpPSL3), body);
  helpLine(c, 164, tr(Str::HelpPSL4), body);
  helpLine(c, 184, tr(Str::HelpPSL5), body);
}

static void drawHelpPageSport(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleSport));
  helpLine(c,  84, tr(Str::HelpPSPL1), accent);
  helpLine(c, 104, tr(Str::HelpPSPL2), body);
  helpLine(c, 132, tr(Str::HelpPSPL3), body);
  helpLine(c, 156, tr(Str::HelpPSPL4), body);
  helpLine(c, 184, tr(Str::HelpPSPL5), body);
}

static void drawHelpPage5(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleMotion));
  helpLine(c,  84, tr(Str::HelpP5L1), accent);
  helpLine(c, 104, tr(Str::HelpP5L2), body);
  helpLine(c, 132, tr(Str::HelpP5L3), accent);
  helpLine(c, 152, tr(Str::HelpP5L4), body);
}

static void drawHelpPage6(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleRock));
  helpLine(c,  90, tr(Str::HelpP6L1), body);
  helpLine(c, 110, tr(Str::HelpP6L2), accent);
  helpLine(c, 144, tr(Str::HelpP6L3), body);
  helpLine(c, 164, tr(Str::HelpP6L4), body);
}

static void drawHelpPage7(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleStanding));
  helpLine(c,  84, tr(Str::HelpP7L1), accent);
  helpLine(c, 104, tr(Str::HelpP7L2), body);
  helpLine(c, 132, tr(Str::HelpP7L3), accent);
  helpLine(c, 152, tr(Str::HelpP7L4), body);
  helpLine(c, 180, tr(Str::HelpP7L5), accent);
  helpLine(c, 200, tr(Str::HelpP7L6), body);
}

static void drawHelpPage8(M5Canvas& c) {
  uint16_t body = c.color565(230, 235, 245);
  helpTitle(c, tr(Str::HelpTitleButtons));

  // Single column of 7 icon+label rows so all labels render at full size.
  struct Row { void (*icon)(M5Canvas&, int, int); Str label; };
  static const Row rows[7] = {
    { drawHelpIconGear,    Str::SettingsTitle    },
    { drawHelpIconTV,      Str::MediaTitle       },
    { drawHelpIconCompass, Str::HelpLabelTravel  },
    { drawHelpIconStar,    Str::HelpLabelMinigame},
    { drawHelpIconBall,    Str::ToyTitle         },
    { drawHelpIconBasket,  Str::HelpLabelFood    },
    { drawHelpIconClean,   Str::CleaningTitle    },
  };
  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(body);
  for (int i = 0; i < 7; ++i) {
    int x = 36;
    int y = 84 + i * 19;
    rows[i].icon(c, x, y);
    c.drawString(tr(rows[i].label), x + 22, y + 1);
  }
  c.setTextDatum(top_left);
}

static void drawHelpPage9(M5Canvas& c) {
  uint16_t body = c.color565(230, 235, 245);
  helpTitle(c, tr(Str::HelpTitleNeeds));

  c.setTextDatum(middle_left);
  c.setTextSize(2);
  c.setTextColor(body);

  drawHelpIconHeart(c, 60,  92);
  c.drawString(tr(Str::HelpP9Joy),    88,  92);
  drawHelpIconBolt (c, 60, 124);
  c.drawString(tr(Str::HelpP9Energy), 88, 124);
  drawHelpIconApple(c, 60, 156);
  c.drawString(tr(Str::HelpP9Hunger), 88, 156);

  c.setTextDatum(top_center);
  c.setTextSize(2);
  c.setTextColor(c.color565(255, 220, 180));
  c.drawString(tr(Str::HelpP9Hint1), 160, 184);
  c.drawString(tr(Str::HelpP9Hint2), 160, 204);
  c.setTextDatum(top_left);
}

static void drawHelpPage10(M5Canvas& c) {
  uint16_t body = c.color565(230, 235, 245);
  helpTitle(c, tr(Str::HelpTitleGather));
  helpLine(c,  84, tr(Str::HelpP10L1), body);
  helpLine(c, 116, tr(Str::HelpP10L2), body);
  helpLine(c, 136, tr(Str::HelpP10L3), body);
  helpLine(c, 168, tr(Str::HelpP10L4), body);
  helpLine(c, 188, tr(Str::HelpP10L5), body);
}

static void drawHelpPage11(M5Canvas& c) {
  uint16_t body = c.color565(230, 235, 245);
  helpTitle(c, tr(Str::HelpTitleMinigames));
  helpLine(c,  84, tr(Str::HelpP11L1), body);
  helpLine(c, 104, tr(Str::HelpP11L2), body);
  helpLine(c, 132, tr(Str::HelpP11L3), body);
  helpLine(c, 152, tr(Str::HelpP11L4), body);
  helpLine(c, 172, tr(Str::HelpP11L5), body);
  helpLine(c, 192, tr(Str::HelpP11L6), body);
}

static void drawHelpPage12(M5Canvas& c) {
  uint16_t body = c.color565(230, 235, 245);
  helpTitle(c, tr(Str::HelpTitleBreak));
  helpLine(c,  84, tr(Str::HelpP12L1), body);
  helpLine(c, 104, tr(Str::HelpP12L2), body);
  helpLine(c, 136, tr(Str::HelpP12L3), body);
  helpLine(c, 156, tr(Str::HelpP12L4), body);
  helpLine(c, 188, tr(Str::HelpP12L5), body);
}

#if TARGET_HAS_LLM
// Page 17 — Voice control: wake word + spoken commands (CoreS3 + Module-LLM)
static void drawHelpPageVoice(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleVoice));
  helpLine(c,  84, tr(Str::HelpPVoiceL1), accent);
  helpLine(c, 104, tr(Str::HelpPVoiceL2), body);
  helpLine(c, 132, tr(Str::HelpPVoiceL3), accent);
  helpLine(c, 152, tr(Str::HelpPVoiceL4), body);
  helpLine(c, 172, tr(Str::HelpPVoiceL5), body);
  helpLine(c, 192, tr(Str::HelpPVoiceL6), body);
}
#endif

#if TARGET_HAS_CAMERA
// Page 18 — Camera: face detection on the front-facing CoreS3 cam
static void drawHelpPageCamera(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(160, 220, 255);
  helpTitle(c, tr(Str::HelpTitleCamera));
  helpLine(c,  88, tr(Str::HelpPCamL1), accent);
  helpLine(c, 116, tr(Str::HelpPCamL2), body);
  helpLine(c, 136, tr(Str::HelpPCamL3), body);
  helpLine(c, 168, tr(Str::HelpPCamL4), body);
  helpLine(c, 188, tr(Str::HelpPCamL5), body);
}
#endif

#if TARGET_HAS_WIFI
// Page 19 — Webradio: WDR Maus (DE) bzw. Fun Kids UK (EN)
static void drawHelpPageRadio(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(255, 220, 150);
  helpTitle(c, tr(Str::HelpTitleRadio));
  helpLine(c,  88, tr(Str::HelpPRadioL1), body);
  helpLine(c, 108, tr(Str::HelpPRadioL2), accent);
  helpLine(c, 136, tr(Str::HelpPRadioL3), body);
  helpLine(c, 168, tr(Str::HelpPRadioL4), body);
  helpLine(c, 188, tr(Str::HelpPRadioL5), body);
}
#endif

#if TARGET_HAS_CAMERA
// Page 20 — Foto + Galerie (Pet-Selfie auf Muffin/Visu)
static void drawHelpPagePhoto(M5Canvas& c) {
  uint16_t body   = c.color565(230, 235, 245);
  uint16_t accent = c.color565(220, 200, 255);
  helpTitle(c, tr(Str::HelpTitlePhoto));
  helpLine(c,  88, tr(Str::HelpPPhotoL1), accent);
  helpLine(c, 108, tr(Str::HelpPPhotoL2), body);
  helpLine(c, 136, tr(Str::HelpPPhotoL3), body);
  helpLine(c, 168, tr(Str::HelpPPhotoL4), body);
  helpLine(c, 188, tr(Str::HelpPPhotoL5), body);
}
#endif

void drawHelpScreen(M5Canvas& c, const HelpView& v) {
  c.fillSprite(c.color565(28, 32, 50));
  // Soft starry tint at the top so it feels distinct from settings.
  for (int i = 0; i < 18; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 40;
    c.drawPixel(sx, sy, c.color565(120, 140, 200));
  }

  drawHelpChrome(c, v.page);
  switch (v.page) {
    case  0: drawHelpPage1       (c); break;
    case  1: drawHelpPage2       (c); break;
    case  2: drawHelpPage3       (c); break;
    case  3: drawHelpPage4       (c); break;
    case  4: drawHelpPageGestures(c); break;
    case  5: drawHelpPageWarming (c); break;
    case  6: drawHelpPageHopChain(c); break;
    case  7: drawHelpPageSinging (c); break;
    case  8: drawHelpPageSport   (c); break;
    case  9: drawHelpPage5       (c); break;
    case 10: drawHelpPage6       (c); break;
    case 11: drawHelpPage7       (c); break;
    case 12: drawHelpPage8       (c); break;
    case 13: drawHelpPage9       (c); break;
    case 14: drawHelpPage10      (c); break;
    case 15: drawHelpPage11      (c); break;
    case 16: drawHelpPage12      (c); break;
    // Optional pages in fixed order: Voice (HAS_LLM) → Camera-
    // Detection (HAS_CAMERA) → Radio (HAS_WIFI) → Photo+Gallery (HAS_CAMERA).
    // The concrete case index shifts when a predecessor is missing.
#if TARGET_HAS_LLM
    case 17: drawHelpPageVoice   (c); break;
#  if TARGET_HAS_CAMERA
    case 18: drawHelpPageCamera  (c); break;
#    if TARGET_HAS_WIFI
    case 19: drawHelpPageRadio   (c); break;
    case 20: drawHelpPagePhoto   (c); break;
#    else
    case 19: drawHelpPagePhoto   (c); break;
#    endif
#  elif TARGET_HAS_WIFI
    case 18: drawHelpPageRadio   (c); break;
#  endif
#elif TARGET_HAS_CAMERA
    case 17: drawHelpPageCamera  (c); break;
#  if TARGET_HAS_WIFI
    case 18: drawHelpPageRadio   (c); break;
    case 19: drawHelpPagePhoto   (c); break;
#  else
    case 18: drawHelpPagePhoto   (c); break;
#  endif
#elif TARGET_HAS_WIFI
    case 17: drawHelpPageRadio   (c); break;
#endif
    default: break;
  }
  drawHelpNav(c, v.page);
  (void)v.now_ms;
}

// Language picker — used both during first-run setup and from
// Settings → Sprache. firstRun=true hides the back X (user must pick).
void drawLangSelectScreen(M5Canvas& c, const LangSelectView& v) {
  c.fillSprite(c.color565(28, 32, 50));

  uint16_t white     = c.color565(240, 240, 240);
  uint16_t panel     = c.color565(255, 240, 240);
  uint16_t glyph     = c.color565( 60,  50,  50);
  uint16_t selBorder = c.color565(255, 150,  50);

  // Title — on first run we stack the two language names so neither
  // overflows the screen at textSize 3 (the inline "Sprache / Language"
  // ran past the right edge).
  c.setTextSize(3);
  c.setTextColor(white);
  if (v.firstRun) {
    c.setTextDatum(top_center);
    c.drawString("Sprache", 160, 4);
    c.setTextColor(c.color565(160, 200, 240));
    c.drawString("Language", 160, 30);
    c.setTextDatum(top_left);
  } else {
    c.setTextDatum(top_left);
    c.setCursor(20, 14);
    c.print("Sprache");
  }

  // Back X (only when not first-run)
  if (!v.firstRun) {
    const Rect& r = kLangSelectBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  static const char* labels[2] = { "Deutsch", "English" };
  for (int i = 0; i < 2; ++i) {
    const Rect& r = kLangChoiceRect[i];
    bool sel = (v.current == (uint8_t)i);
    uint16_t border = sel ? selBorder : c.color565(110, 110, 130);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 12, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 12, border);
    if (sel) c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 11, border);

    int cx = r.x + r.w / 2;
    int flagY = r.y + 50;
    if (i == 0) {
      // German flag — three horizontal stripes (black / red / gold)
      uint16_t black = c.color565( 30,  30,  30);
      uint16_t red   = c.color565(220,  30,  40);
      uint16_t gold  = c.color565(255, 200,  60);
      uint16_t edge  = c.color565( 60,  50,  40);
      int fw = 88, fh = 60;
      int fx = cx - fw / 2;
      int fy = flagY - fh / 2;
      c.fillRect(fx, fy,            fw, fh / 3, black);
      c.fillRect(fx, fy + fh / 3,   fw, fh / 3, red);
      c.fillRect(fx, fy + 2*fh / 3, fw, fh - 2*fh/3, gold);
      c.drawRect(fx, fy, fw, fh, edge);
    } else {
      // British flag — simplified Union Jack
      uint16_t blue  = c.color565( 20,  40, 130);
      uint16_t red   = c.color565(220,  30,  40);
      uint16_t white_ = c.color565(245, 245, 250);
      uint16_t edge  = c.color565( 60,  50,  60);
      int fw = 88, fh = 60;
      int fx = cx - fw / 2;
      int fy = flagY - fh / 2;
      // Blue background
      c.fillRect(fx, fy, fw, fh, blue);
      // Diagonals — white wider, red thinner inside
      for (int t = -3; t <= 3; ++t) {
        c.drawLine(fx + 0, fy + 0 + t, fx + fw - 1, fy + fh - 1 + t, white_);
        c.drawLine(fx + 0, fy + fh - 1 - t, fx + fw - 1, fy + 0 - t, white_);
      }
      for (int t = -1; t <= 1; ++t) {
        c.drawLine(fx + 0, fy + 0 + t, fx + fw - 1, fy + fh - 1 + t, red);
        c.drawLine(fx + 0, fy + fh - 1 - t, fx + fw - 1, fy + 0 - t, red);
      }
      // Vertical + horizontal cross — white wider
      c.fillRect(fx + fw/2 - 7, fy,         14, fh, white_);
      c.fillRect(fx,            fy + fh/2 - 5, fw, 10, white_);
      // Red cross on top, narrower
      c.fillRect(fx + fw/2 - 3, fy,         6, fh, red);
      c.fillRect(fx,            fy + fh/2 - 2, fw, 4, red);
      c.drawRect(fx, fy, fw, fh, edge);
    }

    // Label below flag
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(glyph);
    c.drawString(labels[i], cx, r.y + r.h - 32);
  }

  if (v.firstRun) {
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(180, 200, 240));
    c.drawString("Bitte waehle", 160, 204);
    c.drawString("Please choose", 160, 222);
  }
  c.setTextDatum(top_left);
  (void)v.now_ms;
}

// Credits / contributors screen — single page, back X to close.
void drawCreditsScreen(M5Canvas& c, uint32_t now_ms) {
  c.fillSprite(c.color565(28, 32, 50));

  // Twinkling stars in the top band so it feels like the help screen.
  for (int i = 0; i < 24; ++i) {
    uint32_t r = (uint32_t)i * 2654435761u;
    int sx = (r >> 8)  % 320;
    int sy = (r >> 18) % 36;
    uint32_t phase = ((now_ms / 90) + (r >> 24)) & 0x7F;
    uint8_t bri = 100 + (phase < 64 ? phase : (127 - phase)) * 2;
    c.drawPixel(sx, sy, c.color565(bri, bri + 30 > 255 ? 255 : bri + 30,
                                   bri + 60 > 255 ? 255 : bri + 60));
  }

  // Title bar
  uint16_t title  = c.color565(220, 230, 245);
  uint16_t accent = c.color565(255, 200, 100);
  uint16_t role   = c.color565(160, 200, 240);
  uint16_t name   = c.color565(255, 240, 220);
  uint16_t panel  = c.color565(255, 240, 240);
  uint16_t glyph  = c.color565( 60,  50,  50);

  c.setTextDatum(top_left);
  c.setTextSize(2);
  c.setTextColor(title);
  c.setCursor(20, 12);
  c.print(tr(Str::SettingsCredits));

  // Back X (reuses the same top-right slot as the help screen)
  {
    const Rect& r = kHelpBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // Subtitle with a subtle pulse so it feels alive.
  uint8_t pulse = 200 + (uint8_t)(55 * sinf(now_ms / 600.0f));
  c.setTextDatum(top_center);
  c.setTextSize(2);
  c.setTextColor(c.color565(pulse, 200, 110));
  c.drawString(tr(Str::CreditsSubtitle), 160, 42);

  // Four contributor blocks — both role and name at textSize 2 for
  // legibility. Names are kept as-is (proper names); roles translate.
  c.setTextDatum(top_left);
  static const struct { Str role; const char* name; int yRole; } rows[4] = {
    { Str::CreditsRoleIdeas,       "Justus",          70 },
    { Str::CreditsRoleTech,        "Papa",          112 },
    { Str::CreditsRoleSounds,      "Mama :)",       154 },
    { Str::CreditsRoleProgramming, "KI (Opus 4.7)", 196 },
  };
  for (int i = 0; i < 4; ++i) {
    c.setTextSize(2);
    c.setTextColor(role);
    c.setCursor(20, rows[i].yRole);
    c.print(tr(rows[i].role));
    c.setTextColor(name);
    c.setCursor(20, rows[i].yRole + 18);
    c.print(rows[i].name);
  }
  (void)accent;
}

void drawTimeEditScreen(M5Canvas& c, uint32_t now_ms) {
  c.fillSprite(c.color565(30, 30, 40));

  uint16_t white = c.color565(240, 240, 240);
  uint16_t panel = c.color565(255, 240, 240);
  uint16_t glyph = c.color565(60, 50, 50);

  // Title + back X
  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(white);
  c.setCursor(20, 10);
  c.print(tr(Str::TimeEditTitle));

  {
    const Rect& r = kTimeBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // Helper lambdas to draw an arrow button
  auto drawArrow = [&](const Rect& r, bool up) {
    c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (up) c.fillTriangle(cx, cy - 10, cx - 14, cy + 7, cx + 14, cy + 7, glyph);
    else    c.fillTriangle(cx, cy + 10, cx - 14, cy - 7, cx + 14, cy - 7, glyph);
  };
  drawArrow(kHourUpRect,   true);
  drawArrow(kMinUpRect,    true);
  drawArrow(kHourDownRect, false);
  drawArrow(kMinDownRect,  false);

  // Big time display
  m5::rtc_datetime_t dt;
  M5.Rtc.getDateTime(&dt);
  char hh[3], mm[3];
  snprintf(hh, sizeof(hh), "%02d", (int)dt.time.hours);
  snprintf(mm, sizeof(mm), "%02d", (int)dt.time.minutes);

  c.setTextDatum(middle_center);
  c.setTextSize(6);
  c.setTextColor(white);
  c.drawString(hh,  100, 128);
  c.drawString(":", 160, 128);
  c.drawString(mm,  220, 128);

  // Tiny seconds indicator (helps you see the clock is actually ticking)
  c.setTextSize(2);
  c.setTextColor(c.color565(150, 150, 180));
  c.drawString(":", 160, 168);
  char ss[3];
  snprintf(ss, sizeof(ss), "%02d", (int)dt.time.seconds);
  c.drawString(ss, 178, 168);

  c.setTextDatum(top_left);
  (void)now_ms;
}

// ─── Animal-select screen ───────────────────────────────────────────────────

// Mini avatar (~70 px tall) for the choice buttons.
static void drawAnimalAvatar(M5Canvas& c, int cx, int cy, AnimalType type) {
  AnimalStyle s = styleFor(type, c);
  // Body
  c.fillCircle(cx, cy + 6, 28, s.body);
  c.fillEllipse(cx, cy - 2, 24, 8, s.bodyHighlight);
  // Ears per type
  switch (type) {
    case AnimalType::Cat:
      c.fillTriangle(cx - 24, cy - 12, cx -  6, cy - 12, cx - 18, cy - 32, s.body);
      c.fillTriangle(cx + 24, cy - 12, cx +  6, cy - 12, cx + 18, cy - 32, s.body);
      c.fillTriangle(cx - 20, cy - 14, cx - 10, cy - 14, cx - 16, cy - 26, s.innerEar);
      c.fillTriangle(cx + 20, cy - 14, cx + 10, cy - 14, cx + 16, cy - 26, s.innerEar);
      break;
    case AnimalType::Dog:
      c.fillEllipse(cx - 30, cy + 10, 9, 22, s.bodyShade);
      c.fillEllipse(cx + 30, cy + 10, 9, 22, s.bodyShade);
      c.fillEllipse(cx - 30, cy + 12, 5, 16, s.innerEar);
      c.fillEllipse(cx + 30, cy + 12, 5, 16, s.innerEar);
      break;
    case AnimalType::Bear:
    default:
      c.fillCircle(cx - 22, cy - 22, 9, s.body);
      c.fillCircle(cx + 22, cy - 22, 9, s.body);
      c.fillCircle(cx - 22, cy - 22, 4, s.innerEar);
      c.fillCircle(cx + 22, cy - 22, 4, s.innerEar);
  }
  // Muzzle
  c.fillEllipse(cx, cy + 14, 16, 9, s.muzzle);
  // Eyes
  c.fillCircle(cx - 9, cy + 2, 3, c.color565(255, 255, 255));
  c.fillCircle(cx + 9, cy + 2, 3, c.color565(255, 255, 255));
  c.fillCircle(cx - 9, cy + 2, 2, c.color565(40, 30, 25));
  c.fillCircle(cx + 9, cy + 2, 2, c.color565(40, 30, 25));
  // Nose
  if (type == AnimalType::Cat) {
    c.fillTriangle(cx - 3, cy + 9, cx + 3, cy + 9, cx, cy + 13, s.nose);
  } else {
    c.fillTriangle(cx - 4, cy + 8, cx + 4, cy + 8, cx, cy + 13, s.nose);
  }
}

void drawAnimalSelectScreen(M5Canvas& c, const AnimalSelectView& v) {
  c.fillSprite(c.color565(30, 30, 40));

  uint16_t white = c.color565(240, 240, 240);
  uint16_t panel = c.color565(255, 240, 240);
  uint16_t panelSel = c.color565(255, 220, 180);
  uint16_t glyph = c.color565( 60,  50,  50);
  uint16_t selBorder = c.color565(255, 150,  50);

  // Title — first-run shows a friendly welcome instead of the bare label.
  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(white);
  c.setCursor(20, 10);
  c.print(tr(v.firstRun ? Str::AnimalWelcome : Str::AnimalTitle));

  // Back X (only when not first-run)
  if (!v.firstRun) {
    const Rect& r = kAnimalBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // Three choice buttons
  static const Str names[3] = {
    Str::AnimalBear, Str::AnimalCat, Str::AnimalDog
  };
  for (int i = 0; i < 3; ++i) {
    const Rect& r = kAnimalChoiceRect[i];
    bool sel = ((int)v.current == i);
    uint16_t bg     = sel ? panelSel : panel;
    uint16_t border = sel ? selBorder : glyph;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 10, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 10, border);
    if (sel) c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, border);

    drawAnimalAvatar(c, r.x + r.w / 2, r.y + 60, (AnimalType)i);

    // Name label
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(glyph);
    c.drawString(tr(names[i]), r.x + r.w / 2, r.y + r.h - 30);
  }

  c.setTextDatum(top_left);
  (void)v.now_ms;
}

void drawToySelectScreen(M5Canvas& c, const ToySelectView& v) {
  c.fillSprite(c.color565(30, 30, 40));

  uint16_t white     = c.color565(240, 240, 240);
  uint16_t panel     = c.color565(255, 240, 240);
  uint16_t panelSel  = c.color565(255, 220, 180);
  uint16_t glyph     = c.color565( 60,  50,  50);
  uint16_t selBorder = c.color565(255, 150,  50);
  uint16_t boredCol  = c.color565(220, 180,  80);

  // Title
  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(white);
  c.setCursor(20, 10);
  c.print(tr(Str::ToyTitle));

  // Back X
  {
    const Rect& r = kToySelectBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  static const Str names[5] = {
    Str::ToyNameBall, Str::ToyNameMouse, Str::ToyNameRattle,
    Str::ToyNameButterfly, Str::ToyNamePlush
  };
  for (int i = 0; i < 5; ++i) {
    const Rect& r = kToyChoiceRect[i];
    bool sel = ((int)v.current == i);
    uint16_t bg     = sel ? panelSel : panel;
    uint16_t border = sel ? selBorder : glyph;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 8, border);
    if (sel) c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, border);

    int cx = r.x + r.w / 2;
    int cy = r.y + 70;
    drawToy(c, (Toy)i, cx, cy, true, v.now_ms);

    // Boredom indicator on the currently-selected toy.
    if (sel && v.repeatCount >= 3) {
      c.fillCircle(cx + 22, cy - 28, 6, boredCol);
      c.setTextDatum(middle_center);
      c.setTextSize(1);
      c.setTextColor(c.color565(70, 50, 20));
      c.drawString("!", cx + 22, cy - 28);
    }

    // Name label
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(glyph);
    c.drawString(tr(names[i]), r.x + r.w / 2, r.y + r.h - 28);
  }

  // Hint when bored
  if (v.repeatCount >= 3) {
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(255, 220, 150));
    c.drawString(tr(Str::ToyBoredHint), 160, 222);
  }
  c.setTextDatum(top_left);
}

// Tiny scene preview painted into a Rect on the travel-select screen.
// Cells are 100×60 with a 14 px label band at the bottom — drawing area is
// effectively 100×46.
void drawScenePreview(M5Canvas& c, Scene s, const Rect& r, TimePhase phase,
                      uint32_t now_ms) {
  switch (s) {
    case Scene::Meadow: {
      uint16_t sky = (phase == TimePhase::Night)
                       ? c.color565( 60,  70, 110)
                       : c.color565(140, 200, 240);
      c.fillRect(r.x, r.y, r.w, r.h, sky);
      uint16_t sun = (phase == TimePhase::Night)
                       ? c.color565(220, 220, 230) : c.color565(255, 220, 90);
      c.fillCircle(r.x + 18, r.y + 12, 6, sun);
      c.fillEllipse(r.x + 30, r.y + 42, 35, 10, c.color565(110, 175, 95));
      c.fillEllipse(r.x + 75, r.y + 44, 30,  9, c.color565( 80, 145,  70));
      break;
    }
    case Scene::Bedroom: {
      uint16_t wallA = c.color565(255, 220, 230);
      uint16_t wallB = c.color565(245, 200, 220);
      c.fillRect(r.x, r.y, r.w, r.h, wallA);
      for (int x = r.x + 6; x < r.x + r.w; x += 12) {
        c.fillRect(x, r.y, 6, 32, wallB);
      }
      c.fillRect(r.x, r.y + 32, r.w,  3, c.color565(120, 80, 50));
      c.fillRect(r.x, r.y + 35, r.w, 11, c.color565(180, 130, 85));
      // Window
      c.fillRect(r.x + 60, r.y + 8, 20, 16, c.color565(150, 200, 235));
      c.drawRect(r.x + 60, r.y + 8, 20, 16, c.color565( 80,  60,  40));
      c.drawFastVLine(r.x + 70, r.y + 8, 16, c.color565(80, 60, 40));
      break;
    }
    case Scene::Forest: {
      uint16_t sky = c.color565( 65, 115,  85);
      c.fillRect(r.x, r.y, r.w, r.h, sky);
      for (int x = r.x - 4; x < r.x + r.w + 4; x += 12) {
        c.fillCircle(x, r.y + 8, 9, c.color565(35, 85, 55));
      }
      for (int i = 0; i < 4; ++i) {
        int tx = r.x + 14 + i * 22;
        c.fillRect(tx, r.y + 16, 3, 22, c.color565(80, 55, 35));
      }
      c.fillRect(r.x, r.y + 38, r.w,  8, c.color565(70, 110, 55));
      break;
    }
    case Scene::Beach: {
      uint16_t sky = c.color565(160, 215, 245);
      c.fillRect(r.x, r.y, r.w, r.h, sky);
      c.fillCircle(r.x + 78, r.y + 12, 6, c.color565(255, 220, 90));
      // Sea
      c.fillRect(r.x, r.y + 22, r.w, 12, c.color565(60, 145, 200));
      c.drawFastHLine(r.x, r.y + 22, r.w, c.color565(20, 80, 130));
      // Sand
      c.fillRect(r.x, r.y + 34, r.w, 12, c.color565(245, 220, 165));
      // Mini palm
      c.fillRect(r.x + 18, r.y + 26, 2, 18, c.color565(110,  70,  35));
      c.fillTriangle(r.x + 18, r.y + 28, r.x +  8, r.y + 22, r.x + 22, r.y + 18,
                     c.color565( 65, 145,  60));
      c.fillTriangle(r.x + 20, r.y + 28, r.x + 30, r.y + 22, r.x + 22, r.y + 18,
                     c.color565( 65, 145,  60));
      break;
    }
    case Scene::Desert: {
      uint16_t sky = c.color565(255, 180, 110);
      c.fillRect(r.x, r.y, r.w, r.h, sky);
      c.fillCircle(r.x + 70, r.y + 14, 9, c.color565(255, 240, 130));
      c.fillEllipse(r.x + 25, r.y + 38, 40, 10, c.color565(245, 195, 130));
      c.fillEllipse(r.x + 80, r.y + 42, 30, 10, c.color565(225, 165, 100));
      c.fillRect(r.x, r.y + 42, r.w, 4, c.color565(200, 140, 85));
      // Mini cactus
      int cx = r.x + 18, cy = r.y + 38;
      c.fillRect(cx - 2, cy - 16, 4, 16, c.color565(70, 130, 70));
      c.fillRect(cx + 1, cy - 10, 4,  5, c.color565(70, 130, 70));
      c.fillRect(cx + 1, cy - 10, 7,  2, c.color565(70, 130, 70));
      break;
    }
    case Scene::Space: {
      c.fillRect(r.x, r.y, r.w, r.h, c.color565(8, 5, 22));
      for (int i = 0; i < 24; ++i) {
        uint32_t rr = (uint32_t)i * 2654435761u;
        int sx = r.x + ((rr >> 8)  % r.w);
        int sy = r.y + ((rr >> 18) % (r.h - 14));
        c.drawPixel(sx, sy, c.color565(220, 220, 220));
      }
      // Planet with rings
      c.fillCircle (r.x + 70, r.y + 32, 10, c.color565(200, 130, 80));
      c.drawEllipse(r.x + 70, r.y + 32, 16,  4, c.color565(220, 180, 130));
      // Moon
      c.fillCircle(r.x + 22, r.y + 14, 5, c.color565(220, 220, 230));
      break;
    }
    case Scene::City: {
      uint16_t sky = (phase == TimePhase::Night)
                       ? c.color565( 25,  30,  60)
                       : c.color565(170, 200, 230);
      c.fillRect(r.x, r.y, r.w, r.h, sky);
      // Background skyline
      uint16_t farTone = (phase == TimePhase::Night)
                           ? c.color565( 50,  55,  90)
                           : c.color565(150, 165, 195);
      c.fillRect(r.x +  4, r.y + 26, 14, 18, farTone);
      c.fillRect(r.x + 14, r.y + 18, 16, 26, farTone);
      c.fillRect(r.x + 30, r.y + 22, 12, 22, farTone);
      c.fillRect(r.x + 60, r.y + 24, 14, 20, farTone);
      c.fillRect(r.x + 76, r.y + 18, 18, 26, farTone);
      // Foreground skyscrapers
      uint16_t midTone = (phase == TimePhase::Night)
                           ? c.color565( 30,  35,  55)
                           : c.color565( 90,  90, 110);
      uint16_t winLit  = c.color565(255, 220, 90);
      c.fillRect(r.x +  6, r.y + 14, 18, 32, midTone);
      c.fillRect(r.x + 28, r.y +  6, 22, 40, midTone);   // tallest
      c.fillRect(r.x + 54, r.y + 18, 18, 28, midTone);
      c.fillRect(r.x + 76, r.y + 12, 18, 34, midTone);
      // Roof bumps
      c.fillRect(r.x + 36, r.y +  2, 6, 4, midTone);
      // Window dots — small lit windows
      if (phase == TimePhase::Night) {
        for (int wy = r.y + 18; wy < r.y + 44; wy += 5) {
          c.drawPixel(r.x + 12, wy, winLit);
          c.drawPixel(r.x + 18, wy, winLit);
          c.drawPixel(r.x + 32, wy, winLit);
          c.drawPixel(r.x + 40, wy, winLit);
          c.drawPixel(r.x + 60, wy, winLit);
          c.drawPixel(r.x + 82, wy, winLit);
          c.drawPixel(r.x + 88, wy, winLit);
        }
      } else {
        uint16_t winDay = c.color565(180, 200, 220);
        for (int wy = r.y + 18; wy < r.y + 44; wy += 5) {
          c.drawPixel(r.x + 12, wy, winDay);
          c.drawPixel(r.x + 32, wy, winDay);
          c.drawPixel(r.x + 60, wy, winDay);
          c.drawPixel(r.x + 82, wy, winDay);
        }
      }
      // Street strip
      c.fillRect(r.x, r.y + 44, r.w, 2, c.color565(80, 80, 90));
      break;
    }
  }
  (void)now_ms;
}

void drawActivityScreen(M5Canvas& c, const ActivityView& v) {
  switch (v.scene) {
    case Scene::Meadow:  drawActivityButterflyGame(c, v); break;
    case Scene::Bedroom: drawActivityStackGame    (c, v); break;
    case Scene::Forest:  drawActivityMushroomGame (c, v); break;
    case Scene::Beach:   drawActivitySurfGame     (c, v); break;
    case Scene::Desert:  drawActivityScorpionGame (c, v); break;
    case Scene::Space:   drawActivityAsteroidsGame(c, v); break;
    case Scene::City:    drawActivityCrossGame    (c, v); break;
  }

  // Common back X (top-right) on every activity screen
  {
    const Rect& r = kActivityBackRect;
    uint16_t bp = c.color565(255, 240, 240);
    uint16_t bg = c.color565( 60,  50,  50);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, bp);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, bg);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, bg);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, bg);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, bg);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, bg);
  }

  if (v.gameOver) {
    drawActivityGameOver(c, v);
  }
}

// ─── Timer screens / countdown badge / expiry overlay ──────────────────────

static void formatHMSorMS(char* buf, size_t bufLen, uint32_t ms) {
  uint32_t totalSec = (ms + 999) / 1000;
  uint32_t mins = totalSec / 60;
  uint32_t secs = totalSec % 60;
  if (mins >= 100) {
    // overflow display
    snprintf(buf, bufLen, "99:59");
    return;
  }
  snprintf(buf, bufLen, "%02u:%02u", (unsigned)mins, (unsigned)secs);
}

void drawTimerScreen(M5Canvas& c, const TimerView& v) {
  c.fillSprite(c.color565(20, 20, 30));

  uint16_t white     = c.color565(240, 240, 240);
  uint16_t panel     = c.color565(255, 240, 240);
  uint16_t glyph     = c.color565( 60,  50,  50);
  uint16_t accent    = c.color565(255, 150,  60);
  uint16_t panelSel  = c.color565(255, 220, 180);

  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(white);
  c.setCursor(20, 10);
  c.print(tr(Str::TimerTitle));

  // Back X
  {
    const Rect& r = kTimerBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  if (v.active) {
    // Running screen — big countdown + Stoppen button
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(200, 200, 220));
    c.drawString(tr(Str::TimerRemaining), 160, 50);

    char buf[16];
    formatHMSorMS(buf, sizeof(buf), v.remainingMs);
    c.setTextSize(7);
    c.setTextColor(accent);
    c.drawString(buf, 160, 72);

    // Stop button
    const Rect& r = kTimerStopRect;
    uint16_t red = c.color565(220, 60, 60);
    uint16_t redDark = c.color565(140, 25, 25);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 12, red);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 12, redDark);
    c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 11, redDark);
    c.setTextDatum(middle_center);
    c.setTextSize(3);
    c.setTextColor(white);
    c.drawString(tr(Str::TimerStop), r.x + r.w / 2, r.y + r.h / 2);
    c.setTextDatum(top_left);
    return;
  }

  // Idle screen — preset picker. Labels at size 2 so they fit comfortably
  // inside the 100-wide buttons (size 3 was clipping the wider entries).
  // The 6th button shows a pencil icon instead of a "Custom" label —
  // visually clearer and works for non-readers.
  static const char* labels[5] = {
    "1 min", "5 min", "15 min", "30 min", "60 min"
  };
  for (int i = 0; i < 6; ++i) {
    const Rect& r = kTimerPresetRect[i];
    bool isCustom = (i == 5);
    uint16_t bg     = isCustom ? panelSel : panel;
    uint16_t border = isCustom ? accent   : glyph;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 10, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 10, border);
    if (isCustom) c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, border);

    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    if (isCustom) {
      // Pencil sprite — angled body, graphite tip on the lower-left,
      // pink eraser ferrule on the upper-right.
      uint16_t wood    = c.color565(245, 215, 130);
      uint16_t woodSh  = c.color565(180, 140,  60);
      uint16_t graph   = c.color565( 60,  55,  60);
      uint16_t eraser  = c.color565(240, 130, 160);
      uint16_t ferrule = c.color565(180, 180, 200);
      // Body (rotated rectangle approximated with parallelogram fill):
      // top-left to bottom-right diagonal pencil.
      c.fillTriangle(cx - 22, cy + 12, cx - 18, cy + 16, cx + 14, cy - 16, wood);
      c.fillTriangle(cx - 18, cy + 16, cx + 18, cy - 12, cx + 14, cy - 16, wood);
      // Pencil outline
      c.drawLine(cx - 22, cy + 12, cx + 14, cy - 16, woodSh);
      c.drawLine(cx - 18, cy + 16, cx + 18, cy - 12, woodSh);
      c.drawLine(cx - 22, cy + 12, cx - 18, cy + 16, woodSh);
      c.drawLine(cx + 14, cy - 16, cx + 18, cy - 12, woodSh);
      // Graphite tip (lower-left corner)
      c.fillTriangle(cx - 22, cy + 12, cx - 18, cy + 16, cx - 26, cy + 18, graph);
      c.drawLine(cx - 22, cy + 12, cx - 26, cy + 18, woodSh);
      c.drawLine(cx - 18, cy + 16, cx - 26, cy + 18, woodSh);
      // Ferrule + eraser at the top-right
      c.fillTriangle(cx + 14, cy - 16, cx + 18, cy - 12, cx + 19, cy - 18, ferrule);
      c.fillTriangle(cx + 18, cy - 12, cx + 22, cy - 14, cx + 19, cy - 18, ferrule);
      c.fillTriangle(cx + 19, cy - 18, cx + 22, cy - 14, cx + 24, cy - 19, eraser);
    } else {
      c.setTextDatum(middle_center);
      c.setTextSize(2);
      c.setTextColor(glyph);
      c.drawString(labels[i], cx, cy);
    }
  }
  c.setTextDatum(top_left);
  (void)v.now_ms;
  (void)v.durationMs;
}

void drawTimerCustomScreen(M5Canvas& c, const TimerCustomView& v) {
  c.fillSprite(c.color565(20, 20, 30));

  uint16_t white  = c.color565(240, 240, 240);
  uint16_t panel  = c.color565(255, 240, 240);
  uint16_t glyph  = c.color565( 60,  50,  50);
  uint16_t accent = c.color565(255, 150,  60);
  uint16_t green  = c.color565( 90, 180, 110);
  uint16_t greenSh = c.color565( 50, 110,  70);

  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(white);
  c.setCursor(20, 10);
  c.print(tr(Str::TimerCustomTitle));

  // Back X
  {
    const Rect& r = kTimerCustomBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // Two large tap zones for minutes / seconds
  uint16_t cellBg = c.color565(255, 245, 230);
  uint16_t cellBd = c.color565(150, 100, 50);
  for (int side = 0; side < 2; ++side) {
    const Rect& r = side == 0 ? kTimerCustomMinRect : kTimerCustomSecRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 10, cellBg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 10, cellBd);

    char buf[8];
    snprintf(buf, sizeof(buf), "%02u",
             (unsigned)(side == 0 ? v.minutes : v.seconds));
    c.setTextDatum(middle_center);
    c.setTextSize(7);
    c.setTextColor(accent);
    c.drawString(buf, r.x + r.w / 2, r.y + r.h / 2);

    // Caption + increment hint
    c.setTextSize(2);
    c.setTextColor(glyph);
    c.drawString(tr(side == 0 ? Str::TimerCustomMin : Str::TimerCustomSec),
                 r.x + r.w / 2, r.y + 14);
    c.setTextSize(1);
    c.setTextColor(c.color565(120, 90, 50));
    c.drawString(tr(side == 0 ? Str::TimerCustomTapMin : Str::TimerCustomTapSec),
                 r.x + r.w / 2, r.y + r.h - 12);
  }
  // Big colon between cells
  c.setTextDatum(middle_center);
  c.setTextSize(7);
  c.setTextColor(accent);
  c.drawString(":", 160, 115);

  // Reset button
  {
    const Rect& r = kTimerCustomResetRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 8, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 8, glyph);
    c.setTextDatum(middle_center);
    c.setTextSize(2);
    c.setTextColor(glyph);
    c.drawString(tr(Str::TimerReset), r.x + r.w / 2, r.y + r.h / 2);
  }
  // Start button
  {
    const Rect& r = kTimerCustomStartRect;
    bool zero = (v.minutes == 0 && v.seconds == 0);
    uint16_t bg = zero ? c.color565(120, 120, 130) : green;
    uint16_t bd = zero ? c.color565( 70,  70,  80) : greenSh;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 8, bd);
    c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, bd);
    c.setTextDatum(middle_center);
    c.setTextSize(3);
    c.setTextColor(white);
    c.drawString(tr(Str::TimerStart), r.x + r.w / 2, r.y + r.h / 2);
  }
  c.setTextDatum(top_left);
  (void)v.now_ms;
}

// Small pill-shaped timer countdown badge — drawn over modal screens that
// already render their own HUD. `topRight` true → place where the pet-view
// clock sits, else top-center.
void drawTimerBadge(M5Canvas& c, uint32_t remainingMs, uint32_t now_ms,
                    bool topRight) {
  bool urgent = remainingMs < 11000;
  uint8_t pulse = urgent
                    ? 200 + (uint8_t)(55 * sinf(now_ms / 120.0f))
                    : 230;
  uint16_t bg = urgent
                   ? c.color565(pulse, 50, 50)
                   : c.color565( 30,  30,  45);
  uint16_t fg = c.color565(255, 220,  90);
  uint16_t bd = urgent
                   ? c.color565(160, 30, 30)
                   : c.color565(255, 150, 60);

  int w = 78, h = 22;
  int x = topRight ? (320 - w - 4) : (160 - w / 2);
  int y = 6;
  c.fillRoundRect(x, y, w, h, 5, bg);
  c.drawRoundRect(x, y, w, h, 5, bd);

  // Tiny hourglass / clock icon at left
  c.fillCircle(x + 11, y + h / 2, 6, c.color565(40, 30, 30));
  c.drawCircle(x + 11, y + h / 2, 6, fg);
  c.drawFastVLine(x + 11, y + h / 2 - 5, 5, fg);
  c.drawFastHLine(x + 11, y + h / 2,     5, fg);

  // Time text
  char buf[16];
  formatHMSorMS(buf, sizeof(buf), remainingMs);
  c.setTextDatum(middle_center);
  c.setTextSize(2);
  c.setTextColor(urgent ? c.color565(255, 255, 255) : fg);
  c.drawString(buf, x + 11 + 26, y + h / 2 + 1);
  c.setTextDatum(top_left);
}

void drawTimerExpiryOverlay(M5Canvas& c, uint32_t now_ms,
                             uint32_t expireStartMs) {
  uint32_t age = now_ms - expireStartMs;

  // Striped translucent overlay so the pet's expression remains visible
  uint16_t band = c.color565(220, 60, 60);
  for (int y = 0; y < 240; y += 6) {
    int phase = ((y / 6) + (now_ms / 80)) & 1;
    if (phase) c.drawFastHLine(0, y, 320, band);
  }

  // Big pulsing "ALARM!" banner
  float p = (age % 600) / 600.0f;
  uint8_t bri = (uint8_t)(180 + 75 * sinf(p * 6.283f));
  uint16_t bgPanel = c.color565(40, 25, 30);
  uint16_t bdPanel = c.color565(255, bri, bri / 3);
  c.fillRoundRect(40, 30, 240, 50, 10, bgPanel);
  c.drawRoundRect(40, 30, 240, 50, 10, bdPanel);
  c.drawRoundRect(41, 31, 238, 48,  9, bdPanel);
  c.setTextDatum(middle_center);
  c.setTextSize(4);
  c.setTextColor(c.color565(255, bri, bri / 2));
  c.drawString(tr(Str::TimerAlarmTitle), 160, 55);

  // Hint
  if (age > 1500) {
    c.setTextSize(2);
    c.setTextColor(c.color565(255, 240, 200));
    c.drawString(tr(Str::TimerAlarmHint), 160, 210);
  }
  c.setTextDatum(top_left);
}

// ─── Travel-transition vehicles ─────────────────────────────────────────────
//
// One small vehicle sprite per destination scene. Coordinates are centered
// at (x,y) — the vehicle's logical center. The mini-pet is drawn into the
// rider position by each renderer.

static void drawVehicleBike(M5Canvas& c, int x, int y, AnimalType animal,
                            uint32_t elapsed) {
  uint16_t metal  = c.color565( 90,  90, 100);
  uint16_t metalSh= c.color565( 50,  50,  60);
  uint16_t frame  = c.color565(220,  60,  60);
  uint16_t spoke  = c.color565(180, 180, 190);
  // Wheels
  for (int side = -1; side <= 1; side += 2) {
    int wx = x + side * 18;
    c.fillCircle(wx, y + 14, 11, metal);
    c.fillCircle(wx, y + 14,  9, c.color565(50, 50, 60));
    c.drawCircle(wx, y + 14, 11, metalSh);
    // Spokes (rotating)
    float a = elapsed * 0.025f;
    for (int i = 0; i < 4; ++i) {
      float ang = a + i * (float)PI / 2.0f;
      c.drawLine(wx, y + 14,
                 wx + (int)(cosf(ang) * 9.0f),
                 y + 14 + (int)(sinf(ang) * 9.0f),
                 spoke);
    }
    c.fillCircle(wx, y + 14, 2, metalSh);
  }
  // Frame triangle + seat post
  c.drawLine(x - 18, y + 14, x +  3, y -  2, frame);
  c.drawLine(x + 18, y + 14, x +  3, y -  2, frame);
  c.drawLine(x - 18, y + 14, x +  3, y -  2, frame);
  c.drawLine(x - 16, y + 14, x +  5, y -  2, frame);
  c.drawLine(x + 16, y + 14, x +  5, y -  2, frame);
  // Handlebars
  c.drawLine(x +  3, y -  2, x + 14, y - 10, metalSh);
  c.drawLine(x + 14, y - 10, x + 18, y -  6, metalSh);
  // Seat
  c.fillRect(x - 1, y - 4, 6, 2, c.color565(40, 30, 25));
  // Pet on seat (slight bob)
  int bob = (int)(sinf(elapsed / 130.0f) * 1.5f);
  drawPetMini(c, x + 1, y - 18 + bob, animal);
}

static void drawVehicleWalk(M5Canvas& c, int x, int y, AnimalType animal,
                            uint32_t elapsed) {
  // Pet bouncing as if walking; little dust puffs trail behind.
  int bob = ((elapsed / 200) & 1) ? -3 : 0;
  // Tote bag
  uint16_t bag    = c.color565(160, 110,  60);
  uint16_t bagSh  = c.color565(100,  70,  35);
  c.fillRoundRect(x + 14, y - 4, 10, 12, 1, bag);
  c.drawRoundRect(x + 14, y - 4, 10, 12, 1, bagSh);
  c.drawLine(x + 16, y - 4, x + 18, y -  9, bagSh);
  c.drawLine(x + 22, y - 4, x + 20, y -  9, bagSh);
  // Legs (alternating)
  uint16_t leg = c.color565(60, 50, 40);
  if (bob == 0) {
    c.fillRect(x - 7, y + 13, 4, 8, leg);
    c.fillRect(x + 3, y + 13, 4, 4, leg);
  } else {
    c.fillRect(x - 7, y + 13, 4, 4, leg);
    c.fillRect(x + 3, y + 13, 4, 8, leg);
  }
  // Pet
  drawPetMini(c, x, y + bob, animal);
  // Dust trail
  uint16_t dust = c.color565(200, 200, 200);
  c.fillCircle(x - 22, y + 22, 2, dust);
  c.fillCircle(x - 30, y + 23, 1, dust);
}

static void drawVehicleHiker(M5Canvas& c, int x, int y, AnimalType animal,
                             uint32_t elapsed) {
  uint16_t pack   = c.color565( 65, 110,  85);
  uint16_t packSh = c.color565( 40,  75,  55);
  uint16_t stick  = c.color565(110,  70,  35);
  // Backpack on the shoulder
  c.fillRoundRect(x - 18, y - 4, 12, 18, 3, pack);
  c.drawRoundRect(x - 18, y - 4, 12, 18, 3, packSh);
  c.drawFastHLine(x - 18, y +  2, 12, packSh);
  // Walking stick (sways with bob)
  int sway = ((elapsed / 200) & 1) ? -1 : 1;
  c.drawLine(x + 16 + sway, y - 14, x + 22 + sway, y + 18, stick);
  c.drawLine(x + 17 + sway, y - 14, x + 23 + sway, y + 18, stick);
  // Pet
  int bob = ((elapsed / 200) & 1) ? -3 : 0;
  drawPetMini(c, x, y + bob, animal);
}

static void drawVehicleBoat(M5Canvas& c, int x, int y, AnimalType animal,
                            uint32_t elapsed) {
  uint16_t hull   = c.color565(140,  80,  40);
  uint16_t hullSh = c.color565( 90,  50,  20);
  uint16_t mast   = c.color565( 80,  60,  40);
  uint16_t sail   = c.color565(245, 245, 240);
  uint16_t sailSh = c.color565(180, 180, 175);
  uint16_t band   = c.color565(220,  60,  60);
  // Boat hull (hexagonal flat-bottom)
  c.fillTriangle(x - 28, y, x + 28, y, x + 22, y + 12, hull);
  c.fillTriangle(x - 28, y, x - 22, y + 12, x + 22, y + 12, hull);
  c.fillRect(x - 28, y - 2, 56, 2, hull);
  c.drawLine(x - 28, y, x - 22, y + 12, hullSh);
  c.drawLine(x + 28, y, x + 22, y + 12, hullSh);
  c.drawFastHLine(x - 22, y + 12, 44, hullSh);
  c.fillRect(x - 26, y - 2, 52, 2, band);
  // Mast
  c.fillRect(x, y - 28, 2, 28, mast);
  // Sail (triangular, flutters with elapsed)
  int billow = (int)(sinf(elapsed / 140.0f) * 3.0f);
  c.fillTriangle(x + 2, y - 28, x + 2, y - 4, x + 18 + billow, y - 4, sail);
  c.fillTriangle(x + 2, y - 28, x + 18 + billow, y - 4, x + 14 + billow, y - 16, sailSh);
  c.drawTriangle(x + 2, y - 28, x + 2, y - 4, x + 18 + billow, y - 4, hullSh);
  // Pet peeking from cabin
  drawPetMini(c, x - 8, y - 6, animal);
}

static void drawVehicleCamel(M5Canvas& c, int x, int y, AnimalType animal,
                             uint32_t elapsed) {
  uint16_t tan   = c.color565(195, 145,  90);
  uint16_t tanSh = c.color565(140,  95,  55);
  uint16_t belly = c.color565(225, 185, 130);
  // Body
  c.fillRoundRect(x - 22, y, 44, 14, 5, tan);
  c.fillRoundRect(x - 22, y + 6, 44, 8, 4, belly);
  c.drawRoundRect(x - 22, y, 44, 14, 5, tanSh);
  // Two humps
  c.fillCircle(x - 6, y - 4, 9, tan);
  c.fillCircle(x + 8, y - 4, 8, tan);
  c.drawCircle(x - 6, y - 4, 9, tanSh);
  c.drawCircle(x + 8, y - 4, 8, tanSh);
  // Neck + head (front)
  c.fillRoundRect(x + 18, y - 18, 6, 18, 2, tan);
  c.fillEllipse(x + 24, y - 18, 7, 5, tan);
  c.drawEllipse(x + 24, y - 18, 7, 5, tanSh);
  c.fillCircle(x + 26, y - 19, 1, c.color565(20, 20, 30));      // eye
  // Tail
  c.drawLine(x - 22, y, x - 28, y - 5, tanSh);
  // Legs (alternating gait)
  uint16_t leg = tanSh;
  int phase = ((elapsed / 220) & 1);
  c.fillRect(x - 18 + phase, y + 13, 4, 12, leg);
  c.fillRect(x -  6 - phase, y + 13, 4, 12, leg);
  c.fillRect(x +  6 + phase, y + 13, 4, 12, leg);
  c.fillRect(x + 16 - phase, y + 13, 4, 12, leg);
  // Pet on first hump
  int bob = ((elapsed / 220) & 1) ? -1 : 0;
  drawPetMini(c, x - 6, y - 14 + bob, animal);
}

static void drawVehicleRocket(M5Canvas& c, int x, int y, AnimalType animal,
                              uint32_t elapsed) {
  uint16_t metal   = c.color565(220, 220, 230);
  uint16_t metalSh = c.color565(140, 140, 160);
  uint16_t accent  = c.color565(220,  60,  60);
  uint16_t glass   = c.color565(140, 200, 240);
  uint16_t glassSh = c.color565( 60, 130, 180);
  uint16_t flameY  = c.color565(255, 230, 100);
  uint16_t flameO  = c.color565(255, 140,  60);
  // Body
  c.fillRoundRect(x - 11, y - 22, 22, 38, 4, metal);
  c.drawRoundRect(x - 11, y - 22, 22, 38, 4, metalSh);
  // Nose cone
  c.fillTriangle(x - 11, y - 22, x + 11, y - 22, x, y - 38, accent);
  c.drawTriangle(x - 11, y - 22, x + 11, y - 22, x, y - 38, c.color565(140, 30, 30));
  // Window with pet
  c.fillCircle(x, y - 8, 7, glass);
  c.drawCircle(x, y - 8, 7, glassSh);
  drawPetMini(c, x, y - 7, animal);
  // Side fins
  c.fillTriangle(x - 11, y +  8, x - 11, y + 16, x - 20, y + 18, accent);
  c.fillTriangle(x + 11, y +  8, x + 11, y + 16, x + 20, y + 18, accent);
  c.drawTriangle(x - 11, y +  8, x - 11, y + 16, x - 20, y + 18, metalSh);
  c.drawTriangle(x + 11, y +  8, x + 11, y + 16, x + 20, y + 18, metalSh);
  // Flames (animated length)
  int fLen = 8 + ((elapsed / 60) % 6);
  c.fillTriangle(x - 7, y + 16, x + 7, y + 16, x, y + 16 + fLen, flameO);
  c.fillTriangle(x - 4, y + 16, x + 4, y + 16, x, y + 16 + fLen - 4, flameY);
}

static void drawVehicleTaxi(M5Canvas& c, int x, int y, AnimalType animal,
                            uint32_t elapsed) {
  uint16_t yellow   = c.color565(250, 200,  60);
  uint16_t yellowSh = c.color565(180, 130,  20);
  uint16_t glass    = c.color565(180, 220, 240);
  uint16_t glassSh  = c.color565( 90, 140, 180);
  uint16_t tire     = c.color565( 30,  30,  35);
  uint16_t rim      = c.color565(160, 160, 170);
  uint16_t black    = c.color565( 20,  20,  25);
  // Body
  c.fillRoundRect(x - 30, y - 6, 60, 18, 4, yellow);
  c.drawRoundRect(x - 30, y - 6, 60, 18, 4, yellowSh);
  // Roof
  c.fillRoundRect(x - 16, y - 18, 32, 14, 3, yellow);
  c.drawRoundRect(x - 16, y - 18, 32, 14, 3, yellowSh);
  // Windshield + rear (split)
  c.fillRect(x - 14, y - 16, 12, 11, glass);
  c.fillRect(x +  2, y - 16, 12, 11, glass);
  c.drawRect(x - 14, y - 16, 12, 11, glassSh);
  c.drawRect(x +  2, y - 16, 12, 11, glassSh);
  // Pet driver
  drawPetMini(c, x - 7, y - 11, animal);
  // Wheels (rotating)
  float a = elapsed * 0.025f;
  for (int side = -1; side <= 1; side += 2) {
    int wx = x + side * 18;
    c.fillCircle(wx, y + 14, 6, tire);
    c.fillCircle(wx, y + 14, 3, rim);
    int sx1 = wx + (int)(cosf(a) * 4.0f);
    int sy1 = y + 14 + (int)(sinf(a) * 4.0f);
    c.drawLine(wx, y + 14, sx1, sy1, black);
    c.drawLine(wx, y + 14,
               wx - (int)(cosf(a) * 4.0f),
               y + 14 - (int)(sinf(a) * 4.0f), black);
  }
  // Taxi sign
  c.fillRect(x - 8, y - 24, 16, 6, black);
  c.setTextDatum(middle_center);
  c.setTextSize(1);
  c.setTextColor(yellow);
  c.drawString("TAXI", x, y - 20);
  c.setTextDatum(top_left);
}

static void drawVehicleFor(Scene target, M5Canvas& c, int x, int y,
                           AnimalType animal, uint32_t elapsed) {
  switch (target) {
    case Scene::Meadow:  drawVehicleBike  (c, x, y, animal, elapsed); break;
    case Scene::Bedroom: drawVehicleWalk  (c, x, y, animal, elapsed); break;
    case Scene::Forest:  drawVehicleHiker (c, x, y, animal, elapsed); break;
    case Scene::Beach:   drawVehicleBoat  (c, x, y, animal, elapsed); break;
    case Scene::Desert:  drawVehicleCamel (c, x, y, animal, elapsed); break;
    case Scene::Space:   drawVehicleRocket(c, x, y, animal, elapsed); break;
    case Scene::City:    drawVehicleTaxi  (c, x, y, animal, elapsed); break;
  }
}

static const char* travelTransitionTitle(Scene s) {
  switch (s) {
    case Scene::Meadow:  return tr(Str::TransitionMeadow);
    case Scene::Bedroom: return tr(Str::TransitionBedroom);
    case Scene::Forest:  return tr(Str::TransitionForest);
    case Scene::Beach:   return tr(Str::TransitionBeach);
    case Scene::Desert:  return tr(Str::TransitionDesert);
    case Scene::Space:   return tr(Str::TransitionSpace);
    case Scene::City:    return tr(Str::TransitionCity);
  }
  return tr(Str::TravelTitle);
}

// Tints any (r,g,b) base color toward the time-of-day mood. Space callers
// shouldn't invoke this — they paint deep space directly.
static uint16_t tintForPhase(M5Canvas& c, uint8_t r, uint8_t g, uint8_t b,
                             TimePhase phase) {
  int R = r, G = g, B = b;
  switch (phase) {
    case TimePhase::Morning:
      // Soft warm/pink wash over the daytime base.
      R = R * 92 / 100 + 22;
      G = G * 92 / 100 + 12;
      B = B * 95 / 100 + 12;
      break;
    case TimePhase::Day:
      // Base palette unchanged.
      break;
    case TimePhase::Evening:
      // Warm sunset orange, dimmed.
      R = R * 78 / 100 + 38;
      G = G * 55 / 100 + 22;
      B = B * 50 / 100 + 18;
      break;
    case TimePhase::Night:
      // Cool deep-blue night, strongly dimmed.
      R = R * 28 / 100 +  8;
      G = G * 32 / 100 + 12;
      B = B * 55 / 100 + 32;
      break;
  }
  if (R > 255) R = 255;
  if (G > 255) G = 255;
  if (B > 255) B = 255;
  return c.color565((uint8_t)R, (uint8_t)G, (uint8_t)B);
}

// Draws a small sun (day/morning), setting sun (evening) or moon + a few
// stars (night) in the upper sky region. Slow horizontal drift gives a
// faint sense of forward motion.
static void drawTransitionCelestial(M5Canvas& c, uint32_t elapsedMs,
                                    TimePhase phase) {
  // Slow drift right→left so it reads like passing scenery.
  int drift = (int)(elapsedMs / 18) % 360;
  int sx = 280 - drift;
  if (sx < -30) sx += 360;
  int sy = 36;

  if (phase == TimePhase::Night) {
    // A few stars dotted around (deterministic positions).
    uint16_t star = c.color565(240, 240, 220);
    c.drawPixel(40,  20, star);
    c.drawPixel(95,  44, star);
    c.drawPixel(175, 28, star);
    c.drawPixel(225, 60, star);
    c.drawPixel(305, 26, star);
    // Crescent moon
    c.fillCircle(sx, sy, 12, c.color565(245, 240, 220));
    c.fillCircle(sx + 5, sy - 2, 10, c.color565(20, 25, 80));
  } else if (phase == TimePhase::Evening) {
    // Low setting sun, deep orange with a halo.
    int lowY = 78;
    c.fillCircle(sx, lowY, 16, c.color565(255, 160,  90));
    c.fillCircle(sx, lowY, 12, c.color565(255, 200, 120));
    c.fillCircle(sx, lowY,  6, c.color565(255, 230, 160));
  } else {
    // Day / morning: bright yellow sun with short rays.
    uint16_t rayC = c.color565(255, 210,  90);
    for (int i = 0; i < 8; ++i) {
      float a = i * (float)PI / 4.0f;
      int ex = sx + (int)(cosf(a) * 14);
      int ey = sy + (int)(sinf(a) * 14);
      c.drawLine(sx, sy, ex, ey, rayC);
    }
    c.fillCircle(sx, sy, 9, c.color565(255, 230, 100));
    c.fillCircle(sx, sy, 5, c.color565(255, 245, 170));
  }
}

void drawTravelTransition(M5Canvas& c, Scene target, AnimalType animal,
                          uint32_t elapsedMs, TimePhase phase) {
  // Pick a base sky per destination, then tint by time-of-day. Space is
  // a special case — it's already deep night and wouldn't benefit from
  // additional dimming.
  bool isSpace = (target == Scene::Space);
  uint16_t sky;
  if (isSpace) {
    sky = c.color565(10, 8, 28);
  } else {
    uint8_t sr, sg, sb;
    switch (target) {
      case Scene::Meadow:   sr = 160; sg = 210; sb = 240; break;
      case Scene::Bedroom:  sr = 255; sg = 220; sb = 230; break;
      case Scene::Forest:   sr =  95; sg = 140; sb = 110; break;
      case Scene::Beach:    sr = 160; sg = 215; sb = 245; break;
      case Scene::Desert:   sr = 255; sg = 180; sb = 110; break;
      case Scene::City:     sr = 170; sg = 200; sb = 230; break;
      default:              sr = 150; sg = 200; sb = 240; break;
    }
    sky = tintForPhase(c, sr, sg, sb, phase);
  }
  c.fillSprite(sky);

  // Sun / moon in the upper sky for all non-Space transitions.
  if (!isSpace) drawTransitionCelestial(c, elapsedMs, phase);

  if (target == Scene::Space) {
    // Rocket flies upward — world streams downward. Three depth layers of
    // stars at different speeds, the nearest as long vertical streaks.
    uint16_t starFar  = c.color565(120, 130, 180);
    uint16_t starMid  = c.color565(200, 200, 230);
    uint16_t starNear = c.color565(255, 255, 240);

    for (int i = 0; i < 28; ++i) {
      uint32_t r = (uint32_t)(i + 1) * 2654435761u;
      int sx = (int)((r >> 8)  & 0x1FF) % 320;
      int sy = ((int)((r >> 16) & 0xFF) + (int)(elapsedMs / 26)) % 240;
      c.drawPixel(sx, sy, starFar);
    }
    for (int i = 0; i < 16; ++i) {
      uint32_t r = (uint32_t)(i + 101) * 2654435761u;
      int sx = (int)((r >> 8)  & 0x1FF) % 320;
      int sy = ((int)((r >> 16) & 0xFF) + (int)(elapsedMs / 9)) % 260 - 10;
      c.drawFastVLine(sx, sy, 5, starMid);
    }
    for (int i = 0; i < 10; ++i) {
      uint32_t r = (uint32_t)(i + 501) * 2654435761u;
      int sx = (int)((r >> 8)  & 0x1FF) % 320;
      int sy = ((int)((r >> 16) & 0xFF) + (int)(elapsedMs / 4)) % 280 - 20;
      c.drawFastVLine(sx, sy, 14, starNear);
    }

    // Receding Earth at the bottom — shrinks and drops out of frame as
    // the rocket climbs. Visible for the first ~3 s of the trip.
    if (elapsedMs < 3500) {
      float t = (float)elapsedMs / 3500.0f;          // 0..1
      int   ey   = 240 + (int)(t * 60.0f);            // slides down off screen
      int   erad = 70  - (int)(t * 50.0f);            // shrinks 70 → 20
      uint16_t earth   = c.color565( 70, 130, 200);
      uint16_t earthSh = c.color565( 40,  90, 150);
      uint16_t land    = c.color565( 90, 160,  90);
      c.fillCircle(160, ey, erad,     earth);
      c.drawCircle(160, ey, erad,     earthSh);
      c.fillEllipse(140, ey - 8, 18, 6, land);
      c.fillEllipse(180, ey + 6, 14, 5, land);
    }
  } else {
    // Distant parallax band (slow scroll). For Beach the foreground is open
    // ocean (so the boat actually floats), so the distant band shows the
    // sandy coastline instead of distant water. All colours are tinted by
    // the current phase so dusk/night transitions feel coherent with the
    // sky overhead.
    uint8_t br =  80, bg = 130, bb =  90;
    if (target == Scene::Beach)   { br = 245; bg = 220; bb = 165; }
    if (target == Scene::Desert)  { br = 225; bg = 165; bb = 100; }
    if (target == Scene::Bedroom) { br = 245; bg = 200; bb = 220; }
    if (target == Scene::City)    { br = 110; bg = 110; bb = 130; }
    uint16_t bandFar = tintForPhase(c, br, bg, bb, phase);
    c.fillRect(0, 145, 320, 25, bandFar);
    // Slow far-shapes (distant hills / coast / buildings)
    int slow = (int)(elapsedMs / 8) % 80;
    uint16_t hillCity   = tintForPhase(c,  70,  70,  90, phase);
    uint16_t sandMound  = tintForPhase(c, 220, 195, 140, phase);
    uint16_t palmTrunk  = tintForPhase(c, 110,  70,  35, phase);
    uint16_t palmLeaf   = tintForPhase(c,  70, 140,  60, phase);
    uint16_t farHill    = tintForPhase(c,  60, 100,  70, phase);
    for (int i = 0; i < 5; ++i) {
      int bx = ((i * 80) - slow + 360) % 360 - 40;
      if (target == Scene::City) {
        c.fillRect(bx, 130, 30, 40, hillCity);
        // Lit windows on the buildings at evening / night.
        if (phase == TimePhase::Evening || phase == TimePhase::Night) {
          uint16_t litW = (phase == TimePhase::Night)
                            ? c.color565(255, 220, 110)
                            : c.color565(255, 200,  90);
          for (int wy = 134; wy < 168; wy += 6) {
            for (int wx = bx + 4; wx < bx + 30; wx += 6) {
              uint32_t r = (uint32_t)(wx * 73 + wy * 11) * 2654435761u;
              if ((r & 0x7) < 4) c.fillRect(wx, wy, 3, 3, litW);
            }
          }
        }
      } else if (target == Scene::Beach) {
        c.fillEllipse(bx + 14, 162, 22, 5, sandMound);
        c.drawFastVLine(bx + 8, 150, 12, palmTrunk);
        c.fillTriangle(bx +  8, 150, bx +  1, 145, bx + 14, 144, palmLeaf);
      } else {
        c.fillEllipse(bx + 12, 152, 24, 8, farHill);
      }
    }

    // Foreground ground (fast scroll). Colors per destination, tinted.
    uint8_t gr = 95, gg = 155, gb = 75;
    switch (target) {
      case Scene::Meadow:  gr =  95; gg = 155; gb =  75; break;
      case Scene::Bedroom: gr = 180; gg = 130; gb =  85; break;
      case Scene::Forest:  gr =  70; gg = 110; gb =  55; break;
      case Scene::Beach:   gr =  60; gg = 145; gb = 200; break;
      case Scene::Desert:  gr = 220; gg = 175; gb = 110; break;
      case Scene::City:    gr =  80; gg =  80; gb =  90; break;
      default:             gr =  95; gg = 155; gb =  75; break;
    }
    uint16_t ground = tintForPhase(c, gr, gg, gb, phase);
    c.fillRect(0, 170, 320, 70, ground);
    // Fast scrolling stripes / dashes
    int fast = (int)(elapsedMs / 3) % 40;
    uint8_t mr, mg, mb;
    switch (target) {
      case Scene::City:    mr = 250; mg = 220; mb =  90; break;
      case Scene::Beach:   mr = 220; mg = 235; mb = 245; break;
      case Scene::Desert:  mr = 255; mg = 220; mb = 170; break;
      case Scene::Bedroom: mr = 150; mg = 100; mb =  60; break;
      case Scene::Forest:  mr =  45; mg =  85; mb =  45; break;
      default:             mr =  60; mg = 110; mb =  50; break;
    }
    uint16_t mark = tintForPhase(c, mr, mg, mb, phase);
    for (int x = -fast; x < 320; x += 40) {
      if (target == Scene::City) {
        // Dashed lane markings
        c.fillRect(x + 8, 218, 22, 3, mark);
      } else if (target == Scene::Beach) {
        // Wave foam
        c.drawFastHLine(x, 200, 14, mark);
        c.drawFastHLine(x + 18, 218, 12, mark);
      } else {
        // Generic ground texture
        c.drawFastHLine(x + 6, 215, 12, mark);
        c.drawFastHLine(x + 22, 226, 8, mark);
      }
    }
  }

  // Vehicle in center. Most scenes get a vertical bob; the rocket gets a
  // tiny horizontal engine shake instead so the upward motion reads cleanly.
  int bob = 0, jitter = 0;
  if (target == Scene::Space) {
    jitter = (int)(sinf(elapsedMs / 28.0f) * 1.0f);
  } else {
    bob = (int)(sinf(elapsedMs / 220.0f) * 2.0f);
  }
  drawVehicleFor(target, c, 160 + jitter, 175 + bob, animal, elapsedMs);

  // Title banner — fades in over the first 800ms, fades out in last 800ms.
  uint8_t alpha = 255;
  if (elapsedMs < 800) alpha = (uint8_t)(elapsedMs * 255 / 800);
  if (elapsedMs > 7200) {
    uint32_t fade = elapsedMs - 7200;
    if (fade > 800) fade = 800;
    alpha = (uint8_t)(255 - fade * 255 / 800);
  }
  uint16_t fg     = c.color565((255 * alpha) / 255,
                                (240 * alpha) / 255,
                                (200 * alpha) / 255);
  uint16_t shadow = c.color565((40 * alpha) / 255,
                                (30 * alpha) / 255,
                                (20 * alpha) / 255);
  c.setTextDatum(top_center);
  c.setTextSize(3);
  c.setTextColor(shadow);
  c.drawString(travelTransitionTitle(target), 161, 31);
  c.setTextColor(fg);
  c.drawString(travelTransitionTitle(target), 160, 30);
  c.setTextDatum(top_left);
}

void drawTravelSelectScreen(M5Canvas& c, const TravelSelectView& v) {
  c.fillSprite(c.color565(25, 30, 40));

  uint16_t white     = c.color565(240, 240, 240);
  uint16_t panel     = c.color565(255, 240, 240);
  uint16_t glyph     = c.color565( 60,  50,  50);
  uint16_t selBorder = c.color565(255, 150,  50);

  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(white);
  c.setCursor(20, 10);
  c.print(tr(Str::TravelTitle));

  // Back X
  {
    const Rect& r = kTravelSelectBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  static const Str names[7] = {
    Str::SceneMeadow, Str::SceneBedroom, Str::SceneForest, Str::SceneBeach,
    Str::SceneDesert, Str::SceneSpace, Str::SceneCity
  };
  for (int i = 0; i < 7; ++i) {
    const Rect& r = kTravelChoiceRect[i];
    bool sel = ((int)v.current == i);
    uint16_t border = sel ? selBorder : c.color565(120, 120, 130);
    drawScenePreview(c, (Scene)i, r, v.phase, v.now_ms);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 4, border);
    if (sel) {
      c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, border);
      c.drawRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 2, border);
    }
    // Label band at bottom (14 px for the smaller cells)
    c.fillRect(r.x, r.y + r.h - 14, r.w, 14, c.color565(20, 25, 35));
    c.setTextDatum(top_center);
    c.setTextSize(1);
    c.setTextColor(white);
    c.drawString(tr(names[i]), r.x + r.w / 2, r.y + r.h - 11);
  }
  c.setTextDatum(top_left);
}

void drawSyncProgressOverlay(M5Canvas& c, uint8_t stage) {
  uint16_t bg     = c.color565(  8,   0,  18);
  uint16_t purple = c.color565(205, 163, 255);
  uint16_t dim    = c.color565( 90,  70, 120);
  c.fillSprite(bg);

  // WiFi-Glyph wie schon im alten "Checke WiFi"-Screen.
  int icx = 160, icy = 96;
  c.fillCircle(icx, icy + 12, 3, purple);
  c.drawCircle(icx, icy + 12,  9, purple);
  c.drawCircle(icx, icy + 12, 15, purple);
  c.drawCircle(icx, icy + 12, 21, purple);
  c.fillRect(icx - 24, icy + 13, 48, 16, bg);

  c.setTextDatum(top_center);
  c.setTextSize(2);
  c.setTextColor(purple, bg);
  // Stage-Label.
  Str label;
  switch (stage) {
    case 1:  label = Str::BootCheckingWifi;   break;  // Wifi
    case 2:  label = Str::BootSyncingTime;    break;  // Time
    case 3:  label = Str::BootFetchingWorld;  break;  // World
    default: label = Str::BootCheckingWifi;   break;  // Idle/Done — sollte caller filtern
  }
  c.drawString(tr(label), 160, 138);

  // 3 Punkte als Stage-Indikator unter dem Label, der aktuelle Stage
  // hervorgehoben — dem User signalisiert dass die Sync mehrstufig ist.
  int dotsY = 168;
  for (int i = 1; i <= 3; ++i) {
    bool active = (i == stage);
    int dx = 160 + (i - 2) * 16;
    c.fillCircle(dx, dotsY, active ? 5 : 3, active ? purple : dim);
  }
  c.setTextDatum(top_left);
}

void drawRadioConnectingOverlay(M5Canvas& c) {
  // Semi-transparent dimming of the backdrop (scanlines trick — M5GFX
  // does not support true alpha fills without an extra sprite).
  for (int y = 0; y < 240; y += 2) {
    c.drawFastHLine(0, y, 320, c.color565(0, 0, 0));
  }
  uint16_t bg     = c.color565(245, 230, 200);
  uint16_t border = c.color565( 90,  55,  20);
  uint16_t accent = c.color565(255, 150,  50);
  uint16_t fg     = c.color565( 90,  55,  20);
  c.fillRoundRect(50, 90, 220, 70, 12, bg);
  c.drawRoundRect(50, 90, 220, 70, 12, border);
  // Antenna icon on the left: small dot + two wave arcs.
  int ix = 80, iy = 125;
  c.fillCircle(ix, iy, 5, accent);
  for (int r = 10; r <= 18; r += 4) {
    // 90° arc top-right — drawn as a 2-line approximation
    c.drawCircle(ix, iy, r, fg);
    // Mask out the lower and left half
    c.fillRect(ix - r - 1, iy + 1, 2 * r + 2, r + 2, bg);
    c.fillRect(ix - r - 1, iy - r - 1, r + 1, 2 * r + 2, bg);
  }
  c.setTextDatum(middle_center);
  c.setTextSize(2);
  c.setTextColor(fg);
  c.drawString(tr(Str::RadioConnecting), 180, iy);
  c.setTextDatum(top_left);
}

void drawMediaSelectScreen(M5Canvas& c, const MediaSelectView& v) {
  c.fillSprite(c.color565(25, 25, 38));

  uint16_t white     = c.color565(240, 240, 240);
  uint16_t panel     = c.color565(255, 240, 240);
  uint16_t panelSel  = c.color565(255, 220, 180);
  uint16_t glyph     = c.color565( 60,  50,  50);
  uint16_t selBorder = c.color565(255, 150,  50);

  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(white);
  c.setCursor(20, 10);
  c.print(tr(Str::MediaTitle));

  // Back X
  {
    const Rect& r = kMediaSelectBackRect;
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  static const Str names[8] = {
    Str::MediaMovies, Str::MediaGames, Str::MediaInternet, Str::MediaSocial,
    Str::MediaFriendsLabel, Str::MediaRadio, Str::MediaCamera, Str::MediaGallery,
  };
  static const Media types[8] = {
    Media::Movies, Media::Games, Media::Internet, Media::Social,
    Media::Friends, Media::Radio, Media::Camera, Media::Gallery,
  };
  // Icon/label positions depend on cell height: in the flat 80 px cells
  // (HAS_CAMERA layout) the icon has to move up so it doesn't collide
  // with the label. In the tall 160 px cells the old layout point
  // y=70 stays.
  for (int i = 0; i < kMediaChoiceCount; ++i) {
    const Rect& r = kMediaChoiceRect[i];
    bool sel = (types[i] == v.current);
    // Accents: Friends green, Radio amber, Camera/Gallery violet —
    // clearly distinct from the (mood-draining) screen-time options.
    bool friendsCell = (i == 4);
    bool radioCell   = (i == 5);
    bool cameraCell  = (i == 6);
    bool galleryCell = (i == 7);
    uint16_t bg     = friendsCell ? c.color565(220, 240, 220) :
                      radioCell   ? c.color565(245, 230, 200) :
                      cameraCell  ? c.color565(225, 215, 240) :
                      galleryCell ? c.color565(225, 215, 240)
                                  : (sel ? panelSel : panel);
    uint16_t border = friendsCell ? c.color565( 80, 160,  90) :
                      radioCell   ? c.color565(180, 110,  50) :
                      cameraCell  ? c.color565(110,  70, 170) :
                      galleryCell ? c.color565(110,  70, 170)
                                  : (sel ? selBorder : glyph);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 10, bg);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 10, border);
    if (sel) c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, border);

    int iconY  = (r.h >= 120) ? r.y + 70 : r.y + r.h / 2 - 6;
    int labelY = (r.h >= 120) ? r.y + r.h - 24 : r.y + r.h - 12;

    if (friendsCell) {
      // Two-pet "hand-holding" silhouette — clearly different look from
      // the screen-time icons.
      int cx = r.x + r.w / 2, cy = iconY;
      uint16_t pCol  = c.color565( 70, 140,  90);
      uint16_t pCol2 = c.color565( 90, 170, 110);
      c.fillCircle(cx - 9, cy - 4, 8, pCol);
      c.fillCircle(cx + 9, cy - 4, 8, pCol2);
      c.fillCircle(cx - 14, cy - 11, 3, pCol);
      c.fillCircle(cx -  4, cy - 11, 3, pCol);
      c.fillCircle(cx +  4, cy - 11, 3, pCol2);
      c.fillCircle(cx + 14, cy - 11, 3, pCol2);
      // Heart between them
      uint16_t heart = c.color565(220, 70, 110);
      c.fillCircle(cx - 3, cy +  6, 3, heart);
      c.fillCircle(cx + 3, cy +  6, 3, heart);
      c.fillTriangle(cx - 5, cy + 7, cx + 5, cy + 7, cx, cy + 13, heart);
    } else {
      drawMediaIcon(c, types[i], r.x + r.w / 2, iconY);
    }

    c.setTextDatum(top_center);
    c.setTextSize(1);
    uint16_t labelCol = friendsCell ? c.color565( 30,  80,  40) :
                        radioCell   ? c.color565( 90,  55,  20) :
                        (cameraCell || galleryCell)
                                    ? c.color565( 60,  35,  90)
                                    : glyph;
    c.setTextColor(labelCol);
    c.drawString(tr(names[i]), r.x + r.w / 2, labelY);
  }
  c.setTextDatum(top_left);
  (void)v.now_ms;
}

void drawCleaningScreen(M5Canvas& c, const CleaningView& v) {
  // Same landscape as the pet view — "the pet's environment without the
  // pet" — so the user cleans piles in whichever scene the pet is in.
  drawSceneBackground(c, v.scene, v.phase, v.now_ms);

  // Piles to be cleaned
  if (v.pileX && v.pileY) {
    for (int i = 0; i < v.pileCount; ++i) {
      drawPile(c, v.pileX[i], v.pileY[i]);
    }
  }

  // Title
  c.setTextDatum(top_left);
  c.setTextSize(3);
  c.setTextColor(c.color565(255, 255, 255));
  c.setCursor(20, 10);
  c.print(tr(Str::CleaningTitle));

  // Back X — same style as the settings X
  {
    const Rect& r = kCleaningBackRect;
    uint16_t panel = c.color565(255, 240, 240);
    uint16_t glyph = c.color565( 60,  50,  50);
    c.fillRoundRect(r.x, r.y, r.w, r.h, 6, panel);
    c.drawRoundRect(r.x, r.y, r.w, r.h, 6, glyph);
    int cx = r.x + r.w/2, cy = r.y + r.h/2;
    c.drawLine(cx - 7, cy - 7, cx + 7, cy + 7, glyph);
    c.drawLine(cx - 7, cy + 7, cx + 7, cy - 7, glyph);
    c.drawLine(cx - 6, cy - 7, cx + 8, cy + 7, glyph);
    c.drawLine(cx - 6, cy + 7, cx + 8, cy - 7, glyph);
  }

  // Status / instructions
  c.setTextDatum(top_center);
  c.setTextSize(2);
  if (v.pileCount > 0) {
    c.setTextColor(c.color565(245, 245, 250));
    char buf[40];
    snprintf(buf, sizeof(buf), tr(Str::CleaningRemaining),
             (unsigned)v.pileCount);
    c.drawString(buf,                      160, 60);
    c.drawString(tr(Str::CleaningHint),    160, 92);
  } else {
    // Celebration when finished. Pulses a bit until exit.
    uint8_t bright = 200 + (uint8_t)(50 * sinf((v.now_ms % 600) / 600.0f * 2.0f * (float)PI));
    c.setTextColor(c.color565(bright, 255, bright));
    c.setTextSize(4);
    c.drawString(tr(Str::CleaningDoneBig), 160, 80);
    c.setTextSize(2);
    c.setTextColor(c.color565(220, 240, 220));
    c.drawString(tr(Str::CleaningDoneSub), 160, 130);
  }
  c.setTextDatum(top_left);
  (void)v.cleanedAtMs;
}

// ─── Foraging screen ────────────────────────────────────────────────────────

void drawForagingScreen(M5Canvas& c, const ForagingView& v) {
  // Underlying scene background (sky / horizon / scene-specific props).
  drawSceneBackground(c, v.scene, v.phase, v.now_ms);

  // Foraging gameplay props (tree for apples, bush for berries, water for
  // fish) drawn on top of the scene so the user always has a sensible
  // place for the items to live, regardless of the chosen scenery.
  drawForageScene(c, v.now_ms);

  // Items — collected ones lerp toward their inventory slot.
  for (uint8_t i = 0; i < kForageMaxItems; ++i) {
    const ForageItemView& it = v.items[i];
    if (it.type == ForageItem::None) continue;
    int ix = it.x, iy = it.y;
    if (it.flyStartMs != 0) {
      float t = (v.now_ms - it.flyStartMs) / 400.0f;
      if (t < 0) t = 0;
      if (t > 1) t = 1;
      float ease = 1.0f - (1.0f - t) * (1.0f - t);   // ease-out
      ix = it.x + (int)((it.flyToX - it.x) * ease);
      iy = it.y + (int)((it.flyToY - it.y) * ease);
    }
    drawForageItem(c, it.type, ix, iy, it.facingLeft);
  }

  // Inventory strip + back X — drawn last so the panel sits on top.
  drawForageInventoryBar(c, v);

  // Hint text
  c.setTextDatum(top_center);
  c.setTextSize(2);
  c.setTextColor(c.color565(255, 250, 240));
  c.drawString(tr(Str::ForagingHint), 160, 44);
  c.setTextDatum(top_left);
}

void drawClock(M5Canvas& c, TimePhase phase) {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt) || dt.date.year < 2024) return;

  bool dark = (phase == TimePhase::Night);
  uint16_t fg     = dark ? c.color565(255, 255, 255) : c.color565( 30,  25,  25);
  uint16_t shadow = dark ? c.color565( 20,  20,  40) : c.color565(255, 255, 255);

  c.setTextSize(2);
  c.setTextDatum(top_right);

  // HH:MM only — seconds would crowd the corner. The full HH:MM:SS still
  // appears on the time-edit screen.
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d",
           (int)dt.time.hours, (int)dt.time.minutes);

  c.setTextColor(shadow);
  c.drawString(buf, 317, 11);
  c.setTextColor(fg);
  c.drawString(buf, 316, 10);

  c.setTextDatum(top_left);
}

// ─── Battery indicator ──────────────────────────────────────────────────────

void drawBatteryIcon(M5Canvas& c, int x, int y,
                     int level, bool charging, bool dark) {
  uint16_t outline = dark ? c.color565(200, 210, 230) : c.color565(60, 50, 50);

  // Outline-only body so the icon sits clean over any background.
  c.drawRoundRect(x, y, 16, 10, 2, outline);
  // Cap on the right side
  c.fillRect(x + 16, y + 3, 2, 4, outline);

  if (level < 0)   level = 0;
  if (level > 100) level = 100;
  int fillW = (level * 12) / 100;
  if (level > 0 && fillW < 1) fillW = 1;     // always show *something* if non-zero
  if (fillW > 0) {
    uint16_t color = (level > 50) ? c.color565(110, 200,  90)
                   : (level > 20) ? c.color565(230, 180,  60)
                                  : c.color565(220,  90,  90);
    c.fillRect(x + 2, y + 2, fillW, 6, color);
  }

  // Small lightning bolt when charging — drawn in yellow over the fill.
  if (charging) {
    uint16_t bolt = c.color565(255, 230,  90);
    int bx = x + 8, by = y + 5;
    c.drawLine(bx + 2, by - 3, bx,     by,     bolt);
    c.drawLine(bx,     by,     bx + 2, by,     bolt);
    c.drawLine(bx + 2, by,     bx,     by + 3, bolt);
  }
}

// ─── Bottom strip: physical-button hints ────────────────────────────────────

void drawButtonHints(M5Canvas& c, TimePhase phase) {
  bool dark = (phase == TimePhase::Night);
  uint16_t separator = dark ? c.color565( 80,  80, 110) : c.color565(180, 170, 170);
  uint16_t panelCol  = dark ? c.color565( 40,  45,  60) : c.color565(255, 240, 240);
  uint16_t glyphCol  = dark ? c.color565(220, 220, 240) : c.color565( 60,  50,  50);

  // Separator line at the top of the strip
  c.drawFastHLine(20, 218, 280, separator);

  // The three chin buttons are roughly under x = 53, 160, 267.
  static const int cellX[3] = { 53, 160, 267 };
  const int cellY = 230;

  // BtnA — speech bubble "Hi!"  (greet / wake)
  {
    int cx = cellX[0];
    c.fillRoundRect(cx - 14, cellY - 8, 28, 16, 5, panelCol);
    c.drawRoundRect(cx - 14, cellY - 8, 28, 16, 5, glyphCol);
    c.fillTriangle(cx - 9, cellY + 7, cx - 14, cellY + 12, cx - 5, cellY + 7, panelCol);
    c.drawLine(cx - 14, cellY + 12, cx - 9, cellY + 7, glyphCol);
    c.drawLine(cx - 14, cellY + 12, cx - 5, cellY + 7, glyphCol);
    c.setTextSize(1);
    c.setTextDatum(middle_center);
    c.setTextColor(glyphCol);
    c.drawString("Hi!", cx, cellY);
    c.setTextDatum(top_left);
  }

  // BtnB — smiley face (tickle / play)
  {
    int cx = cellX[1];
    uint16_t face = c.color565(255, 210, 80);
    c.fillCircle(cx, cellY, 9, face);
    c.drawCircle(cx, cellY, 9, glyphCol);
    // closed-laughing eyes (^_^) — short horizontal blocks
    c.fillRect(cx - 5, cellY - 2, 3, 1, glyphCol);
    c.fillRect(cx + 2, cellY - 2, 3, 1, glyphCol);
    // smile mouth
    c.drawLine(cx - 4, cellY + 2, cx - 1, cellY + 4, glyphCol);
    c.drawLine(cx - 1, cellY + 4, cx + 1, cellY + 4, glyphCol);
    c.drawLine(cx + 1, cellY + 4, cx + 4, cellY + 2, glyphCol);
  }

  // BtnC — Zzz (sleep)
  {
    int cx = cellX[2];
    c.setTextDatum(top_left);
    c.setTextColor(glyphCol);
    c.setTextSize(2);
    c.setCursor(cx - 9, cellY - 7);
    c.print('Z');
    c.setTextSize(1);
    c.setCursor(cx + 5, cellY + 1);
    c.print('z');
  }
}

