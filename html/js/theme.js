// SPDX-License-Identifier: GPL-3.0-or-later
/* theme.js - dark/light theme toggle: data-theme + localStorage (dark by default) */
(function () {
  var t = 'dark';
  try { t = localStorage.getItem('theme') || 'dark'; } catch (e) {}
  document.documentElement.setAttribute('data-theme', t);
})();

/* 主题按钮用内联 SVG 而不是 emoji：emoji 的字重、基线、大小跟着系统字体走，
   在车机浏览器上尤其不一致。图标颜色继承 currentColor，自动跟随主题。 */
var ICON_MOON = '<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">' +
  '<path d="M20.2 14.6A8.6 8.6 0 1 1 9.4 3.8a7 7 0 0 0 10.8 10.8z"/></svg>';
var ICON_SUN = '<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">' +
  '<circle cx="12" cy="12" r="4"/>' +
  '<path d="M12 2.6v2.2M12 19.2v2.2M2.6 12h2.2M19.2 12h2.2M5.4 5.4l1.6 1.6M17 17l1.6 1.6M18.6 5.4L17 7M7 17l-1.6 1.6"/></svg>';

function updateThemeBtn() {
  var b = document.getElementById('themeBtn');
  if (!b) return;
  var light = document.documentElement.getAttribute('data-theme') === 'light';
  b.innerHTML = light ? ICON_SUN : ICON_MOON;
  b.setAttribute('aria-label', light ? '切换到深色主题' : '切换到浅色主题');
  b.setAttribute('aria-pressed', light ? 'true' : 'false');
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
