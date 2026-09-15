#!/usr/bin/env python3
#
# wheel_cal.py - remote measurement and calibration for the Speed Meter.
#
# Talks to the firmware over BLE using MCUmgr/SMP (shell group). The device
# must run the diagnostics build (see diag.conf). On macOS the client uses
# smpmgr with native BLE (bleak) - no cable needed.
#
# Setup:
#   export PYTHONPATH="$TMPDIR/pylib312"
#
# Examples:
#   tools/wheel_cal.py scan
#   tools/wheel_cal.py status
#   tools/wheel_cal.py capture --seconds 10 -o ride1.csv
#   tools/wheel_cal.py analyze ride1.csv --axes xy --circ 2100
#   tools/wheel_cal.py cal-set amp_mg 200 step_mrad 1600
#   tools/wheel_cal.py live
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import csv
import math
import os
import re
import subprocess
import sys
import time

DEFAULT_UUID_FILE = "/tmp/board_uuid.txt"
DEFAULT_SMP_PYTHON = "/opt/homebrew/bin/python3.12"
MAX_CHUNK = 128
LINE_RE = re.compile(r"^(\d+),(-?\d+),(-?\d+),(-?\d+)$")
KV_RE = re.compile(r"([A-Za-z_]+)=(-?\d+)")


class ClientError(RuntimeError):
    pass


# ------------------------------------------------------------------ client

def resolve_uuid(args):
    if args.uuid:
        return args.uuid
    if os.path.exists(DEFAULT_UUID_FILE):
        with open(DEFAULT_UUID_FILE, encoding="utf-8") as fh:
            value = fh.read().strip()
        if value:
            return value
    raise ClientError(
        f"no BLE UUID: pass --uuid or write it to {DEFAULT_UUID_FILE}")


def smp(uuid, command, timeout=15, python=DEFAULT_SMP_PYTHON):
    """Run one shell command on the device over MCUmgr/SMP and return its output."""
    argv = [python, "-m", "smpmgr", "--ble", uuid, "--timeout", str(timeout),
            "shell", command]
    try:
        res = subprocess.run(argv, capture_output=True, text=True,
                             timeout=timeout + 25, check=False)
    except FileNotFoundError as exc:
        raise ClientError(f"cannot run {python}: {exc}") from exc
    output = ((res.stdout or "") + (res.stderr or "")).replace("\r", "")
    if res.returncode != 0:
        raise ClientError(output.strip() or f"smpmgr rc={res.returncode}")
    return output


def parse_kv(text):
    return {key: int(value) for key, value in KV_RE.findall(text)}


def fetch_dump(uuid, start, count, chunk, timeout):
    """Fetch recorded samples as (t_us, ax_mg, ay_mg, az_mg) tuples."""
    rows = []
    offset = start
    remaining = count
    chunk = min(chunk, MAX_CHUNK)

    while remaining > 0:
        take = min(chunk, remaining)
        out = smp(uuid, f"wheel record dump {offset} {take}", timeout)
        got = 0
        for line in out.splitlines():
            match = LINE_RE.match(line.strip())
            if match:
                rows.append(tuple(int(group) for group in match.groups()))
                got += 1
        if got == 0 or got < take:
            break
        offset += got
        remaining -= got

    return rows


def write_csv(path, rows, meta):
    with open(path, "w", newline="", encoding="utf-8") as fh:
        for key, value in meta.items():
            fh.write(f"# {key}={value}\n")
        writer = csv.writer(fh)
        writer.writerow(["t_us", "ax_mg", "ay_mg", "az_mg"])
        writer.writerows(rows)


# --------------------------------------------------------------- commands

def cmd_scan(args):
    script = (
        "import asyncio\n"
        "from bleak import BleakScanner\n"
        "async def m():\n"
        "    found = await BleakScanner.discover(timeout=6, return_adv=True)\n"
        "    for dev, adv in found.values():\n"
        "        uu = ' '.join(adv.service_uuids or [])\n"
        "        if (adv.local_name or '') or 'febb' in uu:\n"
        "            print(dev.address, repr(adv.local_name), adv.rssi, uu)\n"
        "asyncio.run(m())\n"
    )
    res = subprocess.run([args.smp_python, "-c", script],
                         capture_output=True, text=True, timeout=30, check=False)
    print((res.stdout or "") + (res.stderr or ""), end="")


def cmd_status(args, uuid):
    print(smp(uuid, "wheel status", args.timeout), end="")


def cmd_live(args, uuid):
    try:
        while True:
            text = smp(uuid, "wheel status", args.timeout)
            line = next((l.strip() for l in text.splitlines()
                         if l.strip().startswith("wheel ")), "")
            print(time.strftime("%H:%M:%S"), line, flush=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass


def cmd_cal_get(args, uuid):
    which = f" {args.key}" if args.key else ""
    print(smp(uuid, f"wheel cal get{which}", args.timeout), end="")


def cmd_cal_set(args, uuid):
    if len(args.pairs) % 2 != 0:
        raise ClientError("cal-set needs KEY VALUE pairs")
    for key, value in zip(args.pairs[::2], args.pairs[1::2]):
        print(smp(uuid, f"wheel cal set {key} {value}", args.timeout), end="")


def cmd_capture(args, uuid):
    detect = " 1" if args.detect else ""
    print(smp(uuid, f"wheel record start {args.odr} {args.range_g}{detect}",
              args.timeout), end="")

    deadline = time.time() + args.seconds
    while time.time() < deadline:
        time.sleep(min(2.0, max(0.0, deadline - time.time())))

    print(smp(uuid, "wheel record stop", args.timeout), end="")

    info = parse_kv(smp(uuid, "wheel record info", args.timeout))
    count = int(info.get("len", 0))
    print(f"fetching {count} samples (chunks of {min(args.chunk, MAX_CHUNK)})...")

    rows = fetch_dump(uuid, 0, count, args.chunk, args.timeout)
    meta = {
        "tool": "wheel_cal.py capture",
        "captured": time.strftime("%Y-%m-%d %H:%M:%S"),
        "odr_hz": args.odr,
        "range_g": args.range_g,
        "samples": len(rows),
    }
    write_csv(args.output, rows, meta)
    print(f"saved {len(rows)} samples to {args.output}")

    if args.detect:
        for line in smp(uuid, "wheel status", args.timeout).splitlines():
            if line.strip().startswith("det:"):
                print(line.strip())


def cmd_dump(args, uuid):
    info = parse_kv(smp(uuid, "wheel record info", args.timeout))
    total = int(info.get("len", 0))
    start = args.start
    count = args.count if args.count else max(0, total - start)
    rows = fetch_dump(uuid, start, count, args.chunk, args.timeout)
    meta = {
        "tool": "wheel_cal.py dump",
        "captured": time.strftime("%Y-%m-%d %H:%M:%S"),
        "odr_hz": info.get("odr_hz", "?"),
        "range_g": info.get("range_g", "?"),
        "samples": len(rows),
    }
    write_csv(args.output, rows, meta)
    print(f"saved {len(rows)} samples to {args.output}")


# ---------------------------------------------------------------- analysis

def read_csv(path):
    meta, rows = {}, []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("#"):
                key, _, value = line[1:].strip().partition("=")
                meta[key] = value
            elif line and not line[0].isalpha():
                parts = line.split(",")
                if len(parts) == 4:
                    try:
                        rows.append(tuple(int(x) for x in parts))
                    except ValueError:
                        pass
    return meta, rows


def pctl(values, p):
    if not values:
        return float("nan")
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, round(p * (len(ordered) - 1))))
    return ordered[idx]


def run_detector(rows, axes, alpha, amp_gate, max_step, rpm_min, rpm_max,
                 lp_hz=20.0):
    axis_index = {"x": 0, "y": 1, "z": 2}
    ia, ib = axis_index[axes[0]], axis_index[axes[1]]

    center_a = rows[0][1 + ia] / 1000.0
    center_b = rows[0][1 + ib] / 1000.0

    prev_phase = None
    accumulated = 0.0
    revolutions = 0
    revs_pos = revs_neg = 0
    last_rev_t = None
    periods = []
    rejected = 0
    mags = []
    steps_abs = []
    skipped = 0
    trace = []

    t_prev = rows[0][0]
    t_acc = 0.0
    lp_a = lp_b = None

    for row in rows:
        delta = (row[0] - t_prev) & 0xFFFFFFFF
        t_prev = row[0]
        dt = delta / 1e6
        t_acc += dt
        t = t_acc

        a = row[1 + ia] / 1000.0
        b = row[1 + ib] / 1000.0

        if lp_hz > 0.0 and dt > 0.0:
            rc = 1.0 / (2.0 * math.pi * lp_hz)
            k = dt / (rc + dt)
            lp_a = a if lp_a is None else lp_a + k * (a - lp_a)
            lp_b = b if lp_b is None else lp_b + k * (b - lp_b)
            fa, fb = lp_a, lp_b
        else:
            fa, fb = a, b

        center_a += alpha * (fa - center_a)
        center_b += alpha * (fb - center_b)

        va, vb = fa - center_a, fb - center_b
        mag = math.hypot(va, vb)
        mags.append(mag)

        if mag < amp_gate:
            prev_phase = None
            accumulated = 0.0
            skipped += 1
            continue

        phase = math.atan2(vb, va)

        if prev_phase is None:
            prev_phase = phase
            trace.append((t, phase, revolutions))
            continue

        step = phase - prev_phase
        prev_phase = phase
        if step > math.pi:
            step -= 2 * math.pi
        if step < -math.pi:
            step += 2 * math.pi
        steps_abs.append(abs(step))

        if abs(step) > max_step:
            accumulated = 0.0
            trace.append((t, phase, revolutions))
            continue

        accumulated += step
        if abs(accumulated) >= 2 * math.pi:
            direction = 1.0 if accumulated > 0 else -1.0
            accumulated -= direction * 2 * math.pi
            revolutions += 1
            if direction > 0:
                revs_pos += 1
            else:
                revs_neg += 1
            if last_rev_t is not None:
                period = t - last_rev_t
                rpm = 60.0 / period if period > 0 else float("inf")
                if rpm_min <= rpm <= rpm_max:
                    periods.append(period)
                else:
                    rejected += 1
            last_rev_t = t

        trace.append((t, phase, revolutions))

    return {
        "n": len(rows), "duration": t_acc,
        "mags": mags, "steps_abs": steps_abs, "skipped": skipped,
        "revolutions": revolutions, "revs_pos": revs_pos, "revs_neg": revs_neg,
        "periods": periods, "rejected": rejected, "trace": trace,
    }


def score_axes(res):
    """Rate one axis pair's detector result: more clean revolutions, a low
    period spread (steady speed) and smooth phase steps score higher."""
    revs = res["revolutions"]
    periods = res["periods"]
    steps = res["steps_abs"]
    mags = res["mags"]
    mag_p50 = pctl(mags, 0.50) if mags else 0.0
    step_p90 = pctl(steps, 0.90) if steps else float("inf")

    if revs == 0 or not periods:
        return {"revs": revs, "rejected": res["rejected"], "period_cv": float("inf"),
                "step_p90": step_p90, "mag_p50": mag_p50, "score": -1.0}

    mean_p = sum(periods) / len(periods)
    variance = sum((p - mean_p) ** 2 for p in periods) / len(periods)
    period_cv = (variance ** 0.5) / mean_p if mean_p > 0 else float("inf")
    rejected_ratio = res["rejected"] / max(1, revs + res["rejected"])

    score = revs - 5.0 * rejected_ratio * revs - 10.0 * period_cv - 2.0 * step_p90
    return {"revs": revs, "rejected": res["rejected"], "period_cv": period_cv,
            "step_p90": step_p90, "mag_p50": mag_p50, "score": score}


def cmd_analyze(args):
    meta, rows = read_csv(args.file)
    if len(rows) < 10:
        raise ClientError(f"not enough samples in {args.file} ({len(rows)})")

    res = run_detector(rows, args.axes, args.alpha, args.amp_gate,
                       args.max_step, args.rpm_min, args.rpm_max,
                       lp_hz=args.lp_hz)

    n = res["n"]
    duration = max(res["duration"], 1e-9)
    print(f"samples: {n}  duration: {duration:.2f} s  (~{n / duration:.1f} Hz)")
    print(f"filter: lpf={args.lp_hz:g} Hz, center alpha={args.alpha:g}")

    if "odr_hz" in meta:
        print(f"recorder: odr_hz={meta['odr_hz']} range_g={meta.get('range_g', '?')}")

    mags = res["mags"]
    print(f"|rot vector| [g]: p10={pctl(mags, 0.10):.3f} p50={pctl(mags, 0.50):.3f} "
          f"p90={pctl(mags, 0.90):.3f}")
    print(f"|phase step| [rad]: p50={pctl(res['steps_abs'], 0.50):.3f} "
          f"p90={pctl(res['steps_abs'], 0.90):.3f} max={pctl(res['steps_abs'], 1.0):.3f}")
    print(f"low-amplitude samples skipped: {res['skipped']}")
    print(f"revolutions: {res['revolutions']} (forward={res['revs_pos']} "
          f"reverse={res['revs_neg']}, rpm-rejected={res['rejected']})")

    periods = res["periods"]
    if periods:
        rpms = [60.0 / p for p in periods]
        speeds = [args.circ / 1000.0 / p * 3.6 for p in periods]
        print(f"period [s]: mean={sum(periods) / len(periods):.4f} "
              f"min={min(periods):.4f} max={max(periods):.4f}")
        print(f"rpm: mean={sum(rpms) / len(rpms):.1f} min={min(rpms):.1f} "
              f"max={max(rpms):.1f}")
        print(f"speed [km/h] (circ={args.circ:.0f} mm): "
              f"mean={sum(speeds) / len(speeds):.1f} min={min(speeds):.1f} "
              f"max={max(speeds):.1f}")

    suggested_amp = max(50, round(pctl(mags, 0.10) * 1000 * 0.5))
    steps = res["steps_abs"]
    suggested_step = (min(3141, max(400, round(pctl(steps, 0.99) * 1000 * 1.2)))
                      if steps else 1600)
    suggested_rpm_max = round(60.0 / min(periods) * 1.2) if periods else 1200
    print("suggested (rough - verify on more data):")
    print(f"  amp_mg={suggested_amp}  step_mrad={suggested_step}  "
          f"rpm_max={suggested_rpm_max}  rpm_min=12")

    phase_path = args.file + ".phase.csv"
    with open(phase_path, "w", encoding="utf-8") as fh:
        fh.write("t_s,phase_rad,revolutions\n")
        for t, phase, revs in res["trace"]:
            fh.write(f"{t:.6f},{phase:.6f},{revs}\n")
    print(f"phase trace written to {phase_path}")


def cmd_autocal(args, uuid):
    """Record a calibration spin, auto-pick the best axis pair (xy/xz/yz) and
    write the resulting calibration to the device. Keep the wheel spinning at
    a roughly steady speed for --seconds."""
    print(f"recording {args.seconds:.0f}s at odr={args.odr}Hz range={args.range_g}g "
          "- keep the wheel spinning steadily...")
    print(smp(uuid, f"wheel record start {args.odr} {args.range_g}", args.timeout),
          end="")

    deadline = time.time() + args.seconds
    while time.time() < deadline:
        time.sleep(min(2.0, max(0.0, deadline - time.time())))
    print(smp(uuid, "wheel record stop", args.timeout), end="")

    info = parse_kv(smp(uuid, "wheel record info", args.timeout))
    count = int(info.get("len", 0))
    print(f"fetching {count} samples...")
    rows = fetch_dump(uuid, 0, count, args.chunk, args.timeout)
    if len(rows) < 20:
        raise ClientError(f"only {len(rows)} samples captured; spin the wheel "
                          "longer or check the mount")

    meta = {
        "tool": "wheel_cal.py autocal",
        "captured": time.strftime("%Y-%m-%d %H:%M:%S"),
        "odr_hz": args.odr,
        "range_g": args.range_g,
        "samples": len(rows),
    }
    write_csv(args.output, rows, meta)
    print(f"saved {len(rows)} samples to {args.output}")

    print(f"\n{'axes':<5} {'revs':>5} {'reject':>6} {'period_cv':>9} "
          f"{'step_p90':>8} {'mag_p50':>7} {'score':>7}")
    best_axes, best_metrics, best_res = None, None, None
    for axes in ("xy", "xz", "yz"):
        res = run_detector(rows, axes, args.alpha, args.amp_gate, args.max_step,
                           args.rpm_min, args.rpm_max, lp_hz=args.lp_hz)
        metrics = score_axes(res)
        print(f"{axes:<5} {metrics['revs']:>5} {metrics['rejected']:>6} "
              f"{metrics['period_cv']:>9.3f} {metrics['step_p90']:>8.3f} "
              f"{metrics['mag_p50']:>7.3f} {metrics['score']:>7.2f}")
        if best_metrics is None or metrics["score"] > best_metrics["score"]:
            best_axes, best_metrics, best_res = axes, metrics, res

    if best_metrics["revs"] == 0:
        raise ClientError("no axis pair produced a clean rotation - recapture "
                          "with the wheel actually spinning")

    mags = best_res["mags"]
    steps = best_res["steps_abs"]
    periods = best_res["periods"]
    amp_mg = max(50, round(pctl(mags, 0.10) * 1000 * 0.5))
    step_mrad = (min(3141, max(400, round(pctl(steps, 0.99) * 1000 * 1.2)))
                if steps else 1600)
    rpm_max = round(60.0 / min(periods) * 1.2) if periods else 1200

    print(f"\nbest axes: {best_axes} (score={best_metrics['score']:.2f}, "
          f"{best_metrics['revs']} clean revolutions)")
    print(f"writing calibration: axes={best_axes} amp_mg={amp_mg} "
          f"step_mrad={step_mrad} rpm_max={rpm_max} odr_hz={args.odr} "
          f"range_g={args.range_g}")

    for key, value in (("axes", best_axes), ("amp_mg", amp_mg),
                      ("step_mrad", step_mrad), ("rpm_max", rpm_max),
                      ("odr_hz", args.odr), ("range_g", args.range_g)):
        smp(uuid, f"wheel cal set {key} {value}", args.timeout)

    print("done - verify with: tools/wheel_cal.py capture --detect --seconds 10")


# -------------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(
        description="Remote measurement & calibration for the Speed Meter "
                    "(MCUmgr/SMP over BLE; diagnostics firmware build).")
    parser.add_argument("--uuid", help="BLE UUID (default: contents of "
                                       f"{DEFAULT_UUID_FILE})")
    parser.add_argument("--timeout", type=int, default=15,
                        help="smpmgr per-command timeout in seconds")
    parser.add_argument("--smp-python", default=DEFAULT_SMP_PYTHON,
                        help="Python interpreter with smpmgr installed")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("scan", help="scan BLE devices (name / SMP UUID)")

    sub.add_parser("status", help="show device status and calibration")

    live = sub.add_parser("live", help="poll status once per interval")
    live.add_argument("--interval", type=float, default=1.0)

    cal_get = sub.add_parser("cal-get", help="read calibration parameters")
    cal_get.add_argument("key", nargs="?", help="single key (default: all)")

    cal_set = sub.add_parser("cal-set", help="set calibration parameters")
    cal_set.add_argument("pairs", nargs="+", metavar="KEY VALUE")

    capture = sub.add_parser("capture",
                             help="record on the device, fetch and save to CSV")
    capture.add_argument("--seconds", type=float, default=10.0)
    capture.add_argument("--odr", type=int, default=100)
    capture.add_argument("--range-g", type=int, default=2, dest="range_g")
    capture.add_argument("--chunk", type=int, default=64)
    capture.add_argument("--detect", action="store_true",
                         help="run the on-device wheel detector while recording")
    capture.add_argument("-o", "--output", default="wheel_capture.csv")

    dump = sub.add_parser("dump", help="fetch the current device buffer to CSV")
    dump.add_argument("--start", type=int, default=0)
    dump.add_argument("--count", type=int, default=0, help="0 = all")
    dump.add_argument("--chunk", type=int, default=64)
    dump.add_argument("-o", "--output", default="wheel_dump.csv")

    autocal = sub.add_parser("autocal",
                             help="record a calibration spin, auto-pick the "
                                  "best axes and write calibration to the device")
    autocal.add_argument("--seconds", type=float, default=15.0)
    autocal.add_argument("--odr", type=int, default=100)
    autocal.add_argument("--range-g", type=int, default=16, dest="range_g")
    autocal.add_argument("--chunk", type=int, default=64)
    autocal.add_argument("--alpha", type=float, default=0.005)
    autocal.add_argument("--lp-hz", type=float, default=20.0, dest="lp_hz")
    autocal.add_argument("--amp-gate", type=float, default=0.15, dest="amp_gate")
    autocal.add_argument("--max-step", type=float, default=1.6, dest="max_step")
    autocal.add_argument("--rpm-min", type=float, default=12.0, dest="rpm_min")
    autocal.add_argument("--rpm-max", type=float, default=1200.0, dest="rpm_max")
    autocal.add_argument("-o", "--output", default="autocal_capture.csv")

    analyze = sub.add_parser("analyze",
                             help="offline phase analysis of a captured CSV")
    analyze.add_argument("file")
    analyze.add_argument("--axes", default="xy", choices=["xy", "xz", "yz"])
    analyze.add_argument("--alpha", type=float, default=0.005,
                         help="gravity-center filter alpha (default 0.005)")
    analyze.add_argument("--lp-hz", type=float, default=20.0, dest="lp_hz",
                         help="low-pass filter cutoff in Hz before atan2 "
                              "(0 disables; default 20)")
    analyze.add_argument("--amp-gate", type=float, default=0.15, dest="amp_gate",
                         help="rotating-vector gate in g (default 0.15)")
    analyze.add_argument("--max-step", type=float, default=1.6, dest="max_step",
                         help="max plausible phase step in rad (default 1.6)")
    analyze.add_argument("--rpm-min", type=float, default=12.0, dest="rpm_min")
    analyze.add_argument("--rpm-max", type=float, default=1200.0, dest="rpm_max")
    analyze.add_argument("--circ", type=float, default=2100.0,
                         help="wheel circumference in mm (for speed)")

    args = parser.parse_args()

    try:
        if args.cmd == "scan":
            cmd_scan(args)
            return

        if args.cmd == "analyze":
            cmd_analyze(args)
            return

        uuid = resolve_uuid(args)

        if args.cmd == "status":
            cmd_status(args, uuid)
        elif args.cmd == "live":
            cmd_live(args, uuid)
        elif args.cmd == "cal-get":
            cmd_cal_get(args, uuid)
        elif args.cmd == "cal-set":
            cmd_cal_set(args, uuid)
        elif args.cmd == "capture":
            cmd_capture(args, uuid)
        elif args.cmd == "dump":
            cmd_dump(args, uuid)
        elif args.cmd == "autocal":
            cmd_autocal(args, uuid)
    except ClientError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
