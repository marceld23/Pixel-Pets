// Glue script — boots the per-card pet renderers once the DOM is ready.
// The hero used to host an interactive browser-canvas pet; that was
// replaced with a real-hardware video so visitors immediately see this
// is about physical M5 devices, not a browser app. Card renderers
// remain — they're small stylised previews of each pet variant, which
// reads as "this is what it'll look like on your device", not as
// "play with the demo".

(function () {
'use strict';

document.addEventListener('DOMContentLoaded', () => {
  if (window.setupPetCards) window.setupPetCards();
});

})();
