"""Offline Vulkan reference from the installed SDK: valid-usage rules, API definitions, flag
decoding and what this machine's GPU supports.

  python vkref.py vuid   <VUID-...> [more]          rule text for one or more VUIDs (prefix match ok)
  python vkref.py rules  <vkCmdFoo|VkFooInfo> [--grep word]   every valid-usage rule of a command/struct
  python vkref.py search <word> [word ...]          rules whose text contains all words
  python vkref.py def    <vkCmdFoo|VkFoo|VkFooFlagBits>       definition from vk.xml + who provides it
  python vkref.py flags  <VkFooFlags[2]|VkFooFlagBits[2]> <value>   decode a bitmask value (0x.. or decimal)
  python vkref.py device [query ...] [--gpu N] [--refresh]    this GPU: name/API/driver, then matching
                                                    extensions, features, properties/limits, formats

Data: $VULKAN_SDK/share/vulkan/registry/{validusage.json,vk.xml} and `vulkaninfo --json`
(cached in build/vkref/).
"""

import glob
import html
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
SPEC_URL = "https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#"


def sdk_root():
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk:
        sys.exit("error: VULKAN_SDK is not set")
    return sdk


def registry_file(name):
    for p in (os.path.join(sdk_root(), "share", "vulkan", "registry", name), os.path.join(sdk_root(), "..", "share", "vulkan", "registry", name)):
        if os.path.isfile(p):
            return p
    sys.exit("error: %s not found under %s" % (name, sdk_root()))


def plain(text):
    text = re.sub(r"<code>(.*?)</code>", r"`\1`", text)
    text = re.sub(r"<strong[^>]*>(.*?)</strong>", r"\1", text)
    text = re.sub(r"<a [^>]*>(.*?)</a>", r"\1", text)
    text = re.sub(r"<li>", "\n      - ", text)
    text = re.sub(r"<[^>]+>", "", text)
    return html.unescape(text).strip()


# ---------------------------------------------------------------- valid usage

def load_vu():
    with open(registry_file("validusage.json"), encoding="utf-8") as f:
        return json.load(f)["validation"]


def all_rules(vu):
    for owner, groups in vu.items():
        for group, rules in groups.items():
            for r in rules:
                yield owner, group, r


def print_rule(owner, group, r):
    cond = "" if group == "core" else "  [when %s]" % group
    print("%s%s\n    %s\n    %s%s" % (r["vuid"], cond, plain(r["text"]), SPEC_URL, r["vuid"]))


def cmd_vuid(args):
    vu = load_vu()
    found = 0
    for owner, group, r in all_rules(vu):
        if any(r["vuid"] == a or (r["vuid"].startswith(a) and not a.endswith(tuple("0123456789"))) for a in args):
            print_rule(owner, group, r)
            found += 1
    if not found:
        print("no VUID matches %s. Best-practices (BestPractices-*), sync (SYNC-*) and UNASSIGNED-* ids are layer checks, not spec rules: read the layer message itself." % " ".join(args))


def cmd_rules(args):
    grep = None
    if "--grep" in args:
        i = args.index("--grep")
        grep = args[i + 1].lower()
        del args[i:i + 2]
    vu = load_vu()
    name = args[0]
    if name not in vu:
        close = [k for k in vu if name.lower() in k.lower()][:15]
        sys.exit("no rules for %s. Close: %s" % (name, ", ".join(close)))
    n = 0
    for group, rules in vu[name].items():
        for r in rules:
            if grep and grep not in plain(r["text"]).lower():
                continue
            print_rule(name, group, r)
            n += 1
    print("(%d rules)" % n)


def cmd_search(args):
    words = [w.lower() for w in args]
    n = 0
    for owner, group, r in all_rules(load_vu()):
        t = plain(r["text"]).lower()
        if all(w in t for w in words):
            print_rule(owner, group, r)
            n += 1
            if n >= 40:
                print("... (first 40 shown, add words to narrow)")
                return
    print("(%d rules)" % n)


# ---------------------------------------------------------------- vk.xml

def load_xml():
    return ET.parse(registry_file("vk.xml")).getroot()


def is_vulkan(el):
    api = el.get("api")
    return api is None or "vulkan" in api.split(",")


def decl(el):
    parts = []
    for node in el.iter():
        if node.tag == "comment":
            continue
        if node is el:
            parts.append(node.text or "")
        else:
            parts.append((node.text or "") + (node.tail or ""))
    return re.sub(r"\s+", " ", "".join(parts)).strip()


def providers(root, name):
    out = []
    for feat in root.findall("feature"):
        if not is_vulkan(feat):
            continue
        for req in feat.findall("require"):
            if any(c.get("name") == name for c in req):
                out.append(feat.get("name"))
    for ext in root.find("extensions"):
        if "vulkan" not in (ext.get("supported") or "").split(","):
            continue
        for req in ext.findall("require"):
            if any(c.get("name") == name for c in req):
                out.append(ext.get("name") + (" (promoted to %s)" % ext.get("promotedto") if ext.get("promotedto") else ""))
    return sorted(set(out))


def enum_values(root, enum_name):
    vals = []
    block = next((e for e in root.findall("enums") if e.get("name") == enum_name), None)
    if block is not None:
        for e in block.findall("enum"):
            if not is_vulkan(e):
                continue
            vals.append((e.get("name"), e.get("value"), e.get("bitpos"), e.get("alias")))
    for parent in list(root.findall("feature")) + list(root.find("extensions")):
        if not is_vulkan(parent) and parent.tag == "feature":
            continue
        if parent.tag == "extension" and "vulkan" not in (parent.get("supported") or "").split(","):
            continue
        for req in parent.findall("require"):
            for e in req.findall("enum"):
                if e.get("extends") != enum_name or not is_vulkan(e):
                    continue
                value = e.get("value")
                if e.get("offset") is not None:
                    ext_number = int(e.get("extnumber") or parent.get("number") or 0)
                    v = 1000000000 + (ext_number - 1) * 1000 + int(e.get("offset"))
                    value = str(-v if e.get("dir") == "-" else v)
                vals.append((e.get("name"), value, e.get("bitpos"), e.get("alias")))
    seen, out = set(), []
    for v in vals:
        if v[0] not in seen:
            seen.add(v[0])
            out.append(v)
    return out


def bits_type_for(root, name):
    if name.endswith("FlagBits") or name.endswith("FlagBits2") or "FlagBits" in name:
        return name
    for t in root.find("types").findall("type"):
        if t.get("category") == "bitmask" and (t.get("name") == name or t.findtext("name") == name):
            return t.get("requires") or t.get("bitvalues")
    return None


def cmd_def(args):
    root = load_xml()
    name = args[0]
    for t in root.find("types").findall("type"):
        tname = t.get("name") or t.findtext("name")
        if tname != name or not is_vulkan(t):
            continue
        if t.get("alias"):
            print("%s is an alias of %s" % (name, t.get("alias")))
            name = t.get("alias")
            return cmd_def([name])
        cat = t.get("category")
        print("%s %s" % (cat, name))
        if cat in ("struct", "union"):
            if t.get("structextends"):
                print("  extends (pNext of): %s" % t.get("structextends"))
            for m in t.findall("member"):
                if not is_vulkan(m):
                    continue
                notes = []
                if m.get("optional"):
                    notes.append("optional")
                if m.get("len"):
                    notes.append("len=" + m.get("len"))
                if m.get("values"):
                    notes.append("= " + m.get("values"))
                print("  %-60s %s" % (decl(m) + ";", ("// " + ", ".join(notes)) if notes else ""))
        elif cat == "enum":
            for n, v, b, a in enum_values(root, name):
                print("  %-70s %s" % (n, ("alias of " + a) if a else ("bit %s" % b) if b is not None else v))
        elif cat == "bitmask":
            print("  " + decl(t))
            bits = bits_type_for(root, name)
            if bits:
                print("  bits: %s (python vkref.py def %s)" % (bits, bits))
        else:
            print("  " + decl(t))
        print("provided by: %s" % (", ".join(providers(root, name)) or "?"))
        return
    for c in root.find("commands"):
        if c.get("name") == name and c.get("alias"):
            print("%s is an alias of %s" % (name, c.get("alias")))
            return cmd_def([c.get("alias")])
        proto = c.find("proto")
        if proto is None or proto.findtext("name") != name or not is_vulkan(c):
            continue
        params = [decl(p) for p in c.findall("param") if is_vulkan(p)]
        print("%s(\n    %s);" % (decl(proto), ",\n    ".join(params)))
        if c.get("queues"):
            print("queues: %s  render pass: %s  cmdbuffer level: %s" % (c.get("queues"), c.get("renderpass"), c.get("cmdbufferlevel")))
        print("provided by: %s" % (", ".join(providers(root, name)) or "?"))
        print("rules: python vkref.py rules %s" % name)
        return
    vals = enum_values(root, name)
    if vals:
        print("enum %s" % name)
        for n, v, b, a in vals:
            print("  %-70s %s" % (n, ("alias of " + a) if a else ("bit %s" % b) if b is not None else v))
        return
    sys.exit("no type, command or enum named %s in vk.xml" % name)


def cmd_flags(args):
    root = load_xml()
    bits_name = bits_type_for(root, args[0])
    if not bits_name:
        sys.exit("%s is not a bitmask type" % args[0])
    value = int(args[1], 0)
    names, rest = [], value
    for n, v, b, a in enum_values(root, bits_name):
        if a or b is None:
            continue
        bit = 1 << int(b)
        if value & bit:
            names.append(n)
            rest &= ~bit
    print("%s 0x%x = %s%s" % (bits_name, value, " | ".join(names) or "0", ("  (unknown bits 0x%x)" % rest) if rest else ""))


# ---------------------------------------------------------------- device

def vulkaninfo_json(gpu, refresh):
    out_dir = os.path.join(REPO, "build", "vkref")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "vulkaninfo_gpu%d.json" % gpu)
    if refresh or not os.path.isfile(path):
        exe = None
        for d in (os.path.join(sdk_root(), "Bin"), os.path.join(sdk_root(), "bin")):
            for n in ("vulkaninfoSDK.exe", "vulkaninfo.exe", "vulkaninfo"):
                if os.path.isfile(os.path.join(d, n)):
                    exe = os.path.join(d, n)
        exe = exe or "vulkaninfo"
        subprocess.run([exe, "--json=%d" % gpu, "-o", path], capture_output=True)
        if not os.path.isfile(path):
            sys.exit("error: vulkaninfo produced no JSON (gpu %d)" % gpu)
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def flatten(prefix, obj, out):
    if isinstance(obj, dict):
        for k, v in obj.items():
            flatten(prefix + "." + k if prefix else k, v, out)
    else:
        out.append((prefix, obj))


def cmd_device(args):
    gpu = 0
    if "--gpu" in args:
        i = args.index("--gpu")
        gpu = int(args[i + 1])
        del args[i:i + 2]
    refresh = "--refresh" in args
    args = [a for a in args if a != "--refresh"]
    dev = vulkaninfo_json(gpu, refresh)["capabilities"]["device"]
    props = dev["properties"]["VkPhysicalDeviceProperties"]
    api = props["apiVersion"]
    print("%s  API %d.%d.%d  driver 0x%x  vendor 0x%x" % (props["deviceName"], api >> 22, (api >> 12) & 0x3ff, api & 0xfff, props["driverVersion"], props["vendorID"]))
    if not args:
        print("query examples: device scalarBlockLayout | device maxPushConstantsSize | device VK_EXT_mesh_shader | device R16G16B16A16_SFLOAT")
        return
    rows = []
    for ext, ver in dev["extensions"].items():
        rows.append(("extension", ext, "spec %s" % ver))
    for section, label in (("features", "feature"), ("properties", "property")):
        for struct_name, body in dev[section].items():
            flat = []
            flatten("", body, flat)
            for k, v in flat:
                rows.append((label, "%s.%s" % (struct_name, k), v))
    for fmt, body in dev.get("formats", {}).items():
        fp = body.get("VkFormatProperties") or body
        for tiling in ("optimalTilingFeatures", "linearTilingFeatures", "bufferFeatures"):
            feats = [f.replace("VK_FORMAT_FEATURE_", "").replace("_BIT", "") for f in fp.get(tiling, [])]
            rows.append(("format", "%s.%s" % (fmt, tiling), ", ".join(feats) or "-"))
    words = [a.lower() for a in args]
    hits = [r for r in rows if all(w in r[1].lower() for w in words)]
    for kind, key, value in hits[:80]:
        print("  %-9s %-75s %s" % (kind, key, value))
    if len(hits) > 80:
        print("  ... %d more, narrow the query" % (len(hits) - 80))
    if not hits:
        print("  no match: not supported/exposed on this GPU, or a different name (try one word)")


COMMANDS = {"vuid": cmd_vuid, "rules": cmd_rules, "search": cmd_search, "def": cmd_def, "flags": cmd_flags, "device": cmd_device}

if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS or (len(sys.argv) < 3 and sys.argv[1] != "device"):
        print(__doc__)
        sys.exit(0 if len(sys.argv) < 2 else 2)
    COMMANDS[sys.argv[1]](sys.argv[2:])
