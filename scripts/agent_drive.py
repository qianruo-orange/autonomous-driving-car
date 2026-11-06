#!/usr/bin/env python3
"""Bounded plan executor for the on-board agent capability layer.

The language model never sits in the control loop. It produces a plan — a
short list of constant-speed segments — and this process turns the plan into
the 8 Hz primitive stream the vehicle actually consumes. Everything the model
cannot be trusted to do lives here: envelope checks, distance accounting,
lease refresh, and the stop call on every exit path.

    agent_drive.py plan.json --token $AGENT_CAP
    agent_drive.py --v 0.10 --steer 0.0 --ms 1500 --token $AGENT_CAP

Exit codes: 0 completed, 2 plan or vehicle state refused before motion,
3 aborted at runtime, 4 transport failure.
"""
import argparse
import json
import os
import signal
import sys
import time
import urllib.error
import urllib.request

HZ = 8.0
TTL_MS = 500


class Refused(Exception):
    """The plan, or the vehicle state, forbids motion. Nothing was sent."""


class Client:
    def __init__(self, host, token):
        self.base = host.rstrip("/")
        self.token = token
        self.stopped = False

    def _req(self, method, path, form=None):
        data = None
        headers = {"X-Agent-Cap": self.token}
        if form is not None:
            data = "&".join(f"{k}={v}" for k, v in form.items()).encode()
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        req = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")
        except OSError as e:
            return 0, str(e)

    def observe(self):
        code, body = self._req("GET", "/api/agent/observe")
        if code != 200:
            return None, f"observe HTTP {code}: {body.strip()}"
        try:
            return json.loads(body), None
        except json.JSONDecodeError:
            return None, "observe returned non-JSON"

    def motion(self, v, steer, ttl_ms=TTL_MS):
        return self._req("POST", "/api/agent/motion",
                         {"v": f"{v:.4f}", "steer": f"{steer:.4f}", "ttl_ms": str(ttl_ms)})

    def stop(self):
        if self.stopped:
            return
        self.stopped = True
        self._req("POST", "/api/agent/stop", {})


def load_plan(args):
    if args.plan:
        try:
            with open(args.plan, encoding="utf-8") as f:
                plan = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            raise Refused(f"cannot read plan: {e}")
        segs = plan.get("segments", [])
    else:
        segs = [{"v": args.v, "steer": args.steer, "ms": args.ms}]
    out = []
    for i, s in enumerate(segs):
        try:
            v, steer, ms = float(s["v"]), float(s.get("steer", 0.0)), int(s["ms"])
        except (KeyError, TypeError, ValueError):
            raise Refused(f"segment {i}: expected numeric v, steer, ms")
        if ms <= 0 or ms > 10000:
            raise Refused(f"segment {i}: ms must be within 1..10000")
        out.append((v, steer, ms))
    if not out:
        raise Refused("plan has no segments")
    return out


def check_plan(segs, limits):
    """Refuse locally rather than let the server reject mid-run."""
    v_max = float(limits["v_max"])
    s_max = float(limits["steer_max"])
    budget = float(limits["budget_m"])
    total = 0.0
    for i, (v, steer, ms) in enumerate(segs):
        if v < 0 or v > v_max:
            raise Refused(f"segment {i}: v={v} outside credential envelope 0..{v_max}")
        if abs(steer) > s_max:
            raise Refused(f"segment {i}: steer={steer} outside credential envelope +/-{s_max}")
        total += v * ms / 1000.0
    if total > budget:
        raise Refused(f"plan needs {total:.2f} m but the credential has {budget:.2f} m left")
    return total


def preflight(state, dry_run):
    """Everything that must hold before the first command leaves this process."""
    if not state["agent"]["armed"]:
        raise Refused("credential is not armed; an operator must POST /api/agent/arm")
    if not state["profile"]["enabled"]:
        raise Refused("follow profile is disabled; an operator must enable it first")
    if not state["actuation"]["ready"] and not dry_run:
        blockers = {
            "motor_bound": state["actuation"]["motor_bound"],
            "can_enabled": state["actuation"]["can_enabled"],
        }
        raise Refused(f"actuation blocked: {blockers}")


def main():
    ap = argparse.ArgumentParser(description="Execute a bounded motion plan through /api/agent/motion")
    ap.add_argument("plan", nargs="?", help="plan JSON file; omit to build a one-segment plan from flags")
    ap.add_argument("--v", type=float, default=0.0, help="speed for the single-segment form, m/s")
    ap.add_argument("--steer", type=float, default=0.0, help="differential steering, m/s")
    ap.add_argument("--ms", type=int, default=0, help="duration for the single-segment form")
    ap.add_argument("--host", default="http://127.0.0.1")
    ap.add_argument("--token", default=os.environ.get("AGENT_CAP", ""))
    ap.add_argument("--dry-run", action="store_true", help="validate only, send nothing")
    args = ap.parse_args()

    if len(args.token) != 32:
        print(json.dumps({"error": "missing credential: pass --token or set AGENT_CAP"}))
        return 4

    c = Client(args.host, args.token)

    def on_signal(_sig, _frm):
        c.stop()
        sys.exit(3)

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    try:
        segs = load_plan(args)
        state, err = c.observe()
        if state is None:
            print(json.dumps({"error": err}), flush=True)
            return 4
        limits = state.get("limits")
        if not limits:
            print(json.dumps({"error": "server did not report credential limits"}), flush=True)
            return 4
        total = check_plan(segs, limits)
        preflight(state, args.dry_run)
    except Refused as e:
        print(json.dumps({"refused": str(e)}), flush=True)
        return 2

    print(json.dumps({"plan_meters": round(total, 3), "segments": len(segs),
                      "limits": limits, "dry_run": args.dry_run}), flush=True)
    if args.dry_run:
        return 0

    sent = 0
    started = time.monotonic()
    try:
        for idx, (v, steer, ms) in enumerate(segs):
            end = time.monotonic() + ms / 1000.0
            while time.monotonic() < end:
                code, body = c.motion(v, steer)
                if code != 200:
                    print(json.dumps({"abort": True, "segment": idx, "http": code,
                                      "body": body.strip()}), flush=True)
                    return 3
                sent += 1
                time.sleep(1.0 / HZ)
                # No status polling inside the segment: any fault shows up as a
                # rejected command, and a slow poll would itself starve the lease.
    finally:
        c.stop()
    print(json.dumps({"done": True, "commands": sent,
                      "seconds": round(time.monotonic() - started, 2),
                      "meters": round(total, 3)}), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
