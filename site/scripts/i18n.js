// Minimal i18n: every translatable element carries data-en and data-de
// attributes; the toggle swaps which one becomes textContent.  List
// items inside <ul data-en-items="a|b|c"> are also handled.
//
// Default language is derived from navigator.language; the user's choice
// is stored in localStorage so it survives reloads.

(function () {
'use strict';

function pickDefault() {
  const stored = localStorage.getItem('lang');
  if (stored === 'de' || stored === 'en') return stored;
  const nav = (navigator.language || 'en').toLowerCase();
  return nav.startsWith('de') ? 'de' : 'en';
}

let lang = pickDefault();

function applyLang(target) {
  lang = (target === 'de') ? 'de' : 'en';
  localStorage.setItem('lang', lang);
  document.documentElement.lang = lang;

  document.querySelectorAll('[data-en], [data-de]').forEach((el) => {
    const next = el.dataset[lang];
    if (next != null && el.children.length === 0) {
      el.textContent = next;
    } else if (next != null) {
      // Nodes with children: try to update their first text node only,
      // so trailing inline elements (small, <em>, links) survive.
      const t = [...el.childNodes].find((n) => n.nodeType === 3 && n.textContent.trim());
      if (t) t.textContent = next + ' ';
    }
  });

  // <ul data-en-items="a|b|c"> → rebuild <li>s
  document.querySelectorAll('ul[data-en-items], ul[data-de-items]').forEach((ul) => {
    const raw = ul.dataset[lang + 'Items'];
    if (!raw) return;
    const items = raw.split('|');
    const lis = ul.querySelectorAll('li');
    items.forEach((txt, i) => {
      if (lis[i]) lis[i].textContent = txt;
    });
  });

  // Localised media: data-src-en / data-src-de, data-poster-en / data-poster-de.
  // Lets the same <video> swap its source + poster on language toggle. For
  // VIDEO elements we call .load() after a src change so the new file is
  // actually fetched (the browser keeps the previous decoder otherwise).
  const cap = (lang === 'de') ? 'De' : 'En';
  document.querySelectorAll('[data-src-en], [data-src-de]').forEach((el) => {
    const next = el.dataset['src' + cap];
    if (!next || el.getAttribute('src') === next) return;
    el.setAttribute('src', next);
    if (el.tagName === 'VIDEO') el.load();
  });
  document.querySelectorAll('[data-poster-en], [data-poster-de]').forEach((el) => {
    const next = el.dataset['poster' + cap];
    if (next && el.getAttribute('poster') !== next) el.setAttribute('poster', next);
  });

  // Update the toggle's label
  const btn = document.getElementById('lang-toggle');
  if (btn) btn.textContent = (lang === 'de') ? 'DE / EN' : 'EN / DE';
}

function currentLang() { return lang; }

document.addEventListener('DOMContentLoaded', () => {
  applyLang(lang);
  const btn = document.getElementById('lang-toggle');
  if (btn) {
    btn.addEventListener('click', () => {
      applyLang(lang === 'en' ? 'de' : 'en');
    });
  }
});

window.applyLang   = applyLang;
window.currentLang = currentLang;

})();
