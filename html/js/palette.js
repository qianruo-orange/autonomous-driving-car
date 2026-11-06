// SPDX-License-Identifier: GPL-3.0-or-later
/* palette.js - 界面配色方案切换：<html data-palette> + localStorage
   四套方案（graphite/neon/aurora/amber）只覆盖深色主题下的颜色变量，
   浅色主题保持单一蓝调不变。默认 graphite（石墨极简，用户选定方向）。
   设计原则：界面本体的饱和度压低，高饱和只留给数据和状态（--data-*）。 */

var PALETTES = ['graphite', 'neon', 'aurora', 'amber'];
var PALETTE_LABELS = {
  graphite: '石墨',
  neon: '霓虹',
  aurora: '极光',
  amber: '琥珀'
};

(function () {
  var p = 'graphite';
  try { p = localStorage.getItem('palette') || 'graphite'; } catch (e) {}
  if (PALETTES.indexOf(p) < 0) p = 'graphite';
  document.documentElement.setAttribute('data-palette', p);
})();

/* 调色盘图标（画家调色板），跟随 currentColor 保持单色，不引入额外彩色 */
var ICON_PALETTE = '<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">' +
  '<path d="M12 3.6a8.4 8.4 0 1 0 0 16.8c1.1 0 1.9-.9 1.9-1.9 0-.5-.2-.9-.5-1.2-.3-.3-.4-.7-.4-1.1 0-1 .8-1.8 1.8-1.8h2.1c2 0 3.5-1.6 3.5-3.5C20.4 6.8 16.7 3.6 12 3.6z" fill="none" stroke="currentColor" stroke-width="1.5"/>' +
  '<circle cx="8" cy="9.4" r="1.3"/><circle cx="12" cy="7.4" r="1.3"/><circle cx="16" cy="9.4" r="1.3"/>' +
  '<circle cx="8.4" cy="13.9" r="1.3"/><circle cx="13" cy="13.4" r="1.3"/></svg>';

function paletteName() {
  var cur = document.documentElement.getAttribute('data-palette') || 'graphite';
  return PALETTE_LABELS[cur] || cur;
}

function updatePaletteBtn() {
  var b = document.getElementById('paletteBtn');
  if (!b) return;
  b.innerHTML = ICON_PALETTE;
  var name = paletteName();
  b.setAttribute('aria-label', '当前配色：' + name + '，点击切换');
  b.title = '当前配色：' + name + '（点击切换）';
}

function cyclePalette() {
  var d = document.documentElement;
  var cur = d.getAttribute('data-palette') || 'graphite';
  var i = PALETTES.indexOf(cur);
  var next = PALETTES[(i + 1) % PALETTES.length];
  d.setAttribute('data-palette', next);
  try { localStorage.setItem('palette', next); } catch (e) {}
  updatePaletteBtn();
}

document.addEventListener('DOMContentLoaded', updatePaletteBtn);
