// SPDX-License-Identifier: GPL-3.0-or-later
(function() {
    const grid = document.querySelector('.can-grid');
    if (!grid) return;
    const card = document.createElement('form');
    card.className = 'card';
    card.id = 'card_car_ctrl';
    card.style.gridColumn = '1 / -1';
    card.innerHTML = '<h3><span data-i18n="智能跟随"></span><span id="car_save_status" role="status" class="card-status"></span></h3>' +
        '<div class="cfg-grid vid2" id="car_fields"></div>';
    grid.appendChild(card);
    const fields = card.querySelector('#car_fields');
    const dirtyKeys = new Set();
    let selectedTarget = null;
    let renderedTargets = [];
    let loaded = false,
        busy = false,
        latest;
    const names = {
        enabled: '控制状态',
        manual: '速度模式',
        target: '跟踪目标',
        max_speed: '最大轮速',
        setpoint: '手动目标速度',
        max_turn_delta: '最大转向修正量',
        deadband: '中心误差死区',
        speed_kp: '比例系数',
        speed_kd: '微分系数',
        turn_kp: '比例系数',
        turn_kd: '微分系数',
        wheel_kp: '比例系数',
        wheel_ki: '积分系数',
        wheel_integral_limit: '积分限幅'
    };
    const units = {max_speed: 'm/s', setpoint: 'm/s', max_turn_delta: 'm/s'};
    const groups = [
        ['tracking', '跟踪设置', ['enabled', 'target']],
        ['speed', '速度策略', ['manual', 'max_speed']],
        ['steering', '转向参数', ['max_turn_delta', 'deadband', 'turn_kp', 'turn_kd']],
        ['automatic', '自动调速', ['speed_kp', 'speed_kd']],
        ['wheels', '手动调速', ['setpoint', 'wheel_kp', 'wheel_ki', 'wheel_integral_limit']]
    ];
    const containers = {};
    groups.forEach(([id, title, keys]) => {
        const section = document.createElement('section');
        section.className = 'car-param-group';
        section.dataset.carGroup = id;
        const heading = document.createElement('h4');
        const text = document.createElement('span');
        setI18nText(text, title);
        heading.appendChild(text);
        const save = document.createElement('button');
        save.type = 'button';
        save.className = 'btn btn-save';
        save.dataset.saveGroup = id;
        save.disabled = true;
        setI18nText(save, '保存');
        save.addEventListener('click', () => saveField(keys));
        heading.appendChild(save);
        section.appendChild(heading);
        const body = document.createElement('div');
        body.className = 'cfg-grid vid2';
        section.appendChild(body);
        fields.appendChild(section);
        keys.forEach(key => containers[key] = body);
    });
    for (const [key, label] of Object.entries(names)) {
        const row = document.createElement('div');
        row.className = 'row';
        const l = document.createElement('label');
        l.htmlFor = 'car_' + key;
        setI18nText(l, label);
        row.appendChild(l);
        let input;
        if (['enabled', 'manual', 'target'].includes(key)) {
            input = document.createElement('select');
            const options = key === 'enabled' ? [
                ['0', '停止'],
                ['1', '启用跟随']
            ] : key === 'manual' ? [
                ['0', '自动设定速度'],
                ['1', '手动设定速度']
            ] : [
                ['saved', '已保存的目标']
            ];
            options.forEach(([value, text]) => {
                const o = document.createElement('option');
                o.value = value;
                setI18nText(o, text);
                input.appendChild(o);
            });
        } else {
            input = document.createElement('input');
            input.type = 'number';
            input.min = '0';
            input.max = '100';
            input.step = '0.001';
            input.required = true;
            if (key === 'timeout_ms') {
                input.min = '100';
                input.max = '1000';
                input.step = '1';
            }
            if (key === 'class_id') {
                input.max = '99';
                input.step = '1';
            }
            if (key === 'max_speed' || key === 'setpoint') input.max = '2';
            if (key === 'max_turn_delta') input.max = '4';
            if (key === 'deadband') input.max = '.5';
            if (key === 'wheel_integral_limit') {
                input.max = '1';
                const help = '限制积分项输出，范围 0–1；0 禁用积分';
                input.setAttribute('data-i18n-title', help);
                input.title = t(help);
            }
        }
        input.id = 'car_' + key;
        input.name = key;
        input.disabled = true;
        if (units[key]) {
            const control = document.createElement('div');
            control.className = 'field-unit';
            const unit = document.createElement('span');
            unit.className = 'unit';
            unit.id = input.id + '_unit';
            unit.textContent = units[key];
            input.setAttribute('aria-describedby', unit.id);
            control.append(input, unit);
            row.appendChild(control);
        } else row.appendChild(input);
        containers[key].appendChild(row);
    }

    function translate() {
        card.querySelectorAll('[data-i18n]').forEach(el => setI18nText(el, el.getAttribute('data-i18n')));
        card.querySelectorAll('[data-i18n-title]').forEach(el => {
            el.title = t(el.getAttribute('data-i18n-title'));
        });
    }
    translate();
    function updateSpeedMode() {
        const manual = card.querySelector('[name=manual]').value === '1';
        const input = card.querySelector('[name=setpoint]');
        input.closest('.row').style.display = manual ? '' : 'none';
        input.disabled = !loaded || busy || !manual;
        input.max = card.querySelector('[name=max_speed]').value || '2';
        for (const [id, show] of [['automatic', !manual], ['wheels', manual]]) {
            const section = card.querySelector('[data-car-group="' + id + '"]');
            section.style.display = show ? '' : 'none';
            section.querySelectorAll('input,button').forEach(control => {
                control.disabled = !loaded || busy || !show;
            });
        }
    }
    updateSpeedMode();
    card.addEventListener('input', event => {
        dirtyKeys.add(event.target.name);
        updateSpeedMode();
    });
    card.addEventListener('change', event => {
        dirtyKeys.add(event.target.name);
        if (event.target.name === 'target') selectedTarget = renderedTargets[Number(event.target.value)] || null;
        updateSpeedMode();
    });

    function render(data) {
        latest = data;
        {
            for (const [key, value] of Object.entries(data.config)) {
                const input = card.querySelector('[name="' + key + '"]');
                if (input && !dirtyKeys.has(key)) input.value = String(value);
            }
            const select = card.querySelector('[name=target]');
            // An empty selector must recover when detections arrive, even if
            // it still has focus. Preserve an open, populated list for selection.
            if (!dirtyKeys.has('target') && (document.activeElement !== select ||
                    (!renderedTargets.length && data.targets.length > 0))) {
                renderedTargets = data.targets.slice();
                select.replaceChildren();
                const saved = document.createElement('option');
                saved.value = 'saved';
                setI18nText(saved, '已保存的目标');
                select.appendChild(saved);
                if (!renderedTargets.length) {
                    const empty = document.createElement('option');
                    empty.disabled = true;
                    setI18nText(empty, '暂无识别目标，请将目标置于摄像头画面内');
                    select.appendChild(empty);
                }
                renderedTargets.forEach((target, index) => {
                    const o = document.createElement('option');
                    o.value = String(index);
                    o.textContent = target.name + ' #' + index + ' (' + target.x.toFixed(2) + ', ' + target.y.toFixed(2) + ')';
                    select.appendChild(o);
                });
            }
        }
        fields.querySelectorAll('input,select,button').forEach(el => el.disabled = busy);
        loaded = true;
        updateSpeedMode();
    }
    async function refresh() {
        if (busy || document.hidden) return;
        try {
            const r = await fetch('/api/follow');
            if (!r.ok) throw Error();
            render(await r.json());
        } catch (e) {
            setI18nText(card.querySelector('#car_save_status'), '控制状态读取失败');
        }
    }
    async function saveField(groupKeys) {
        if (!loaded || busy) return;
        const body = new URLSearchParams();
        for (const key of Object.keys(names)) {
            if (key === 'target' || !groupKeys.includes(key)) continue;
            const input = card.querySelector('[name="' + key + '"]');
            if (!input.reportValidity()) return;
            body.set(key, input.value);
        }
        const selection = card.querySelector('[name=target]').value;
        if (groupKeys.includes('target') && selection !== 'saved') {
            const target = selectedTarget;
            if (!target) return;
            body.set('class_id', target.class_id);
            body.set('seed_x', target.x);
            body.set('seed_y', target.y);
            body.set('reference_area', target.area);
        }
        if (!body.size) return;
        busy = true;
        fields.querySelectorAll('input,select,button').forEach(el => el.disabled = true);
        try {
            const r = await authFetch('/api/follow', {
                method: 'POST',
                body
            });
            if (!r.ok) throw Error();
            groupKeys.forEach(key => dirtyKeys.delete(key));
            setI18nText(card.querySelector('#car_save_status'), '✓ 已保存');
        } catch (e) {
            setI18nText(card.querySelector('#car_save_status'), '控制参数保存失败');
        } finally {
            busy = false;
            fields.querySelectorAll('input,select,button').forEach(el => el.disabled = false);
            updateSpeedMode();
            refresh();
        }
    }
    card.addEventListener('submit', event => {
        event.preventDefault();
    });
    document.addEventListener('languagechange', () => {
        translate();
        if (latest) render(latest);
    });
    refresh();
    setInterval(refresh, 500);
})();
