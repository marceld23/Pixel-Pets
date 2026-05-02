// Pixel Pets — Web Audio tone synthesiser.
//
// The firmware uses real WAV samples; the demo can't ship 5 MB of audio
// just for a landing page, so we synthesise small "pet" tones at runtime.
// Same idea as Pip's tone-only sound engine in src/pip/sound_pip.cpp,
// adapted to Web Audio.
//
// Public API:
//   const sound = new PetSound();   // lazy-creates the AudioContext on
//                                   //   the first user gesture
//   sound.play('greet');            // greet | happy | tickle | startle |
//                                   //   eat | yawn | wake | love |
//                                   //   excited | sad
//   sound.setMuted(true);           // toggle (mirrored in localStorage)

(function () {
'use strict';

const RECIPES = {
  greet:    [[700,  90], [900,  90], [1100, 110]],
  happy:    [[900,  70], [1100, 90]],
  tickle:   [[1200, 60], [1500, 60], [1800, 80]],
  excited:  [[1400, 60], [1700, 60], [2000, 80]],
  startle:  [[1500, 50], [800,  200]],
  sad:      [[700,  200], [550, 200], [400, 280]],
  yawn:     [[600,  240], [450, 240], [320, 320]],
  wake:     [[500,  100], [700, 100], [1000, 140]],
  love:     [[660,  120], [880, 120], [990, 160]],
  eat:      [[480,  60], [400,  60], [340, 70]],
};

class PetSound {
  constructor() {
    this.ctx = null;
    this.muted = (localStorage.getItem('petsound-muted') === '1');
  }

  setMuted(m) {
    this.muted = !!m;
    localStorage.setItem('petsound-muted', m ? '1' : '0');
  }

  isMuted() { return this.muted; }

  _ensureCtx() {
    if (this.ctx) return;
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return;
    this.ctx = new Ctx();
  }

  play(name) {
    if (this.muted) return;
    this._ensureCtx();
    if (!this.ctx) return;
    if (this.ctx.state === 'suspended') this.ctx.resume();

    const recipe = RECIPES[name];
    if (!recipe) return;

    let t = this.ctx.currentTime;
    const master = this.ctx.createGain();
    master.gain.value = 0.18;
    master.connect(this.ctx.destination);

    for (const [freq, durMs] of recipe) {
      const dur = durMs / 1000;
      const osc = this.ctx.createOscillator();
      const gain = this.ctx.createGain();
      // Triangle wave is gentler than square — closer to a Tamagotchi beep.
      osc.type = 'triangle';
      osc.frequency.setValueAtTime(freq, t);
      // Tiny attack + release so we don't pop.
      gain.gain.setValueAtTime(0, t);
      gain.gain.linearRampToValueAtTime(1, t + 0.01);
      gain.gain.setValueAtTime(1, t + dur - 0.02);
      gain.gain.linearRampToValueAtTime(0, t + dur);
      osc.connect(gain).connect(master);
      osc.start(t);
      osc.stop(t + dur + 0.02);
      t += dur + 0.02;
    }

    // Stop master a bit later, then garbage-collect.
    setTimeout(() => { try { master.disconnect(); } catch (_) {} },
               (t - this.ctx.currentTime) * 1000 + 100);
  }
}

window.PetSound = PetSound;

})();
