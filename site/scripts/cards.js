// Renders one tiny bear into each .pet-card canvas.  Each canvas has its
// own PixelPet instance running an idle loop with a slow per-card phase
// offset so they don't blink in unison.  Click-on-card cycles the mood.

(function () {
'use strict';

function setupPetCards() {
  const canvases = document.querySelectorAll('canvas[data-pet-canvas]');
  const moodCycle = ['Idle', 'Happy', 'Excited', 'Love', 'Sleepy'];
  let idx = 0;

  canvases.forEach((c, i) => {
    // Pip's card mimics the actual on-device UI: 3 hearts under the
    // bear + a small battery icon, instead of the Core2-style happiness
    // bar and BtnA/B/C hints. The renderer reads opts.variant to switch.
    const variant = c.dataset.petCanvas === 'pip' ? 'pip' : 'core';
    const pet = new window.PixelPet(c, { variant });
    // Stagger initial state per card — visually pleasant, less synchronous.
    pet.lastInteractionMs = -i * 1500;
    pet.idleSaccadeMs    = 1500 + i * 700;
    pet.blinkNextMs      = 1500 + i * 400;
    // Different default mood hints per pet — Muffin happy, Pip excited, etc.
    if (c.dataset.petCanvas === 'muffin')  pet.flashFace('Happy',   60000);
    if (c.dataset.petCanvas === 'visu')    pet.flashFace('Idle',    60000);
    if (c.dataset.petCanvas === 'goo-goo') pet.flashFace('Love',    60000);
    if (c.dataset.petCanvas === 'pip')     pet.flashFace('Excited', 60000);
    pet.start();

    c.addEventListener('click', () => {
      idx = (idx + 1) % moodCycle.length;
      pet.flashFace(moodCycle[idx], 4000);
      // Spawn a heart for tactile feedback.
      pet.spawnFloat('heart', c.width / 2, 60);
    });
  });
}

window.setupPetCards = setupPetCards;

})();
