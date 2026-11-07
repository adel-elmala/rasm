"""Verify a RASM change: build, unit tests, then run every example N frames with validation,
collect validation messages, screenshot frame N and diff it against a baseline.

  python verify.py baseline [options]    run everything, save screenshots as the baseline
  python verify.py run      [options]    run everything, compare screenshots with the baseline
  python verify.py diff <a.png> <b.png> [--out diff.png] [--tolerance T]

options: --preset x64-debug  --examples 00_hello_scene,02_gltf  --frame 60  --tolerance 2
         --no-build  --no-tests  --keep-captures
         --sync (synchronization validation)  --best-practices  --gpu-av (GPU-assisted validation)

Output goes to build/verify/<preset>/ (screens/, diffs/, logs/, baseline/).
Screenshots come from GFXReconstruct (Vulkan SDK): its layer quits the app after frame N,
and gfxrecon-replay renders that frame to PNG. No engine change needed.
"""

import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
NOISE = ("loader_get_json: Failed to open JSON file", "env var 'VK_INSTANCE_LAYERS' defined")
# GFXR's frame snapshot records its own barriers with the original vkCmdPipelineBarrier; RASM only
# uses vkCmdPipelineBarrier2. Drop this filter if RASM ever calls the original.
GFXR_ARTIFACTS = ("vkCmdPipelineBarrier():",)


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


def sdk_tool(name):
    exe = name + (".exe" if os.name == "nt" else "")
    sdk = os.environ.get("VULKAN_SDK")
    dirs = [os.path.join(sdk, "Bin"), os.path.join(sdk, "bin")] if sdk else []
    for d in dirs + os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, exe)
        if os.path.isfile(p):
            return p
    sys.exit("error: %s not found (install the Vulkan SDK or set VULKAN_SDK)" % exe)


# ---------------------------------------------------------------- png (8-bit RGB/RGBA, no interlace)

def png_read(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError("%s: only 8-bit RGB/RGBA non-interlaced PNG supported" % path)
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * w * 3)
    prev = bytearray(stride)
    i = 0
    for y in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        row = y * w * 3
        if bpp == 3:
            out[row:row + w * 3] = line
        else:
            out[row:row + w * 3] = bytes(v for k, v in enumerate(line) if k % 4 != 3)
    return w, h, out


def png_write(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def image_diff(a_path, b_path, out_path, tolerance):
    wa, ha, a = png_read(a_path)
    wb, hb, b = png_read(b_path)
    if (wa, ha) != (wb, hb):
        return {"error": "size differs: %dx%d vs %dx%d" % (wa, ha, wb, hb)}
    changed, max_d, sum_d = 0, 0, 0
    x0, y0, x1, y1 = wa, ha, -1, -1
    out = bytearray(len(a))
    for p in range(wa * ha):
        k = p * 3
        d = max(abs(a[k] - b[k]), abs(a[k + 1] - b[k + 1]), abs(a[k + 2] - b[k + 2]))
        sum_d += d
        if d > max_d:
            max_d = d
        if d > tolerance:
            changed += 1
            x, y = p % wa, p // wa
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
            out[k], out[k + 1], out[k + 2] = 255, max(0, 160 - d), 0
        else:
            g = (a[k] + a[k + 1] + a[k + 2]) // 9
            out[k] = out[k + 1] = out[k + 2] = g
    if out_path and changed:
        png_write(out_path, wa, ha, out)
    return {"changed": changed, "percent": 100.0 * changed / (wa * ha), "max": max_d, "mean": sum_d / (wa * ha),
            "bbox": (x0, y0, x1, y1) if changed else None, "diff": out_path if changed else None}


# ---------------------------------------------------------------- build / test

def run_build(preset):
    proc = subprocess.run(["cmake", "--build", "--preset", preset], cwd=REPO, capture_output=True, text=True, errors="replace")
    text = proc.stdout + proc.stderr
    errors = [l for l in text.splitlines() if re.search(r"\berror\b", l, re.I) and "warning" not in l.lower()]
    warnings = sorted(set(l for l in text.splitlines() if "warning:" in l))
    return proc.returncode == 0, errors, warnings


def run_tests(preset):
    proc = subprocess.run(["ctest", "--preset", preset, "--output-on-failure"], cwd=REPO, capture_output=True, text=True, errors="replace")
    text = proc.stdout + proc.stderr
    summary = [l for l in text.splitlines() if "tests passed" in l or "tests failed" in l]
    failed = [l for l in text.splitlines() if "***Failed" in l or "ERROR:" in l or "FAILED" in l]
    return proc.returncode == 0, summary[-1] if summary else "no ctest summary", failed


# ---------------------------------------------------------------- examples

def example_names():
    text = open(os.path.join(REPO, "examples", "CMakeLists.txt")).read()
    return re.findall(r"rasm_add_example\(\s*([A-Za-z0-9_]+)\s*\)", text)


def parse_messages(log):
    """vk-bootstrap messenger blocks: '[SEVERITY: Type]' then message lines. Plus spdlog errors and asserts.
    Returns (messages, count of messages caused by the GFXR layer)."""
    msgs = []
    ignored = 0
    lines = log.splitlines()
    i = 0
    while i < len(lines):
        m = re.match(r"^\[(ERROR|WARNING|INFO|VERBOSE): ([A-Za-z]+)\](.*)$", lines[i].strip())
        if m:
            body = [m.group(3).strip(" -")] if m.group(3).strip(" -") else []
            i += 1
            while i < len(lines) and not re.match(r"^\[(ERROR|WARNING|INFO|VERBOSE): ", lines[i].strip()) and not lines[i].startswith("[20"):
                body.append(lines[i].strip())
                i += 1
            text = " ".join(body)
            if any(a in text for a in GFXR_ARTIFACTS):
                ignored += 1
            elif not any(n in text for n in NOISE) and m.group(1) in ("ERROR", "WARNING"):
                msgs.append((m.group(1), m.group(2), text))
            continue
        line = lines[i]
        if re.search(r"\] \[(error|critical)\]", line) or "Assertion failed" in line:
            msgs.append(("ERROR", "Engine", line.strip()))
        elif re.search(r"\] \[warning\]", line):
            msgs.append(("WARNING", "Engine", line.strip()))
        i += 1
    return msgs, ignored


def message_key(text):
    m = re.search(r"(VUID-[\w-]+|SYNC-[\w-]+|BestPractices-[\w-]+|UNASSIGNED-[\w-]+)", text)
    return m.group(0) if m else text[:120]


def run_example(name, preset, frame, out_dir, keep):
    exe = os.path.join(REPO, "build", preset, "examples", name + (".exe" if os.name == "nt" else ""))
    if not os.path.isfile(exe):
        return {"error": "not built: %s" % exe}
    cap_dir = os.path.join(out_dir, "captures")
    os.makedirs(cap_dir, exist_ok=True)
    base = os.path.join(cap_dir, name + ".gfxr")
    for old in glob.glob(os.path.join(cap_dir, name + "_frame_*.gfxr")):
        os.remove(old)
    env = dict(os.environ)
    env.update({"VK_INSTANCE_LAYERS": "VK_LAYER_LUNARG_gfxreconstruct", "GFXRECON_CAPTURE_FRAMES": str(frame),
                "GFXRECON_CAPTURE_FILE": base, "GFXRECON_CAPTURE_FILE_TIMESTAMP": "false", "GFXRECON_QUIT_AFTER_CAPTURE_FRAMES": "true"})
    sdk = os.environ.get("VULKAN_SDK")
    if os.name != "nt" and sdk and os.path.isdir(os.path.join(sdk, "share", "vulkan", "explicit_layer.d")):
        env["VK_ADD_LAYER_PATH"] = os.path.join(sdk, "share", "vulkan", "explicit_layer.d")
    try:
        proc = subprocess.run([exe, "--fixed-step"], cwd=os.path.dirname(exe), env=env, capture_output=True, text=True, errors="replace", timeout=180)
        log, code = proc.stdout + proc.stderr, proc.returncode
    except subprocess.TimeoutExpired as e:
        log, code = ((e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")), "timeout"
    os.makedirs(os.path.join(out_dir, "logs"), exist_ok=True)
    with open(os.path.join(out_dir, "logs", name + ".log"), "w", encoding="utf-8") as f:
        f.write(log)
    messages, ignored = parse_messages(log)
    result = {"exit": code, "messages": messages, "ignored": ignored, "screenshot": None}
    captures = glob.glob(os.path.join(cap_dir, name + "_frame_*.gfxr"))
    if not captures:
        result["error"] = "app exited (code %s) before frame %d" % (code, frame)
        return result
    shots = os.path.join(out_dir, "screens")
    os.makedirs(shots, exist_ok=True)
    tmp = os.path.join(shots, "_tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    subprocess.run([sdk_tool("gfxrecon-replay"), "--screenshot-all", "--screenshot-format", "png", "--screenshot-dir", tmp, captures[0]],
                   capture_output=True, text=True, errors="replace", timeout=300)
    pngs = sorted(glob.glob(os.path.join(tmp, "*.png")))
    if pngs:
        result["screenshot"] = os.path.join(shots, name + ".png")
        shutil.move(pngs[-1], result["screenshot"])
    else:
        result["error"] = "replay produced no screenshot"
    shutil.rmtree(tmp, ignore_errors=True)
    if not keep:
        for c in captures:
            os.remove(c)
    return result


# ---------------------------------------------------------------- commands

def cmd_check(args, make_baseline):
    preset = take_opt(args, "--preset", "x64-debug")
    only = take_opt(args, "--examples")
    frame = int(take_opt(args, "--frame", "60"))
    tolerance = int(take_opt(args, "--tolerance", "2"))
    no_build = take_flag(args, "--no-build")
    no_tests = take_flag(args, "--no-tests")
    keep = take_flag(args, "--keep-captures")
    layer_env = {}
    if take_flag(args, "--sync"):
        layer_env["VK_LAYER_VALIDATE_SYNC"] = "1"
    if take_flag(args, "--best-practices"):
        layer_env["VK_LAYER_VALIDATE_BEST_PRACTICES"] = "1"
    if take_flag(args, "--gpu-av"):
        layer_env["VK_LAYER_GPUAV_ENABLE"] = "1"
    os.environ.update(layer_env)
    out_dir = os.path.join(REPO, "build", "verify", preset)
    baseline_dir = os.path.join(out_dir, "baseline")
    failed = False

    if not no_build:
        ok, errors, warnings = run_build(preset)
        print("build   %s  (%d unique warnings)" % ("OK" if ok else "FAILED", len(warnings)))
        for l in (errors if not ok else [])[:20]:
            print("   " + l)
        for l in warnings[:10]:
            print("   " + l)
        if not ok:
            sys.exit(1)
    if not no_tests:
        ok, summary, fails = run_tests(preset)
        print("tests   %s  %s" % ("OK" if ok else "FAILED", summary))
        for l in fails[:20]:
            print("   " + l)
        failed |= not ok

    names = example_names()
    if only:
        names = [n for n in names if n in only.split(",")]
    print("examples at frame %d, preset %s:" % (frame, preset))
    for name in names:
        r = run_example(name, preset, frame, out_dir, keep)
        errs = [m for m in r.get("messages", []) if m[0] == "ERROR"]
        warns = [m for m in r.get("messages", []) if m[0] == "WARNING"]
        verdict = "OK"
        if r.get("error") or errs:
            verdict = "FAIL"
        elif warns:
            verdict = "WARN"
        detail = ""
        if r.get("error"):
            detail += "  | " + r["error"]
        if r.get("screenshot"):
            base = os.path.join(baseline_dir, name + ".png")
            if make_baseline:
                os.makedirs(baseline_dir, exist_ok=True)
                shutil.copy(r["screenshot"], base)
                detail += "  | baseline saved"
            elif os.path.isfile(base):
                os.makedirs(os.path.join(out_dir, "diffs"), exist_ok=True)
                d = image_diff(base, r["screenshot"], os.path.join(out_dir, "diffs", name + "_diff.png"), tolerance)
                if d.get("error"):
                    detail += "  | image: " + d["error"]
                    verdict = "FAIL"
                elif d["changed"]:
                    detail += "  | image CHANGED %.3f%% px, max diff %d, bbox %s" % (d["percent"], d["max"], d["bbox"])
                    if verdict != "FAIL":
                        verdict = "DIFF"
                else:
                    detail += "  | image same (max diff %d)" % d["max"]
            else:
                detail += "  | no baseline"
        print("  %-16s %-4s  validation/engine: %d errors, %d warnings%s" % (name, verdict, len(errs), len(warns), detail))
        grouped = {}
        for sev, kind, text in errs + warns:
            key = message_key(text)
            if key not in grouped:
                grouped[key] = [sev, kind, text, 0]
            grouped[key][3] += 1
        for sev, kind, text, count in grouped.values():
            print("      [%s %s] x%d %s" % (sev, kind, count, text[:400]))
        if r.get("ignored"):
            print("      (ignored %d validation messages about GFXR's own vkCmdPipelineBarrier calls)" % r["ignored"])
        failed |= verdict in ("FAIL", "DIFF")
    print("screens: %s" % os.path.join(out_dir, "screens"))
    if not make_baseline and os.path.isdir(os.path.join(out_dir, "diffs")):
        print("diffs:   %s" % os.path.join(out_dir, "diffs"))
    sys.exit(1 if failed else 0)


def cmd_diff(args):
    tolerance = int(take_opt(args, "--tolerance", "2"))
    out = take_opt(args, "--out")
    d = image_diff(args[0], args[1], out, tolerance)
    if d.get("error"):
        sys.exit("error: " + d["error"])
    print("changed %d px (%.3f%%), max diff %d, mean diff %.3f, bbox %s%s" % (d["changed"], d["percent"], d["max"], d["mean"], d["bbox"], ", diff -> " + d["diff"] if d["diff"] else ""))


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("run", "baseline", "diff"):
        print(__doc__)
        sys.exit(0 if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help") else 2)
    if sys.argv[1] == "diff":
        cmd_diff(sys.argv[2:])
    else:
        cmd_check(sys.argv[2:], sys.argv[1] == "baseline")
