// Pixel Pets — JS port of the Core2-style bear renderer.
//
// Single canvas (default 320x240), one bear, five moods.  The drawing
// constants mirror src/face.cpp closely enough that the demo looks at
// home next to a real Core2 / CoreS3 screen.  No external dependencies.
//
// Public API:
//   const pet = new PixelPet(canvas, {
//     animal: 'bear',         // future-proofing — only 'bear' for now
//     scale: 1,               // unused (canvas size drives scale)
//     onMoodChange: fn        // optional, fired when mood changes
//   });
//   pet.start();              // request-animation-frame loop
//   pet.applyAction('greet'); // greet | pet | tickle | sleep | wake |
//                             //   eat | startle | sad | love | excited
//   pet.spawnFloat('heart', x, y);
//   pet.flashFace('Happy', 800);
//
// All methods are safe to call on the same frame; state mutations are
// applied in the next render tick.

(function () {
'use strict';

const W = 320, H = 240;
const CX = 160, CY = 128 + 8;          // shifts head slightly down — same offset as face.cpp
const HEAD_RX = 80, HEAD_RY = 70;
const EYE_DX  = 30;
const EYE_OFF_Y = -16;
const MOUTH_OFF_Y = 28;

const COL = {
  bg:        '#FFEAD8',     // soft warm cream — bedroom-esque
  bgDeep:    '#FFD8C2',
  fur:       '#C19272',
  furDark:   '#8C6850',
  snout:     '#F5DCC4',
  nose:      '#3A2A20',
  white:     '#FFFFFF',
  pupil:     '#2A1F2C',
  blush:     '#F2A6B4',
  heart:     '#E66688',
  star:      '#FFD66B',
  hint:      '#9C7BA8',
  buttonBg:  '#FFFAF5',
  buttonInk: '#3F3252',
  separator: '#B8A2C4',
};

const FACES = ['Idle', 'Happy', 'Excited', 'Sleepy', 'Sad', 'Sleeping',
               'Startled', 'Love', 'Eating', 'Laughing'];

class PixelPet {
  constructor(canvas, opts = {}) {
    this.canvas = canvas;
    this.ctx    = canvas.getContext('2d');
    this.opts   = opts;
    this.lastTimeMs   = 0;
    this.now          = 0;

    // Pet state
    this.face         = 'Idle';
    this.flashFaceUntil = 0;
    this.flashFaceName  = null;
    this.happiness    = 75;
    this.energy       = 80;
    this.fullness    = 80;

    // Animation state
    this.blinkOpen    = 1;        // 1 = fully open, 0 = closed
    this.blinkNextMs  = 1500;
    this.gazeX        = 0;
    this.gazeY        = 0;
    this.gazeTargetX  = 0;
    this.gazeTargetY  = 0;
    this.tiltX        = 0;
    this.tiltY        = 0;
    this.lastInteractionMs = 0;
    this.idleSaccadeMs = 2000;
    this.blushUntilMs  = 0;
    this.shakeUntilMs  = 0;
    this.hopUntilMs    = 0;
    this.hopStartMs    = 0;

    // Float icons (hearts, sparkles, "Z" etc.)
    this.floats = [];

    // Decay
    this.lastDecayMs = 0;
    this.decayMul    = 0.4;       // demo runs slower than the device

    // Mouse position relative to canvas (for gaze tracking)
    canvas.addEventListener('mousemove', (e) => {
      const rect = canvas.getBoundingClientRect();
      const sx = canvas.width  / rect.width;
      const sy = canvas.height / rect.height;
      this._lastMouseX = (e.clientX - rect.left) * sx;
      this._lastMouseY = (e.clientY - rect.top)  * sy;
    });
    canvas.addEventListener('mouseleave', () => {
      this._lastMouseX = null;
      this._lastMouseY = null;
    });

    this._running = false;
  }

  start() {
    if (this._running) return;
    this._running = true;
    const tick = (t) => {
      if (!this._running) return;
      const delta = this.lastTimeMs ? Math.min(60, t - this.lastTimeMs) : 16;
      this.lastTimeMs = t;
      this.now = t;
      this.update(delta);
      this.render();
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }

  stop() { this._running = false; }

  // ── State manipulation ──────────────────────────────────────────────────

  applyAction(action) {
    this.lastInteractionMs = this.now;
    switch (action) {
      case 'pet':
        this.happiness = Math.min(100, this.happiness + 6);
        this.flashFace('Happy', 800);
        this.spawnFloat('heart', 160, 60);
        this.blushUntilMs = this.now + 1500;
        if (this.opts.onSound) this.opts.onSound('happy');
        break;
      case 'tickle':
        this.happiness = Math.min(100, this.happiness + 8);
        this.flashFace('Laughing', 1100);
        this.spawnFloat('heart', 200, 50);
        this.blushUntilMs = this.now + 1800;
        if (this.opts.onSound) this.opts.onSound('tickle');
        break;
      case 'startle':
        this.happiness = Math.max(0, this.happiness - 4);
        this.flashFace('Startled', 600);
        this.shakeUntilMs = this.now + 400;
        if (this.opts.onSound) this.opts.onSound('startle');
        break;
      case 'eat':
        this.fullness = Math.min(100, this.fullness + 18);
        this.flashFace('Eating', 1100);
        this.spawnFloat('apple', 160, 110);
        if (this.opts.onSound) this.opts.onSound('eat');
        break;
      case 'greet':
        this.happiness = Math.min(100, this.happiness + 4);
        this.energy    = Math.min(100, this.energy    + 3);
        this.flashFace('Happy', 900);
        this.spawnFloat('heart', 130, 50);
        if (this.opts.onSound) this.opts.onSound('greet');
        break;
      case 'sleep':
        this.flashFace('Sleeping', 5000);
        if (this.opts.onSound) this.opts.onSound('yawn');
        break;
      case 'wake':
        this.flashFaceUntil = 0;
        this.flashFaceName  = null;
        this.face = 'Idle';
        if (this.opts.onSound) this.opts.onSound('wake');
        break;
      case 'love':
        this.happiness = Math.min(100, this.happiness + 12);
        this.flashFace('Love', 1500);
        for (let i = 0; i < 3; ++i) {
          this.spawnFloat('heart', 150 + i * 20, 60 + (i * 8) % 30);
        }
        if (this.opts.onSound) this.opts.onSound('love');
        break;
      case 'excited':
        this.happiness = Math.min(100, this.happiness + 5);
        this.flashFace('Excited', 1400);
        this.hopStartMs = this.now;
        this.hopUntilMs = this.now + 600;
        if (this.opts.onSound) this.opts.onSound('excited');
        break;
      case 'sad':
        this.happiness = Math.max(0, this.happiness - 8);
        this.flashFace('Sad', 1400);
        if (this.opts.onSound) this.opts.onSound('sad');
        break;
    }
    if (this.opts.onMoodChange) this.opts.onMoodChange(this.currentFace());
  }

  flashFace(name, durMs) {
    this.flashFaceName  = name;
    this.flashFaceUntil = this.now + durMs;
  }

  spawnFloat(kind, fromX, fromY) {
    this.floats.push({
      kind,
      x: fromX,
      y: fromY,
      startMs: this.now,
      durMs: 1500,
      driftX: (Math.random() - .5) * 30,
      driftY: -50 - Math.random() * 30,
    });
  }

  currentFace() {
    if (this.flashFaceName && this.now < this.flashFaceUntil)
      return this.flashFaceName;
    if (this.energy < 18)        return 'Sleeping';
    const idle = this.now - this.lastInteractionMs;
    if (idle > 22000)            return 'Sleeping';
    if (idle > 14000)            return 'Sleepy';
    const mood = this.computeMood();
    if (mood >= 90)              return 'Love';
    if (mood >= 75)              return 'Excited';
    if (mood >= 50)              return 'Happy';
    if (mood >= 25)              return 'Idle';
    return 'Sad';
  }

  computeMood() {
    let lo = this.happiness;
    if (this.energy   < 30) lo = Math.min(lo, this.energy   * 100 / 30);
    if (this.fullness < 30) lo = Math.min(lo, this.fullness * 100 / 30);
    return Math.max(0, Math.min(100, lo));
  }

  // ── Update ──────────────────────────────────────────────────────────────

  update(delta) {
    // Decay (slow — visitors are temporary).
    if (this.now - this.lastDecayMs >= 1000) {
      this.lastDecayMs = this.now;
      this.happiness = Math.max(0, this.happiness - 0.2 * this.decayMul);
      this.energy    = Math.max(0, this.energy    - 0.1 * this.decayMul);
      this.fullness  = Math.max(0, this.fullness  - 0.08 * this.decayMul);
    }

    // Blink — close + open quickly.
    this.blinkNextMs -= delta;
    if (this.blinkNextMs <= 0) {
      // 200 ms blink: 0..100 close, 100..200 open
      this.blinkOpen = Math.abs((this.blinkNextMs + 200) - 100) / 100;
      if (this.blinkNextMs < -200) {
        this.blinkNextMs = 3000 + Math.random() * 2500;
        this.blinkOpen   = 1;
      }
    }

    // Gaze tracking — follow mouse if hovering, otherwise random saccades.
    if (this._lastMouseX != null) {
      // Map mouse position relative to head centre, clamp to ±5 px.
      const dx = (this._lastMouseX - CX) / 30;
      const dy = (this._lastMouseY - CY) / 30;
      this.gazeTargetX = Math.max(-5, Math.min(5, dx));
      this.gazeTargetY = Math.max(-3, Math.min(3, dy));
    } else {
      this.idleSaccadeMs -= delta;
      if (this.idleSaccadeMs <= 0) {
        this.idleSaccadeMs = 1800 + Math.random() * 2400;
        this.gazeTargetX = (Math.random() - .5) * 8;
        this.gazeTargetY = (Math.random() - .5) * 4;
      }
    }
    // Lerp 0.12 / frame (matches face.cpp's 0.15 roughly at 60 fps).
    this.gazeX += (this.gazeTargetX - this.gazeX) * 0.12;
    this.gazeY += (this.gazeTargetY - this.gazeY) * 0.12;

    // Floats — drift up + fade out.
    this.floats = this.floats.filter(f => this.now - f.startMs < f.durMs);
  }

  // ── Render ──────────────────────────────────────────────────────────────

  render() {
    const ctx = this.ctx;
    const face = this.currentFace();

    // Logische Canvas-Größe ist immer 320×240 (Bär ist mit absoluten
    // Pixelkonstanten gezeichnet). Tatsächliche <canvas> kann kleiner
    // sein (Pet-Cards: 180×135 für CoreS3/Core2-Variante, 76×135 für
    // den portrait-formatigen Pip). Vor jedem Frame: Identitäts-Trans-
    // form für den BG-Fill, dann gleichmäßig skalierter + zentrierter
    // Transform für den Bär.
    const cw = this.canvas.width;
    const ch = this.canvas.height;
    const sc = Math.min(cw / W, ch / H);
    const ox = (cw - W * sc) / 2;
    const oy = (ch - H * sc) / 2;
    ctx.setTransform(1, 0, 0, 1, 0, 0);

    // Background — sleeping has a deep palette, otherwise warm cream.
    const sleeping = (face === 'Sleeping' || face === 'Sleepy');
    if (sleeping) {
      ctx.fillStyle = '#1F1530';
      ctx.fillRect(0, 0, cw, ch);
    } else {
      const grad = ctx.createLinearGradient(0, 0, 0, ch);
      grad.addColorStop(0, COL.bg);
      grad.addColorStop(1, COL.bgDeep);
      ctx.fillStyle = grad;
      ctx.fillRect(0, 0, cw, ch);
    }
    // Ab hier alles im 320×240-Logik-Frame — wird auf das echte Canvas
    // gemappt.
    ctx.setTransform(sc, 0, 0, sc, ox, oy);
    if (sleeping) this.drawStars();

    // Pip ist ein Zubehör, kein Pet — hat kein Bärgesicht, sondern eine
    // Treat-Jar-UI: Header, Apfel/Karotte/Knochen großes Icon, Name,
    // Akku. Komplett anderer Renderpfad ohne Bär-Komponenten.
    if (this.opts.variant === 'pip') {
      this.drawPipAccessory();
      this.drawFloats();
      return;
    }

    // Head shake on Startled
    let headShakeX = 0;
    if (this.now < this.shakeUntilMs) {
      headShakeX = (Math.random() - .5) * 8;
    }
    // Hop on Excited
    let hopY = 0;
    if (this.now < this.hopUntilMs) {
      const t = (this.now - this.hopStartMs) / 600;
      hopY = -Math.sin(t * Math.PI) * 18;
    }

    const hx = CX + headShakeX;
    const hy = CY + hopY;

    this.drawBody(hx, hy, sleeping);
    this.drawEars(hx, hy, sleeping);
    this.drawHead(hx, hy, sleeping);
    this.drawSnout(hx, hy);
    this.drawEyesFor(face, hx, hy);
    this.drawMouthFor(face, hx, hy);
    if (this.now < this.blushUntilMs) this.drawBlush(hx, hy);

    this.drawFloats();
    this.drawHud();
    this.drawButtonHints();

    // Sleeping Z's drifting up
    if (face === 'Sleeping' && Math.floor(this.now / 1500) % 2 === 0 &&
        this.now - this.lastZSpawnMs > 1500) {
      this.lastZSpawnMs = this.now;
      this.spawnFloat('z', hx + 30, hy - 40);
    }
  }

  drawBody(hx, hy, sleeping) {
    const ctx = this.ctx;
    ctx.fillStyle = sleeping ? '#7E5F49' : COL.fur;
    ctx.beginPath();
    ctx.ellipse(hx, hy + 78, 72, 48, 0, 0, Math.PI * 2);
    ctx.fill();
    // Subtle shading hint
    ctx.fillStyle = COL.furDark;
    ctx.beginPath();
    ctx.ellipse(hx + 24, hy + 92, 20, 8, 0, 0, Math.PI * 2);
    ctx.fill();
  }

  drawEars(hx, hy, sleeping) {
    const ctx = this.ctx;
    const fur     = sleeping ? '#7E5F49' : COL.fur;
    const furDark = COL.furDark;
    const earX1 = hx - HEAD_RX + 14, earX2 = hx + HEAD_RX - 14;
    const earY  = hy - HEAD_RY + 8;
    ctx.fillStyle = fur;
    ctx.beginPath(); ctx.arc(earX1, earY, 18, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.arc(earX2, earY, 18, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = furDark;
    ctx.beginPath(); ctx.arc(earX1, earY + 1, 9, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.arc(earX2, earY + 1, 9, 0, Math.PI * 2); ctx.fill();
  }

  drawHead(hx, hy, sleeping) {
    const ctx = this.ctx;
    ctx.fillStyle = sleeping ? '#7E5F49' : COL.fur;
    ctx.beginPath();
    ctx.ellipse(hx, hy, HEAD_RX, HEAD_RY, 0, 0, Math.PI * 2);
    ctx.fill();
  }

  drawSnout(hx, hy) {
    const ctx = this.ctx;
    ctx.fillStyle = COL.snout;
    ctx.beginPath();
    ctx.ellipse(hx, hy + MOUTH_OFF_Y - 6, 30, 22, 0, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = COL.nose;
    ctx.beginPath();
    ctx.ellipse(hx, hy + MOUTH_OFF_Y - 14, 8, 6, 0, 0, Math.PI * 2);
    ctx.fill();
  }

  drawBlush(hx, hy) {
    const ctx = this.ctx;
    ctx.fillStyle = COL.blush;
    ctx.globalAlpha = 0.55;
    ctx.beginPath(); ctx.ellipse(hx - 35, hy + 8, 11, 6, 0, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.ellipse(hx + 35, hy + 8, 11, 6, 0, 0, Math.PI * 2); ctx.fill();
    ctx.globalAlpha = 1;
  }

  drawEyesFor(face, hx, hy) {
    switch (face) {
      case 'Sleeping':  return this.drawClosedEyes(hx, hy, true);
      case 'Sleepy':    return this.drawDroopyEyes(hx, hy);
      case 'Happy':     return this.drawHappyArcs(hx, hy);
      case 'Laughing':  return this.drawHappyArcs(hx, hy, true);
      case 'Love':      return this.drawHeartEyes(hx, hy);
      case 'Excited':   return this.drawStarEyes(hx, hy);
      case 'Sad':       return this.drawSadEyes(hx, hy);
      case 'Startled':  return this.drawStartledEyes(hx, hy);
      case 'Eating':    return this.drawIdleEyes(hx, hy, 0.4);
      case 'Idle':
      default:          return this.drawIdleEyes(hx, hy, 1);
    }
  }

  drawIdleEyes(hx, hy, openOverride) {
    const ctx = this.ctx;
    const open = (openOverride != null) ? openOverride : this.blinkOpen;
    const ry = Math.max(1, 11 * open);
    // Sclera
    ctx.fillStyle = COL.white;
    ctx.beginPath(); ctx.ellipse(hx - EYE_DX, hy + EYE_OFF_Y, 9, ry, 0, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.ellipse(hx + EYE_DX, hy + EYE_OFF_Y, 9, ry, 0, 0, Math.PI * 2); ctx.fill();
    if (open > 0.4) {
      // Pupils with gaze offset
      ctx.fillStyle = COL.pupil;
      ctx.beginPath();
      ctx.arc(hx - EYE_DX + this.gazeX, hy + EYE_OFF_Y + this.gazeY, 4, 0, Math.PI * 2);
      ctx.fill();
      ctx.beginPath();
      ctx.arc(hx + EYE_DX + this.gazeX, hy + EYE_OFF_Y + this.gazeY, 4, 0, Math.PI * 2);
      ctx.fill();
      // Highlight
      ctx.fillStyle = COL.white;
      ctx.fillRect(hx - EYE_DX - 1 + this.gazeX, hy + EYE_OFF_Y - 1 + this.gazeY, 2, 2);
      ctx.fillRect(hx + EYE_DX - 1 + this.gazeX, hy + EYE_OFF_Y - 1 + this.gazeY, 2, 2);
    }
  }

  drawHappyArcs(hx, hy, big) {
    const ctx = this.ctx;
    ctx.strokeStyle = COL.pupil;
    ctx.lineWidth = big ? 3 : 2.5;
    ctx.lineCap = 'round';
    const r = big ? 9 : 7;
    ctx.beginPath();
    ctx.arc(hx - EYE_DX, hy + EYE_OFF_Y + 2, r, Math.PI, 0);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(hx + EYE_DX, hy + EYE_OFF_Y + 2, r, Math.PI, 0);
    ctx.stroke();
  }

  drawHeartEyes(hx, hy) {
    const ctx = this.ctx;
    const drawHeart = (cx, cy) => {
      ctx.fillStyle = COL.heart;
      ctx.beginPath(); ctx.arc(cx - 3, cy - 1, 4, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(cx + 3, cy - 1, 4, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath();
      ctx.moveTo(cx - 6, cy);
      ctx.lineTo(cx + 6, cy);
      ctx.lineTo(cx, cy + 7);
      ctx.closePath();
      ctx.fill();
    };
    drawHeart(hx - EYE_DX, hy + EYE_OFF_Y);
    drawHeart(hx + EYE_DX, hy + EYE_OFF_Y);
  }

  drawStarEyes(hx, hy) {
    // Open wide with a highlight star
    this.drawIdleEyes(hx, hy, 1);
    const ctx = this.ctx;
    const drawStar = (cx, cy) => {
      ctx.fillStyle = COL.star;
      ctx.beginPath(); ctx.arc(cx, cy, 1.6, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath();
      ctx.moveTo(cx, cy - 3); ctx.lineTo(cx + 3, cy); ctx.lineTo(cx, cy + 3); ctx.lineTo(cx - 3, cy);
      ctx.closePath(); ctx.fill();
    };
    drawStar(hx - EYE_DX + 3, hy + EYE_OFF_Y - 2);
    drawStar(hx + EYE_DX + 3, hy + EYE_OFF_Y - 2);
  }

  drawSadEyes(hx, hy) {
    const ctx = this.ctx;
    // Drooping arcs (inverted happy)
    ctx.strokeStyle = COL.pupil;
    ctx.lineWidth = 2;
    ctx.lineCap = 'round';
    ctx.beginPath();
    ctx.arc(hx - EYE_DX, hy + EYE_OFF_Y - 2, 7, 0.2, Math.PI - 0.2);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(hx + EYE_DX, hy + EYE_OFF_Y - 2, 7, 0.2, Math.PI - 0.2);
    ctx.stroke();
    // Tear
    ctx.fillStyle = '#79B4D9';
    ctx.beginPath();
    ctx.arc(hx - EYE_DX + 4, hy + EYE_OFF_Y + 6, 2.5, 0, Math.PI * 2);
    ctx.fill();
  }

  drawStartledEyes(hx, hy) {
    const ctx = this.ctx;
    ctx.fillStyle = COL.white;
    ctx.beginPath(); ctx.arc(hx - EYE_DX, hy + EYE_OFF_Y, 11, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.arc(hx + EYE_DX, hy + EYE_OFF_Y, 11, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = COL.pupil;
    ctx.beginPath(); ctx.arc(hx - EYE_DX, hy + EYE_OFF_Y, 3, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.arc(hx + EYE_DX, hy + EYE_OFF_Y, 3, 0, Math.PI * 2); ctx.fill();
  }

  drawClosedEyes(hx, hy /*, withZ */) {
    const ctx = this.ctx;
    ctx.strokeStyle = COL.pupil;
    ctx.lineWidth = 2;
    ctx.lineCap = 'round';
    ctx.beginPath(); ctx.moveTo(hx - EYE_DX - 7, hy + EYE_OFF_Y); ctx.lineTo(hx - EYE_DX + 7, hy + EYE_OFF_Y); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(hx + EYE_DX - 7, hy + EYE_OFF_Y); ctx.lineTo(hx + EYE_DX + 7, hy + EYE_OFF_Y); ctx.stroke();
  }

  drawDroopyEyes(hx, hy) {
    const ctx = this.ctx;
    // Half-closed sclera
    ctx.fillStyle = COL.white;
    ctx.beginPath(); ctx.ellipse(hx - EYE_DX, hy + EYE_OFF_Y + 2, 9, 5, 0, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.ellipse(hx + EYE_DX, hy + EYE_OFF_Y + 2, 9, 5, 0, 0, Math.PI * 2); ctx.fill();
    // Heavy lid
    ctx.strokeStyle = COL.pupil;
    ctx.lineWidth = 2.5;
    ctx.lineCap = 'round';
    ctx.beginPath();
    ctx.moveTo(hx - EYE_DX - 9, hy + EYE_OFF_Y - 1);
    ctx.lineTo(hx - EYE_DX + 9, hy + EYE_OFF_Y);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(hx + EYE_DX - 9, hy + EYE_OFF_Y);
    ctx.lineTo(hx + EYE_DX + 9, hy + EYE_OFF_Y - 1);
    ctx.stroke();
  }

  drawMouthFor(face, hx, hy) {
    const ctx = this.ctx;
    ctx.strokeStyle = COL.pupil;
    ctx.lineWidth = 2.5;
    ctx.lineCap = 'round';
    const my = hy + MOUTH_OFF_Y + 8;
    ctx.beginPath();
    switch (face) {
      case 'Happy':
      case 'Excited':
      case 'Love':
        ctx.arc(hx, my, 9, 0, Math.PI);
        break;
      case 'Laughing':
        // Open mouth oval with tongue
        ctx.fillStyle = '#5C2737';
        ctx.beginPath();
        ctx.ellipse(hx, my, 11, 7, 0, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = '#E8788C';
        ctx.beginPath();
        ctx.ellipse(hx, my + 3, 7, 3, 0, 0, Math.PI * 2);
        ctx.fill();
        return;
      case 'Sad':
        ctx.arc(hx, my + 8, 9, Math.PI, Math.PI * 2);
        break;
      case 'Sleeping':
      case 'Sleepy':
        ctx.moveTo(hx - 5, my);
        ctx.lineTo(hx + 5, my);
        break;
      case 'Startled':
        ctx.arc(hx, my, 5, 0, Math.PI * 2);
        break;
      case 'Eating':
        ctx.fillStyle = '#7C2737';
        ctx.beginPath();
        ctx.ellipse(hx, my, 8, 6, 0, 0, Math.PI * 2);
        ctx.fill();
        return;
      case 'Idle':
      default:
        ctx.arc(hx, my, 6, 0.1, Math.PI - 0.1);
        break;
    }
    ctx.stroke();
  }

  drawFloats() {
    const ctx = this.ctx;
    for (const f of this.floats) {
      const t = (this.now - f.startMs) / f.durMs;
      const x = f.x + f.driftX * t;
      const y = f.y + f.driftY * t;
      const alpha = (t < 0.7) ? 1 : (1 - (t - 0.7) / 0.3);
      ctx.globalAlpha = Math.max(0, alpha);
      switch (f.kind) {
        case 'heart':
          ctx.fillStyle = COL.heart;
          ctx.beginPath(); ctx.arc(x - 3, y - 1, 3, 0, Math.PI * 2); ctx.fill();
          ctx.beginPath(); ctx.arc(x + 3, y - 1, 3, 0, Math.PI * 2); ctx.fill();
          ctx.beginPath();
          ctx.moveTo(x - 5, y);
          ctx.lineTo(x + 5, y);
          ctx.lineTo(x, y + 6);
          ctx.closePath();
          ctx.fill();
          break;
        case 'apple':
          ctx.fillStyle = '#D86060';
          ctx.beginPath(); ctx.arc(x, y, 5, 0, Math.PI * 2); ctx.fill();
          ctx.fillStyle = '#7DA64A';
          ctx.fillRect(x, y - 7, 2, 3);
          break;
        case 'z':
          ctx.fillStyle = '#A2C8E5';
          ctx.font = 'bold 14px sans-serif';
          ctx.textAlign = 'center';
          ctx.fillText('Z', x, y);
          break;
      }
      ctx.globalAlpha = 1;
    }
  }

  drawHud() {
    // Tiny happiness bar in the upper-right corner.
    const ctx = this.ctx;
    const x = W - 64, y = 8;
    ctx.fillStyle = 'rgba(45, 36, 64, 0.18)';
    ctx.fillRect(x, y, 56, 6);
    const w = (this.happiness / 100) * 56;
    ctx.fillStyle = COL.heart;
    ctx.fillRect(x, y, w, 6);
    ctx.fillStyle = '#3D2D52';
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'left';
    ctx.fillText('♥', x - 12, y + 7);
  }

  // Pip Accessory UI — minimal Treat-Jar layout (kein Bär). Mirrors the
  // on-device renderer in src/pip/face_pip.cpp: PIP-Header oben, ein
  // großes Treat-Icon (Apfel/Karotte/Knochen) zentral, Name darunter,
  // Akku unten rechts. Cycelt im Demo automatisch durch die drei Treats.
  // Labels follow the page-level language toggle (window.currentLang()).
  drawPipAccessory() {
    const ctx = this.ctx;
    const cx = W / 2;

    // Treat cycles every 3 seconds (Apple → Carrot → Bone → repeat).
    const treats = ['apple', 'carrot', 'bone'];
    const labels = (window.currentLang && window.currentLang() === 'de')
      ? { apple: 'Apfel', carrot: 'Karotte', bone: 'Knochen' }
      : { apple: 'Apple', carrot: 'Carrot', bone: 'Bone'    };
    const treatIdx = Math.floor(this.now / 3000) % 3;
    const treat = treats[treatIdx];

    // Header "PIP" — small wordmark in the brand purple.
    ctx.fillStyle = '#BE82E6';
    ctx.font = 'bold 22px sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    ctx.fillText('PIP', cx, 15);

    // Treat icon — gentle bob, ±4 px over 2 s.
    const bob = Math.sin(this.now / 320) * 4;
    const ty = 110 + bob;
    this.drawSiteTreat(treat, cx, ty);

    // Treat name — large size below.
    ctx.fillStyle = '#322338';
    ctx.font = 'bold 28px sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    ctx.fillText(labels[treat], cx, 175);

    // Battery — bottom right.
    const bx = W - 32, by = 220;
    ctx.strokeStyle = '#B4B4C8';
    ctx.lineWidth = 1;
    ctx.strokeRect(bx + 0.5, by + 0.5, 22, 11);
    ctx.fillStyle = '#B4B4C8';
    ctx.fillRect(bx + 22, by + 3, 3, 5);
    ctx.fillStyle = '#78DC78';
    ctx.fillRect(bx + 1, by + 1, Math.floor(78 * 20 / 100), 9);
  }

  // Treat-Icon-Helfer — analog zu drawApple/drawCarrot/drawBone in
  // src/pip/face_pip.cpp aber als Canvas-Pfade. Die Größenordnungen
  // (Radii, Höhen) sind ans 320×240-Logik-Frame angepasst.
  drawSiteTreat(kind, cx, cy) {
    const ctx = this.ctx;
    if (kind === 'apple') {
      // Two slightly-offset filled circles read as a classic apple.
      ctx.fillStyle = '#DC3C3C';
      ctx.beginPath(); ctx.arc(cx - 17, cy + 5, 35, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(cx + 17, cy + 5, 35, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#AA1E1E';
      ctx.beginPath(); ctx.arc(cx - 13, cy + 18, 22, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(cx + 13, cy + 18, 22, 0, Math.PI * 2); ctx.fill();
      // Stem + leaf.
      ctx.fillStyle = '#5A3C1E';
      ctx.fillRect(cx - 2, cy - 38, 4, 12);
      ctx.fillStyle = '#50AA46';
      ctx.beginPath();
      ctx.moveTo(cx + 3, cy - 32);
      ctx.lineTo(cx + 24, cy - 38);
      ctx.lineTo(cx + 11, cy - 22);
      ctx.closePath(); ctx.fill();
      // Highlight.
      ctx.fillStyle = '#FFC8C8';
      ctx.beginPath(); ctx.ellipse(cx - 18, cy - 8, 6, 11, 0, 0, Math.PI * 2); ctx.fill();
    } else if (kind === 'carrot') {
      // Tapering body + leafy tuft.
      ctx.fillStyle = '#F0821E';
      ctx.beginPath();
      ctx.moveTo(cx - 32, cy - 26);
      ctx.lineTo(cx + 32, cy - 26);
      ctx.lineTo(cx,      cy + 38);
      ctx.closePath(); ctx.fill();
      ctx.fillStyle = '#C85A14';
      ctx.beginPath();
      ctx.moveTo(cx - 18, cy + 6);
      ctx.lineTo(cx + 18, cy + 6);
      ctx.lineTo(cx,      cy + 38);
      ctx.closePath(); ctx.fill();
      // Texture lines.
      ctx.strokeStyle = '#B4460A';
      ctx.lineWidth = 1.5;
      for (let i = 0; i < 4; ++i) {
        const y = cy - 16 + i * 11;
        ctx.beginPath();
        ctx.moveTo(cx - 10 + i, y);
        ctx.lineTo(cx + 10 - i, y);
        ctx.stroke();
      }
      // Tuft.
      ctx.fillStyle = '#50B446';
      const top = cy - 26;
      ctx.beginPath();
      ctx.moveTo(cx - 18, top);
      ctx.lineTo(cx -  5, top - 22);
      ctx.lineTo(cx +  5, top);
      ctx.closePath(); ctx.fill();
      ctx.beginPath();
      ctx.moveTo(cx -  5, top);
      ctx.lineTo(cx +  5, top - 28);
      ctx.lineTo(cx + 15, top);
      ctx.closePath(); ctx.fill();
      ctx.fillStyle = '#328228';
      ctx.beginPath();
      ctx.moveTo(cx +  5, top);
      ctx.lineTo(cx + 18, top - 18);
      ctx.lineTo(cx + 24, top);
      ctx.closePath(); ctx.fill();
    } else if (kind === 'bone') {
      // Classic dog-bone: shaft + 4 lobes.
      ctx.fillStyle = '#F5F0DC';
      const sw = 64, sh = 18;
      // Shaft.
      ctx.fillRect(cx - sw / 2, cy - sh / 2, sw, sh);
      // Lobes.
      const lr = 17;
      const lx = cx - sw / 2 + 3, rx = cx + sw / 2 - 3;
      ctx.beginPath(); ctx.arc(lx,     cy - 13, lr, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(lx + 8, cy + 13, lr - 3, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(rx,     cy - 13, lr, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(rx - 8, cy + 13, lr - 3, 0, Math.PI * 2); ctx.fill();
      // Edge highlight.
      ctx.strokeStyle = '#B4AA8C';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(cx - sw / 2 + 5, cy + sh / 2 - 1);
      ctx.lineTo(cx + sw / 2 - 5, cy + sh / 2 - 1);
      ctx.stroke();
    }
  }

  drawButtonHints() {
    const ctx = this.ctx;
    const y0 = 218;
    // Separator line
    ctx.strokeStyle = COL.separator;
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(20, y0); ctx.lineTo(W - 20, y0); ctx.stroke();

    const cellY = 230;
    const cellX = [53, 160, 267];

    // BtnA — speech bubble "Hi!"
    ctx.fillStyle = COL.buttonBg;
    this.roundRect(cellX[0] - 14, cellY - 8, 28, 16, 5);
    ctx.fill();
    ctx.fillStyle = COL.buttonInk;
    ctx.font = 'bold 9px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('Hi!', cellX[0], cellY + 1);

    // BtnB — smiley
    ctx.fillStyle = '#FFD86B';
    ctx.beginPath(); ctx.arc(cellX[1], cellY, 9, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = COL.buttonInk;
    // closed eyes
    ctx.fillRect(cellX[1] - 5, cellY - 2, 3, 1);
    ctx.fillRect(cellX[1] + 2, cellY - 2, 3, 1);
    ctx.beginPath();
    ctx.moveTo(cellX[1] - 4, cellY + 2);
    ctx.lineTo(cellX[1],     cellY + 4);
    ctx.lineTo(cellX[1] + 4, cellY + 2);
    ctx.strokeStyle = COL.buttonInk;
    ctx.lineWidth = 1.4;
    ctx.stroke();

    // BtnC — Z
    ctx.fillStyle = COL.buttonInk;
    ctx.font = 'bold 13px sans-serif';
    ctx.textAlign = 'left';
    ctx.fillText('Z', cellX[2] - 9, cellY + 4);
    ctx.font = 'bold 9px sans-serif';
    ctx.fillText('z', cellX[2] + 5, cellY + 7);
  }

  drawStars() {
    const ctx = this.ctx;
    ctx.fillStyle = '#F8F0FF';
    const seeds = [[40, 30], [110, 12], [180, 35], [240, 18], [285, 48],
                   [60, 70], [200, 70], [270, 92], [30, 110]];
    for (const [x, y] of seeds) {
      const flicker = 0.6 + 0.4 * Math.sin(this.now / 800 + x * 0.13);
      ctx.globalAlpha = flicker;
      ctx.fillRect(x, y, 1, 1);
    }
    ctx.globalAlpha = 1;
  }

  // Path-helper because Canvas2D has roundRect only in newer browsers.
  roundRect(x, y, w, h, r) {
    const ctx = this.ctx;
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y,     x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x,     y + h, r);
    ctx.arcTo(x,     y + h, x,     y,     r);
    ctx.arcTo(x,     y,     x + w, y,     r);
    ctx.closePath();
  }
}

// Touch-zone definitions, exported for the interaction handler.
const PET_ZONES = {
  forehead:  { x: CX - 35, y: CY - 70, w: 70, h: 30, action: 'pet' },
  eye_l:     { x: CX - 50, y: CY - 30, w: 28, h: 25, action: 'startle' },
  eye_r:     { x: CX + 22, y: CY - 30, w: 28, h: 25, action: 'startle' },
  cheek_l:   { x: CX - 90, y: CY + 5,  w: 38, h: 30, action: 'pet' },
  cheek_r:   { x: CX + 52, y: CY + 5,  w: 38, h: 30, action: 'pet' },
  mouth:     { x: CX - 25, y: CY + 28, w: 50, h: 22, action: 'eat' },
  btn_greet: { x: 10,      y: 216,     w: 100, h: 24, action: 'greet' },
  btn_tickle:{ x: 110,     y: 216,     w: 100, h: 24, action: 'tickle' },
  btn_sleep: { x: 210,     y: 216,     w: 100, h: 24, action: 'sleep' },
};

// Public exports for other scripts.
window.PixelPet  = PixelPet;
window.PET_ZONES = PET_ZONES;

})();
