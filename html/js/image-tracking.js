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
            idle: '点击识别框记录外观并开始追踪',
            tracking: '正在追踪所选目标',
            lost: '目标丢失，请重新选择',
            stale: '等待实时识别画面'
        };
        const appearance = latest && latest.appearance;
        if (appearance && appearance.reference_ready) {
            states.tracking = '正在按外观追踪目标';
            if (appearance.reacquire_remaining_ms > 0) states.lost = '目标丢失，正在按外观找回…';
        }
        const text = message || (latest ? states[latest.state] : states.stale);
        status.textContent = t(text) + (!message && latest && latest.state === 'tracking' ?
            ' #' + latest.track_id : '');
        if (!message && latest && latest.state === 'tracking' && appearance &&
                Number.isFinite(appearance.similarity))
            status.textContent += ' · ' + t('外观相似度') + ' ' + Math.round(appearance.similarity * 100) + '%';
        stop.disabled = busy || !latest || !latest.active;
    }

    function render() {
        updateStatus();
        if (pressing) return;
        if (!latest || failed || latest.state === 'stale') {
            overlay.replaceChildren();
            return;
        }
        // Match object-fit:contain, including fullscreen letterboxing.
        const w = overlay.clientWidth, h = overlay.clientHeight;
        const scale = Math.min(w / latest.width, h / latest.height);
        if (!Number.isFinite(scale) || scale <= 0) return;
        const iw = latest.width * scale, ih = latest.height * scale;
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
            button.disabled = busy || target.appearance_available === false;
            button.title = target.appearance_available === false ? t('目标太小，无法提取外观特征') : '';
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
                '目标已过期或外观特征不足，请重新选择' : '无法更新追踪目标');
            message = '';
        } catch (error) {
            message = error.message === '目标已过期或外观特征不足，请重新选择' ? error.message : '无法更新追踪目标';
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
