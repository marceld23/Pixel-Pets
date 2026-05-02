// Click-hotspot dispatcher for the hero pet.  Maps canvas clicks to the
// PET_ZONES rectangles defined in pet.js and forwards the action to the
// pet instance.  Also rotates the hint text to nudge the user into
// trying different zones.

(function () {
'use strict';

function clickAction(canvas, pet) {
  canvas.addEventListener('click', (e) => {
    const rect = canvas.getBoundingClientRect();
    const sx = canvas.width  / rect.width;
    const sy = canvas.height / rect.height;
    const x = (e.clientX - rect.left) * sx;
    const y = (e.clientY - rect.top)  * sy;
    for (const [name, z] of Object.entries(window.PET_ZONES)) {
      if (x >= z.x && x <= z.x + z.w && y >= z.y && y <= z.y + z.h) {
        pet.applyAction(z.action);
        return;
      }
    }
    // Outside any zone — gentle ambient pat anyway.
    pet.applyAction('pet');
  });
}

function rotateHint(hintEl) {
  if (!hintEl) return;
  const en = [
    'Try poking my forehead, cheeks or nose.',
    'Click the "Hi!" button on the chin to wake me.',
    'Click the smiley to tickle me.',
    'Click "Zz" to send me to sleep.',
    'Try clicking my eyes — I\'ll get startled!',
  ];
  const de = [
    'Tipp mich auf die Stirn, die Wangen oder die Nase.',
    'Tipp den „Hi!"-Knopf am Kinn, um mich zu begrüßen.',
    'Tipp den Smiley, um mich zu kitzeln.',
    'Tipp „Zz", um mich schlafen zu schicken.',
    'Tipp auf meine Augen — dann erschrecke ich!',
  ];
  let i = 0;
  setInterval(() => {
    i = (i + 1) % en.length;
    hintEl.dataset.en = en[i];
    hintEl.dataset.de = de[i];
    if (window.applyLang) window.applyLang(window.currentLang());
  }, 5000);
}

window.bindPetInteraction = clickAction;
window.rotatePetHint     = rotateHint;

})();
