# 智能体的硬件能力层：看见与做到

本文描述外部智能体（语言模型、多智能体编排器）在这台车上**能看到什么**、**能做什么**，以及
这两件事为什么必须走同一条能力凭证。文中数值均来自 RK3576 板上的实测。

实现：`source/agent/agent_hw.c`（凭证、包络、审计、执行入口）、`source/agent/agent_view.c`
（观测与图像）、`source/control/car_sport_ctrl.c`（`follow_agent_*`，执行与联锁）、
`tools/agent/adc_agent.c`（`bin/adc-agent`，模型侧的看与做）。

---

## 1. 能力面只有两件事

```
                 ┌──────────────────────────────────────────┐
   语言模型 /     │            adc-agent (C11)               │
   多智能体  ────►│  see ──► observe + perception + JPEG     │
   秒级决策       │  drive / plan ──► 8 Hz 原语脉冲          │
                 └───────────────┬──────────────────────────┘
                                 │ 能力凭证 X-Agent-Cap (32 hex)
                                 ▼
                 ┌──────────────────────────────────────────┐
                 │  服务进程（root，systemd 沙箱内）          │
                 │  ├─ 观测：NPU 结果队列 + 帧环（只读）      │
                 │  └─ 执行：agent_tick → car_sport_ctrl     │
                 └───────┬──────────────────────┬───────────┘
                         │                      │
                    RK3576 NPU             CAN / 电机适配器
```

模型不接触摄像头驱动，也不接触 CAN。它拿到的是**一份带语义的观测**和**一条带租约的运动原语**，
两次调用之间没有任何它可写的通道。这条边界不因为模型变强而放宽。

关于算术：单次模型往返 2–10 s，0.3 m/s 下 3 s 就是 0.9 m 盲行。所以模型不产生 30 Hz 指令，
它产生计划；`adc-agent` 把计划变成 8 Hz 指令流，本机往返中位数 2.4 ms。

---

## 2. 观测通道：看见

### 2.1 `GET /api/agent/observe`

一次调用同时返回「车况」与「视野」。只读、无副作用、可任意频率调用。

```json
{
  "ts_ms": 42917687,
  "profile":   { "enabled": true,  "reason": "target_lost" },
  "motion":    { "speed_target": 0.000, "steer_target": 0.000,
                 "speed_actual": null,  "steer_actual": null, "wheel_feedback": false },
  "agent":     { "armed": false, "state": "idle", "expires_in_ms": 0,
                 "traveled_m": 0.00, "budget_m": 5.0, "fault": "" },
  "actuation": { "motor_bound": false, "can_enabled": false, "ready": false },
  "limits":    { "v_max": 0.150, "steer_max": 0.100, "budget_m": 5.0, "expires_in_ms": 599991 },
  "perception": {
    "available": true,
    "frame_seq": 128, "width": 1280, "height": 720,
    "age_ms": 151, "frames_behind": 1, "infer_age_ms": 146,
    "objects_total": 0, "objects": [],
    "nearest": null,
    "tracking": { "active": false, "locked": true, "track_id": 0, "age_ms": 202 },
    "summary": "clear road, 1 frame behind"
  }
}
```

每个字段为什么存在：

| 字段 | 为什么模型需要它 |
|---|---|
| `profile.reason` | 回答「刚才为什么停」的唯一合法依据。没有它，模型只能编。 |
| `actuation.ready` | 区分「指令被接受」与「指令能落地」。为 false 时车不会动，模型必须知道。 |
| `frames_behind` | 采集序号减检测序号。推理掉队时画面已经开始骗人，这个差值是可量化的告警。它与 `infer_age_ms` 必须自洽（10 fps 下 1 帧 ≈ 100 ms），两者矛盾说明取错了快照。 |
| `age_ms` | 后处理完成到现在的毫秒数。超过 300 ms 的观测不该再用来判断「正前方有没有人」。 |
| `objects[].lateral` | 归一化横向偏移 −0.5…+0.5（左负右正），比像素坐标更适合语言推理。 |
| `objects[].range` | 由面积比映射的 `near/mid/far` 三级，代替并不存在的测距。 |
| `objects[].in_path` | 下方中部且横向偏移小于 0.28 的启发式「在路径上」标记。 |
| `summary` | 一句话版本，上下文预算紧张时使用。 |

`objects` 最多 12 条，`objects_total` 是真实总数——剪枝是为了响应体大小可预期，不是为了好看。
`GET /api/agent/perception` 只返回这一块，供不需要车况的轮次使用。

### 2.2 `GET /api/agent/frame`

相机原始 JPEG 逐字节透传，不解码、不重编码、不烧框：

```
HTTP/1.1 200 OK
Content-Type: image/jpeg
Content-Length: 41520
X-Frame-Seq: 303
X-Frame-Width: 1280
X-Frame-Height: 720
X-Frame-Age-Ms: 94
```

- `?max_bytes=N`（1024…2097152）：超过即返回 `413`，默认上限 2 MiB。不让 4 MB 的帧顺着上行
  链路去喂模型。
- `?b64=1`：返回 `{"seq":…,"width":…,"height":…,"jpeg_b64":"…"}`，供只接受 JSON 的工具调用
  一次拿到画面。实测 1280×720 单帧 41.5 KB，base64 后约 55 KB。
- 只有 `FRAME_RING_FMT_MJPEG` 的槽位会被取出；YUYV 采集链路返回 `503`，而不是悄悄给一张坏图。

帧与检测靠 `seq` 对齐：`frame` 给的是画面，`observe.perception.frame_seq` 给的是那帧的检测序号。
两者相差 9~15 帧是常态（NPU 流水线深度），不是错误。

---

## 3. 执行通道：做到

### 3.1 原语

| 原语 | 方法 | 语义 |
|---|---|---|
| `motion` | `POST /api/agent/motion` | `v`、`steer`、`ttl_ms(100…500)`，是一次**租约**而非设置 |
| `stop` | `POST /api/agent/stop` | 立即停车、解除武装并关闭 profile |
| `release` | `POST /api/agent/release` | 只放弃租约，保留 profile（计划正常结束后由执行器调用） |
| `estop` | `POST /api/agent/estop` | 强制停车、解除武装、把 profile 置 0 并落盘 |
| `disarm` | `POST /api/agent/disarm` | 收回自己那条武装 |

`motion` 的应答明确区分两种结局：

```json
{ "accepted": true, "state": "active", "v": 0.100, "steer": 0.020,
  "expires_in_ms": 400, "traveled_m": 0.05, "budget_m": 5.0,
  "reason": "agent_active", "actuation": "live" }
```

`state=active` 但 `actuation=blocked` 意味着：指令合法、已被控制层接受，但没有电机适配器把它
变成 PWM。这种「被接受但落不了地」的状态必须显式暴露，否则模型会以为车在走。

### 3.2 执行优先级

控制线程每 1 ms 判定一次，四级顺序不可交换：

```
agent enabled ──► FAULT 锁存 ──► AGENT 租约有效 ──► 视觉跟随
```

- 能力层被 `follow_force_disable()` 关断后，任何残余租约都不生效；
- 视觉跟随与智能体控制互斥，切换靠 profile 开关，而不是靠谁先发指令；
- 租约到期（默认 500 ms）不等于 fault：车停、`reason=agent_idle`，模型可以继续发指令；
  `motor_send_failed`、`stale_detection` 这类才是 fault，只能由人 `arm` 清除。

---

## 4. 凭证与包络

| 项 | 值 | 谁决定 |
|---|---|---|
| 凭证 | 32 hex，`/dev/urandom`，仅内存态 | 人（root 签发） |
| 作用域 | `observe` / `motion+observe` | 人 |
| 速度上限 | `v_max ≤ 0.30 m/s`（硬上限） | 签发时定，控制层再夹一次 |
| 转向上限 | `abs(steer) ≤ 0.15` | 同上 |
| 租约 | `100 ≤ ttl_ms ≤ 500` | 协议固定 |
| 指令间隔 | ≥ 40 ms（25 Hz 上限） | 协议固定 |
| 里程预算 | 默认 20 m，硬上限 100 m | 签发时定 |
| 生命周期 | 到期、撤销、服务重启即失效 | 系统 |

凭证只存在内存里：服务重启后必须重新签发、重新 arm。这是刻意的——一个能跨重启存活的模型授权，
等于一条没人看守的油门。

---

## 5. 工具：`adc-agent`

`bin/adc-agent` 是给人（和模型）共用的 C11 命令行工具。它承担三件模型不该自己做的事：
包络预检、8 Hz 脉冲、任何退出路径上的停车。

```sh
# 看
adc-agent see --save /tmp/frame.jpg          # 观测 JSON + 当前帧
adc-agent frame --b64                        # 只要画面（JSON 内嵌 base64）

# 做
adc-agent drive --v 0.10 --steer 0.0 --ms 1500
adc-agent plan plan.json
adc-agent stop | estop | release

# 管理员（Basic 认证，需 sudo 组成员）
adc-agent token --scope motion --v-max 0.15 --steer-max 0.10 --budget 20 --label nav
adc-agent arm --token $AGENT_CAP
adc-agent revoke --all
adc-agent status | audit
```

计划文件只有一种 schema，解析器是手写的（约 80 行），为这一个结构引入 JSON 库不值得：

```json
{ "segments": [ { "v": 0.08, "steer": 0.00, "ms": 400 },
                { "v": 0.05, "steer": 0.02, "ms": 300 } ] }
```

退出码是契约的一部分：

| 码 | 含义 |
|---|---|
| 0 | 计划执行完毕 |
| 2 | 发出第一条指令前被拒绝（包络、未武装、profile 关闭、执行器未就绪） |
| 3 | 运行中被中止（服务端拒绝或收到信号），已发送 `stop` |
| 4 | 传输失败，或缺少凭证 |

`drive` 在**段内不轮询状态**：任何故障都会表现为「指令被拒」，而一次慢轮询自己就会把 500 ms
租约饿死。这个取舍是被实测逼出来的，注释里写明了，避免后来者「优化」回去。

---

## 6. 为什么工程里没有 Python

运行时曾经有一层 Python：Codex 对话桥、代理配置后端、计划执行器、模型转换工具。现在全部是
C11 + POSIX shell，`git ls-files` 里没有任何 `.py`。

| 原组件 | 现在 | 变化 |
|---|---|---|
| `scripts/codex_bridge.py`（1326 行） | `tools/codex/codex-bridge.c` → `bin/codex-bridge` | `ldd` 只依赖 libc；18 个子命令逐个与旧版对齐 |
| `scripts/proxy_config.py`（275 行） | `source/http/http_api_proxy.c` 进程内实现 | HTTP 路径上不再有解释器与临时文件 |
| `scripts/agent_drive.py`（198 行） | `tools/agent/adc_agent.c` → `bin/adc-agent` | 8 Hz 执行器，退出码语义不变 |
| `tools/model/*.py` | 移除；见 `tools/model/README.md` | 转换在固定容器内完成，产物与哈希入库 |

动机不是「讨厌 Python」，而是三条具体约束：

1. **部署面**：`ProtectSystem=strict` 下多一个解释器就多一条要维护的可写路径和一个要打补丁的
   运行时；C 二进制的失败模式只有「起不来」。
2. **时延**：`drive` 每 125 ms 起一次进程。CPython 冷启动 30–40 ms，占掉租约的 8% 且带抖动；
   C 版本在 2 ms 量级，不随环境变量与 `site-packages` 变化。
3. **可审计**：`codex-bridge` 处理设备码登录与会话记录，`proxy_config` 改的是整机流量。这两件
   事的状态机用 C 写出来是显式的 switch，用 Python 写出来是一组隐式异常路径。

---

## 7. 实测

2026-11-07，RK3576 / 相机 1280×720 MJPEG / `follow` profile 在测试中开合一次：

| 项 | 结果 |
|---|---|
| 签发凭证 | `200`，32 hex，`scope=motion+observe`，`v_max=0.15`，`budget=5.0` |
| `observe` | `perception.available=true`，`frame_seq=128`，`age_ms=151`，`frames_behind=1` |
| `frame` | `200 image/jpeg`，41520 B，`X-Frame-Seq: 303`，`X-Frame-Age-Ms: 94` |
| 未武装时 `drive` | 退出码 2，`credential is not armed` |
| `arm` | `200`，`armed=true`，`armed_token` 前缀 `0170f0` |
| 武装后 `drive --dry-run` | 退出码 0，`plan_meters=0.050` |
| 计划文件（2 段） | 退出码 0，`plan_meters=0.047` |
| 包络外计划 `v=0.5` | 退出码 2，`segment 0: v=0.500 outside credential envelope 0..0.150` |
| `stop` / `revoke` | `200`，`stops` 计数 +1，凭证撤销 |
| `audit` | JSONL 逐条落地，含 `tool`、`actor`、`verdict`、`reason` |

`actuation.ready` 全程为 `false`：`can0` 未起、电机映射未绑定。所以本轮验证的是「看见」与
「被接受」，没有验证「车子真的动了」。这一点在 §8 列为未完成项，不当作通过。

---

## 8. 已知缺口

| 缺口 | 影响 | 处理 |
|---|---|---|
| 无电机映射（`motor_bound=false`） | 指令被接受但不产生 PWM | 接真实 CAN 映射后做台架联调，是下一阶段前置条件 |
| `range` 由面积比推导 | 远近变化时分级会跳变 | 不作测距使用，只作粗分类 |
| 无偏航角来源 | 缺 `turn(θ)`、`go(d)` 的闭环 | 等 IMU 融合或轮速里程标定 |
| `tracking.locked` 与 `active` 可同时为真 | 出现「锁定但未激活」的组合 | 语义是「有选中目标、当前帧未匹配」，已在 §2.1 注明 |
| 10⁴ 次随机包络压力测试未做 | P3 验收缺口 | 见 README §6.8 |
