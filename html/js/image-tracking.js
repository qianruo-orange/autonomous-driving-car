// SPDX-License-Identifier: GPL-3.0-or-later
(function() {
    const overlay = document.getElementById('tracking_overlay');
    const status = document.getElementById('tracking_status');
    const stop = document.getElementById('tracking_stop');
    if (!overlay || !status || !stop) return;
    let latest = null, busy = false, pressing = false, closed = false, timer;
    let message = '', failed = false;

    function updateStatus() {
        const states = {
            idle: '点击画面中的识别框开始追踪',
            tracking: '正在追踪所选目标',
            lost: '目标丢失，请重新选择',
            stale: '等待实时识别画面'
        };
        const text = message || (latest ? states[latest.state] : states.stale);
        status.textContent = t(text) + (!message && latest && latest.state === 'tracking' ?
            ' #' + latest.track_id : '');
        stop.disabled = busy || !latest || !latest.active;
    }

    function render() {
        updateStatus();
        if (pressing) return;
        if (!latest || failed || latest.state === 'stale') {
            overlay.replaceChildren();
            return;
        }
        // 与画面上实际的 object-fit 保持一致：读计算样式而不是假设 contain。
        // cover 时内容比容器大、偏移为负，热区跟着画面裁切走（越界部分被 overflow:hidden 裁掉）。
        const w = overlay.clientWidth, h = overlay.clientHeight;
        const media = overlay.parentNode && overlay.parentNode.querySelector('img,video');
        let fit = '';
        try { fit = (getComputedStyle(media || overlay).objectFit || '').trim(); } catch (e) {}
        let sx, sy;
        if (fit === 'fill') {
            sx = w / latest.width; sy = h / latest.height;   // 非等比拉伸：两轴各自换算
        } else {
            let s;
            if (fit === 'cover')       s = Math.max(w / latest.width, h / latest.height);
            else if (fit === 'none')   s = 1;
            else                       s = Math.min(w / latest.width, h / latest.height);
            if (fit === 'scale-down')  s = Math.min(1, s);
            sx = sy = s;
        }
        if (!Number.isFinite(sx) || !Number.isFinite(sy) || sx <= 0 || sy <= 0) return;
        const iw = latest.width * sx, ih = latest.height * sy;
        const x = (w - iw) / 2, y = (h - ih) / 2;
        const existing = new Map(Array.from(overlay.children, el => [el.dataset.index, el]));
        const retained = new Set();
        latest.targets.forEach(target => {
            const key = String(target.index);
            let button = existing.get(key);
            if (!button) {
                button = document.createElement('button');
                button.type = 'button';
                button.className = 'tracking-target';
                button.dataset.index = key;
                overlay.appendChild(button);
            }
            retained.add(key);
            button.dataset.seq = String(latest.seq);
            button.disabled = busy;
            const label = target.name + ' #' + (target.object_id || target.index + 1) + ' ' + Math.round(target.confidence * 100) + '%';
            button.setAttribute('aria-label', t('追踪目标') + ': ' + label);
            button.setAttribute('aria-pressed', String(latest.active && latest.selected === target.index));
            Object.assign(button.style, {
                zIndex: String(Math.round(1000 * (1 - (target.x2 - target.x1) * (target.y2 - target.y1)))),
                left: (x + target.x1 * iw) + 'px', top: (y + target.y1 * ih) + 'px',
                width: ((target.x2 - target.x1) * iw) + 'px',
                height: ((target.y2 - target.y1) * ih) + 'px'
            });
        });
        existing.forEach((button, key) => { if (!retained.has(key)) button.remove(); });
    }

    async function command(body) {
        if (busy) return;
        busy = true;
        message = '正在更新追踪目标…';
        render();
        try {
            const response = await authFetch('/api/tracking', {method: 'POST', body});
            if (!response.ok) throw new Error(response.status === 409 ?
                '目标已过期或丢失，请重新选择' : '无法更新追踪目标');
            message = '';
        } catch (error) {
            message = error.message === '目标已过期或丢失，请重新选择' ? error.message : '无法更新追踪目标';
        } finally {
            busy = false;
            render();
        }
    }

    overlay.addEventListener('pointerdown', () => { pressing = true; });
    window.addEventListener('pointerup', () => {
        // Run after click so a polling response cannot change the clicked frame.
        setTimeout(() => { pressing = false; render(); }, 0);
    });
    window.addEventListener('pointercancel', () => { pressing = false; render(); });
    overlay.addEventListener('click', event => {
        const button = event.target.closest('.tracking-target');
        if (!button || busy) return;
        command(new URLSearchParams({action: 'select', seq: button.dataset.seq, index: button.dataset.index}));
    });
    stop.addEventListener('click', () => command(new URLSearchParams({action: 'stop'})));

    async function poll() {
        try {
            if (closed || document.hidden) return;
            const response = await fetch('/api/tracking', {signal: AbortSignal.timeout(3000)});
            if (!response.ok) throw new Error();
            latest = await response.json();
            failed = false;
            if (message === '无法读取图像追踪状态') message = '';
            render();
        } catch (error) {
            failed = true;
            if (!busy) message = '无法读取图像追踪状态';
            render();
        } finally {
            if (!closed) timer = setTimeout(poll, 200);
        }
    }
    new ResizeObserver(render).observe(overlay);
    document.addEventListener('languagechange', render);
    window.addEventListener('pagehide', () => { closed = true; clearTimeout(timer); });
    window.addEventListener('pageshow', event => { if (event.persisted) { closed = false; poll(); } });
    poll();
})();
