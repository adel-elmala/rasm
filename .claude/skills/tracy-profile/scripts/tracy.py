"""Tracy CPU profiling helper for RASM (Windows / Linux / macOS).

Uses tracy-capture and tracy-csvexport built by `setup` into <repo>/build/tracy-tools
(same vcpkg port as the engine's client, so versions always match), else TRACY_TOOLS_DIR, else PATH.

  python tracy.py setup
  python tracy.py capture <exe> [--seconds S] [--out f.tracy] [--timeout S] [-- app args]
  python tracy.py zones   <f.tracy> [--from S] [--to S] [--sort total|mean|p95|max|count] [--top N] [--filter text] [--self]
  python tracy.py frames  <f.tracy> [--from S] [--to S] [--anchor zone] [--spikes N]
  python tracy.py compare <a.tracy> <b.tracy> [--from S] [--top N]

Times: --from/--to are seconds since capture start (skip loading with --from 3).
"""

import csv
import glob
import io
import os
import socket
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
SKILL = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
INSTALL_ROOT = os.path.join(REPO, "build", "tracy-tools")
DEFAULT_ANCHOR = "rasm::Engine::beginFrame"
PORT = 8086


def take_opt(args, name, default=None):
    if name in args:
        i = args.index(name)
        value = args[i + 1]
        del args[i:i + 2]
        return value
    return default


def take_flag(args, name):
    if name in args:
        args.remove(name)
        return True
    return False


def tool(name, required=True):
    exe = name + (".exe" if os.name == "nt" else "")
    dirs = glob.glob(os.path.join(INSTALL_ROOT, "*", "tools", "tracy"))
    if os.environ.get("TRACY_TOOLS_DIR"):
        dirs.insert(0, os.environ["TRACY_TOOLS_DIR"])
    dirs += os.environ.get("PATH", "").split(os.pathsep)
    for d in dirs:
        p = os.path.join(d, exe)
        if os.path.isfile(p):
            return p
    if required:
        sys.exit("error: %s not found. Run: python %s setup" % (exe, os.path.relpath(__file__, REPO)))
    return None


# ---------------------------------------------------------------- setup / capture

def cmd_setup(args):
    root = os.environ.get("VCPKG_ROOT")
    if not root:
        sys.exit("error: VCPKG_ROOT is not set")
    vcpkg = os.path.join(root, "vcpkg.exe" if os.name == "nt" else "vcpkg")
    cmd = [vcpkg, "install", "--x-manifest-root=" + os.path.join(SKILL, "tools"), "--x-install-root=" + INSTALL_ROOT]
    print(" ".join(cmd))
    if subprocess.call(cmd) != 0:
        sys.exit("error: vcpkg install failed")
    print("tracy tools: %s" % os.path.dirname(tool("tracy-capture")))


def port_busy():
    s = socket.socket()
    s.settimeout(0.3)
    try:
        return s.connect_ex(("127.0.0.1", PORT)) == 0
    finally:
        s.close()


def cmd_capture(args):
    app_args = []
    if "--" in args:
        i = args.index("--")
        app_args = args[i + 1:]
        args = args[:i]
    seconds = float(take_opt(args, "--seconds", "10"))
    timeout = float(take_opt(args, "--timeout", str(seconds + 120)))
    exe = os.path.abspath(args[0])
    out = os.path.abspath(take_opt(args, "--out", os.path.splitext(os.path.basename(exe))[0] + ".tracy"))
    if port_busy():
        sys.exit("error: something already listens on port %d (another Tracy-instrumented app?). Close it first." % PORT)
    cap = subprocess.Popen([tool("tracy-capture"), "-a", "127.0.0.1", "-p", str(PORT), "-o", out, "-f", "-s", "%g" % seconds],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    time.sleep(0.5)
    app = subprocess.Popen([exe] + app_args, cwd=os.path.dirname(exe), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        log, _ = cap.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        cap.kill()
        log, _ = cap.communicate()
        print(log[-2000:])
        sys.exit("error: capture did not finish in %g s" % timeout)
    finally:
        exit_code = app.poll()
        if exit_code is None:
            app.kill()
    summary = [l.strip() for l in log.replace("\r", "\n").splitlines() if l.strip().startswith(("Frames:", "Time span:", "Zones:", "Trace size"))]
    if not os.path.isfile(out):
        print(log[-2000:])
        sys.exit("error: no trace written")
    print("trace -> %s" % out)
    for l in summary:
        print("  " + l)
    if exit_code is not None:
        print("  note: app exited (code %s) before the capture ended, so the trace is shorter than asked" % exit_code)


# ---------------------------------------------------------------- loading

class Zone(object):
    __slots__ = ("name", "src", "start", "total", "self_ns", "thread")


def export_csv(trace, self_times):
    suffix = ".self.csv" if self_times else ".zones.csv"
    path = os.path.splitext(trace)[0] + suffix
    if not os.path.isfile(path) or os.path.getmtime(path) < os.path.getmtime(trace):
        cmd = [tool("tracy-csvexport"), "-u"] + (["-e"] if self_times else []) + [trace]
        data = subprocess.run(cmd, capture_output=True, check=True).stdout
        with open(path, "wb") as f:
            f.write(data)
    with open(path, encoding="utf-8", errors="replace") as f:
        return list(csv.DictReader(f))


def load_zones(trace):
    if not os.path.isfile(trace):
        sys.exit("error: %s not found" % trace)
    totals = export_csv(trace, False)
    selfs = export_csv(trace, True)
    zones = []
    for t, s in zip(totals, selfs):
        z = Zone()
        z.name = t["name"]
        z.src = "%s:%s" % (os.path.relpath(t["src_file"], REPO) if os.path.isabs(t["src_file"]) else t["src_file"], t["src_line"])
        z.start = int(t["ns_since_start"])
        z.total = int(t["exec_time_ns"])
        z.self_ns = int(s["exec_time_ns"])
        z.thread = t["thread"]
        zones.append(z)
    return zones


def window(zones, frm, to):
    lo = int(frm * 1e9)
    hi = int(to * 1e9) if to is not None else None
    return [z for z in zones if z.start >= lo and (hi is None or z.start < hi)]


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0
    k = min(len(sorted_vals) - 1, int(round(p / 100.0 * (len(sorted_vals) - 1))))
    return sorted_vals[k]


def ms(ns):
    return ns / 1e6


def frame_list(zones, anchor):
    starts = sorted(z.start for z in zones if z.name == anchor)
    return [(a, b - a) for a, b in zip(starts, starts[1:])]


def zone_stats(zones, use_self):
    by = {}
    for z in zones:
        by.setdefault(z.name, []).append(z)
    stats = {}
    for name, lst in by.items():
        vals = sorted(z.self_ns if use_self else z.total for z in lst)
        threads = sorted(set(z.thread for z in lst))
        stats[name] = {"count": len(vals), "total": sum(vals), "mean": sum(vals) / len(vals), "p95": pct(vals, 95),
                       "max": vals[-1], "src": lst[0].src, "threads": ",".join(threads)}
    return stats


# ---------------------------------------------------------------- analysis commands

def cmd_zones(args):
    frm = float(take_opt(args, "--from", "0"))
    to = take_opt(args, "--to")
    to = float(to) if to else None
    sort = take_opt(args, "--sort", "total")
    top = int(take_opt(args, "--top", "30"))
    flt = (take_opt(args, "--filter", "") or "").lower()
    use_self = take_flag(args, "--self")
    zones = window(load_zones(args[0]), frm, to)
    if not zones:
        sys.exit("error: no zones in that time window")
    frames = frame_list(zones, DEFAULT_ANCHOR)
    span = (max(z.start + z.total for z in zones) - min(z.start for z in zones))
    stats = zone_stats(zones, use_self)
    rows = sorted(((n, s) for n, s in stats.items() if flt in n.lower()), key=lambda r: -r[1][sort])[:top]
    print("%s times, window %.2fs..%s, %d frames, span %.2fs" % ("self" if use_self else "inclusive", frm, "%.2fs" % to if to else "end", len(frames), span / 1e9))
    print("%-42s %7s %10s %8s %9s %9s %9s %6s  %s" % ("zone", "count", "total ms", "% span", "mean ms", "p95 ms", "max ms", "thr", "source"))
    for n, s in rows:
        print("%-42s %7d %10.2f %8.2f %9.3f %9.3f %9.3f %6s  %s" % (n[-42:], s["count"], ms(s["total"]), 100.0 * s["total"] / span, ms(s["mean"]), ms(s["p95"]), ms(s["max"]), s["threads"], s["src"]))


def cmd_frames(args):
    frm = float(take_opt(args, "--from", "0"))
    to = take_opt(args, "--to")
    to = float(to) if to else None
    anchor = take_opt(args, "--anchor", DEFAULT_ANCHOR)
    nspikes = int(take_opt(args, "--spikes", "5"))
    all_zones = load_zones(args[0])
    zones = window(all_zones, frm, to)
    frames = frame_list(zones, anchor)
    if not frames:
        sys.exit("error: no '%s' zones in the window (use --anchor)" % anchor)
    durs = sorted(d for _, d in frames)
    mean = sum(durs) / len(durs)
    print("frames %d (from successive '%s' starts), window %.2fs..%s" % (len(frames), anchor, frm, "%.2fs" % to if to else "end"))
    print("  mean %.3f ms (%.1f fps)  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f  min %.3f" % (ms(mean), 1e9 / mean, ms(pct(durs, 50)), ms(pct(durs, 95)), ms(pct(durs, 99)), ms(durs[-1]), ms(durs[0])))
    over = [d for d in durs if d > 2 * pct(durs, 50)]
    print("  frames > 2x median: %d" % len(over))
    anchor_thread = next(z.thread for z in zones if z.name == anchor)
    print("slowest frames (self time of zones inside, thread %s):" % anchor_thread)
    for start, dur in sorted(frames, key=lambda f: -f[1])[:nspikes]:
        inside = [z for z in all_zones if start <= z.start < start + dur and z.thread == anchor_thread]
        agg = {}
        for z in inside:
            agg[z.name] = agg.get(z.name, 0) + z.self_ns
        top = sorted(agg.items(), key=lambda kv: -kv[1])[:4]
        covered = sum(agg.values())
        print("  @%.3fs  %.3f ms: %s | untracked %.3f ms" % (start / 1e9, ms(dur), ", ".join("%s %.3f" % (n.split("::")[-1], ms(v)) for n, v in top), ms(max(0, dur - covered))))


def cmd_compare(args):
    frm = float(take_opt(args, "--from", "0"))
    top = int(take_opt(args, "--top", "25"))
    a = window(load_zones(args[0]), frm, None)
    b = window(load_zones(args[1]), frm, None)
    fa, fb = frame_list(a, DEFAULT_ANCHOR), frame_list(b, DEFAULT_ANCHOR)
    if not fa or not fb:
        sys.exit("error: no frames in one of the traces")
    for label, fr in (("A", fa), ("B", fb)):
        d = sorted(x for _, x in fr)
        print("%s frames %d  mean %.3f ms  p50 %.3f  p95 %.3f  max %.3f" % (label, len(d), ms(sum(d) / len(d)), ms(pct(d, 50)), ms(pct(d, 95)), ms(d[-1])))
    sa, sb = zone_stats(a, True), zone_stats(b, True)
    rows = []
    for n in set(sa) | set(sb):
        pa = sa[n]["total"] / len(fa) if n in sa else 0.0
        pb = sb[n]["total"] / len(fb) if n in sb else 0.0
        rows.append((n, pa, pb))
    rows.sort(key=lambda r: -abs(r[2] - r[1]))
    print("self time per frame (ms), sorted by change:")
    print("%-42s %10s %10s %10s %8s" % ("zone", "A", "B", "B-A", "%"))
    for n, pa, pb in rows[:top]:
        change = ("%+.1f" % (100.0 * (pb - pa) / pa)) if pa else "new"
        print("%-42s %10.4f %10.4f %+10.4f %8s" % (n[-42:], ms(pa), ms(pb), ms(pb - pa), change))


COMMANDS = {"setup": cmd_setup, "capture": cmd_capture, "zones": cmd_zones, "frames": cmd_frames, "compare": cmd_compare}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(0 if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help") else 2)
    COMMANDS[sys.argv[1]](sys.argv[2:])
