// SPDX-License-Identifier: GPL-3.0-or-later
(function() {
    const grid = document.querySelector('.can-grid');
    if (!grid) return;
    const card = document.createElement('form');
    card.className = 'card';
    card.id = 'card_car_ctrl';
    card.style.gridColumn = '1 / -1';
    card.innerHTML = '<h3><span data-i18n="小车跟随控制"></span><span id="car_save_status" role="status" class="card-status"></span></h3>' +
        '<div class="cfg-grid vid2" id="car_fields"></div><p class="tip" data-i18n="电机 CAN 协议接口预留，未接入时仅预览；每次启动默认停止。"></p><pre id="car_runtime" style="white-space:pre-wrap;overflow-wrap:anywhere" aria-live="polite"></pre>';
    grid.appendChild(card);
    const fields = card.querySelector('#car_fields');
    const dirtyKeys = new Set();
    let selectedTarget = null;
    let loaded = false,
        busy = false,
        latest;
    const names = {
        enabled: '控制状态',
        manual: '速度模式',
        target: '跟踪目标',
        max_speed: '最大轮速 (m/s)',
        setpoint: '手动目标速度 (m/s)',
        max_turn_delta: '最大转向修正量 (m/s)',
        deadband: '中心误差死区',
        speed_kp: '比例系数',
        speed_kd: '微分系数',
        turn_kp: '比例系数',
        turn_kd: '微分系数',
        wheel_kp: '比例系数',
        wheel_ki: '积分系数'
    };
    const groups = [
        ['tracking', '跟踪设置', ['enabled', 'target']],
        ['speed', '速度模式与限速', ['manual', 'max_speed']],
        ['automatic', '自动调速', ['speed_kp', 'speed_kd']],
        ['steering', '转向参数', ['max_turn_delta', 'deadband', 'turn_kp', 'turn_kd']],
        ['wheels', '手动调速', ['setpoint', 'wheel_kp', 'wheel_ki']]
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
                ['0', '按目标大小自动调速'],
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
        }
        input.id = 'car_' + key;
        input.name = key;
        input.disabled = true;
        row.appendChild(input);
        containers[key].appendChild(row);
    }

    function translate() {
        card.querySelectorAll('[data-i18n]').forEach(el => setI18nText(el, el.getAttribute('data-i18n')));
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
        if (event.target.name === 'target') selectedTarget = latest.targets[Number(event.target.value)] || null;
        updateSpeedMode();
    });

    function render(data) {
        const s = data.status;
        latest = data;
        {
            for (const [key, value] of Object.entries(data.config)) {
                const input = card.querySelector('[name="' + key + '"]');
                if (input && !dirtyKeys.has(key)) input.value = String(value);
            }
            const select = card.querySelector('[name=target]');
            if (!dirtyKeys.has('target') && document.activeElement !== select) {
                select.replaceChildren();
                const saved = document.createElement('option');
                saved.value = 'saved';
                setI18nText(saved, '已保存的目标');
                select.appendChild(saved);
                data.targets.forEach((target, index) => {
                    const o = document.createElement('option');
                    o.value = String(index);
                    o.textContent = target.name + ' #' + index + ' (' + target.x.toFixed(2) + ', ' + target.y.toFixed(2) + ')';
                    select.appendChild(o);
                });
            }
        }
        const reason = {
            disabled: '停止',
            stale_detection: '识别数据过期',
            target_lost: '目标丢失',
            priming: '等待下一识别帧',
            adapter_not_connected: '未接入电机协议',
            realtime_kernel_required: '需要实时内核',
            fifo_unavailable: '实时调度不可用',
            can_disabled: 'CAN 已停用',
            stale_wheel_feedback: '轮速反馈过期',
            active: '正在控制',
            settings_changed: '参数已更新',
            shutdown: '停止',
            motor_send_failed: '电机发送失败',
            motor_stop_failed: '电机停止失败'
        };
        card.querySelector('#car_runtime').textContent = t(reason[s.reason] || '停止') + ' | ' + (s.fifo ? 'SCHED_FIFO / ' + s.priority : 'SCHED_OTHER') + ' | PREEMPT_RT: ' + (s.rt_kernel ? 'Yes' : 'No') +
            (s.rt_error ? ' | ' + t('实时调度错误') + ': ' + s.rt_error : '') + '\n' +
            t('中心误差') + ': X ' + s.error_x.toFixed(3) + ' / Y ' + s.error_y.toFixed(3) + ' | ' + t('左右轮目标速度') + ': ' + s.left_target.toFixed(3) + ' / ' + s.right_target.toFixed(3) + ' m/s\n' +
            t('目标大小误差') + ': ' + s.area_error.toFixed(3) + ' | ' +
            t('左右轮控制输出') + ': ' + s.left_output.toFixed(3) + ' / ' + s.right_output.toFixed(3) + ' | ' + t('识别帧') + ': ' + s.seq;
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
