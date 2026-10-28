// SPDX-License-Identifier: GPL-3.0-or-later
(function () {
    'use strict';
    const grid = document.querySelector('.can-grid');
    if (!grid) return;

    const card = document.createElement('form');
    card.id = 'card_vofa';
    card.className = 'card';
    card.innerHTML =
        '<h3><span data-i18n="VOFA 上位机"></span>' +
        '<span id="vofa_save_status" class="card-status" role="status" aria-live="polite"></span>' +
        '<button class="btn btn-save" type="submit" id="vofa_save" disabled data-i18n="保存"></button></h3>' +
        '<div class="vofa-layout"><div class="cfg-grid vid2 vofa-fields">' +
        '<div class="row"><label for="vofa_enabled" data-i18n="通信状态"></label>' +
        '<select id="vofa_enabled" name="enabled" disabled><option value="0" data-i18n="停用 VOFA"></option>' +
        '<option value="1" data-i18n="启用 VOFA"></option></select></div>' +
        '<div class="row"><label for="vofa_port" data-i18n="TCP端口"></label>' +
        '<input id="vofa_port" name="port" type="number" min="1024" max="65535" step="1" required disabled></div>' +
        '<div class="row"><label for="vofa_rate_hz" data-i18n="发送频率 (Hz)"></label>' +
        '<input id="vofa_rate_hz" name="rate_hz" type="number" min="1" max="100" step="1" required disabled></div>' +
        '<div class="row"><label data-i18n="数据协议"></label><div class="vofa-protocol">TCP / JustFloat</div></div>' +
        '</div><div class="vofa-connection">' +
        '<div class="vofa-endpoint-label" data-i18n="VOFA 连接地址"></div><output id="vofa_endpoint" class="vofa-endpoint">--</output>' +
        '<div class="vofa-status-line"><span id="vofa_connection_status" role="status" data-i18n="正在读取 VOFA 状态…"></span>' +
        '<span id="vofa_clients"></span></div>' +
        '</div></div>' +
        '<div class="vofa-curves">' +
        '<section><h4 data-i18n="速度曲线 (m/s)"></h4><div class="vofa-channel-row">' +
        '<span><span data-i18n="CH0 目标"></span><output id="vofa_speed_target">--</output></span>' +
        '<span><span data-i18n="CH1 实际"></span><output id="vofa_speed_actual">--</output></span></div></section>' +
        '<section><h4 data-i18n="转向曲线 (m/s)"></h4><div class="vofa-channel-row">' +
        '<span><span data-i18n="CH2 目标"></span><output id="vofa_steering_target">--</output></span>' +
        '<span><span data-i18n="CH3 实际"></span><output id="vofa_steering_actual">--</output></span></div></section>' +
        '</div>';
    grid.appendChild(card);
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
    function endpoint() {
        const port = inputs[1].value;
        const hostname = location.hostname;
        const host = hostname.includes(':') && hostname[0] !== '[' ? '[' + hostname + ']' : hostname;
        card.querySelector('#vofa_endpoint').textContent = port ? host + ':' + port : '--';
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
        // Show the saved endpoint; edits do not change the active listener until saved.
        const hostname = location.hostname;
        const host = hostname.includes(':') && hostname[0] !== '[' ? '[' + hostname + ']' : hostname;
        card.querySelector('#vofa_endpoint').textContent = host + ':' + data.config.port;
        let connection = 'VOFA 已关闭';
        if (data.config.enabled) {
            if (data.status.listening) connection = Number(data.status.clients) > 0 ? 'VOFA 已连接' : 'VOFA 等待连接';
            else connection = 'VOFA 未启动，请检查端口';
        }
        const connectionStatus = card.querySelector('#vofa_connection_status');
        setI18nText(connectionStatus, connection);
        connectionStatus.className = data.config.enabled && !data.status.listening ? 'err' : '';
        setI18nText(card.querySelector('#vofa_clients'), '客户端：{count} / {max}', {
            count: Number(data.status.clients) || 0,
            max: Number(data.max_clients) || 4
        });
        const feedbackValid = data.status.feedback_valid === true || data.status.feedback_valid === 1;
        const values = data.channels || {};
        ['speed_target', 'speed_actual', 'steering_target', 'steering_actual'].forEach(key => {
            const value = values[key];
            const valid = typeof value === 'number' && Number.isFinite(value) &&
                (!key.endsWith('_actual') || feedbackValid);
            card.querySelector('#vofa_' + key).textContent = valid ? value.toFixed(3) : '--';
        });
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
    endpoint();
    poll();
})();
