# codex-bridge（C11）

`tools/codex/codex-bridge.c` 是 `scripts/codex_bridge.py` 的 C11 移植，供
`source/http/http_api_codex.c` 调用：每次请求 fork 一次本程序，结果以 JSON
写入 `--out`，进程随即退出。仅依赖 libc/POSIX，不引入任何 Python 运行时；
需要 HTTP 时通过 `popen("curl ...")`。

## 命令行契约

```
codex-bridge <command> --out <path> [--input <path>] [--arg <str>]
```

- `--out` 必填：结果 JSON 写入该文件。原子写：先写 `<path>.tmp`、`fsync`、再 `rename`。
- `--input` 可选：需要用户文本的命令（`config`、`chat-start`）从这里读，**绝不走 argv**。
- `--arg` 可选：`history` 的游标、`record` / `records-delete` 的文件名。
- stdout/stderr 不承载结果（调用方重定向到 `/dev/null`），只在参数错误时打印用法。
- 退出码：`0` 成功；`1` 命令内部异常（同时仍写出 `{"ok": false, "error": "..."}`）；`2` 命令行参数错误。

长任务（一轮对话、设备码登录）由父进程 fork 出后台子进程（`setsid` 脱离、重定向到
`/dev/null`）后立即返回，前端通过 `status` / `login_state.json` 轮询进度。

## 命令清单

| 命令 | 作用 | 主要字段 |
| --- | --- | --- |
| `status` | 会话/登录/记录总览 | `ok cli_found cli_path cli_version logged_in auth_mode login settings busy job thread_id last_error started_at finished_at messages records{files,bytes}` |
| `check-update` | 只读查询 npm 最新版 | `ok current latest cli_found update_available stale source update_command checked_at` |
| `update-install` | 触发 `codex update`（root 下走 systemd-run） | `ok started before version unit log output note update_command` |
| `update-status` | 更新进度（字节数/阶段/日志尾巴） | `ok running unit downloaded total percent phase log cli_version path` |
| `models` | `codex debug models` 过滤后的模型表（缓存 1h） | `ok models[] cache selected cached` |
| `quota` | ChatGPT 套餐与滚动窗口额度（缓存 60s） | `ok signed_in plan allowed primary secondary credits reached reset_credits fetched_at` |
| `config` | 更新 model / effort / cli_path 并重写 config.toml | `ok status{...}` |
| `login-device` | 启动设备码登录（后台） | `ok` |
| `chat-start` | 启动一轮对话（后台） | `ok job` |
| `reset` | 清空 thread_id / job | `ok status{...}` |
| `stop` | 终止运行中的对话进程组 | `ok stopped` |
| `logout` | 删 auth.json 并复位状态 | `ok status{...}` |
| `history` | 最近 500 条对话（`--arg` 为游标） | `ok messages[] total after` |
| `records` | 记录文件列表 | `ok files[{name,date,size,mtime}]` |
| `export` | 合并全部记录为可下载文件 | `ok path name size records` |
| `record` | 单个记录文件的元信息 | `ok path name size` |
| `records-clear` | 删除全部记录 | `ok deleted` |
| `records-delete` | 删除单个记录 | `ok deleted` / `ok:false,error` |

所有错误路径都返回 `{"ok": false, "error": "..."}`，不会留下副作用。

## 环境变量

| 变量 | 说明 |
| --- | --- |
| `CODEX_WEB_ROOT` | 运行根目录，默认 `/opt/autonomous-driving-car` |

派生子进程时本程序自行设置：`CODEX_HOME=$CODEX_WEB_ROOT/.codex`、`HOME`（同上）、
`NO_COLOR=1`、`TERM=dumb`、`PATH=/usr/local/bin:/usr/bin:/bin:/home/qianruo/.local/bin`；
`update-install` / `update-status` 额外用 `/etc/proxy-client.conf` 里的代理变量。

## 目录布局

```
$CODEX_WEB_ROOT/.codex/{settings.json,state.json,login_state.json,auth.json,config.toml,
                        models_cache.json,quota_cache.json,update_cache.json,out/,inbox/}
$CODEX_WEB_ROOT/recordings/codex/chat_YYYYMMDD.txt
```

## 权限

- 二进制以 root 被服务拉起；每个 CLI 子进程在 `execve` 之前 `setgroups(0)` →
  `setgid` → `setuid` 切到 `qianruo`（uid/gid 从 `getpwnam` 读取）。
- 目录 `.codex/`、`recordings/codex/`、`out/`、`inbox/` 由本程序创建时用 `0700`
  （root 创建时顺带 `chown` 给 `qianruo`，否则 CLI 无法写入）。
- `--out`、`settings.json`、`state.json`、`login_state.json` 为 `0600`；
  `config.toml`、`chat_*.txt`、`*.cache.json` 为 `0644`（`codex` 以 `qianruo` 身份读取）。

## 与旧 Python 版的差异

1. **非 root 运行**：Python 版总是给 `Popen` 传 `extra_groups=[]`，非 root 时
   `setgroups()` 必然 `EPERM`，于是所有拉起 CLI 的路径（`models` 冷启动、`quota`
   的 app-server、`chat-start`、`login-device`、`update-install`）都会失败。C 版只在
   `geteuid()==0` 时降权，非 root 直接透传——生产环境（systemd 以 root 启动）两者等价。
2. **错误字符串细节**：`app-server` 返回的 `error` 对象在 Python 里是 `str(dict)`（单引号
   repr），C 版给的是等价的 JSON 文本；`TimeoutExpired` 之类的类型名按 Python 的写法拼，
   但子进程 `execve` 失败的 errno 文本由 C 版自行格式化。
3. **`str()` 语义**：`config` 对 `null`/`true`/数字仍按 Python 的 `None`/`True`/字面量处理，
   但对对象/数组给出 JSON 而不是 Python repr（前端只会传字符串）。
4. **超时**：`chat-start` 300 s 超时后杀的是整个进程组（Python 只 kill 直接子进程），
   能真正停掉 `codex` 拉起的子孙进程。
5. **权限位**：见上一节，`--out` 与内部状态文件比 Python 版更收紧（0600/0700）。
6. **依赖**：不再需要 `python3`；网络请求统一走 `curl`（Python 版首选用 `urllib`）。

命令集合与 Python 版一致（18 个，`http_api_codex.c` 与前端只用到这些）。

## 构建

```
gcc -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -o codex-bridge tools/codex/codex-bridge.c -lpthread
```

CMake（由 `CMakeLists.txt` 的维护者添加）：

```cmake
add_executable(codex-bridge tools/codex/codex-bridge.c)
target_compile_features(codex-bridge PRIVATE c_std_11)
target_compile_options(codex-bridge PRIVATE -Wall -Wextra)
target_compile_definitions(codex-bridge PRIVATE _GNU_SOURCE)
target_link_libraries(codex-bridge PRIVATE pthread)
```
