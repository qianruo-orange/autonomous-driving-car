#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Standalone web backend for the board-wide network proxy.

It edits /etc/proxy-client.conf, the EnvironmentFile of proxy-client.service.
That service regenerates /etc/environment, /etc/profile.d/proxy.sh,
/etc/systemd/system.conf.d/90-proxy-client.conf and apt's proxy settings,
so a reload changes the proxy used by the whole board, not by any single
process. This module has no dependency on the Codex chat bridge.
"""
import argparse
import datetime
import json
import os
import re
import socket
import subprocess
import sys

CONF = "/etc/proxy-client.conf"
SERVICE = "proxy-client.service"
ROOT = os.environ.get("PROXY_WEB_ROOT", "/opt/autonomous-driving-car")
BACKUP_DIR = os.path.join(ROOT, "recordings", "proxy-backups")

KEYS = ("PROXY_HOST", "PROXY_PORT", "PROXY_SCHEME", "NO_PROXY")
DEFAULTS = {
    "PROXY_HOST": "127.0.0.1",
    "PROXY_PORT": "7890",
    "PROXY_SCHEME": "http",
    "NO_PROXY": "localhost,127.0.0.1,::1",
}
SCHEMES = {"http", "socks5", "socks5h"}
HOST_RE = re.compile(r"^[A-Za-z0-9.-]+$")
BYPASS_RE = re.compile(r"^[A-Za-z0-9.,:/_-]+$")
ASSIGN_RE = re.compile(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fp:
            return fp.read()
    except OSError:
        return ""


def result(out, value):
    tmp = out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fp:
        json.dump(value, fp, ensure_ascii=False)
    os.replace(tmp, out)
    try:
        os.chmod(out, 0o644)
    except OSError:
        pass


def parse_conf(text):
    values = {}
    for line in text.splitlines():
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#") or stripped.startswith(";"):
            continue
        m = ASSIGN_RE.match(line)
        if not m:
            continue
        raw = m.group(2).strip()
        if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in ("'", '"'):
            raw = raw[1:-1]
        else:
            raw = raw.split("#", 1)[0].strip()
        values[m.group(1)] = raw
    return values


def settings():
    raw = parse_conf(read_text(CONF))
    return {k: str(raw.get(k) or DEFAULTS[k]) for k in KEYS}


def valid(cfg):
    host = str(cfg.get("PROXY_HOST", ""))
    port = str(cfg.get("PROXY_PORT", ""))
    scheme = str(cfg.get("PROXY_SCHEME", ""))
    bypass = str(cfg.get("NO_PROXY", ""))
    return (bool(HOST_RE.match(host)) and
            port.isdigit() and 0 < int(port) < 65536 and
            scheme in SCHEMES and bool(BYPASS_RE.match(bypass)))


def proxy_url(cfg):
    return "%s://%s:%s" % (cfg["PROXY_SCHEME"], cfg["PROXY_HOST"],
                           cfg["PROXY_PORT"])


def render_conf(cfg, original):
    """Replace managed assignments, keeping comments and unrelated lines."""
    lines, seen = [], set()
    for line in original.splitlines():
        m = ASSIGN_RE.match(line)
        if m and m.group(1) in KEYS:
            key = m.group(1)
            lines.append('%s="%s"' % (key, cfg[key]))
            seen.add(key)
        else:
            lines.append(line)
    for key in KEYS:
        if key not in seen:
            lines.append('%s="%s"' % (key, cfg[key]))
    return "\n".join(lines).rstrip() + "\n"


def write_conf(cfg, original):
    text = render_conf(cfg, original)
    backup = ""
    if original:
        os.makedirs(BACKUP_DIR, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        backup = os.path.join(BACKUP_DIR, "proxy-client.conf.%s" % stamp)
        with open(backup, "w", encoding="utf-8") as fp:
            fp.write(original)
        os.chmod(backup, 0o644)
    # ProtectSystem=strict exposes only this one file under /etc, so an atomic
    # replace from a temp file in /etc is not possible; write in place.
    with open(CONF, "w", encoding="utf-8") as fp:
        fp.write(text)
        fp.flush()
        os.fsync(fp.fileno())
    try:
        os.chmod(CONF, 0o644)
    except OSError:
        pass
    return backup


def service_active():
    try:
        p = subprocess.run(["systemctl", "is-active", SERVICE],
                           capture_output=True, text=True, timeout=5)
        return p.stdout.strip() or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def apply_service():
    """Reload the host unit so the whole board picks up the new proxy."""
    try:
        p = subprocess.run(["systemctl", "reload", SERVICE],
                           capture_output=True, text=True, timeout=30)
        return p.returncode == 0, (p.stderr or p.stdout or "").strip()[:300]
    except (OSError, subprocess.SubprocessError) as exc:
        return False, str(exc)[:300]


def system_environment():
    """Values proxy-client.service applied to the whole board."""
    wanted = ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY")
    values = {k: "" for k in wanted}
    for line in read_text("/etc/environment").splitlines():
        m = ASSIGN_RE.match(line)
        if m and m.group(1) in wanted:
            values[m.group(1)] = m.group(2).strip().strip('"').strip("'")
    return values


def cmd_get(args):
    original = read_text(CONF)
    cfg = settings()
    result(args.out, {
        "ok": True, "path": CONF, "exists": bool(original),
        "config": cfg, "url": proxy_url(cfg), "raw": original[:8192],
        "system": system_environment(),
        "service": service_active(),
    })


def cmd_set(args):
    payload = json.loads(read_text(args.input) or "{}")
    cfg = {k: str(payload.get(k, DEFAULTS[k])).strip()[:256] for k in KEYS}
    if not valid(cfg):
        result(args.out, {"ok": False,
                          "error": "invalid proxy host, port, scheme or bypass list"})
        return
    original = read_text(CONF)
    try:
        backup = write_conf(cfg, original)
    except OSError as exc:
        result(args.out, {"ok": False,
                          "error": "cannot write %s: %s" % (CONF, exc)})
        return
    applied, apply_error = False, ""
    if payload.get("apply"):
        applied, apply_error = apply_service()
    result(args.out, {"ok": True, "config": cfg, "url": proxy_url(cfg),
                      "backup": backup, "applied": applied,
                      "apply_error": apply_error, "service": service_active()})


def cmd_test(args):
    cfg = settings()
    if not valid(cfg):
        result(args.out, {"ok": False, "error": "invalid proxy configuration"})
        return
    url = proxy_url(cfg)
    tcp_ok, tcp_error = False, ""
    try:
        with socket.create_connection(
                (cfg["PROXY_HOST"], int(cfg["PROXY_PORT"])), 5):
            tcp_ok = True
    except OSError as exc:
        tcp_error = str(exc)[:160]
    http_code, curl_error = 0, ""
    # The service inherits proxy variables from systemd. Strip them so curl
    # uses only the explicit --proxy argument instead of mixing schemes.
    clean_env = {k: v for k, v in os.environ.items()
                 if not k.lower().endswith("_proxy")}
    try:
        p = subprocess.run(
            ["curl", "-sS", "-o", "/dev/null", "-w", "%{http_code}",
             "--proxy", url, "--connect-timeout", "8", "--max-time", "15",
             "https://api.openai.com/v1/models"],
            capture_output=True, text=True, timeout=20, env=clean_env)
        digits = "".join(c for c in p.stdout.strip() if c.isdigit())
        http_code = int(digits) if digits else 0
        if p.returncode != 0:
            curl_error = (p.stderr or "curl failed").strip()[:160]
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        curl_error = str(exc)[:160]
    # 401 is expected: the endpoint answers, but no credentials are supplied.
    reachable = tcp_ok and http_code in (200, 301, 302, 400, 401, 403, 404)
    result(args.out, {"ok": True, "url": url, "tcp": tcp_ok,
                      "tcp_error": tcp_error, "http_code": http_code,
                      "curl_error": curl_error, "reachable": reachable})


COMMANDS = {"get": cmd_get, "set": cmd_set, "test": cmd_test}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=sorted(COMMANDS))
    parser.add_argument("--out", required=True)
    parser.add_argument("--input")
    args = parser.parse_args()
    try:
        COMMANDS[args.command](args)
    except Exception as exc:
        result(args.out, {"ok": False,
                          "error": "%s: %s" % (type(exc).__name__, exc)})
        sys.exit(1)


if __name__ == "__main__":
    main()
