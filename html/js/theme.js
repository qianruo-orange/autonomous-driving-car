// SPDX-License-Identifier: GPL-3.0-or-later
/* theme.js - dark/light theme toggle: data-theme + localStorage (dark by default) */
(function () {
  var t = 'dark';
  try { t = localStorage.getItem('theme') || 'dark'; } catch (e) {}
  document.documentElement.setAttribute('data-theme', t);
})();

function updateThemeBtn() {
  var b = document.getElementById('themeBtn');
  if (b) b.textContent = document.documentElement.getAttribute('data-theme') === 'light' ? '\u2600\ufe0f' : '\ud83c\udf19';
}

function toggleTheme() {
  var d = document.documentElement;
  var n = d.getAttribute('data-theme') === 'light' ? 'dark' : 'light';
  d.setAttribute('data-theme', n);
  try { localStorage.setItem('theme', n); } catch (e) {}
  updateThemeBtn();
}

document.addEventListener('DOMContentLoaded', updateThemeBtn);

/* keep the icon in sync when the language changes */
window.onLangChange = updateThemeBtn;
