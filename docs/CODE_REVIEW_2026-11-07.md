# 代码审查报告（2026-11-07）

审查对象：`7d3711b`，160 个跟踪文件，C/C++ 19,476 行（实现 8,315 行，其余为厂商头文件与前端）。
本报告接续 [CODE_REVIEW.md](CODE_REVIEW.md)（2026-11-01），重点放在此后新增的能力层、视觉通道、
`adc-agent`、`codex-bridge` 与重写的代理编辑器上；对旧条目只核对修复状态。

方法：`gcc 14.2 -Wall -Wextra -Wshadow -Wformat=2 -fanalyzer` 全量编译（无 error，21 条告警，
其中 19 条为 `%s` 截断提示），新增 6,200 行逐行通读，板端对四条可疑路径做了实测。

结论：**3 个 P1**，其中 1 个可远程打崩整个服务（上一轮已提，未修），1 个让智能体看到的画面系统性
滞后约 1 秒并谎报自己的落后程度，1 个让整机 HTTP 单线程被阻塞接口卡死并顺势把车锁进故障态。

> **本文档记录的是审查当时的代码状态（`7d3711b`）。** 表中三个 P1 与全部 P2 已在下节
> 「修复状态」所列的提交里关闭，并逐条在板端回归；正文保留原样，便于对照「问题是怎么被发现、
> 怎么被证实的」。

---

## 修复状态（提交 `40b302c`，板端已部署并回归）

| 条目 | 状态 | 验证方式与结果 |
|---|---|---|
| **P1-1** 感知快照取最旧 | **已修** | 新增 `rknn_yolo_latest()`。板端实测 `perception.frame_seq` 与真正最新检测相差 0–2 帧，`frames_behind` 1–2，与 `infer_age_ms=146` 自洽（修前 11 / 9 / 11）。 |
| **P1-2** 阻塞接口卡死事件循环 | **已修** | `proxy/test` 改为提交 + 轮询（worker 执行，curl 3/5 s，tcp 1.5 s，`systemctl --no-block`）。实测 POST 返回 161 ms，运行期间 `/api/follow` 21 ms（修前 2562 ms），第二次 POST 立即 429。 |
| **P1-2 附** 租约过期即锁存 FAULT | **已修** | 新增 `AGENT_LEASE_GRACE_MS=1500`：过期后第一帧就停车，但不锁存。台架固件实测：过期 0.2 s / 0.65 s 仍 `active`、`fault` 为空，1.5 s 后 `fault=lease_expired`；宽限内续租 200 直接回到 `active`，无需重新 arm。 |
| **P1-3** 录像列表 `(size_t)-1` → `memcpy` 巨长 | **已修** | `conn_append` 全部改成减法判断；构造失败返回 500，列表按缓冲上限截断并带 `total`/`truncated`。板端塞 1103 个文件实测：返回 `total=1024, truncated=true`、61074→65479 字节的合法 JSON，服务 `active`、其余接口 200。修前此处必段错误。 |
| P2-1 审计 8 Hz open/close | **已修** | 句柄常开、跨天切换、64 行或 1 s flush；`mkdir` 移到 init。 |
| P2-2 只读凭证能 stop | **已修** | `stop`/`disarm` 要求 MOTION，`estop` 保留 OBSERVE 并在审计里记 scope。实测只读凭证 `stop` 401、`estop` 200。 |
| P2-3 `persist` 落到 `config/agent.token` | **已修** | 移到 `recordings/agent/token`（已被 `.gitignore` 覆盖），`adc-agent` 在 `$AGENT_CAP` 为空时读取。 |
| P2-4 `stop` 与 `release` 同义 | **已修** | 新增 `POST /api/agent/release`，`follow_agent_cancel(int disable_profile)` 分流。实测 release 后 `enabled=1`，stop 后 `enabled=0`；`drive` 正常结束改调 release，连续两次 dry-run 均 rc=0。 |
| P2-5 降权失败静默以 root 继续 | **已修** | 三个调用点全部在失败时 `_exit(127)`（exec 路径把 errno 写回父进程），并加了 `setuid(0)` 反查。 |
| P2-6 `write_atomic` 跟随软链 | **已修** | 加 `O_NOFOLLOW` 与路径长度检查。 |
| P2-7 `snprintf` 累加下溢 | **已修** | 新增 `sadd()`（proxy）与 `jadd()`+`CAT()`（agent_hw），全部按「只在完整写入时前进偏移」处理；`observe` 的 13 处链式拼接一并换掉。 |
| P2-8 `frame --b64` 静默截断 | **已修** | 按 `max_bytes*4/3` 定容；`http_call` 截断返回 -2 并报错，不再输出半截 JSON。 |
| P2-9 `record_name_ok` 越界读 | **已修** | 先校验 `strlen(name)==17`。 |
| P2-10 `motion` 的 503 | **已修** | 快照重试 3 次（每次 1 ms），仍失败才 503 且文案改为 `controller busy; retry`。 |
| 上轮 P1-2 DBC 负起始位 | **已修** | `parse_signal` 拒绝 `start<0 / len<=0 / len>64 / start+len>512`，`dbc_decode_raw` 再兜一道。 |
| P3 `observe` 路由 `pre=0` | **已修** | 改为前缀匹配，带 `?` 参数不再 404。 |

未修（保留原判断）：P3 余下的形参命名、`s.rejected` 锁外更新、`reply_config` 的 GET 路径未校验、
`follow_mix` 的预览复用。这些不影响本轮机能，且改动面大于收益。

**回归脚本**：`/tmp/verify.sh`、`/tmp/verify2.sh`、`/tmp/lease3.sh`（板端临时脚本，未入库）。
台架验证用 `cmake -DCMAKE_C_FLAGS=-DADCBENCH` 构建到 `/tmp/bench`，以 `systemd-run --unit=adc-bench`
临时起在 80 端口，测完停掉并重建生产固件（`grep -ac "mock actuator"` 回到 0）。

---

## P1-1 感知快照取的是「最旧的 8 条检测」而不是最新一条

位置：`source/agent/agent_view.c:72`、`source/yolo/rknn_yolo.c:503`

```c
yolo_result_t recs[VIEW_HIST_RECORDS];          /* 8 */
int n = rknn_yolo_results_since(0, recs, VIEW_HIST_RECORDS);
const yolo_result_t *r = n > 0 ? &recs[n - 1] : NULL;
```

`rknn_yolo_results_since()` 从最旧一条开始顺序填充（`rknn_yolo.c:511`），`max` 又是 8，
而环形缓冲深度 `YOLO_HIST_N` 是 16。于是 `recs[7]` 是**第 8 旧的**那条，不是最新那条。
注释写的是「取最后一条 = 最新」，在 `max == 缓冲深度` 时才成立，这里不成立。

板端实测（`/api/detections/live?after=produce-6` 给出真正的最新检测）：

```
produce=6285  newest_detection=6284   perception.frame_seq=6275  frames_behind=11
produce=6293  newest_detection=6292   perception.frame_seq=6284  frames_behind=9
produce=6300  newest_detection=6298   perception.frame_seq=6289  frames_behind=11
```

两件事同时错：

1. `frame_seq` 比真实最新检测落后 8–9 帧（约 0.8 s，相机 10 fps）；
2. `frames_behind = produce_seq - r->seq` 因此虚报 11，而同一响应里的 `infer_age_ms`
   （`processing_age_ms`）是 147 ms，按 10 fps 只对应 1–2 帧。**两个字段自相矛盾**。

对一个把 `frames_behind` 当安全信号用的模型，这是最坏的一种错：健康的管线被描述成掉队，
而它拿到的障碍物位置来自将近一秒之前。README 里那段示例 `"frames_behind": 9` 就是这个 bug
的产物（§6.6 的 jsonc 片段，中英各一处）。

修复：`g_yolo.result` 本来就是「只保留最大 seq」的单一最新快照（`rknn_yolo.c:492-500`），
但没有任何公开读取口。加一个访问器即可，不要用「要一个足够大的窗口再取末尾」来绕过：

```c
/* include/yolo/rknn_yolo.h */
int rknn_yolo_latest(yolo_result_t *out);   /* 1 = 有结果，0 = 还没有 */

/* source/yolo/rknn_yolo.c */
int rknn_yolo_latest(yolo_result_t *out)
{
    if (!out) return 0;
    pthread_mutex_lock(&g_yolo.result_mutex);
    int have = g_yolo.result.seq != 0;
    if (have) *out = g_yolo.result;
    pthread_mutex_unlock(&g_yolo.result_mutex);
    return have;
}
```

`agent_view.c` 改成 `rknn_yolo_latest(&rec) ? &rec : NULL`，`VIEW_HIST_RECORDS` 随之删掉。
修完后 `frames_behind` 应回落到 1–2，与 `infer_age_ms` 一致；这两个数的一致性本身就是一条
可回归的断言。

## P1-2 阻塞接口跑在单线程事件循环上，会把车锁进故障态

位置：`source/http/http_api_proxy.c:500`（`curl --max-time 15`）、`:267`/`:271`（`systemctl`）、
`source/http/http.c:1095-1160`（唯一的事件循环线程）

HTTP 服务是一个 epoll 线程：accept、读、**执行 handler**、写全在这一个线程里
（`http.c:1146` 的 `conn_read` 会直接调用路由处理函数）。而下面这些 handler 会同步阻塞：

| handler | 阻塞源 | 上限 |
|---|---|---|
| `POST /api/proxy/test` | `popen("curl … --connect-timeout 8 --max-time 15")` + `tcp_probe` 的 5 s | 20 s |
| `GET /api/proxy/config` | `systemctl is-active` | ~50 ms |
| `POST /api/proxy/config` | `systemctl restart` | 秒级 |
| 所有 `/api/codex/*` | `waitpid` 等桥接进程 | 桥接自身耗时 |

实测（板端，代理在线）：

```
/api/follow 基线            0.6 ms
proxy/test 运行期间         2562 ms      ← 整个 Web 面被串行化
proxy/test 本身             2987 ms
```

代理不可达时这里是 15–20 s。后果不止是「界面卡一下」：

`adc-agent` 以 8 Hz、`ttl_ms=500` 续租，`agent_tick` 在租约到期时执行
`stop_wheels("lease_expired")` + `agent_latch("lease_expired")`（`car_sport_ctrl.c:213-217`），
而 `agent_latch` 会把 `ENABLED` 置 0 并进入 `AGENT_MOTION_FAULT`——**只能由操作员重新 arm 解除**。
也就是说：在智能体行驶期间，操作员打开一次代理测试页，车会硬停在原地并进入故障态，
事后还得人工恢复。诊断接口不该有这种权力。

修复优先级从低到高：

1. `proxy/test` 缩短到 `--connect-timeout 3 --max-time 5`，并对同一客户端做 10 s 冷却；
2. 把 curl 与 systemctl 挪到 `http_worker_start()`（项目已有这个设施，`http.c:79`）里跑，
   handler 立即返回 `{"ok":true,"pending":true}`，前端轮询结果——现有前端已经在轮询别的字段，
   不改变契约形态；
3. 无论选哪条，`agent_latch("lease_expired")` 都值得重新讨论：一次 500 ms 的调度抖动不应该
   需要人工重新武装。建议区分「租约到期 = IDLE + 停车」（可被下一条指令唤醒）与
   「显式 estop / 硬件故障 = FAULT」（需人工），否则任何抖动都会变成一次需要人到现场的停车。

## P1-3 录像列表溢出后把 -1 当长度传出，加法回绕后 memcpy 巨大长度

位置：`source/http/http_api_rec.c:101-104`、`source/http/http.c:262-280`

```c
off = http_json_append(json, sizeof(json), off, ...);
if (off < 0) break;                       /* off = -1 */
...
off = http_json_append(json, sizeof(json), off, "]}");   /* 立即返回 -1 */
http_ok_json(fd, json, (size_t)off);      /* (size_t)-1 */
```

`conn_append` 的判断是 `if (c->wlen + len > HTTP_WBUF_MAX)`：`len == SIZE_MAX` 时
`wlen + len` 回绕成 `wlen - 1`，**检查恒不成立**；随后 `c->wlen + len > c->wcap` 的比较
用的是回绕后的 `wlen - 1`，`while (ncap < c->wlen + len) ncap *= 2;` 因此一轮都不循环，
直接 `memcpy(c->wbuf + c->wlen, data, SIZE_MAX)` —— 进程当场段错误。

板端把这段算术单独跑了一遍（`wlen=100, wcap=65536, len=SIZE_MAX`），回绕确实发生：

```
wlen + len        = 99
> HTTP_WBUF_MAX ? = 0        ← 应为真而实际为假
want              = 99
循环后 ncap       = 131072   ← 一轮都不放大
随后 memcpy(dst, src, 18446744073709551615)
```

触发条件是 `/api/rec/list` 的 JSON 超过 64 KB，即 `recordings/` 下约 730 个录像文件。
这是**上一轮审查的 P1 第 1 条，至今未修**（`serve_rec_list` 与 `conn_append` 两处都还在原样）。
它是全进程崩溃：控制线程随之消失，电机停在最后一条指令上等看门狗。

修复（两处都要改，只改一处不行）：

```c
/* http_api_rec.c：构造失败必须变成明确错误 */
if (off < 0) { http_err(fd, 500, "Error", "recording list too large"); return; }
/* 或者一开始就分页：/api/rec/list?after=<name>&limit=200 */

/* http.c：容量的加法一律改减法，杜绝回绕 */
if (len > HTTP_WBUF_MAX || c->wlen > HTTP_WBUF_MAX - len) { conn_close(c); return -1; }
```

---

## P2

### 1. 审计日志在 8 Hz 指令路径上逐条 fopen/写/fclose，无轮转

`agent_hw.c:610` 每条被接受的 `motion` 都调 `audit()`，后者两次 `mkdir` + `fopen("a")` +
`fprintf` + `fclose`（`:150-171`），跑在执行 HTTP 的那个线程上。按 8 Hz 持续行驶：
28,800 行/小时、约 **4.3 MB/小时**、100 MB/天，且从不轮转（只有按天分文件）。
板上 29 KB 的审计文件是我今天几十次测试留下的，正常行驶会快两个数量级。

建议：模块内持有一个 `FILE *`，按行写、每 100 行或 1 s `fflush` 一次；`motion ok` 只记录
**状态迁移**（开始/速度变化/结束），逐条指令的明细已经有 `cmds` 计数器与 CSV 侧记录。
`audit()` 里的两次 `mkdir` 也应该只在 init 做一次。

### 2. `stop` / `estop` / `disarm` 只要求 observe 权限

`agent_hw.c:630`、`:658`、`:497` 都用 `AGENT_SCOPE_OBSERVE` 校验。于是一个**只读**凭证
（例如给只做描述、不该动车的模型签发的那一枚）可以 `POST /api/agent/stop`，
而 `follow_agent_cancel()` 会把操作员的 `follow` profile 整个关掉（`car_sport_ctrl.c:501`）。
停车本身是安全方向，但「只读身份能反复把车踢出自动驾驶模式」是权限模型上的裂缝，
审计里也看不出这是越权。

建议：`stop` 要 MOTION，`estop` 可以保留 OBSERVE（紧急停车应尽量容易），但要在审计里
把 `scope` 记清楚。

### 3. `persist=1` 把凭证明文写到 `config/agent.token`，而没有任何消费者

`agent_hw.c:378-392` 写 `config/agent.token`（0640，组 qianruo）。但 `adc-agent` 只读
`$AGENT_CAP`，从不读这个文件——功能是死的，副作用是**凭证落盘后一直有效到过期**。
更实际的风险是这个路径不在 `.gitignore` 里（`config/` 下别的运行态文件都在），
`git add -A` 会把它提交上去。

建议二选一：删掉 `persist`，或者让 `adc-agent` 在 `$AGENT_CAP` 为空时读它，并把路径挪到
`recordings/agent/` + 补 `.gitignore`。

### 4. `stop` 与 `release` 是同一个动作，且都会关掉 profile

`adc_agent.c:729` 把两个子命令映射到同一个端点；`http_agent_stop` 无条件调用
`follow_agent_cancel()`，后者 `follow_force_disable("agent_stopped")`。结果是
`adc-agent drive` 每次正常结束时都会关掉 profile——**第二次 `plan` 必然被预检拒绝**
（"follow profile is disabled; an operator must enable it first"），必须人工重开。
工具文档把 `release` 列成一个独立动作，实际语义没有区别。

建议：`release` 走「放弃租约、保持 profile」，`stop` 保持现在的硬停；两者在
`follow_agent_cancel()` 里分流，而不是在 CLI 里分流。

### 5. 降权失败即静默以 root 继续

`codex-bridge.c:1320`：

```c
struct passwd *pw = getpwnam(CLI_USER);
if (!pw) return;            /* ← 找不到账号：继续以 root 跑 */
if (setgid(pw->pw_gid) != 0) return;
if (setuid(pw->pw_uid) != 0) return;
```

服务以 root 启动，子进程是 `codex --sandbox danger-full-access`。账号被删、`setuid` 因
LSM 拒绝、`setgroups` 失败，任何一种都会让这个沙箱在 root 身份下运行，而且没有任何日志。
Python 版这里是 `Popen(user=…)`，失败会抛异常——比现在安全。

建议：任一降权步骤失败就把 `errno` 写进 `exec_pipe` 并 `_exit(127)`，父进程据此返回
结构化错误；只在 `geteuid() != 0` 时才允许跳过。

### 6. `write_atomic` 的临时文件可能跟随软链

`codex-bridge.c:381`：`snprintf(tmp, "%s.tmp", path)` + `open(tmp, O_WRONLY|O_CREAT|O_TRUNC)`
没有 `O_NOFOLLOW`。桥接是 root，`.codex/out`、`recordings/codex` 一旦对 qianruo 可写
（子进程以 qianruo 身份写录像），本地用户放一个 `reply.json.tmp -> /etc/shadow` 就能让 root
截断任意文件。加 `O_NOFOLLOW` 是零成本，另外建议 `openat` + `renameat` 固定到目录 fd 上。

### 7. `http_api_proxy.c` 三处「先累加长度、再当剩余空间用」

`:333-352` 的 `eoff += snprintf(sysenv + eoff, sizeof(sysenv) - eoff, …)`：`snprintf` 返回的是
**本来要写的长度**，一旦发生截断，`eoff` 就超过 `sizeof(sysenv)`，下一轮 `sizeof(sysenv) - eoff`
下溢成巨大的 `size_t`，`sysenv + eoff` 直接指到栈外。`sysenv` 是 1024 字节，四个键各上限 256，
理论上能凑到 1080。

`:361-377` 的 `cap = strlen(esc_raw) + 2048` 后接一个包含 `sysenv`(≤1024)、`backup`(≤320)、
`apply_error`(≤360)、四个 value(≤512)、`url`(≤192) 的 `snprintf`，非 raw 部分最坏约 2440 > 2048，
截断后 `off` 大于 `cap`，`http_ok_json(fd, json, off)` 会从堆缓冲后面继续读出去。

`agent_hw.c:694-746` 的 `observe` 用的是同一套写法（`off += snprintf(json + off, sizeof(json) - off, …)`），
目前因为各字段有界没出事，但 `agent_view.c` 已经提供了正确的 `jadd()`，把它提成公共工具、
`off` 只接受「未截断」的返回值，这三处一起消掉。

### 8. `adc-agent frame --b64` 会在 512 KB 处静默截断

`RESP_MAX` 是 512 KB，而 `?max_bytes` 默认 1 MiB，base64 后约 1.37 MB。`http_call` 里
`snprintf(resp, resp_size, "%s", body)` 把 JSON 截一半，`cmd_frame` 照样 `printf` 出去——
调用方拿到的是一个语法不完整的 JSON，而退出码是 0。相机当前帧只有 41 KB，所以没暴露。

建议：`--b64` 时按 `max_bytes * 4 / 3 + 512` 动态分配；或干脆让 `frame` 只走落盘/裸流，
把 base64 留给 HTTP 侧。

### 9. `record_name_ok` 越界读

`codex-bridge.c:2497` 对 `name` 先 `strncmp(name,"chat_",5)`，再无条件读 `name[5..12]`。
`name` 是 `"chat_"` 这种短串时读过 NUL（同一 argv 缓冲内，实际不会崩）。加一句
`if (strlen(name) != 17) return 0;` 即可。

### 10. `motion` 的 503 会把合法指令当「控制器启动中」

`agent_hw.c:573` 用 `follow_agent_snapshot()` 的 `trylock`，控制线程正在 1 ms tick 里时
必然失败，于是返回 `503 controller starting`，指令被丢弃。`adc-agent` 把它当传输失败
（exit 3）中止整条计划。建议 `follow_agent_snapshot` 用带超时的 `pthread_mutex_timedlock`
（1–2 ms），或在 handler 里重试 3 次——这块锁的临界区只有几十纳秒。

---

## 上一轮 P1 的当前状态

| 条目 | 状态 |
|---|---|
| P1-1 录像列表负长度 → `memcpy` 巨长（`http_api_rec.c:101` + `http.c:265`） | **未修**，本轮升级为 P1-3（全进程崩溃） |
| P1-2 DBC 负起始位未被拒绝（`dbc_parser.c:107` / `:185`） | **未修**。`SP_` 仍无 `start >= 0` 校验；Intel 路径 `max_byte = (start + len - 1)/8`，`start=-16,len=8` 时得 `-1`，`-1 >= frame->len` 不成立，检查被绕过，`extract_intel()` 用 `data[pos/8]` 读到帧缓冲之前，`pos % 8` 为负还触发移位 UB |
| 其余 P2 条目 | 未逐条复核，本轮结论不依赖它们 |

DBC 的修法很小：`parse_signal` 里 `if (start < 0 || len <= 0 || start + len > 8 * 64) return -1;`，
`dbc_decode_raw` 里对两条路径统一 `if (sig->start_bit < 0) return -1;`。

---

## P3（记为待办，不影响本轮机能）

- `agent_hw.c` 的 handler 形参叫 `body`，实际收到的是**完整请求**（`http_form_parse` 内部自己
  找 `\r\n\r\n`，`cap_from_request` 也要靠它读头）。改名 `req`，否则下一个人一定会写错。
- `s.rejected` / `s.stops` / `s.last_reject` 在锁外更新。当前正确性依赖「HTTP 只有一个线程」
  这一事实，而这条约束没有写在任何地方；要么全部进锁，要么在模块头注释里写明。
- `/api/agent/observe` 的 `pre` 标志是 0（其余两条视觉路由是 1）。加个 `?` 参数就会 404。
- `reply_config` 把 `c->value[]` 直接拼进 JSON，只有 POST 路径过了 `conf_valid()`；
  GET 路径从 `/etc/proxy-client.conf` 读到的值未经验证（该文件只有 root 能写，故列 P3）。
- `follow_mix()` 在租约已过期时仍按 `g.ag.v` 算目标（`:211`，注释说是为了预览）。
  预览可以，但 `left_target/right_target` 同时是「将要下发的值」，两者共用一组变量容易误读。

---

## 建议的修复顺序

1. **P1-3**（一行减法 + 一处错误返回）——这是唯一能远程打崩进程的，改动也最小；
2. **P1-1**（新增 `rknn_yolo_latest`，`agent_view` 改用它）——智能体的眼睛，且修完可回归；
3. **P1-2** 的 1+2（先缩短超时，再把 curl/systemctl 挪进 worker），随后单独讨论
   `lease_expired` 是否应该锁存成 FAULT；
4. P2-1（审计）与 P2-2（scope）在接真实电机之前必须解决——前者会在长时间台架试验里
   把卡写满，后者影响的是「谁能停车」这件事的定义。
