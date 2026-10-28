// SPDX-License-Identifier: GPL-3.0-or-later
(function () {
    'use strict';
    const networkCard = document.querySelector('#card_net');
    if (!networkCard) return;

    const card = document.createElement('form');
    card.id = 'network_vofa';
    card.className = 'network-vofa';
    card.innerHTML =
        '<div class="vofa-actions">' +
        '<span id="vofa_save_status" class="card-status" role="status" aria-live="polite"></span>' +
        '<button class="btn btn-save" type="submit" id="vofa_save" disabled data-i18n="保存"></button></div>' +
        '<div class="cfg-grid vid2 vofa-fields">' +
        '<div class="row"><label for="vofa_enabled" data-i18n="通信状态"></label>' +
        '<select id="vofa_enabled" name="enabled" disabled><option value="0" data-i18n="停用 VOFA"></option>' +
        '<option value="1" data-i18n="启用 VOFA"></option></select></div>' +
        '<div class="row"><label for="vofa_port" data-i18n="VOFA端口"></label>' +
        '<input id="vofa_port" name="port" type="number" min="1024" max="65535" step="1" required disabled></div>' +
        '<div class="row"><label for="vofa_rate_hz" data-i18n="发送频率 (Hz)"></label>' +
        '<input id="vofa_rate_hz" name="rate_hz" type="number" min="1" max="100" step="1" required disabled></div>' +
        '</div>';
    networkCard.appendChild(card);
    applyI18n(card);

    const status = card.querySelector('#vofa_save_status');
    const save = card.querySelector('#vofa_save');
    const inputs = ['enabled', 'port', 'rate_hz'].map(key => card.elements.namedItem(key));
    const dirty = new Set();
    let loaded = false;
    let busy = false;
    let readFailed = false;
    let generation = 0;
    let latest = null;

    function message(key, error) {
        status.className = 'card-status' + (error ? ' err' : ' ok');
        setI18nText(status, key);
    }
    function disabled() {
        inputs.forEach(input => { input.disabled = busy || !loaded; });
        save.disabled = busy || !loaded;
    }
    function validSnapshot(data) {
        if (!data || !data.config || !data.status) throw new Error('Invalid VOFA response');
        if (!Number.isInteger(data.config.port) || data.config.port < 1024 || data.config.port > 65535 ||
            !Number.isInteger(data.config.rate_hz) || data.config.rate_hz < 1 || data.config.rate_hz > 100 ||
            ![true, false, 0, 1].includes(data.config.enabled)) throw new Error('Invalid VOFA configuration');
        return data;
    }
    function render(data, acceptAll) {
        validSnapshot(data);
        latest = data;
        inputs.forEach(input => {
            if (acceptAll || !dirty.has(input.name)) {
                input.value = input.name === 'enabled' ? (data.config.enabled ? '1' : '0') : String(data.config[input.name]);
            }
        });
        loaded = true;
        disabled();
    }
    async function poll() {
        if (!busy && !document.hidden) {
            const version = generation;
            const abort = new AbortController();
            const timeout = setTimeout(() => abort.abort(), 5000);
            try {
                const response = await fetch('/api/vofa', {cache: 'no-store', signal: abort.signal});
                if (!response.ok) throw new Error('HTTP ' + response.status);
                const data = await response.json();
                if (version === generation) {
                    render(data, false);
                    if (readFailed) message('', false);
                    readFailed = false;
                }
            } catch (error) {
                if (version === generation) {
                    message('无法读取 VOFA 状态', true);
                    readFailed = true;
                }
            } finally {
                clearTimeout(timeout);
            }
        }
        setTimeout(poll, 1000);
    }
    inputs.forEach(input => {
        input.addEventListener('input', () => { dirty.add(input.name); });
        input.addEventListener('change', () => { dirty.add(input.name); });
    });
    card.addEventListener('submit', async event => {
        event.preventDefault();
        if (!loaded || busy || !card.reportValidity()) return;
        busy = true;
        readFailed = false;
        generation++;
        disabled();
        message('正在保存 VOFA 设置…', false);
        const body = new URLSearchParams();
        inputs.forEach(input => body.set(input.name, input.value));
        try {
            const response = await authFetch('/api/vofa', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: body.toString()
            });
            if (!response.ok) throw new Error('HTTP ' + response.status);
            const data = validSnapshot(await response.json());
            dirty.clear();
            render(data, true);
            message('VOFA 设置已保存', false);
        } catch (error) {
            message('无法保存 VOFA 设置，请检查端口是否被占用', true);
        } finally {
            busy = false;
            disabled();
        }
    });
    document.addEventListener('languagechange', () => {
        if (latest) render(latest, false);
    });
    poll();
})();
