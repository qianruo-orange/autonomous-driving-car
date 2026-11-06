/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mobile-nav.js — 窄屏底部标签栏。
   桌面端导航在顶栏；手机上顶栏塞不下 7 个控件，会折成四五行把首屏内容挤下去，
   所以在窄屏注入一条固定底部标签栏承担导航，顶栏只留标题和全局图标。
   样式在 layout.css（.tabbar / .tab），>720px 时被 display:none 隐藏。 */

(function () {
  'use strict';

  var ICONS = {
    car: '<path d="M4 16.5h16"/><path d="M6 16.5l1.5-5A2 2 0 0 1 9.4 10h5.2a2 2 0 0 1 1.9 1.5l1.5 5"/>' +
         '<circle cx="7.5" cy="18.2" r="1.7"/><circle cx="16.5" cy="18.2" r="1.7"/>',
    pulse: '<path d="M3 12h3.5l2.5-7 4 14 2.5-7H21"/>',
    sliders: '<path d="M4 7h9M17.5 7H20M4 17h2.5M11 17h9"/>' +
             '<circle cx="15" cy="7" r="2.3"/><circle cx="6.5" cy="17" r="2.3"/>',
    folder: '<path d="M3 7.4A1.9 1.9 0 0 1 4.9 5.5h3.8l2 2.6h8.4a1.9 1.9 0 0 1 1.9 1.9v6.6a1.9 1.9 0 0 1-1.9 1.9H4.9A1.9 1.9 0 0 1 3 16.6z"/>'
  };

  var TABS = [
    { href: '/', label: '主界面', icon: ICONS.car },
    { href: '/status', label: '系统状态', icon: ICONS.pulse },
    { href: '/config', label: '系统配置', icon: ICONS.sliders },
    { href: '/logs', label: '文件下载', icon: ICONS.folder }
  ];

  /* 当前路径归一化：/index.html → /，去掉末尾斜杠 */
  function currentPath() {
    var p = location.pathname.replace(/\/index\.html$/, '/');
    p = p.replace(/\/+$/, '');
    return p || '/';
  }

  function build() {
    if (document.querySelector('.tabbar')) return;

    var here = currentPath();
    var nav = document.createElement('nav');
    nav.className = 'tabbar';
    nav.setAttribute('aria-label', '主导航');

    for (var i = 0; i < TABS.length; i++) {
      var tab = TABS[i];
      var active = tab.href === here;
      var a = document.createElement('a');
      a.className = 'tab' + (active ? ' active' : '');
      a.href = tab.href;
      if (active) a.setAttribute('aria-current', 'page');
      a.innerHTML = '<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">' +
                    tab.icon + '</svg>';

      var label = document.createElement('span');
      /* 挂 data-i18n，切换语言时由 i18n 的 applyLang 一并刷新 */
      label.setAttribute('data-i18n', tab.label);
      label.textContent = (typeof window.t === 'function') ? window.t(tab.label) : tab.label;
      a.appendChild(label);

      nav.appendChild(a);
    }

    document.body.appendChild(nav);
  }

  /* 脚本在 body 末尾执行，此时 i18n 已在 head 就绪、body 已可挂载 */
  if (document.body) build();
  else document.addEventListener('DOMContentLoaded', build);
})();
