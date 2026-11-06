#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bridge between the C HTTP service and the local Codex CLI.

Every invocation is short-lived: it writes its result as JSON to --out and
exits. Long-running work (one chat turn, one device-code login) is detached
into a background child so the HTTP worker never blocks.

User supplied text is always passed through --input files, never argv.
"""
import datetime
import glob
import json
import os
import pty
import pwd
import re
import select
import signal
import shutil
import subprocess
import sys
import time
import urllib.request
import uuid

ROOT = os.environ.get("CODEX_WEB_ROOT", "/opt/autonomous-driving-car")
CX = os.path.join(ROOT, ".codex")
REC = os.path.join(ROOT, "recordings", "codex")
SETTINGS = os.path.join(CX, "settings.json")
STATE = os.path.join(CX, "state.json")
LOGIN = os.path.join(CX, "login_state.json")
AUTH = os.path.join(CX, "auth.json")
CONFIG_TOML = os.path.join(CX, "config.toml")
OUT_DIR = os.path.join(CX, "out")
INBOX = os.path.join(CX, "inbox")

DEFAULT_SETTINGS = {
    "cli_path": "/home/qianruo/.local/bin/codex",
    "model": "",
    "effort": "medium",
    "sandbox": "danger-full-access",
    "cli_version": "",
}
# Runtime options (sandbox / workdir) are no longer configurable from the web
# UI; the sandbox is pinned to full access for every run. Model and reasoning
# effort ("速率") come back as two dropdowns on the config page.
FORCED_SANDBOX = "danger-full-access"
EFFORTS = ("minimal", "low", "medium", "high", "xhigh")
RECORD_NAME = re.compile(r"^chat_\d{8}\.txt$")
DEVICE_URL_PAGE = re.compile(r"https://[^\s]*/device(?![A-Za-z])[^\s]*")
DEVICE_URL_ANY = re.compile(r"https://[^\s/]*auth\.openai\.com[^\s]*")
DEVICE_CODE = re.compile(r"\b([A-Z0-9]{4,}-[A-Z0-9]{4,})\b")
# Terminal output carries ANSI colour/OSC-8 escapes that would otherwise end
# up inside the captured URL (e.g. "…/device\x1b[0m") or break the code match.
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")


def find_device_url(text):
    """Prefer the human sign-in page; never show an API endpoint URL."""
    m = DEVICE_URL_PAGE.search(text)
    if m:
        return m.group(0).rstrip(".,;)")
    m = DEVICE_URL_ANY.search(text)
    if m and "/api/" not in m.group(0):
        return m.group(0).rstrip(".,;)")
    return ""


def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def ensure_dirs():
    for d in (CX, REC, OUT_DIR, INBOX):
        os.makedirs(d, exist_ok=True)
    # 交互记录统一为 .txt：旧版本留下的 .jsonl 就地改名
    for old in glob.glob(os.path.join(REC, "chat_*.jsonl")):
        new = old[:-len(".jsonl")] + ".txt"
        if not os.path.exists(new):
            try:
                os.rename(old, new)
            except OSError:
                pass
    migrate_records()


def migrate_records():
    """Rewrite records written in the old JSON-lines format as readable text."""
    for path in glob.glob(os.path.join(REC, "chat_*.txt")):
        try:
            with open(path, "r", encoding="utf-8") as fp:
                first = fp.readline().strip()
            if not first.startswith("{"):
                continue
            entries = []
            with open(path, "r", encoding="utf-8") as fp:
                for line in fp:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        entry = json.loads(line)
                    except ValueError:
                        continue
                    if entry.get("role") in ("user", "assistant"):
                        entries.append(entry)
        except OSError:
            continue
        text = "\n\n".join(format_record(e) for e in entries)
        try:
            with open(path, "w", encoding="utf-8") as fp:
                fp.write(text + ("\n" if text else ""))
        except OSError:
            pass


def read_json(path, fallback):
    try:
        with open(path, "r", encoding="utf-8") as fp:
            return json.load(fp)
    except (OSError, ValueError):
        return dict(fallback)


def write_json(path, value):
    ensure_dirs()
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fp:
        json.dump(value, fp, ensure_ascii=False, indent=2)
        fp.flush()
        os.fsync(fp.fileno())
    os.replace(tmp, path)
    try:
        os.chmod(path, 0o644)
    except OSError:
        pass


def settings():
    s = dict(DEFAULT_SETTINGS)
    s.update(read_json(SETTINGS, {}))
    # The sandbox stays pinned so a stale read-only value can never weaken the
    # runtime; legacy workdir is dropped, and model/effort are validated.
    s["sandbox"] = FORCED_SANDBOX
    s.pop("workdir", None)
    if s.get("effort") not in EFFORTS:
        s["effort"] = "medium"
    if not isinstance(s.get("model"), str):
        s["model"] = ""
    return s


def state():
    return read_json(STATE, {"busy": False, "job": "", "thread_id": "",
                             "last_error": "", "started_at": 0, "finished_at": 0})


def write_state(**changes):
    s = state()
    s.update(changes)
    write_json(STATE, s)


def login_state():
    return read_json(LOGIN, {"state": "idle", "url": "", "code": "",
                             "message": "", "started_at": 0, "expires_at": 0})


def auth_mode():
    """Only device-code (ChatGPT) credentials are accepted."""
    if not os.path.exists(AUTH):
        return ""
    a = read_json(AUTH, {})
    if a.get("tokens") or str(a.get("auth_mode", "")) == "chatgpt":
        return "chatgpt"
    # An old API-key blob is deliberately reported as signed out.
    if a.get("OPENAI_API_KEY"):
        return ""
    return "unknown" if a else ""


def cli_env(extra=None):
    env = dict(os.environ)
    env["CODEX_HOME"] = CX
    env["HOME"] = CX
    env["NO_COLOR"] = "1"
    env["TERM"] = "dumb"
    env["PATH"] = "/usr/local/bin:/usr/bin:/bin:/home/qianruo/.local/bin"
    if extra:
        env.update(extra)
    return env


def drop_privileges_kwargs():
    """Run the CLI as an unprivileged account; the HTTP service itself is root."""
    try:
        account = pwd.getpwnam("qianruo")
        return {"user": account.pw_uid, "group": account.pw_gid,
                "extra_groups": []}
    except KeyError:
        return {}


def cli_available(path):
    return bool(path) and os.path.exists(path)


def record_path(day=None):
    day = day or datetime.date.today().strftime("%Y%m%d")
    return os.path.join(REC, "chat_%s.txt" % day)


REC_LINE = re.compile(r"^(\d{2}:\d{2}:\d{2})\s+(你|Codex|错误)[：:]\s?(.*)$")


def _rec_stamp(entry):
    ts = str(entry.get("ts") or "")
    try:
        return datetime.datetime.fromisoformat(ts).strftime("%H:%M:%S")
    except ValueError:
        return datetime.datetime.now().strftime("%H:%M:%S")


def format_record(entry):
    """One human-readable line: `23:18:38 你：你好`."""
    role = entry.get("role")
    stamp = _rec_stamp(entry)
    if role == "user":
        return "%s 你：%s" % (stamp, entry.get("content", ""))
    if entry.get("ok") is False:
        return "%s 错误：%s" % (stamp, entry.get("content") or entry.get("error") or "")
    return "%s Codex：%s" % (stamp, entry.get("content", ""))


def append_record(entry):
    ensure_dirs()
    if entry.get("role") == "event":
        return                      # 事件行只用于调试，不写进可读记录
    entry.setdefault("ts", now_iso())
    path = record_path()
    try:
        empty = os.path.getsize(path) == 0
    except OSError:
        empty = True
    with open(path, "a", encoding="utf-8") as fp:
        if not empty:
            fp.write("\n")
        fp.write(format_record(entry) + "\n")
        fp.flush()


def read_records(limit=80):
    """Parse the readable transcript back into role/content rows."""
    rows = []
    for path in sorted(glob.glob(os.path.join(REC, "chat_*.txt")))[-2:]:
        cur = None
        try:
            with open(path, "r", encoding="utf-8") as fp:
                for line in fp:
                    line = line.rstrip("\n")
                    m = REC_LINE.match(line)
                    if m:
                        if cur:
                            rows.append(cur)
                        head, body = m.group(2), m.group(3)
                        if head == "你":
                            cur = {"role": "user", "content": body, "ts": m.group(1), "ok": True}
                        elif head == "错误":
                            cur = {"role": "assistant", "content": body,
                                   "ts": m.group(1), "ok": False, "error": body}
                        else:
                            cur = {"role": "assistant", "content": body, "ts": m.group(1), "ok": True}
                    elif cur is not None:
                        cur["content"] += "\n" + line
        except OSError:
            continue
        if cur:
            rows.append(cur)
    return rows[-limit:]


def result(out, value):
    ensure_dirs()
    tmp = out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fp:
        json.dump(value, fp, ensure_ascii=False)
    os.replace(tmp, out)
    try:
        os.chmod(out, 0o644)
    except OSError:
        pass


def config_toml(s):
    """Write CODEX_HOME/config.toml for the built-in ChatGPT provider.

    Device-code credentials are read from auth.json by the CLI itself; no
    custom model provider or API key is ever configured here.
    """
    lines = ["# Managed by the autonomous-driving-car web UI.",
             'approval_policy = "never"',
             'sandbox_mode = "%s"' % FORCED_SANDBOX]
    if s.get("model"):
        lines.append('model = "%s"' % s["model"])
    if s.get("effort"):
        lines.append('model_reasoning_effort = "%s"' % s["effort"])
    path = CONFIG_TOML + ".tmp"
    with open(path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    os.replace(path, CONFIG_TOML)
    try:
        os.chmod(CONFIG_TOML, 0o644)
    except OSError:
        pass


def build_status():
    s = settings()
    st = state()
    lg = login_state()
    mode = auth_mode()
    if not os.path.exists(AUTH) and lg.get("state") == "success":
        lg = dict(lg)
        lg["state"] = "idle"
        lg["message"] = ""
    messages = [r for r in read_records(400)
                if r.get("role") in ("user", "assistant")][-60:]
    files = glob.glob(os.path.join(REC, "chat_*.txt"))
    return {
        "ok": True,
        "cli_found": cli_available(s.get("cli_path", "")),
        "cli_path": s.get("cli_path", ""),
        "cli_version": s.get("cli_version", ""),
        "logged_in": bool(mode and mode != "none"),
        "auth_mode": mode or "none",
        "login": lg,
        "settings": {"sandbox": FORCED_SANDBOX,
                     "model": s.get("model", ""),
                     "effort": s.get("effort", "medium")},
        "busy": bool(st.get("busy")),
        "job": st.get("job", ""),
        "thread_id": st.get("thread_id", ""),
        "last_error": st.get("last_error", ""),
        "started_at": st.get("started_at", 0),
        "finished_at": st.get("finished_at", 0),
        "messages": messages,
        "records": {"files": len(files),
                    "bytes": sum(os.path.getsize(f) for f in files if os.path.exists(f))},
    }


def background(fn, *args):
    """Detach worker; parent returns immediately, child is reparented to init."""
    pid = os.fork()
    if pid > 0:
        return
    os.setsid()
    devnull = os.open(os.devnull, os.O_RDWR)
    for fd in (0, 1, 2):
        os.dup2(devnull, fd)
    if devnull > 2:
        os.close(devnull)
    try:
        fn(*args)
    except Exception as exc:  # never let a detached worker raise into the shell
        try:
            write_state(busy=False, last_error="worker: %s" % exc,
                        finished_at=int(time.time()))
            append_record({"role": "event", "event": "worker_error",
                           "content": str(exc)[:400], "ok": False})
        except Exception:
            pass
    os._exit(0)


def cli_version(path):
    try:
        p = subprocess.run([path, "--version"], env=cli_env(),
                           capture_output=True, text=True, timeout=10,
                           **drop_privileges_kwargs())
        return (p.stdout or p.stderr).strip().splitlines()[0][:80]
    except Exception:
        return ""


NPM_ENDPOINTS = (
    "https://registry.npmjs.org/@openai/codex/latest",
    "https://registry.npmjs.org/-/package/@openai/codex/dist-tags",
)
UPDATE_CACHE = os.path.join(CX, "update_cache.json")
SEMVER = re.compile(r"\d+\.\d+\.\d+(?:[-.][0-9A-Za-z.]+)?")
UPDATE_COMMAND = "codex update"


def version_of(text):
    m = SEMVER.search(text or "")
    return m.group(0) if m else ""


def version_key(text):
    nums = [int(x) for x in re.findall(r"\d+", text or "")[:3]]
    return tuple((nums + [0, 0, 0])[:3])


def fetch_json(url, via, timeout=4):
    """One short fetch attempt; return {} on any proxy/TLS failure."""
    try:
        if via == "urllib":
            with urllib.request.urlopen(url, timeout=timeout) as resp:
                return json.loads(resp.read(200000).decode("utf-8", "replace"))
        p = subprocess.run(
            ["curl", "-sS", "--max-time", str(timeout), "--connect-timeout", "3", url],
            env=os.environ.copy(), capture_output=True, text=True, timeout=timeout + 1)
        return json.loads(p.stdout or "{}")
    except Exception:
        return {}


def npm_latest_version():
    """Read the published version through the board's system proxy.

    The proxy drops roughly half of the TLS handshakes, so try both npm
    endpoints with both HTTP stacks before giving up.
    """
    attempts = [("urllib", NPM_ENDPOINTS[0]), ("curl", NPM_ENDPOINTS[1]),
                ("urllib", NPM_ENDPOINTS[1]), ("curl", NPM_ENDPOINTS[0])]
    for via, url in attempts:
        data = fetch_json(url, via)
        value = data.get("version") or data.get("latest") or ""
        v = version_of(value if isinstance(value, str) else "")
        if v:
            return v
    return ""


def cmd_check_update(args):
    """Read-only version check: never runs `codex update` by itself."""
    s = settings()
    cli = s.get("cli_path", "")
    current = version_of(s.get("cli_version", "")) or version_of(cli_version(cli))
    latest = npm_latest_version()
    stale = False
    if latest:
        write_json(UPDATE_CACHE, {"latest": latest, "checked_at": int(time.time())})
    else:
        cache = read_json(UPDATE_CACHE, {})
        latest = version_of(cache.get("latest", ""))
        stale = bool(latest)
    if not latest:
        result(args.out, {"ok": False, "current": current, "latest": "",
                          "cli_found": cli_available(cli),
                          "update_available": False, "stale": False,
                          "error": "Cannot reach npm registry (check the board proxy)"})
        return
    result(args.out, {
        "ok": True, "current": current, "latest": latest,
        "cli_found": cli_available(cli),
        "update_available": bool(current) and version_key(latest) > version_key(current),
        "stale": stale,
        "source": "npm registry: @openai/codex",
        "update_command": UPDATE_COMMAND,
        "checked_at": int(time.time()),
    })


def cmd_status(args):
    result(args.out, build_status())


# ---------------------------------------------------------------------------
# Model catalog, account quota and one-click CLI update.
#
# The CLI owns both facts, so the bridge asks it instead of hard-coding a list:
#   * `codex debug models` prints the raw catalog the client ships with;
#   * `codex app-server` speaks JSON-RPC and answers account/rateLimits/read.
# Both calls are short-lived and run as the unprivileged account.
# ---------------------------------------------------------------------------
APP_SERVER_TIMEOUT = 20.0
UPDATE_TIMEOUT = 600
MODELS_CACHE = os.path.join(CX, "models_cache.json")
MODELS_TTL = 3600
QUOTA_CACHE = os.path.join(CX, "quota_cache.json")
QUOTA_TTL = 60


def cache_read(path, ttl):
    data = read_json(path, {})
    if data and (time.time() - float(data.get("cached_at") or 0)) < ttl:
        return data
    return None


def cache_write(path, value):
    value = dict(value)
    value["cached_at"] = int(time.time())
    try:
        write_json(path, value)
    except OSError:
        pass
    return value


def _app_server_line(proc, deadline):
    """Read one JSON-RPC line without blocking past the deadline."""
    remaining = deadline - time.time()
    if remaining <= 0:
        return ""
    ready, _, _ = select.select([proc.stdout], [], [], remaining)
    if not ready:
        return ""
    return proc.stdout.readline()


def app_server_call(method, params=None, timeout=APP_SERVER_TIMEOUT):
    """One initialize + call round trip with `codex app-server` over stdio."""
    s = settings()
    cli = s.get("cli_path", "")
    if not cli_available(cli):
        raise RuntimeError("Codex CLI not found")
    proc = subprocess.Popen(
        [cli, "app-server"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, bufsize=1, env=cli_env(),
        **drop_privileges_kwargs())
    deadline = time.time() + timeout
    try:
        def send(obj):
            proc.stdin.write(json.dumps(obj) + "\n")
            proc.stdin.flush()

        send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
              "params": {"clientInfo": {"name": "autonomous-driving-car",
                                        "title": "Board", "version": "1.0.0"}}})
        while time.time() < deadline:
            line = _app_server_line(proc, deadline)
            if not line:
                raise RuntimeError("app-server handshake timed out")
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if msg.get("id") == 1:
                if msg.get("error"):
                    raise RuntimeError("app-server refused initialize")
                break
        send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        send({"jsonrpc": "2.0", "id": 2, "method": method,
              "params": params or {}})
        while time.time() < deadline:
            line = _app_server_line(proc, deadline)
            if not line:
                raise RuntimeError("app-server did not answer " + method)
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if msg.get("id") == 2:
                if msg.get("error"):
                    raise RuntimeError(str(msg.get("error"))[:200])
                return msg.get("result") or {}
        raise RuntimeError("app-server timed out on " + method)
    finally:
        try:
            proc.kill()
            proc.wait(timeout=5)
        except Exception:
            pass


def cmd_models(args):
    """Visible, API-capable models from the CLI's own catalog.

    `codex debug models` costs ~6 s on the board, and the config page asks for
    it on every load, so the parsed catalog is cached for an hour.
    """
    s = settings()
    cached = cache_read(MODELS_CACHE, MODELS_TTL)
    if cached and cached.get("models"):
        payload = dict(cached)
        payload.pop("cached_at", None)
        payload["selected"] = s.get("model", "")
        payload["cached"] = True
        result(args.out, payload)
        return
    cli = s.get("cli_path", "")
    if not cli_available(cli):
        result(args.out, {"ok": False, "models": [], "error": "Codex CLI not found"})
        return
    try:
        p = subprocess.run([cli, "debug", "models"], env=cli_env(),
                           capture_output=True, text=True, timeout=45,
                           **drop_privileges_kwargs())
    except Exception as exc:
        result(args.out, {"ok": False, "models": [],
                          "error": "%s: %s" % (type(exc).__name__, str(exc)[:160])})
        return
    try:
        catalog = json.loads(p.stdout or "")
    except ValueError:
        result(args.out, {"ok": False, "models": [],
                          "error": (p.stderr or "cannot parse model catalog").strip()[:200]})
        return
    models = []
    for m in catalog.get("models") or []:
        if not isinstance(m, dict):
            continue
        if str(m.get("visibility") or "list") != "list":
            continue
        if not m.get("supported_in_api", True):
            continue
        slug = str(m.get("slug") or "").strip()
        if not slug:
            continue
        efforts = [str(level.get("effort"))
                   for level in (m.get("supported_reasoning_levels") or [])
                   if isinstance(level, dict) and level.get("effort")]
        models.append({"slug": slug,
                       "name": str(m.get("display_name") or slug),
                       "default_effort": str(m.get("default_reasoning_level") or ""),
                       "efforts": efforts})
    payload = cache_write(MODELS_CACHE, {"ok": True, "models": models,
                                         "source": "codex debug models"})
    payload.pop("cached_at", None)
    payload["selected"] = s.get("model", "")
    result(args.out, payload)


def _window(value):
    if not isinstance(value, dict):
        return None
    return {"used_percent": value.get("usedPercent"),
            "window_minutes": value.get("windowDurationMins"),
            "resets_at": value.get("resetsAt")}


def cmd_quota(args):
    """ChatGPT plan, rolling-window usage and remaining credits.

    Spawning `codex app-server` costs ~4 s, so a successful snapshot is reused
    for a minute; the UI refreshes once a minute and always gets an answer.
    """
    if not auth_mode():
        result(args.out, {"ok": False, "error": "not signed in", "signed_in": False})
        return
    cached = cache_read(QUOTA_CACHE, QUOTA_TTL)
    if cached:
        payload = dict(cached)
        payload.pop("cached_at", None)
        payload["cached"] = True
        result(args.out, payload)
        return
    try:
        r = app_server_call("account/rateLimits/read", {})
    except Exception as exc:
        stale = read_json(QUOTA_CACHE, {})
        if stale:
            stale.pop("cached_at", None)
            stale["cached"] = True
            stale["stale"] = True
            result(args.out, stale)
            return
        result(args.out, {"ok": False, "signed_in": True,
                          "error": "%s: %s" % (type(exc).__name__, str(exc)[:160])})
        return
    limits = r.get("rateLimits") or {}
    credits = limits.get("credits") or {}
    payload = cache_write(QUOTA_CACHE, {
        "ok": True,
        "signed_in": True,
        "plan": str(limits.get("planType") or ""),
        "allowed": bool(r.get("ordinaryUsageAllowed")),
        "primary": _window(limits.get("primary")),
        "secondary": _window(limits.get("secondary")),
        "credits": {"has": bool(credits.get("hasCredits")),
                    "unlimited": bool(credits.get("unlimited")),
                    "balance": credits.get("balance")},
        "reached": str(limits.get("rateLimitReachedType") or ""),
        "reset_credits": (r.get("rateLimitResetCredits") or {}).get("availableCount", 0),
        "fetched_at": int(time.time()),
    })
    payload.pop("cached_at", None)
    result(args.out, payload)


PROXY_CONF = "/etc/proxy-client.conf"


def proxy_env():
    """Proxy variables for child processes that fetch over the network.

    The HTTP service is started by systemd, which does not read the board's
    shell profile, so the web-managed proxy has to be applied explicitly.
    """
    env = {}
    try:
        with open(PROXY_CONF, "r", encoding="utf-8") as fp:
            values = {}
            for line in fp:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip().strip('"').strip("'")
    except OSError:
        return env
    host = values.get("PROXY_HOST", "")
    port = values.get("PROXY_PORT", "")
    if not host or not port:
        return env
    scheme = values.get("PROXY_SCHEME", "http") or "http"
    url = "%s://%s:%s" % (scheme, host, port)
    env["http_proxy"] = env["HTTP_PROXY"] = url
    env["https_proxy"] = env["HTTPS_PROXY"] = url
    env["all_proxy"] = env["ALL_PROXY"] = url
    no_proxy = values.get("NO_PROXY", "")
    if no_proxy:
        env["no_proxy"] = env["NO_PROXY"] = no_proxy
    return env


def update_env():
    """Environment for `codex update`.

    The updater locates the standalone install through HOME/CODEX_HOME, so it
    must see the real account home (the chat runs keep using the isolated
    CODEX_HOME). Proxy variables come from the web-managed proxy config.
    """
    env = cli_env()
    env["HOME"] = "/home/qianruo"
    env["CODEX_HOME"] = "/home/qianruo/.codex"
    for key, value in proxy_env().items():
        env.setdefault(key, value)
    return env


UPDATE_TMPDIR = os.path.join(REC, "tmp")
UPDATE_PROGRESS = os.path.join(REC, "codex_update_progress.txt")


def update_download_state():
    """Bytes the installer has pulled so far.

    The service runs with PrivateTmp, so it cannot see the host /tmp where the
    updater writes its tarball. The transient unit therefore runs a small
    reporter that samples the file size into UPDATE_PROGRESS, which is inside
    the writable recordings directory.
    """
    best = 0
    try:
        with open(UPDATE_PROGRESS, "r", encoding="utf-8") as fp:
            best = int((fp.read() or "0").strip() or 0)
    except (OSError, ValueError):
        best = 0
    path = ""
    for root, dirs, files in os.walk(UPDATE_TMPDIR):
        for name in files:
            if not name.lower().endswith(".tar.gz"):
                continue
            full = os.path.join(root, name)
            try:
                size = os.path.getsize(full)
            except OSError:
                continue
            if size > best:
                best, path = size, full
    return best, path


UPDATE_TOTAL = 187844332   # codex-package-aarch64-unknown-linux-musl.tar.gz


def cmd_update_status(args):
    """Progress of the running (or last) `codex update`."""
    s = settings()
    cli = s.get("cli_path", "")
    unit = running_update_unit()
    log = read_text(os.path.join(REC, "codex_update.log"))
    done_bytes, path = update_download_state()
    total = UPDATE_TOTAL
    m = re.search(r"out of (\d+) bytes", log)
    if m:
        total = int(m.group(1))
    percent = int(min(100, done_bytes * 100 / total)) if total else 0
    tail = [line for line in log.strip().splitlines() if line.strip()][-3:]
    result(args.out, {
        "ok": True,
        "running": bool(unit),
        "unit": unit,
        "downloaded": done_bytes,
        "total": total,
        "percent": percent,
        "phase": (tail[-1] if tail else ""),
        "log": "\n".join(tail)[-600:],
        "cli_version": s.get("cli_version", "") or cli_version(cli),
        "path": path,
    })


def running_update_unit():
    """Name of an already running update unit, if any (never run two at once)."""
    try:
        p = subprocess.run(["systemctl", "list-units", "--all", "--no-legend",
                            "--plain", "codex-update-*.service"],
                           capture_output=True, text=True, timeout=10)
    except Exception:
        return ""
    for line in (p.stdout or "").splitlines():
        parts = line.split()
        # systemctl columns: UNIT LOAD ACTIVE SUB DESCRIPTION
        if len(parts) >= 4 and parts[0].startswith("codex-update-") and parts[3] == "running":
            return parts[0]
    return ""


def start_update_outside_sandbox(cli):
    """Start `codex update` in the host namespace and return immediately.

    The HTTP service is hardened with ProtectSystem=strict and
    ProtectHome=read-only, so a child process cannot write the new release into
    ~/.codex/packages. systemd-run (the service runs as root) starts the updater
    outside that sandbox. The release is ~190 MB and the board link is slow, so
    the HTTP worker must not wait for it: the unit runs detached and the UI
    reports progress on the next visit.
    """
    log_path = os.path.join(REC, "codex_update.log")
    unit = "codex-update-%d" % int(time.time())
    try:
        os.makedirs(UPDATE_TMPDIR, exist_ok=True)
        os.chmod(UPDATE_TMPDIR, 0o777)
    except OSError:
        pass
    # A flaky TLS proxy kills single attempts; retry a few times before giving up.
    # A background reporter samples the host-side tarball size so the web UI can
    # show real download progress (the service's PrivateTmp hides /tmp).
    reporter = (
        "(for _ in $(seq 1 1800); do "
        "f=$(ls -t /tmp/tmp.*/codex-package-aarch64-unknown-linux-musl.tar.gz "
        "2>/dev/null | head -1); "
        "if [ -n \"$f\" ]; then stat -c %%s \"$f\" > %s 2>/dev/null; fi; "
        "sleep 2; done) & REPORTER=$!; "
        "for i in 1 2 3; do %s update && break; echo \"retry $i\"; sleep 5; done; "
        "kill $REPORTER 2>/dev/null"
    ) % (UPDATE_PROGRESS, cli)
    shell = "{ %s; } > %s 2>&1" % (reporter, log_path)
    cmd = ["systemd-run", "--collect", "--quiet", "--unit", unit,
           "--uid", "qianruo", "--gid", "qianruo",
           "--setenv", "HOME=/home/qianruo",
           "--setenv", "CODEX_HOME=/home/qianruo/.codex",
           "--setenv", "PATH=/usr/local/bin:/usr/bin:/bin:/home/qianruo/.local/bin",
           "--setenv", "TERM=dumb",
           "--setenv", "NO_COLOR=1",
           "--setenv", "TMPDIR=" + UPDATE_TMPDIR]
    for key, value in proxy_env().items():
        cmd += ["--setenv", "%s=%s" % (key, value)]
    cmd += ["/bin/sh", "-c", shell]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
    return unit, p.returncode, (p.stdout or "") + (p.stderr or ""), log_path


def cmd_update_install(args):
    """Start `codex update` in the background and report the current version."""
    s = settings()
    cli = s.get("cli_path", "")
    if not cli_available(cli):
        result(args.out, {"ok": False, "error": "Codex CLI not found"})
        return
    before = version_of(s.get("cli_version", "")) or version_of(cli_version(cli))
    if os.geteuid() == 0 and shutil.which("systemd-run"):
        existing = running_update_unit()
        if existing:
            result(args.out, {"ok": True, "started": True, "before": before,
                              "version": before, "unit": existing,
                              "log": os.path.join(REC, "codex_update.log"),
                              "note": "an update is already running",
                              "update_command": UPDATE_COMMAND})
            return
        try:
            unit, rc, output, log_path = start_update_outside_sandbox(cli)
        except Exception as exc:
            result(args.out, {"ok": False, "before": before, "version": before,
                              "error": "%s: %s" % (type(exc).__name__, str(exc)[:160])})
            return
        result(args.out, {"ok": rc == 0, "started": rc == 0, "before": before,
                          "version": before, "unit": unit,
                          "log": log_path,
                          "output": output.strip()[-500:],
                          "note": "update started in the background",
                          "update_command": UPDATE_COMMAND})
        return
    try:
        p = subprocess.run([cli, "update"], env=update_env(), capture_output=True,
                           text=True, timeout=UPDATE_TIMEOUT, cwd="/home/qianruo",
                           **drop_privileges_kwargs())
        rc = p.returncode
        output = ((p.stdout or "") + (p.stderr or "")).strip()[-4000:]
    except subprocess.TimeoutExpired:
        result(args.out, {"ok": False, "before": before, "version": before,
                          "error": "codex update timed out"})
        return
    except Exception as exc:
        result(args.out, {"ok": False, "before": before, "version": before,
                          "error": "%s: %s" % (type(exc).__name__, str(exc)[:160])})
        return
    after_version = cli_version(cli)
    latest = version_of(after_version) or before
    if after_version:
        s["cli_version"] = after_version
        write_json(SETTINGS, s)
    changed = bool(latest) and latest != before and version_key(latest) > version_key(before)
    result(args.out, {"ok": rc == 0 and changed, "exit_code": rc, "before": before,
                      "version": latest, "cli_version": after_version,
                      "changed": changed,
                      "output": output,
                      "update_command": UPDATE_COMMAND})


def cmd_config(args):
    payload = json.loads(read_text(args.input) or "{}")
    s = settings()
    for key in ("cli_path", "model"):
        if key in payload:
            value = str(payload[key])[:512].strip()
            s[key] = value
    if "effort" in payload:
        value = str(payload["effort"]).strip().lower()
        s["effort"] = value if value in EFFORTS else "medium"
    if not s.get("cli_version") and cli_available(s.get("cli_path", "")):
        s["cli_version"] = cli_version(s["cli_path"])
    write_json(SETTINGS, s)
    config_toml(s)
    result(args.out, {"ok": True, "status": build_status()})


def device_login_worker(cli):
    write_json(LOGIN, {"state": "starting", "url": "", "code": "",
                       "message": "", "started_at": int(time.time()),
                       "expires_at": int(time.time()) + 900})
    master, slave = pty.openpty()
    try:
        proc = subprocess.Popen([cli, "login", "--device-auth"],
                                stdin=slave, stdout=slave, stderr=slave,
                                env=cli_env(), start_new_session=True,
                                **drop_privileges_kwargs())
    except Exception as exc:
        os.close(master)
        os.close(slave)
        write_json(LOGIN, {"state": "error", "url": "", "code": "",
                           "message": str(exc)[:300],
                           "started_at": int(time.time()), "expires_at": 0})
        return
    os.close(slave)
    started = time.time()
    clean_text = ""
    tail = ""
    url = code = ""
    while time.time() - started < 900:
        if proc.poll() is not None and not select.select([master], [], [], 0)[0]:
            break
        if not select.select([master], [], [], 1.0)[0]:
            continue
        try:
            chunk = os.read(master, 4096).decode("utf-8", "replace")
        except OSError:
            break
        if not chunk:
            break
        clean_text = (clean_text + ANSI.sub("", chunk))[-8000:]
        tail = (tail + chunk)[-1200:]
        if not url:
            url = find_device_url(clean_text)
        if not code:
            m = DEVICE_CODE.search(clean_text)
            if m:
                code = m.group(1)
        if url or code:
            write_json(LOGIN, {"state": "pending", "url": url, "code": code,
                               "message": "Waiting for browser authorisation",
                               "started_at": int(started),
                               "expires_at": int(started) + 900})
    try:
        os.close(master)
    except OSError:
        pass
    try:
        proc.wait(timeout=5)
        rc = proc.returncode
    except Exception:
        proc.kill()
        rc = -1
    clean = ANSI.sub("", tail).replace("\r", " ").strip()
    # The one-time code sometimes only shows up in the final output (success
    # banner or failure message); match it there as a last resort.
    if not url:
        url = find_device_url(clean_text)
    if not code:
        m = DEVICE_CODE.search(clean_text) or DEVICE_CODE.search(clean)
        if m:
            code = m.group(1)
    logged = os.path.exists(AUTH)
    final = "success" if (rc == 0 or logged) else "error"
    write_json(LOGIN, {"state": final, "url": url, "code": code,
                       "message": ("Device login finished" if final == "success"
                                   else clean[-300:] or "Device login failed"),
                       "started_at": int(started), "expires_at": 0})
    s = settings()
    if not s.get("cli_version"):
        s["cli_version"] = cli_version(cli)
        write_json(SETTINGS, s)
    config_toml(s)  # device-code login uses the built-in provider


def cmd_login_device(args):
    s = settings()
    cli = s.get("cli_path", "")
    if not cli_available(cli):
        result(args.out, {"ok": False, "error": "Codex CLI not found"})
        return
    config_toml(s)
    write_json(LOGIN, {"state": "starting", "url": "", "code": "",
                       "message": "", "started_at": int(time.time()),
                       "expires_at": int(time.time()) + 900})
    background(device_login_worker, cli)
    result(args.out, {"ok": True})


def build_prompt(message):
    history = [r for r in read_records(20) if r.get("role") in ("user", "assistant")]
    history = history[-6:]
    if not history:
        return message
    lines = ["Previous conversation (for context only):"]
    for row in history:
        who = "User" if row.get("role") == "user" else "Codex"
        lines.append("%s: %s" % (who, str(row.get("content", ""))[:1200]))
    lines += ["", "Current message:", message]
    return "\n".join(lines)


def chat_worker(job, message, cli):
    started = time.time()
    append_record({"role": "user", "content": message, "job": job,
                   "session": state().get("thread_id", ""), "ok": True})
    s = settings()
    config_toml(s)  # always match the provider to the credential in auth.json
    reply_file = os.path.join(OUT_DIR, "reply_%s.txt" % job)
    cmd = [cli, "exec", "--json", "--skip-git-repo-check",
           "-s", FORCED_SANDBOX, "-C", ROOT, "-o", reply_file, "-"]
    events = []
    events_raw = []
    thread_id = state().get("thread_id", "")
    error = ""
    reply = ""
    rc = -1
    try:
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=cli_env(),
                                start_new_session=True, **drop_privileges_kwargs())
        write_state(child_pid=proc.pid)
        try:
            out, err = proc.communicate(input=build_prompt(message).encode("utf-8"),
                                        timeout=300)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
            error = "Codex timed out after 300 s"
        rc = proc.returncode
        for line in out.decode("utf-8", "replace").splitlines():
            line = line.strip()
            if not line:
                continue
            events_raw.append(line)
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            etype = str(ev.get("type", ""))
            if etype == "thread.started":
                thread_id = str(ev.get("thread_id", "")) or thread_id
            elif etype in ("error", "turn.failed"):
                info = ev.get("error") or {}
                error = str(ev.get("message") or info.get("message") or "turn failed")
            events.append({"type": etype, "line": line[:500]})
        if not error and rc not in (0,):
            tail = err.decode("utf-8", "replace").strip()[-300:]
            error = tail or "codex exec exited with %d" % rc
    except Exception as exc:
        error = str(exc)[:300]
    try:
        with open(reply_file, "r", encoding="utf-8") as fp:
            reply = fp.read().strip()
    except OSError:
        pass
    if not reply:
        for ev in reversed(events_raw):
            try:
                obj = json.loads(ev)
                item = obj.get("item") or {}
                if item.get("type") == "agent_message":
                    reply = str(item.get("text", "")).strip()
                    if reply:
                        break
            except (ValueError, AttributeError):
                continue
    for ev in events[:40]:
        append_record({"role": "event", "event": ev["type"],
                       "content": ev["line"], "job": job, "ok": not error})
    append_record({"role": "assistant", "content": reply or error or "(no reply)",
                   "job": job, "session": thread_id,
                   "elapsed_ms": int((time.time() - started) * 1000),
                   "ok": not error, "error": error})
    write_state(busy=False, job=job, thread_id=thread_id, child_pid=0,
                last_error=error, finished_at=int(time.time()))
    try:
        os.unlink(reply_file)
    except OSError:
        pass


def cmd_chat_start(args):
    message = (read_text(args.input) or "").strip()
    if not message:
        result(args.out, {"ok": False, "error": "empty message"})
        return
    if len(message) > 16000:
        message = message[:16000]
    mode = auth_mode()
    if not mode or mode == "none":
        result(args.out, {"ok": False,
                          "error": "Codex CLI is not signed in. Sign in on /config first."})
        return
    s = settings()
    cli = s.get("cli_path", "")
    if not cli_available(cli):
        result(args.out, {"ok": False, "error": "Codex CLI not found"})
        return
    st = state()
    if st.get("busy") and time.time() - int(st.get("started_at") or 0) < 330:
        result(args.out, {"ok": False, "error": "Codex is still answering"})
        return
    job = datetime.datetime.now().strftime("%H%M%S") + "-" + uuid.uuid4().hex[:8]
    write_state(busy=True, job=job, last_error="", started_at=int(time.time()))
    background(chat_worker, job, message, cli)
    result(args.out, {"ok": True, "job": job})


def cmd_reset(args):
    if state().get("busy"):
        result(args.out, {"ok": False, "error": "Codex is still answering"})
        return
    write_state(thread_id="", last_error="", job="")
    result(args.out, {"ok": True, "status": build_status()})


def cmd_stop(args):
    st = state()
    pid = 0
    try:
        pid = int(st.get("child_pid") or 0)
    except (TypeError, ValueError):
        pid = 0
    killed = False
    if pid > 0:
        try:
            os.killpg(pid, signal.SIGTERM)
            killed = True
        except OSError:
            try:
                os.kill(pid, signal.SIGTERM)
                killed = True
            except OSError:
                pass
    write_state(busy=False, child_pid=0,
                last_error="Stopped by user" if killed else "",
                finished_at=int(time.time()))
    result(args.out, {"ok": True, "stopped": killed})


def cmd_logout(args):
    cmd_stop(args)
    try:
        os.unlink(AUTH)
    except OSError:
        pass
    config_toml(settings())
    write_json(LOGIN, {"state": "idle", "url": "", "code": "", "message": "",
                       "started_at": 0, "expires_at": 0})
    write_state(thread_id="", last_error="")
    result(args.out, {"ok": True, "status": build_status()})


def cmd_history(args):
    after = 0
    try:
        after = int(args.arg or "0")
    except ValueError:
        after = 0
    rows = read_records(500)
    result(args.out, {"ok": True, "messages": rows, "total": len(rows),
                      "after": after})


def record_files():
    rows = []
    for path in sorted(glob.glob(os.path.join(REC, "chat_*.txt")), reverse=True):
        st = os.stat(path)
        name = os.path.basename(path)
        day = name[5:13]
        try:
            date = "%s-%s-%s" % (day[:4], day[4:6], day[6:8])
        except Exception:
            date = day
        rows.append({"name": name, "date": date, "size": st.st_size,
                     "mtime": datetime.datetime.fromtimestamp(st.st_mtime)
                     .strftime("%Y-%m-%d %H:%M:%S")})
    return rows


def cmd_records(args):
    result(args.out, {"ok": True, "files": record_files()})


def cmd_export(args):
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    name = "codex_chat_%s.txt" % stamp
    path = os.path.join(REC, ".export_%s" % name)
    count = 0
    with open(path, "w", encoding="utf-8") as dst:
        for src_path in sorted(glob.glob(os.path.join(REC, "chat_*.txt"))):
            with open(src_path, "r", encoding="utf-8") as src:
                for line in src:
                    if line.strip():
                        dst.write(line if line.endswith("\n") else line + "\n")
                        count += 1
    result(args.out, {"ok": True, "path": path, "name": name,
                      "size": os.path.getsize(path), "records": count})


def cmd_record(args):
    name = os.path.basename(args.arg or "")
    if not RECORD_NAME.match(name):
        result(args.out, {"ok": False, "error": "invalid record name"})
        return
    path = os.path.join(REC, name)
    if not os.path.exists(path):
        result(args.out, {"ok": False, "error": "record not found"})
        return
    result(args.out, {"ok": True, "path": path, "name": name,
                      "size": os.path.getsize(path)})



def cmd_records_delete(args):
    """Delete one interaction record by name."""
    name = os.path.basename(args.arg or "")
    if not RECORD_NAME.match(name):
        result(args.out, {"ok": False, "error": "invalid record name"})
        return
    path = os.path.join(REC, name)
    try:
        os.unlink(path)
    except OSError as exc:
        result(args.out, {"ok": False, "error": str(exc)[:160]})
        return
    result(args.out, {"ok": True, "deleted": name})

def cmd_records_clear(args):
    removed = 0
    for path in glob.glob(os.path.join(REC, "chat_*.txt")):
        try:
            os.unlink(path)
            removed += 1
        except OSError:
            pass
    result(args.out, {"ok": True, "deleted": removed})


def read_text(path):
    if not path:
        return ""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fp:
            return fp.read()
    except OSError:
        return ""


COMMANDS = {
    "status": cmd_status,
    "check-update": cmd_check_update,
    "update-install": cmd_update_install,
    "update-status": cmd_update_status,
    "models": cmd_models,
    "quota": cmd_quota,
    "config": cmd_config,
    "login-device": cmd_login_device,
    "chat-start": cmd_chat_start,
    "reset": cmd_reset,
    "stop": cmd_stop,
    "logout": cmd_logout,
    "history": cmd_history,
    "records": cmd_records,
    "export": cmd_export,
    "record": cmd_record,
    "records-clear": cmd_records_clear,
    "records-delete": cmd_records_delete,
}


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=sorted(COMMANDS))
    parser.add_argument("--out", required=True)
    parser.add_argument("--input")
    parser.add_argument("--arg")
    args = parser.parse_args()
    ensure_dirs()
    try:
        COMMANDS[args.command](args)
    except Exception as exc:
        result(args.out, {"ok": False, "error": "%s: %s" % (type(exc).__name__, exc)})
        sys.exit(1)


if __name__ == "__main__":
    main()
