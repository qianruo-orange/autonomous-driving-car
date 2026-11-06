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
    "sandbox": "read-only",
    "workdir": ROOT,
    "api_base_url": "https://api.openai.com/v1",
    "wire_api": "responses",
    "cli_version": "",
}
SANDBOXES = {"read-only", "workspace-write", "danger-full-access"}
WIRE_APIS = {"responses", "chat"}
RECORD_NAME = re.compile(r"^chat_\d{8}\.jsonl$")
DEVICE_URL = re.compile(r"https://[^\s/]*auth\.openai\.com[^\s]*|https://[^\s]+/device[^\s]*")
DEVICE_CODE = re.compile(r"\b([A-Z0-9]{4,}-[A-Z0-9]{4,})\b")


def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def ensure_dirs():
    for d in (CX, REC, OUT_DIR, INBOX):
        os.makedirs(d, exist_ok=True)


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
    """Return 'apikey', 'chatgpt', 'unknown' or '' from CODEX_HOME/auth.json."""
    if not os.path.exists(AUTH):
        return ""
    a = read_json(AUTH, {})
    mode = str(a.get("auth_mode", "") or "")
    if mode:
        return mode
    if a.get("OPENAI_API_KEY"):
        return "apikey"
    if a.get("tokens"):
        return "chatgpt"
    return "unknown"


def cli_env(extra=None):
    env = dict(os.environ)
    env["CODEX_HOME"] = CX
    env["HOME"] = CX
    env["NO_COLOR"] = "1"
    env["TERM"] = "dumb"
    env["PATH"] = "/usr/local/bin:/usr/bin:/bin:/home/qianruo/.local/bin"
    # A custom model provider declares env_key = OPENAI_API_KEY. The CLI stores
    # the key in CODEX_HOME/auth.json, so expose it to the child process only.
    key = read_json(AUTH, {}).get("OPENAI_API_KEY")
    if key:
        env["OPENAI_API_KEY"] = str(key)
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
    return os.path.join(REC, "chat_%s.jsonl" % day)


def append_record(entry):
    ensure_dirs()
    entry.setdefault("ts", now_iso())
    with open(record_path(), "a", encoding="utf-8") as fp:
        fp.write(json.dumps(entry, ensure_ascii=False) + "\n")
        fp.flush()


def read_records(limit=80):
    files = sorted(glob.glob(os.path.join(REC, "chat_*.jsonl")))[-2:]
    rows = []
    for path in files:
        try:
            with open(path, "r", encoding="utf-8") as fp:
                for line in fp:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rows.append(json.loads(line))
                    except ValueError:
                        continue
        except OSError:
            continue
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


def config_toml(s, provider=None):
    """Write CODEX_HOME/config.toml.

    A custom [model_providers.web] is only enabled for API-key logins: it
    declares env_key = OPENAI_API_KEY, which ChatGPT device-code credentials
    never provide. Leaving it in place made `codex exec` fail with
    "Missing environment variable: OPENAI_API_KEY" after a device login.
    """
    mode = provider if provider is not None else auth_mode()
    lines = ["# Managed by the autonomous-driving-car web UI.",
             'approval_policy = "never"',
             'sandbox_mode = "%s"' % s["sandbox"]]
    if s.get("model"):
        lines.append('model = "%s"' % s["model"])
    if mode == "apikey":
        lines.insert(1, 'model_provider = "web"')
        lines += ["", "[model_providers.web]",
                  'name = "Web configured provider"',
                  'base_url = "%s"' % s["api_base_url"],
                  'env_key = "OPENAI_API_KEY"',
                  'wire_api = "%s"' % s["wire_api"], ""]
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
    files = glob.glob(os.path.join(REC, "chat_*.jsonl"))
    return {
        "ok": True,
        "cli_found": cli_available(s.get("cli_path", "")),
        "cli_path": s.get("cli_path", ""),
        "cli_version": s.get("cli_version", ""),
        "logged_in": bool(mode and mode != "none"),
        "auth_mode": mode or "none",
        "login": lg,
        "settings": {k: s.get(k, "") for k in
                     ("model", "sandbox", "workdir", "api_base_url", "wire_api")},
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


def cmd_status(args):
    result(args.out, build_status())


def cmd_config(args):
    payload = json.loads(read_text(args.input) or "{}")
    s = settings()
    for key in ("cli_path", "model", "sandbox", "workdir", "api_base_url", "wire_api"):
        if key in payload:
            value = str(payload[key])[:512].strip()
            if key == "sandbox" and value not in SANDBOXES:
                value = "read-only"
            if key == "wire_api" and value not in WIRE_APIS:
                value = "responses"
            if key == "workdir" and not value.startswith(ROOT):
                value = ROOT
            if key == "api_base_url" and not value.startswith("https://"):
                value = DEFAULT_SETTINGS["api_base_url"]
            s[key] = value
    if not s.get("cli_version") and cli_available(s.get("cli_path", "")):
        s["cli_version"] = cli_version(s["cli_path"])
    write_json(SETTINGS, s)
    config_toml(s)
    result(args.out, {"ok": True, "status": build_status()})


def cmd_login_apikey(args):
    payload = json.loads(read_text(args.input) or "{}")
    key = str(payload.get("api_key", "")).strip()
    if not key:
        result(args.out, {"ok": False, "error": "empty API key"})
        return
    s = settings()
    if payload.get("api_base_url"):
        s["api_base_url"] = str(payload["api_base_url"]).strip()
    if payload.get("model"):
        s["model"] = str(payload["model"]).strip()
    if payload.get("wire_api") in WIRE_APIS:
        s["wire_api"] = payload["wire_api"]
    write_json(SETTINGS, s)
    config_toml(s, provider="")
    try:
        p = subprocess.run([s["cli_path"], "login", "--with-api-key"],
                           input=key, env=cli_env(), capture_output=True,
                           text=True, timeout=60, **drop_privileges_kwargs())
        ok = p.returncode == 0
        message = (p.stderr or p.stdout or "").strip()[-300:]
        if not message:
            message = "API key saved" if ok else "login failed"
    except Exception as exc:
        ok, message = False, str(exc)[:300]
    config_toml(s)  # auth.json now exists: enable the custom provider for API keys
    write_json(LOGIN, {"state": "success" if ok else "error", "url": "",
                       "code": "", "message": message,
                       "started_at": int(time.time()), "expires_at": 0})
    result(args.out, {"ok": ok, "error": "" if ok else message,
                      "status": build_status()})


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
    buffer = ""
    url = code = ""
    tail = ""
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
        buffer += chunk
        tail = (tail + chunk)[-1200:]
        if not url:
            m = DEVICE_URL.search(buffer)
            if m:
                url = m.group(0).rstrip(".,;)")
        if not code:
            m = DEVICE_CODE.search(buffer)
            if m:
                code = m.group(1)
        if url or code:
            write_json(LOGIN, {"state": "pending", "url": url, "code": code,
                               "message": "Waiting for browser authorisation",
                               "started_at": int(started),
                               "expires_at": int(started) + 900})
        buffer = buffer[-4000:]
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
    clean = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", tail).replace("\r", " ").strip()
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
    config_toml(s)  # ChatGPT login: keep the built-in provider, no API key needed


def cmd_login_device(args):
    s = settings()
    cli = s.get("cli_path", "")
    if not cli_available(cli):
        result(args.out, {"ok": False, "error": "Codex CLI not found"})
        return
    config_toml(s, provider="")  # do not ask the device flow for an API key
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
           "-s", s["sandbox"], "-C", s["workdir"], "-o", reply_file, "-"]
    if s.get("model"):
        cmd[2:2] = ["-m", s["model"]]
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
    config_toml(settings())  # signed out: remove the API-key provider override
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
    for path in sorted(glob.glob(os.path.join(REC, "chat_*.jsonl")), reverse=True):
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
    name = "codex_chat_%s.jsonl" % stamp
    path = os.path.join(REC, ".export_%s" % name)
    count = 0
    with open(path, "w", encoding="utf-8") as dst:
        for src_path in sorted(glob.glob(os.path.join(REC, "chat_*.jsonl"))):
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


def cmd_records_clear(args):
    removed = 0
    for path in glob.glob(os.path.join(REC, "chat_*.jsonl")):
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
    "config": cmd_config,
    "login-apikey": cmd_login_apikey,
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
