"""GFXReconstruct capture + resource dump helper (Windows / Linux / macOS, Vulkan).

Uses the gfxrecon-* tools from the Vulkan SDK ($VULKAN_SDK/Bin or /bin, else PATH).

  python gfxr.py capture    <exe> [--frame N] [--out-dir D] [--timeout S] [-- app args]
  python gfxr.py frame      <file.gfxr>
  python gfxr.py dump       <file.gfxr> <outdir> [--draws i,j,...] [--before] [--vertex] [--no-descriptors]
  python gfxr.py screenshot <file.gfxr> <outdir>
"""

import glob
import json
import os
import subprocess
import sys

DRAW_PREFIX = "vkCmdDraw"
DISPATCH_PREFIX = "vkCmdDispatch"
TRACE_PREFIX = "vkCmdTraceRays"
PASS_BEGIN = ("vkCmdBeginRendering", "vkCmdBeginRenderingKHR", "vkCmdBeginRenderPass", "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR")
PASS_NEXT = ("vkCmdNextSubpass", "vkCmdNextSubpass2", "vkCmdNextSubpass2KHR")
PASS_END = ("vkCmdEndRendering", "vkCmdEndRenderingKHR", "vkCmdEndRenderPass", "vkCmdEndRenderPass2", "vkCmdEndRenderPass2KHR")


def tool(name):
    exe = name + (".exe" if os.name == "nt" else "")
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for sub in ("Bin", "bin"):
            p = os.path.join(sdk, sub, exe)
            if os.path.isfile(p):
                return p
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, exe)
        if os.path.isfile(p):
            return p
    sys.exit("error: %s not found. Install the Vulkan SDK (it ships GFXReconstruct) or set VULKAN_SDK." % exe)


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


# ---------------------------------------------------------------- capture

def cmd_capture(args):
    app_args = []
    if "--" in args:
        i = args.index("--")
        app_args = args[i + 1:]
        args = args[:i]
    frame = int(take_opt(args, "--frame", "60"))
    out_dir = os.path.abspath(take_opt(args, "--out-dir", "."))
    timeout = float(take_opt(args, "--timeout", "180"))
    exe = os.path.abspath(args[0])
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.join(out_dir, os.path.splitext(os.path.basename(exe))[0] + ".gfxr")
    env = dict(os.environ)
    env["VK_INSTANCE_LAYERS"] = "VK_LAYER_LUNARG_gfxreconstruct"
    env["GFXRECON_CAPTURE_FRAMES"] = str(frame)
    env["GFXRECON_CAPTURE_FILE"] = base
    env["GFXRECON_CAPTURE_FILE_TIMESTAMP"] = "false"
    env["GFXRECON_QUIT_AFTER_CAPTURE_FRAMES"] = "true"
    sdk = os.environ.get("VULKAN_SDK")
    if os.name != "nt" and sdk:
        layer_dir = os.path.join(sdk, "share", "vulkan", "explicit_layer.d")
        if os.path.isdir(layer_dir):
            env["VK_ADD_LAYER_PATH"] = layer_dir
    try:
        proc = subprocess.run([exe] + app_args, env=env, cwd=os.path.dirname(exe), timeout=timeout, capture_output=True, text=True, errors="replace")
        log = proc.stdout + proc.stderr
        code = proc.returncode
    except subprocess.TimeoutExpired:
        log, code = "", "timeout"
    found = sorted(glob.glob(os.path.splitext(base)[0] + "_frame_%d*.gfxr" % frame), key=os.path.getmtime)
    if not found:
        print(log[-3000:])
        sys.exit("error: no capture written (exit %s). Did the app reach frame %d?" % (code, frame))
    for line in log.splitlines():
        if "[gfxrecon]" in line and ("ERROR" in line or "FATAL" in line):
            print(line)
    print("captured frame %d -> %s (%.1f MB), app exit %s" % (frame, found[-1], os.path.getsize(found[-1]) / 1e6, code))


# ---------------------------------------------------------------- frame parsing

def load_frame(path):
    jsonl = os.path.splitext(path)[0] + ".jsonl"
    if not os.path.isfile(jsonl) or os.path.getmtime(jsonl) < os.path.getmtime(path):
        subprocess.run([tool("gfxrecon-convert"), "--format", "jsonl", "--output", jsonl, path], check=True, capture_output=True)
    cmds = []
    in_frame = False
    for line in open(jsonl, encoding="utf-8"):
        o = json.loads(line)
        st = o.get("state")
        if st and st.get("marker_type") == "EndMarker":
            in_frame = True
            continue
        if in_frame and "function" in o:
            cmds.append((o["index"], o["function"]["name"], o["function"].get("args") or {}))
    return cmds


def submitted_cbs(name, args):
    out = []
    for s in args.get("pSubmits") or []:
        if name.startswith("vkQueueSubmit2"):
            out += [c["commandBuffer"] for c in (s.get("pCommandBufferInfos") or [])]
        else:
            out += list(s.get("pCommandBuffers") or [])
    return out


def build_frame(cmds):
    """Command buffers in submit order: {bcb, submit, cb, cmds:[(idx,name,args)], passes:[[b,..,e]]}"""
    open_cbs = {}
    recorded = {}
    result = []
    for idx, name, args in cmds:
        cb = args.get("commandBuffer")
        if name == "vkBeginCommandBuffer":
            open_cbs[cb] = {"bcb": idx, "cb": cb, "cmds": [], "passes": [], "_pass": None}
        elif name == "vkEndCommandBuffer" and cb in open_cbs:
            recorded[cb] = open_cbs.pop(cb)
        elif name.startswith("vkCmd") and cb in open_cbs:
            rec = open_cbs[cb]
            rec["cmds"].append((idx, name, args))
            if name in PASS_BEGIN:
                rec["_pass"] = [idx]
            elif name in PASS_NEXT and rec["_pass"] is not None:
                rec["_pass"].append(idx)
            elif name in PASS_END and rec["_pass"] is not None:
                rec["_pass"].append(idx)
                rec["passes"].append(rec["_pass"])
                rec["_pass"] = None
        elif name.startswith("vkQueueSubmit"):
            for h in submitted_cbs(name, args):
                if h in recorded:
                    rec = dict(recorded[h])
                    rec["submit"] = idx
                    result.append(rec)
    return result


def short_args(name, args):
    keys = ("vertexCount", "indexCount", "instanceCount", "firstIndex", "vertexOffset", "firstInstance", "drawCount", "stride",
            "groupCountX", "groupCountY", "groupCountZ", "pipeline", "buffer", "offset", "size", "firstSet", "pDescriptorSets", "indexType")
    parts = ["%s=%s" % (k, args[k]) for k in keys if k in args]
    if name in PASS_BEGIN:
        info = args.get("pRenderingInfo") or args.get("pRenderPassBegin") or {}
        ext = (info.get("renderArea") or {}).get("extent") or {}
        parts.append("area=%sx%s" % (ext.get("width"), ext.get("height")))
        att = info.get("pColorAttachments") or []
        parts.append("colors=%d" % len(att))
        for a in att:
            parts.append("load=%s" % a.get("loadOp", "?").replace("VK_ATTACHMENT_LOAD_OP_", ""))
    return " ".join(parts)


def cmd_frame(args):
    frame = build_frame(load_frame(args[0]))
    if not frame:
        sys.exit("error: no submitted command buffers found in the captured frame")
    for rec in frame:
        print("command buffer %s  begin=%d  submit=%d" % (rec["cb"], rec["bcb"], rec["submit"]))
        depth = 0
        for idx, name, a in rec["cmds"]:
            if name in PASS_END:
                depth = max(0, depth - 1)
            mark = "*" if name.startswith((DRAW_PREFIX, DISPATCH_PREFIX, TRACE_PREFIX)) else " "
            print("  %s%6d %s%s %s" % (mark, idx, "  " * depth, name, short_args(name, a)))
            if name in PASS_BEGIN:
                depth += 1
    print("(* = draw/dispatch, dumpable with: gfxr.py dump <file> <outdir> --draws <index>)")


# ---------------------------------------------------------------- dump

def build_dump_config(frame, only, before, vertex, descriptors):
    cfg = {
        "DumpResourcesOptions": {
            "ImageFormat": "png",
            "DumpDepth": True,
            "DumpBeforeCommand": before,
            "DumpVertexIndexBuffer": vertex,
            "DumpAllDescriptors": descriptors,
        },
        "BeginCommandBuffer": [], "QueueSubmit": [], "RenderPass": [], "Draw": [], "Dispatch": [], "TraceRays": [],
    }
    for rec in frame:
        draws = [i for i, n, _ in rec["cmds"] if n.startswith(DRAW_PREFIX) and (not only or i in only)]
        dispatches = [i for i, n, _ in rec["cmds"] if n.startswith(DISPATCH_PREFIX) and (not only or i in only)]
        traces = [i for i, n, _ in rec["cmds"] if n.startswith(TRACE_PREFIX) and (not only or i in only)]
        if not (draws or dispatches or traces):
            continue
        cfg["BeginCommandBuffer"].append(rec["bcb"])
        cfg["QueueSubmit"].append(rec["submit"])
        cfg["RenderPass"].append([p for p in rec["passes"] if any(p[0] < d < p[-1] for d in draws)])
        cfg["Draw"].append(draws)
        cfg["Dispatch"].append(dispatches)
        cfg["TraceRays"].append(traces)
    return cfg


def files_of(entry):
    out = []
    for s in entry.get("subresources") or []:
        tag = "%s mip%s" % (s.get("aspect", ""), s.get("mipLevel", 0))
        if s.get("beforeFile"):
            out.append("%s before=%s" % (tag, s["beforeFile"]))
        if s.get("file"):
            out.append("%s after=%s" % (tag, s["file"]))
    if entry.get("file"):
        out.append(entry["file"])
    return out


def summarize_dump(path):
    data = json.load(open(path, encoding="utf-8"))
    for block in data:
        for kind in ("drawCallCommands", "dispatchCommands", "traceRaysCommands"):
            for cmd in block.get(kind, []):
                idx = cmd.get("drawIndex", cmd.get("dispatchIndex", cmd.get("traceRaysIndex")))
                p = dict(cmd.get("parameters") or {})
                ind = p.pop("indirectParams", None)
                print("== %s %s  %s" % (kind[:-8], idx, json.dumps(p)))
                if ind:
                    print("   indirect: " + "; ".join(",".join(str(v) for v in x.values()) for x in ind[:32]) + (" ..." if len(ind) > 32 else ""))
                for i, att in enumerate(cmd.get("colorAttachments") or []):
                    print("   color[%d] %s %s" % (i, att.get("format"), att.get("extent")))
                    for f in files_of(att):
                        print("      " + f)
                dep = cmd.get("depthAttachment")
                if dep:
                    print("   depth %s" % dep.get("format"))
                    for f in files_of(dep):
                        print("      " + f)
                for key in ("vertexBuffers", "indexBuffer"):
                    if cmd.get(key):
                        print("   %s: %s" % (key, json.dumps(cmd[key])[:300]))
                descs = cmd.get("descriptors") or {}
                for stage, lst in descs.items():
                    print("   descriptors %s: %d bound" % (stage, len(lst)))
                    for d in lst[:16]:
                        fs = files_of(d)
                        print("      set%s b%s [%s] %s %s %s" % (d.get("set"), d.get("binding"), d.get("arrayIndex"), d.get("type", "").replace("VK_DESCRIPTOR_TYPE_", ""), d.get("format", ""), fs[-1] if fs else ""))
                    if len(lst) > 16:
                        print("      ... %d more (see %s)" % (len(lst) - 16, path))


def cmd_dump(args):
    only = take_opt(args, "--draws")
    only = set(int(x) for x in only.split(",")) if only else set()
    before = take_flag(args, "--before")
    vertex = take_flag(args, "--vertex")
    descriptors = not take_flag(args, "--no-descriptors")
    capture, outdir = os.path.abspath(args[0]), os.path.abspath(args[1])
    os.makedirs(outdir, exist_ok=True)
    cfg = build_dump_config(build_frame(load_frame(capture)), only, before, vertex, descriptors)
    if not cfg["BeginCommandBuffer"]:
        sys.exit("error: none of the requested draws/dispatches are in the frame (see: gfxr.py frame)")
    cfg_path = os.path.join(outdir, "dump_config.json")
    json.dump(cfg, open(cfg_path, "w"), indent=1)
    proc = subprocess.run([tool("gfxrecon-replay"), "--dump-resources", cfg_path, "--dump-resources-dir", outdir, capture], capture_output=True, text=True, errors="replace")
    for line in (proc.stdout + proc.stderr).splitlines():
        if ("ERROR" in line or "FATAL" in line) and "loader_get_json" not in line:
            print(line)
    results = glob.glob(os.path.join(outdir, "*_dr.json"))
    if proc.returncode != 0 or not results:
        sys.exit("error: replay failed (exit %s), config at %s" % (proc.returncode, cfg_path))
    summarize_dump(max(results, key=os.path.getmtime))


def cmd_screenshot(args):
    capture, outdir = os.path.abspath(args[0]), os.path.abspath(args[1])
    os.makedirs(outdir, exist_ok=True)
    proc = subprocess.run([tool("gfxrecon-replay"), "--screenshot-all", "--screenshot-format", "png", "--screenshot-dir", outdir, capture], capture_output=True, text=True, errors="replace")
    shots = sorted(glob.glob(os.path.join(outdir, "screenshot_frame_*.png")), key=os.path.getmtime)
    if not shots:
        print((proc.stdout + proc.stderr)[-3000:])
        sys.exit("error: no screenshot written")
    print("screenshot -> %s" % shots[-1])


COMMANDS = {"capture": cmd_capture, "frame": cmd_frame, "dump": cmd_dump, "screenshot": cmd_screenshot}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(0 if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help") else 2)
    COMMANDS[sys.argv[1]](sys.argv[2:])
