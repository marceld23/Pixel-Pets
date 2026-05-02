// Glue script — wires the hero canvas, sound engine, click handler and
// the per-card pet renderers together once DOM is ready.

(function () {
'use strict';

document.addEventListener('DOMContentLoaded', () => {
  const heroCanvas = document.getElementById('pet-canvas');
  if (!heroCanvas || !window.PixelPet) return;

  const sound = new window.PetSound();

  const heroPet = new window.PixelPet(heroCanvas, {
    onSound: (name) => sound.play(name),
  });
  heroPet.start();
  // Greet the visitor on page load — feels alive instead of static.
  setTimeout(() => heroPet.applyAction('greet'), 700);

  if (window.bindPetInteraction) window.bindPetInteraction(heroCanvas, heroPet);
  if (window.rotatePetHint)      window.rotatePetHint(document.getElementById('pet-hint'));

  if (window.setupPetCards) window.setupPetCards();
});

})();
