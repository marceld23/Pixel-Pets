#include "face_pip.h"

#include <math.h>
#include <string.h>

#include "../i18n.h"

namespace pip {

// ─── Display geometry ─────────────────────────────────────────────────────
//
// StickC PLUS2 portrait — 135 wide, 240 tall. Layout (top → bottom):
//   y=  6.. 22  header "PIP" wordmark (small caps, brand colour)
//   y= 28..118  treat icon (~88 px square area, centered)
//   y=124..142  treat name label (size 2 centred)
//   y=148..162  "Shake me!" hint (size 1 italic-ish)
//   y=180..210  BtnA / BtnB hint strip
//   y=222..238  battery icon at right edge
constexpr int CANVAS_W = TARGET_DISPLAY_W;   // 135
constexpr int CANVAS_H = TARGET_DISPLAY_H;   // 240
constexpr int CANVAS_CX = CANVAS_W / 2;      // 67
constexpr int TREAT_CY = 72;                 // centre of the treat icon

// ── Splash ─────────────────────────────────────────────────────────────────
//
// Same energy as the old splash but the tagline now reads "for your pet"
// (companion framing) instead of "your pet".

void drawSplash(M5Canvas& canvas, uint32_t now_ms, uint32_t start_ms) {
    uint32_t e = now_ms - start_ms;
    canvas.fillSprite(canvas.color565(20, 8, 36));

    // Sparkles — six dots, blinking out of phase.
    static const struct { int x, y; uint8_t off; } sparks[6] = {
        { 22,  35,   0}, { 95,  55,  60}, { 45, 200,  20},
        {110, 175,  90}, { 30, 165, 130}, { 80,  20, 200},
    };
    for (int i = 0; i < 6; ++i) {
        uint8_t phase = (uint8_t)((e / 24 + sparks[i].off) & 0xFF);
        float ph = (float)phase / 255.0f;
        uint8_t bri = (uint8_t)(120 + 130 * (0.5f + 0.5f * sinf(ph * 6.283f)));
        canvas.drawPixel(sparks[i].x, sparks[i].y,
                         canvas.color565(bri, bri / 3, bri));
    }

    canvas.setTextDatum(middle_center);

    // Umbrella brand — tiny "PIXEL PETS" above the main wordmark.
    {
        uint8_t aBrand = (e > 250) ? 200 : (uint8_t)(e * 200 / 250);
        canvas.setTextSize(1);
        canvas.setTextColor(canvas.color565(180 * aBrand / 255,
                                            130 * aBrand / 255,
                                            210 * aBrand / 255));
        canvas.drawString("PIXEL PETS", CANVAS_CX, 75);
    }

    // "Pip" wordmark — pulsing.
    uint8_t mix = (uint8_t)(160 + 80 * sinf(e / 280.0f));
    canvas.setTextSize(5);
    canvas.setTextColor(canvas.color565(120, 60, 180));
    canvas.drawString("Pip", CANVAS_CX + 1, 110 + 1);
    canvas.drawString("Pip", CANVAS_CX - 1, 110 - 1);
    canvas.setTextColor(canvas.color565(255, mix, 255));
    canvas.drawString("Pip", CANVAS_CX, 110);

    // Tagline from 800 ms — now "for your pet" (companion framing).
    if (e > 800) {
        uint8_t a = (e > 1800) ? 255 : (uint8_t)((e - 800) * 255 / 1000);
        canvas.setTextSize(1);
        canvas.setTextColor(canvas.color565(200 * a / 255,
                                            130 * a / 255,
                                            220 * a / 255));
        canvas.drawString(tr(Str::PipSplashTagline), CANVAS_CX, 165);
    }
    canvas.setTextDatum(top_left);
}

// ─── Treat icons ──────────────────────────────────────────────────────────
//
// Three simple, recognisable pixel illustrations centred at (cx, cy).
// Roughly fit into a 90 × 100 box; alpha controls the fade-out during the
// throw animation (0 = invisible, 255 = full).

static uint16_t mix565(M5Canvas& c, uint16_t fg, uint16_t bg, uint8_t alpha) {
    if (alpha >= 255) return fg;
    if (alpha == 0)   return bg;
    uint8_t fr = (fg >> 11) & 0x1F, fG = (fg >> 5) & 0x3F, fB = fg & 0x1F;
    uint8_t br = (bg >> 11) & 0x1F, bG = (bg >> 5) & 0x3F, bB = bg & 0x1F;
    uint8_t r = (uint8_t)((fr * alpha + br * (255 - alpha)) / 255);
    uint8_t g = (uint8_t)((fG * alpha + bG * (255 - alpha)) / 255);
    uint8_t b = (uint8_t)((fB * alpha + bB * (255 - alpha)) / 255);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void drawApple(M5Canvas& c, int cx, int cy, uint8_t alpha,
                      uint16_t bg) {
    uint16_t bodyA = mix565(c, c.color565(220, 60,  60), bg, alpha);
    uint16_t bodyB = mix565(c, c.color565(170, 30,  30), bg, alpha);
    uint16_t leaf  = mix565(c, c.color565( 80, 170, 70), bg, alpha);
    uint16_t stem  = mix565(c, c.color565( 90,  60, 30), bg, alpha);
    uint16_t shine = mix565(c, c.color565(255, 200,200), bg, alpha);

    // Two slightly-offset filled circles read as a classic apple silhouette.
    c.fillCircle(cx - 13, cy + 4, 28, bodyA);
    c.fillCircle(cx + 13, cy + 4, 28, bodyA);
    // Lower body shading for some depth.
    c.fillCircle(cx - 10, cy + 14, 18, bodyB);
    c.fillCircle(cx + 10, cy + 14, 18, bodyB);
    // Stem + leaf.
    c.fillRect(cx - 1, cy - 28, 3, 8, stem);
    c.fillTriangle(cx + 2, cy - 24, cx + 18, cy - 28,
                   cx + 8, cy - 18, leaf);
    // Highlight.
    c.fillEllipse(cx - 14, cy - 6, 5, 8, shine);
}

static void drawCarrot(M5Canvas& c, int cx, int cy, uint8_t alpha,
                       uint16_t bg) {
    uint16_t bodyA = mix565(c, c.color565(240, 130, 30), bg, alpha);
    uint16_t bodyB = mix565(c, c.color565(200,  90, 20), bg, alpha);
    uint16_t leafA = mix565(c, c.color565( 80, 180, 70), bg, alpha);
    uint16_t leafB = mix565(c, c.color565( 50, 130, 40), bg, alpha);
    uint16_t lines = mix565(c, c.color565(180,  70, 10), bg, alpha);

    // Body — wide cone pointing down.
    int topY = cy - 20, botY = cy + 30;
    int topW = 26;
    for (int y = topY; y <= botY; ++y) {
        float t = (float)(y - topY) / (float)(botY - topY);
        int half = (int)(topW * (1.0f - t));
        if (half < 0) half = 0;
        // Slight darker shade in the lower half for depth.
        uint16_t col = (y > cy + 5) ? bodyB : bodyA;
        c.drawFastHLine(cx - half, y, half * 2 + 1, col);
    }
    // Texture lines.
    for (int i = 0; i < 4; ++i) {
        int y = cy - 12 + i * 9;
        c.drawFastHLine(cx - 8 + i, y, 16 - i * 2, lines);
    }
    // Tuft of leaves on top.
    c.fillTriangle(cx - 14, topY,    cx - 4,  topY - 16, cx + 4, topY,    leafA);
    c.fillTriangle(cx -  4, topY,    cx + 4,  topY - 22, cx + 12, topY,   leafA);
    c.fillTriangle(cx +  4, topY,    cx + 14, topY - 14, cx + 18, topY,   leafB);
    c.fillTriangle(cx - 12, topY - 4, cx - 6, topY - 14, cx + 0, topY - 4, leafB);
}

static void drawBone(M5Canvas& c, int cx, int cy, uint8_t alpha,
                     uint16_t bg) {
    uint16_t body = mix565(c, c.color565(245, 240, 220), bg, alpha);
    uint16_t edge = mix565(c, c.color565(180, 170, 140), bg, alpha);
    // Classic dog-bone: shaft + 4 lobes (2 at each end).
    int sw = 50, sh = 14;
    c.fillRoundRect(cx - sw / 2, cy - sh / 2, sw, sh, 4, body);
    // Lobes — circles at the four corners.
    int lr = 13;
    int lx = cx - sw / 2 + 2, rx = cx + sw / 2 - 2;
    c.fillCircle(lx,     cy - 10, lr, body);
    c.fillCircle(lx + 6, cy + 10, lr - 2, body);
    c.fillCircle(rx,     cy - 10, lr, body);
    c.fillCircle(rx - 6, cy + 10, lr - 2, body);
    // Outline pass — single-pixel highlight along the bottom for depth.
    c.drawFastHLine(cx - sw / 2 + 4, cy + sh / 2 - 1, sw - 8, edge);
}

static void drawTreat(M5Canvas& c, TreatKind k, int cx, int cy,
                      uint8_t alpha, uint16_t bg) {
    switch (k) {
        case TreatKind::Apple:  drawApple (c, cx, cy, alpha, bg); break;
        case TreatKind::Carrot: drawCarrot(c, cx, cy, alpha, bg); break;
        case TreatKind::Bone:   drawBone  (c, cx, cy, alpha, bg); break;
    }
}

static const char* treatName(TreatKind k) {
    switch (k) {
        case TreatKind::Apple:  return tr(Str::PipTreatApple);
        case TreatKind::Carrot: return tr(Str::PipTreatCarrot);
        case TreatKind::Bone:   return tr(Str::PipTreatBone);
    }
    return "";
}

// ─── Layout pieces ─────────────────────────────────────────────────────────

static void drawHeader(M5Canvas& c) {
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(190, 130, 230));
    c.drawString("PIP", CANVAS_CX, 6);
    c.setTextDatum(top_left);
}

static void drawBattery(M5Canvas& c, uint8_t pct, bool charging) {
    int bx = CANVAS_W - 22, by = 224;
    uint16_t outline = c.color565(180, 180, 200);
    uint16_t fill    = (pct < 20) ? c.color565(220, 60, 60)
                                  : c.color565(120, 220, 120);
    c.drawRect(bx, by, 16, 8, outline);
    c.fillRect(bx + 16, by + 2, 2, 4, outline);
    int innerW = (int)(pct * 14 / 100);
    c.fillRect(bx + 1, by + 1, innerW, 6, fill);
    if (charging) {
        uint16_t bolt = c.color565(255, 230, 80);
        c.drawLine(bx + 8, by + 1, bx + 6, by + 4, bolt);
        c.drawLine(bx + 6, by + 4, bx + 9, by + 4, bolt);
        c.drawLine(bx + 9, by + 4, bx + 7, by + 7, bolt);
    }
}

// ─── Per-state screens ─────────────────────────────────────────────────────

// Empty placeholder — first page in the BtnA cycle. Tells the user to
// pick something explicit instead of letting accidental motion fire a
// throw. Big "Wähle aus" text, dashed-circle empty-jar glyph, no
// shake response.
static void drawEmpty(M5Canvas& c, const PipView& v) {
    drawHeader(c);

    // Empty-jar glyph: dashed circle outline at the treat position.
    int cy = TREAT_CY;
    uint16_t edge = c.color565(160, 140, 175);
    for (int a = 0; a < 360; a += 18) {
        float r = a * 3.14159f / 180.0f;
        int x = CANVAS_CX + (int)(cosf(r) * 30.0f);
        int y = cy        + (int)(sinf(r) * 30.0f);
        c.fillCircle(x, y, 2, edge);
    }
    // Small "?" inside.
    c.setTextDatum(middle_center);
    c.setTextSize(3);
    c.setTextColor(edge);
    c.drawString("?", CANVAS_CX, cy);

    // Title + sub-line.
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(c.color565(50, 35, 60));
    c.drawString(tr(Str::PipMenuEmpty), CANVAS_CX, 138);

    c.setTextSize(1);
    c.setTextColor(c.color565(120, 100, 140));
    c.drawString(tr(Str::PipMenuEmptyHint), CANVAS_CX, 165);
    c.setTextDatum(top_left);

    drawBattery(c, v.batteryPct, v.charging);
}

static void drawTreatPage(M5Canvas& c, const PipView& v, uint16_t bg) {
    drawHeader(c);

    // Treat icon — centred, gentle bob (±2 px over 2 s sine).
    float bob = sinf((float)v.now_ms / 320.0f) * 2.0f;
    int   tx  = CANVAS_CX + (int)(v.tiltX * 2.0f);
    int   ty  = TREAT_CY  + (int)bob + (int)(v.tiltY * 1.0f);
    TreatKind kind = (v.page == MenuPage::Apple)  ? TreatKind::Apple
                  : (v.page == MenuPage::Carrot) ? TreatKind::Carrot
                                                  : TreatKind::Bone;
    drawTreat(c, kind, tx, ty, /*alpha=*/255, bg);

    // Treat name — large, centred, just below the icon. Doubles as the
    // only on-screen label so the device's purpose is unambiguous without
    // explaining how to use it.
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565( 50,  35,  60));
    c.drawString(treatName(kind), CANVAS_CX, 158);
    c.setTextDatum(top_left);

    drawBattery(c, v.batteryPct, v.charging);
}

// Simple wand glyph — thin diagonal stick with a star tip.
static void drawWandIcon(M5Canvas& c, int cx, int cy) {
    uint16_t stick = c.color565( 90,  60,  30);
    uint16_t tipBg = c.color565(255, 220,  90);
    uint16_t tipFg = c.color565(255, 250, 200);
    // Stick from lower-left to upper-right.
    for (int t = -28; t <= 18; ++t) {
        int sx = cx + t;
        int sy = cy - t;
        c.fillCircle(sx, sy, 3, stick);
    }
    // 4-pointed star at the upper-right tip.
    int tx = cx + 22, ty = cy - 22;
    c.fillCircle(tx, ty, 9, tipBg);
    c.fillTriangle(tx, ty - 14, tx - 4, ty,     tx + 4, ty,     tipBg);
    c.fillTriangle(tx, ty + 14, tx - 4, ty,     tx + 4, ty,     tipBg);
    c.fillTriangle(tx - 14, ty, tx,     ty - 4, tx,     ty + 4, tipBg);
    c.fillTriangle(tx + 14, ty, tx,     ty - 4, tx,     ty + 4, tipBg);
    c.fillCircle(tx, ty, 4, tipFg);
}

static void drawWandPage(M5Canvas& c, const PipView& v) {
    drawHeader(c);

    // Wand icon with a slow rotation-shimmer (sine on bob).
    int dy = (int)(sinf((float)v.now_ms / 280.0f) * 3.0f);
    drawWandIcon(c, CANVAS_CX, TREAT_CY + dy);

    // Title.
    c.setTextDatum(top_center);
    c.setTextSize(3);
    c.setTextColor(c.color565(50, 35, 60));
    c.drawString(tr(Str::PipMenuWand), CANVAS_CX, 138);

    // Either the gesture hint OR the running peak count, depending on
    // whether the user is currently mid-gesture.
    if (v.wandPeakCount > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)v.wandPeakCount);
        c.setTextSize(2);
        c.setTextColor(c.color565(220, 90, 140));
        c.drawString(buf, CANVAS_CX, 168);
    } else {
        c.setTextSize(1);
        c.setTextColor(c.color565(120, 100, 140));
        c.drawString(tr(Str::PipMenuWandHint), CANVAS_CX, 168);
    }
    c.setTextDatum(top_left);

    drawBattery(c, v.batteryPct, v.charging);
}

static void drawIdle(M5Canvas& c, const PipView& v, uint16_t bg) {
    switch (v.page) {
        case MenuPage::Empty:  drawEmpty    (c, v);     break;
        case MenuPage::Wand:   drawWandPage (c, v);     break;
        case MenuPage::Apple:
        case MenuPage::Carrot:
        case MenuPage::Bone:
        default:               drawTreatPage(c, v, bg); break;
    }
}

static void drawThrowing(M5Canvas& c, const PipView& v, uint16_t bg) {
    drawHeader(c);

    // 800 ms animation — treat flies up + fades, "Throw!" overlay below.
    constexpr uint32_t kThrowMs = 800;
    uint32_t age = v.now_ms - v.throwStartMs;
    if (age > kThrowMs) age = kThrowMs;
    float p = (float)age / (float)kThrowMs;            // 0..1

    int   yLift = (int)(p * 90.0f);                    // up to 90 px up
    uint8_t alpha = (uint8_t)(255 * (1.0f - p));

    // Treat — moves up + fades. Pick the kind from the menu page (Wand
    // page never enters Throwing, so we only see treat-pages here).
    TreatKind kind = (v.page == MenuPage::Carrot) ? TreatKind::Carrot
                  : (v.page == MenuPage::Bone)   ? TreatKind::Bone
                                                  : TreatKind::Apple;
    drawTreat(c, kind, CANVAS_CX, TREAT_CY - yLift, alpha, bg);

    // "Throw!" text — flashes in for the first 400 ms then fades.
    uint8_t txtAlpha = 255;
    if (age < 100) {
        txtAlpha = (uint8_t)(age * 255 / 100);
    } else if (age > 500) {
        txtAlpha = (uint8_t)(255 - ((age - 500) * 255 / 300));
    }
    c.setTextDatum(top_center);
    c.setTextSize(3);
    uint16_t flash = mix565(c, c.color565(255, 200,  60), bg, txtAlpha);
    c.setTextColor(flash);
    c.drawString(tr(Str::PipThrown), CANVAS_CX, 130);
    c.setTextDatum(top_left);

    drawBattery(c, v.batteryPct, v.charging);
}

static void drawSleeping(M5Canvas& c, const PipView& /*v*/) {
    // Dark background — already filled by drawPip's bg switch.
    c.setTextDatum(middle_center);
    c.setTextSize(6);
    c.setTextColor(c.color565(140, 110, 180));
    c.drawString("Z", CANVAS_CX - 14, 100);
    c.setTextSize(4);
    c.setTextColor(c.color565(180, 150, 220));
    c.drawString("z", CANVAS_CX + 8, 110);
    c.setTextSize(2);
    c.setTextColor(c.color565(220, 190, 255));
    c.drawString("z", CANVAS_CX + 22, 118);
    c.setTextDatum(top_left);
}

// ─── Public entry point ────────────────────────────────────────────────────

void drawPip(M5Canvas& c, const PipView& v) {
    // Background — soft lavender pink, dark while sleeping.
    uint16_t bg = (v.state == UiState::Sleeping)
        ? c.color565( 30,  20,  40)
        : c.color565(245, 220, 235);
    c.fillSprite(bg);

    switch (v.state) {
        case UiState::Throwing: drawThrowing(c, v, bg); break;
        case UiState::Sleeping: drawSleeping(c, v); break;
        case UiState::Idle:
        default:                 drawIdle(c, v, bg);  break;
    }
}

}  // namespace pip
