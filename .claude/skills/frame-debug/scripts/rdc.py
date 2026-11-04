"""RenderDoc capture + analysis from the command line (Windows / Linux).

Run with a normal Python 3. The script re-launches itself inside qrenderdoc,
which embeds the `renderdoc` module, and prints what the inner run wrote.

  python rdc.py capture <exe> [--frame N] [--out file.rdc] [--workdir D] [--timeout S] [-- app args]
  python rdc.py actions   <rdc> [--all]
  python rdc.py resources <rdc> [filter]
  python rdc.py state     <rdc> <eid>
  python rdc.py targets   <rdc> <eid> <outdir>
  python rdc.py texture   <rdc> <name|id> <out.png> [--mip N] [--slice N]
  python rdc.py buffer    <rdc> <name|id|0xGPUADDR> [--offset B] [--size B] [--format u32|i32|f32|u64|hex|"u32,u32,f32x3"] [--rows N]
  python rdc.py pixel     <rdc> <x> <y> [--eid E] [--target I]
  python rdc.py disasm    <rdc> <eid> <vs|ps|cs|ms|ts>

qrenderdoc is found via RENDERDOC_DIR, then the default install dir, then PATH.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import traceback

ARGS_ENV = "RASM_RDC_ARGS"
OUT_ENV = "RASM_RDC_OUT"


# ---------------------------------------------------------------- outer (system python)

def find_qrenderdoc():
    names = ["qrenderdoc.exe", "qrenderdoc"] if os.name == "nt" else ["qrenderdoc"]
    dirs = []
    if os.environ.get("RENDERDOC_DIR"):
        dirs.append(os.environ["RENDERDOC_DIR"])
    if os.name == "nt":
        dirs.append(r"C:\Program Files\RenderDoc")
    dirs += os.environ.get("PATH", "").split(os.pathsep)
    for d in dirs:
        for n in names:
            p = os.path.join(d, n)
            if os.path.isfile(p):
                return p
    return None


def run_outer(argv):
    qrd = find_qrenderdoc()
    if not qrd:
        print("error: qrenderdoc not found. Install RenderDoc or set RENDERDOC_DIR.")
        return 2
    fd, out_path = tempfile.mkstemp(suffix=".txt", prefix="rdc_")
    os.close(fd)
    env = dict(os.environ)
    env[ARGS_ENV] = json.dumps(argv)
    env[OUT_ENV] = out_path
    timeout = 600
    try:
        subprocess.run([qrd, "--python", os.path.abspath(__file__)], env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        print("error: qrenderdoc did not finish in %d s" % timeout)
    text = open(out_path, encoding="utf-8", errors="replace").read()
    os.remove(out_path)
    sys.stdout.write(text)
    return 1 if ("\nerror:" in "\n" + text or "Traceback" in text) else 0


# ---------------------------------------------------------------- inner (inside qrenderdoc)

OUT = []


def emit(*parts):
    OUT.append(" ".join(str(p) for p in parts))


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


def open_capture(rd, path):
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(path, "", None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open %s: %s" % (path, result))
    if cap.LocalReplaySupport() != rd.ReplaySupport.Supported:
        raise RuntimeError("capture can't be replayed on this machine")
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay %s: %s" % (path, result))
    return cap, ctrl


def rid_int(rid):
    try:
        return int(rid)
    except Exception:
        s = str(rid)
        return int(s.split("::")[-1]) if "::" in s else -1


def name_map(ctrl):
    return {rid_int(r.resourceId): r.name for r in ctrl.GetResources()}


def res_label(names, rid):
    i = rid_int(rid)
    if i <= 0:
        return "-"
    return "%s (id %d)" % (names.get(i, "?"), i)


def find_resource(ctrl, key):
    names = name_map(ctrl)
    key_l = key.lower()
    textures = {rid_int(t.resourceId): t for t in ctrl.GetTextures()}
    buffers = {rid_int(b.resourceId): b for b in ctrl.GetBuffers()}
    matches = []
    for i, n in names.items():
        if (key.isdigit() and i == int(key)) or n.lower() == key_l:
            matches = [i]
            break
        if key_l in n.lower():
            matches.append(i)
    matches = [i for i in matches if i in textures or i in buffers]
    if not matches:
        raise RuntimeError("no texture/buffer matches '%s' (try: resources)" % key)
    if len(matches) > 1:
        raise RuntimeError("'%s' is ambiguous: %s" % (key, ", ".join(res_label(names, i) for i in matches[:20])))
    i = matches[0]
    return textures.get(i), buffers.get(i), names.get(i, "?")


def resolve_address(ctrl, addr):
    for b in ctrl.GetBuffers():
        base = getattr(b, "gpuAddress", 0)
        if base and base <= addr < base + b.length:
            return b, addr - base
    return None, 0


def describe_address(ctrl, names, addr):
    b, off = resolve_address(ctrl, addr)
    if b is None:
        return "0x%x (no buffer)" % addr
    return "0x%x = %s + %d" % (addr, res_label(names, b.resourceId), off)


def action_name(ctrl, a):
    try:
        return a.GetName(ctrl.GetStructuredFile())
    except Exception:
        return getattr(a, "name", "?")


def walk_actions(actions, depth=0):
    for a in actions:
        yield a, depth
        for c in walk_actions(a.children, depth + 1):
            yield c


def flag_names(rd, a):
    F = rd.ActionFlags
    out = []
    for n in ["Drawcall", "Dispatch", "MeshDispatch", "Indirect", "Clear", "Copy", "Resolve", "Present", "PushMarker", "BeginPass", "EndPass", "PassBoundary"]:
        f = getattr(F, n, None)
        if f is not None and (a.flags & f):
            out.append(n)
    return out


def last_draw_eid(rd, ctrl):
    eid = 0
    for a, _ in walk_actions(ctrl.GetRootActions()):
        if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
            eid = a.eventId
    return eid


def desc_rid(d):
    for attr in ("resource", "resourceId"):
        if hasattr(d, attr):
            return getattr(d, attr)
    return None


def is_depth_format(tex):
    n = tex.format.Name().upper()
    return n.startswith("D16") or n.startswith("D24") or n.startswith("D32") or "DEPTH" in n


def save_texture(rd, ctrl, tex, path, mip=0, slice_index=0):
    ts = rd.TextureSave()
    ts.resourceId = tex.resourceId
    ts.mip = mip
    ts.slice.sliceIndex = slice_index
    ts.alpha = rd.AlphaMapping.Discard
    ts.destType = rd.FileType.PNG
    if is_depth_format(tex):
        ts.channelExtract = 0
        sub = rd.Subresource(mip, slice_index, 0)
        lo, hi = ctrl.GetMinMax(tex.resourceId, sub, rd.CompType.Typeless)
        ts.comp.blackPoint = lo.floatValue[0]
        ts.comp.whitePoint = hi.floatValue[0] if hi.floatValue[0] > lo.floatValue[0] else lo.floatValue[0] + 1e-6
        emit("  depth range remapped: %g .. %g" % (ts.comp.blackPoint, ts.comp.whitePoint))
    ctrl.SaveTexture(ts, path)


# -- commands

def cmd_capture(rd, args):
    app_args = []
    if "--" in args:
        i = args.index("--")
        app_args = args[i + 1:]
        args = args[:i]
    frame = int(take_opt(args, "--frame", "60"))
    out = take_opt(args, "--out")
    workdir = take_opt(args, "--workdir", "")
    timeout = float(take_opt(args, "--timeout", "180"))
    exe = os.path.abspath(args[0])
    template = os.path.splitext(out)[0] if out else os.path.join(tempfile.gettempdir(), "rasm_capture")
    opts = rd.CaptureOptions()
    opts.refAllResources = True
    cmdline = subprocess.list2cmdline(app_args) if os.name == "nt" else " ".join(app_args)
    res = rd.ExecuteAndInject(exe, workdir or os.path.dirname(exe), cmdline, [], template, opts, False)
    if res.result != rd.ResultCode.Succeeded:
        raise RuntimeError("inject failed: %s" % res.result)
    tc = None
    start = time.time()
    while tc is None and time.time() - start < 20:
        tc = rd.CreateTargetControl("", res.ident, "rasm-frame-debug", True)
        if tc is None:
            time.sleep(0.25)
    if tc is None:
        raise RuntimeError("could not connect to the target")
    pid = tc.GetPID()
    tc.QueueCapture(frame, 1)
    path = None
    while time.time() - start < timeout:
        msg = tc.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            path = msg.newCapture.path
            break
        if msg.type == rd.TargetControlMessageType.Disconnected:
            break
        time.sleep(0.05)
    tc.Shutdown()
    if pid:
        if os.name == "nt":
            subprocess.call(["taskkill", "/PID", str(pid), "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            try:
                os.kill(pid, 9)
            except OSError:
                pass
    if not path:
        raise RuntimeError("no capture: app exited or frame %d never came (timeout %gs)" % (frame, timeout))
    if out and os.path.abspath(path) != os.path.abspath(out):
        import shutil
        shutil.move(path, out)
        path = out
    emit("captured frame %d -> %s" % (frame, path))


def cmd_actions(rd, args):
    show_all = take_flag(args, "--all")
    cap, ctrl = open_capture(rd, args[0])
    names = name_map(ctrl)
    for a, depth in walk_actions(ctrl.GetRootActions()):
        flags = flag_names(rd, a)
        interesting = show_all or a.children or any(f in flags for f in ("Drawcall", "Dispatch", "MeshDispatch", "Clear", "Copy", "Present", "PushMarker", "BeginPass"))
        if not interesting:
            continue
        extra = []
        if "Drawcall" in flags:
            extra.append("idx=%d inst=%d" % (a.numIndices, a.numInstances))
        if "Dispatch" in flags:
            extra.append("groups=%s" % (tuple(a.dispatchDimension),))
        outs = [res_label(names, o) for o in a.outputs if rid_int(o) > 0]
        if outs:
            extra.append("out=" + ", ".join(outs))
        if rid_int(a.depthOut) > 0:
            extra.append("depth=" + res_label(names, a.depthOut))
        emit("%6d %s%s  [%s] %s" % (a.eventId, "  " * depth, action_name(ctrl, a), ",".join(flags), " ".join(extra)))
    ctrl.Shutdown()
    cap.Shutdown()


def cmd_resources(rd, args):
    flt = args[1].lower() if len(args) > 1 else ""
    cap, ctrl = open_capture(rd, args[0])
    names = name_map(ctrl)
    for t in ctrl.GetTextures():
        n = names.get(rid_int(t.resourceId), "?")
        if flt in n.lower():
            emit("tex %6d  %-40s %dx%dx%d mips=%d layers=%d %s" % (rid_int(t.resourceId), n, t.width, t.height, t.depth, t.mips, t.arraysize, t.format.Name()))
    for b in ctrl.GetBuffers():
        n = names.get(rid_int(b.resourceId), "?")
        if flt in n.lower():
            emit("buf %6d  %-40s %d bytes  addr=0x%x" % (rid_int(b.resourceId), n, b.length, getattr(b, "gpuAddress", 0)))
    ctrl.Shutdown()
    cap.Shutdown()


def cmd_state(rd, args):
    cap, ctrl = open_capture(rd, args[0])
    eid = int(args[1])
    names = name_map(ctrl)
    ctrl.SetFrameEvent(eid, True)
    pipe = ctrl.GetPipelineState()
    emit("event", eid)
    for label, stage in (("vs", rd.ShaderStage.Vertex), ("ts", rd.ShaderStage.Task), ("ms", rd.ShaderStage.Mesh), ("ps", rd.ShaderStage.Pixel), ("cs", rd.ShaderStage.Compute)):
        sid = pipe.GetShader(stage)
        if rid_int(sid) <= 0:
            continue
        emit("shader %s: %s entry=%s" % (label, res_label(names, sid), pipe.GetShaderEntryPoint(stage)))
        for kind, fn in (("ro", "GetReadOnlyResources"), ("rw", "GetReadWriteResources"), ("cb", "GetConstantBlocks")):
            try:
                used = getattr(pipe, fn)(stage, True)
            except Exception as e:
                emit("  %s: (%s)" % (kind, e))
                continue
            for u in used[:64]:
                d = getattr(u, "descriptor", u)
                acc = getattr(u, "access", None)
                where = ""
                if acc is not None:
                    where = "idx=%s elem=%s" % (getattr(acc, "index", "?"), getattr(acc, "arrayElement", "?"))
                rid = desc_rid(d)
                emit("  %s %s %s" % (kind, where, res_label(names, rid) if rid is not None else "-"))
            if len(used) > 64:
                emit("  %s ... %d more" % (kind, len(used) - 64))
    try:
        for i, t in enumerate(pipe.GetOutputTargets()):
            rid = desc_rid(t)
            if rid is not None and rid_int(rid) > 0:
                emit("color[%d]: %s" % (i, res_label(names, rid)))
        dt = desc_rid(pipe.GetDepthTarget())
        if dt is not None and rid_int(dt) > 0:
            emit("depth: %s" % res_label(names, dt))
    except Exception as e:
        emit("targets: (%s)" % e)
    try:
        vk = ctrl.GetVulkanPipelineState()
        r = vk.rasterizer
        emit("raster: cull=%s front=%s fill=%s depthClamp=%s" % (r.cullMode, "CCW" if r.frontCCW else "CW", r.fillMode, r.depthClampEnable))
        ds = vk.depthStencil
        emit("depth: test=%s write=%s func=%s bounds=%s" % (ds.depthTestEnable, ds.depthWriteEnable, ds.depthFunction, ds.depthBoundsEnable))
        for i, b in enumerate(vk.colorBlend.blends):
            emit("blend[%d]: enabled=%s color=%s %s %s alpha=%s %s %s mask=%s" % (i, b.enabled, b.colorBlend.source, b.colorBlend.operation, b.colorBlend.destination, b.alphaBlend.source, b.alphaBlend.operation, b.alphaBlend.destination, b.writeMask))
        for i, v in enumerate(vk.viewportScissor.viewportScissors):
            vp = v.vp
            emit("viewport[%d]: x=%g y=%g w=%g h=%g depth=%g..%g" % (i, vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth))
        pc = bytes(vk.pushconsts)
        if pc:
            emit("push constants (%d bytes) u32: %s" % (len(pc), list(struct.unpack("<%dI" % (len(pc) // 4), pc[:len(pc) // 4 * 4]))))
            for i in range(0, len(pc) // 8 * 8, 8):
                v = struct.unpack_from("<Q", pc, i)[0]
                if resolve_address(ctrl, v)[0] is not None:
                    emit("  push[%d] as address: %s" % (i, describe_address(ctrl, names, v)))
    except Exception as e:
        emit("vulkan state: (%s)" % e)
    ctrl.Shutdown()
    cap.Shutdown()


def cmd_targets(rd, args):
    cap, ctrl = open_capture(rd, args[0])
    eid = int(args[1])
    outdir = args[2]
    os.makedirs(outdir, exist_ok=True)
    names = name_map(ctrl)
    textures = {rid_int(t.resourceId): t for t in ctrl.GetTextures()}
    ctrl.SetFrameEvent(eid, True)
    pipe = ctrl.GetPipelineState()
    targets = [("color%d" % i, desc_rid(t)) for i, t in enumerate(pipe.GetOutputTargets())]
    targets.append(("depth", desc_rid(pipe.GetDepthTarget())))
    for label, rid in targets:
        if rid is None or rid_int(rid) not in textures:
            continue
        path = os.path.join(outdir, "eid%d_%s.png" % (eid, label))
        save_texture(rd, ctrl, textures[rid_int(rid)], path)
        emit("%s %s -> %s" % (label, res_label(names, rid), path))
    ctrl.Shutdown()
    cap.Shutdown()


def cmd_texture(rd, args):
    mip = int(take_opt(args, "--mip", "0"))
    slice_index = int(take_opt(args, "--slice", "0"))
    cap, ctrl = open_capture(rd, args[0])
    tex, buf, name = find_resource(ctrl, args[1])
    if tex is None:
        raise RuntimeError("'%s' is a buffer, use: buffer" % name)
    last = 0
    for a, _ in walk_actions(ctrl.GetRootActions()):
        last = a.eventId
    ctrl.SetFrameEvent(last, True)
    save_texture(rd, ctrl, tex, args[2], mip, slice_index)
    emit("%s %dx%d %s (end of frame) -> %s" % (name, tex.width, tex.height, tex.format.Name(), args[2]))
    ctrl.Shutdown()
    cap.Shutdown()


def parse_layout(fmt):
    sizes = {"u32": ("I", 4), "i32": ("i", 4), "f32": ("f", 4), "u64": ("Q", 8)}
    fields = []
    for part in fmt.split(","):
        part = part.strip()
        base, _, count = part.partition("x")
        if base not in sizes:
            raise RuntimeError("bad format field '%s' (use u32/i32/f32/u64, e.g. f32x3)" % part)
        fields += [sizes[base]] * (int(count) if count else 1)
    return fields


def cmd_buffer(rd, args):
    offset = int(take_opt(args, "--offset", "0"), 0)
    size = int(take_opt(args, "--size", "0"), 0)
    fmt = take_opt(args, "--format", "u32")
    rows = int(take_opt(args, "--rows", "64"))
    cap, ctrl = open_capture(rd, args[0])
    if args[1].lower().startswith("0x"):
        buf, base_off = resolve_address(ctrl, int(args[1], 16))
        if buf is None:
            raise RuntimeError("address %s is in no buffer" % args[1])
        offset += base_off
        name = name_map(ctrl).get(rid_int(buf.resourceId), "?")
    else:
        tex, buf, name = find_resource(ctrl, args[1])
        if buf is None:
            raise RuntimeError("'%s' is a texture, use: texture" % name)
    last = 0
    for a, _ in walk_actions(ctrl.GetRootActions()):
        last = a.eventId
    ctrl.SetFrameEvent(last, True)
    names = name_map(ctrl)
    data = bytes(ctrl.GetBufferData(buf.resourceId, offset, size))
    emit("%s: %d bytes total, showing %d from offset %d (end of frame)" % (name, buf.length, len(data), offset))
    if fmt == "hex":
        for i in range(0, min(len(data), rows * 16), 16):
            emit("%08x  %s" % (offset + i, data[i:i + 16].hex(" ") if sys.version_info >= (3, 8) else data[i:i + 16].hex()))
    else:
        fields = parse_layout(fmt)
        stride = sum(s for _, s in fields)
        n = min(len(data) // stride, rows)
        for r in range(n):
            pos = r * stride
            vals = []
            for code, s in fields:
                v = struct.unpack_from("<" + code, data, pos)[0]
                if code == "Q":
                    vals.append(describe_address(ctrl, names, v) if resolve_address(ctrl, v)[0] is not None else "0x%x" % v)
                else:
                    vals.append(("%g" % v) if code == "f" else str(v))
                pos += s
            emit("[%d] @%d  %s" % (r, offset + r * stride, "  ".join(vals)))
        if len(data) // stride > n:
            emit("... %d more rows (use --rows / --offset)" % (len(data) // stride - n))
    ctrl.Shutdown()
    cap.Shutdown()


def cmd_pixel(rd, args):
    eid = int(take_opt(args, "--eid", "0"))
    target = int(take_opt(args, "--target", "0"))
    cap, ctrl = open_capture(rd, args[0])
    x, y = int(args[1]), int(args[2])
    names = name_map(ctrl)
    if eid == 0:
        eid = last_draw_eid(rd, ctrl)
    ctrl.SetFrameEvent(eid, True)
    pipe = ctrl.GetPipelineState()
    outs = [desc_rid(t) for t in pipe.GetOutputTargets()]
    outs = [o for o in outs if o is not None and rid_int(o) > 0]
    if target >= len(outs):
        raise RuntimeError("event %d has %d color targets" % (eid, len(outs)))
    rid = outs[target]
    emit("pixel (%d,%d) of %s, history up to event %d" % (x, y, res_label(names, rid), eid))
    hist = ctrl.PixelHistory(rid, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
    for m in hist:
        if m.eventId > eid:
            continue
        why = [f for f in ("backfaceCulled", "depthClipped", "viewClipped", "scissorClipped", "shaderDiscarded", "depthTestFailed", "stencilTestFailed", "sampleMasked") if getattr(m, f, False)]
        pre = tuple(round(v, 4) for v in m.preMod.col.floatValue)
        post = tuple(round(v, 4) for v in m.postMod.col.floatValue)
        shader = tuple(round(v, 4) for v in m.shaderOut.col.floatValue)
        emit("eid %6d prim %s  %s  before=%s shader=%s after=%s depth %g->%g" % (m.eventId, m.primitiveID, "PASS" if m.Passed() else "FAIL(" + ",".join(why) + ")", pre, shader, post, m.preMod.depth, m.postMod.depth))
    ctrl.Shutdown()
    cap.Shutdown()


def cmd_disasm(rd, args):
    stages = {"vs": rd.ShaderStage.Vertex, "ps": rd.ShaderStage.Pixel, "cs": rd.ShaderStage.Compute, "ms": rd.ShaderStage.Mesh, "ts": rd.ShaderStage.Task}
    cap, ctrl = open_capture(rd, args[0])
    ctrl.SetFrameEvent(int(args[1]), True)
    pipe = ctrl.GetPipelineState()
    stage = stages[args[2]]
    refl = pipe.GetShaderReflection(stage)
    if refl is None:
        raise RuntimeError("no %s shader at event %s" % (args[2], args[1]))
    targets = ctrl.GetDisassemblyTargets(True)
    emit("targets:", ", ".join(targets))
    emit(ctrl.DisassembleShader(pipe.GetGraphicsPipelineObject() if stage != rd.ShaderStage.Compute else pipe.GetComputePipelineObject(), refl, targets[0]))
    ctrl.Shutdown()
    cap.Shutdown()


COMMANDS = {
    "capture": cmd_capture, "actions": cmd_actions, "resources": cmd_resources, "state": cmd_state,
    "targets": cmd_targets, "texture": cmd_texture, "buffer": cmd_buffer, "pixel": cmd_pixel, "disasm": cmd_disasm,
}
def run_inner():
    out_path = os.environ[OUT_ENV]
    try:
        import renderdoc as rd
        argv = json.loads(os.environ[ARGS_ENV])
        if not argv or argv[0] not in COMMANDS:
            raise RuntimeError("usage: rdc.py <%s> ..." % "|".join(COMMANDS))
        COMMANDS[argv[0]](rd, argv[1:])
    except RuntimeError as e:
        emit("error:", e)
    except Exception:
        emit("error:", traceback.format_exc())
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(OUT) + "\n")
    os._exit(0)


if os.environ.get(OUT_ENV) and os.environ.get(ARGS_ENV):
    run_inner()
elif __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0)
    sys.exit(run_outer(sys.argv[1:]))
