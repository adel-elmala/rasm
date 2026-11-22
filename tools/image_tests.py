#!/usr/bin/env python3
"""Run RASM's golden-image tests (rasm_image_tests) on lavapipe, the driver their goldens come from.

    python tools/image_tests.py --lavapipe                      # run every test, print a table
    python tools/image_tests.py --lavapipe --update             # render new goldens into tests/golden/lavapipe
    python tools/image_tests.py --lavapipe --update --scenes sponza,taa

--lavapipe downloads (once) and runs a third-party Mesa build: lavapipe, Mesa's CPU Vulkan driver, from
mesa-dist-win 26.3.0, pinned by its SHA-256. Nothing is downloaded or run without that flag. It works on Windows
(mesa-dist-win is a Windows build), and needs 7-Zip (7z) or a tar that reads .7z archives on the PATH.

The script also fetches Sponza, which the sponza test draws, from a pinned commit of KhronosGroup/glTF-Sample-Assets,
checked by SHA-256, into build/test-assets/. Its license (CRYENGINE Limited License) does not allow it in the repository.

A failed test leaves actual.png, expected.png and diff.png in build/<preset>/image-tests/<test>/.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST_ASSETS = ROOT / "build" / "test-assets"

MESA_VERSION = "26.3.0"
MESA_URL = f"https://github.com/pal1000/mesa-dist-win/releases/download/{MESA_VERSION}/mesa3d-{MESA_VERSION}-release-msvc.7z"
MESA_SHA256 = "c505fcc77e8f63a09d7ddac086591d2f542c0698d37fc9e3094685a34d0d9991"
# lavapipe's settings that change pixels. The vector width is the JIT's SIMD width, which follows the CPU otherwise;
# the thread count did not change a pixel on the CI runner (docs/guides/image-tests.html).
LAVAPIPE_ENV = {"LP_NATIVE_VECTOR_WIDTH": "256"}

SPONZA_COMMIT = "e124e8d8d7330a5372bc2b4d00d39a4a9d7997ea"
SPONZA_URL = f"https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/{SPONZA_COMMIT}/Models/Sponza/glTF/"
SPONZA_GLTF_SHA256 = "646c10cbc8fab990ca29f363e90e2d65155f3a3569506852eb1434a9465b9501"
# The SHA-256 of the lines "<file> <sha256>" of every file Sponza.gltf names, sorted by file name.
SPONZA_FILES_SHA256 = "5565af32695eca2a9912d62b2ad6a6f8f85f8c28ae3769454c1df9af6b14d3db"


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + ".part")
    print(f"  downloading {url}")
    with urllib.request.urlopen(url) as response, open(partial, "wb") as out:
        shutil.copyfileobj(response, out)
    partial.replace(path)


def fetch_checked(url, path, expected):
    if not path.exists() or sha256(path) != expected:
        download(url, path)
    found = sha256(path)
    if found != expected:
        sys.exit(f"{path.name}: SHA-256 {found}, expected {expected}. Not used.")


def fetch_sponza():
    folder = TEST_ASSETS / "Sponza" / "glTF"
    gltf = folder / "Sponza.gltf"
    fetch_checked(SPONZA_URL + "Sponza.gltf", gltf, SPONZA_GLTF_SHA256)
    model = json.loads(gltf.read_text(encoding="utf-8"))
    names = sorted({b["uri"] for b in model.get("buffers", [])} | {i["uri"] for i in model.get("images", []) if "uri" in i})
    for name in names:
        if "/" in name or "\\" in name or name.startswith("."):
            sys.exit(f"Sponza.gltf names an unexpected file: {name}")
        if not (folder / name).exists():
            download(SPONZA_URL + name, folder / name)
    lines = "".join(f"{name} {sha256(folder / name)}\n" for name in names)
    found = hashlib.sha256(lines.encode()).hexdigest()
    if found != SPONZA_FILES_SHA256:
        sys.exit(f"Sponza's files hash to {found}, expected {SPONZA_FILES_SHA256}. Delete {folder} to fetch them again.")
    return gltf


def extract_7z(archive, folder):
    for tool in (["7z", "x", "-y", f"-o{folder}", str(archive)], ["tar", "-xf", str(archive), "-C", str(folder)]):
        if shutil.which(tool[0]) and subprocess.run(tool, stdout=subprocess.DEVNULL).returncode == 0:
            return
    sys.exit(f"Can't extract {archive}: install 7-Zip (7z on the PATH).")


def fetch_lavapipe():
    if os.name != "nt":
        sys.exit("--lavapipe uses mesa-dist-win, a Windows build. On another OS, point VK_DRIVER_FILES at a lavapipe of Mesa "
                 f"{MESA_VERSION} yourself and run the rasm_image_tests executable.")
    folder = TEST_ASSETS / f"mesa-{MESA_VERSION}"
    archive = folder / "mesa.7z"
    fetch_checked(MESA_URL, archive, MESA_SHA256)
    icds = list(folder.glob("x/**/lvp_icd.x86_64.json"))
    if not icds:
        extract = folder / "x"
        shutil.rmtree(extract, ignore_errors=True)
        extract.mkdir(parents=True)
        extract_7z(archive, extract)
        icds = list(folder.glob("x/**/lvp_icd.x86_64.json"))
    if not icds:
        sys.exit(f"No lvp_icd.x86_64.json in {archive}.")
    return icds[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lavapipe", action="store_true", help="download (once) and run a third-party Mesa build, lavapipe, pinned by SHA-256")
    parser.add_argument("--preset", default="x64-release", help="the build preset whose rasm_image_tests runs (default x64-release)")
    parser.add_argument("--build", action="store_true", help="build rasm_image_tests first")
    parser.add_argument("--update", action="store_true", help="write the rendered images as the new goldens")
    parser.add_argument("--scenes", help="only these tests, comma-separated (rasm_image_tests --list names them)")
    parser.add_argument("--fetch-only", action="store_true", help="fetch lavapipe and Sponza, print the driver's manifest path, run nothing")
    args = parser.parse_args()

    if not args.lavapipe:
        sys.exit("The goldens are lavapipe's, so the tests run only on it. Pass --lavapipe: it downloads (once) and runs a "
                 f"third-party Mesa build (mesa-dist-win {MESA_VERSION}, lavapipe, SHA-256 pinned).")

    if args.fetch_only:
        fetch_sponza()
        print(fetch_lavapipe())
        return 0

    if args.build:
        subprocess.run(["cmake", "--build", "--preset", args.preset, "--target", "rasm_image_tests"], cwd=ROOT, check=True)
    exe = ROOT / "build" / args.preset / "tests" / ("rasm_image_tests.exe" if os.name == "nt" else "rasm_image_tests")
    if not exe.exists():
        sys.exit(f"{exe} is not built: run with --build, or cmake --build --preset {args.preset} --target rasm_image_tests.")

    selected = set(args.scenes.split(",")) if args.scenes else None
    command = [str(exe)]
    if not selected or "sponza" in selected:
        command += ["--sponza", str(fetch_sponza())]
    icd = fetch_lavapipe()
    env = dict(os.environ, VK_DRIVER_FILES=str(icd), **LAVAPIPE_ENV)
    env.pop("VK_ICD_FILENAMES", None)
    if args.update:
        command.append("--update")
    if args.scenes:
        command += ["--scenes", args.scenes]

    result = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    log = result.stdout
    (ROOT / "build" / args.preset / "image-tests.log").write_text(log, encoding="utf-8")

    devices = sorted(set(re.findall(r"name: (.+?) -- type", log)))
    table = [line for line in log.splitlines() if re.match(r"^(test |\S+ +(pass|FAIL|update) |\d+ of \d+ tests|  \S)", line)]
    messages = [line for line in log.splitlines() if re.match(r"^\[(ERROR|WARNING): (Validation|Performance|General)\]", line) or re.search(r"\] \[(error|warning)\] ", line)]
    print(f"device: {', '.join(devices) or 'unknown'}")
    print("\n".join(table))
    print(f"validation and engine messages: {len(messages)}")
    for line in sorted(set(messages))[:20]:
        print(f"  {line}")
    print(f"full log: build/{args.preset}/image-tests.log")

    if not devices or any("llvmpipe" not in device for device in devices):
        print("The tests did not run on lavapipe (llvmpipe): the Vulkan loader did not take VK_DRIVER_FILES. An elevated "
              "process ignores it; run unelevated, or register the driver as CI does (.github/workflows/ci.yml).")
        return 1
    if messages:
        return 1
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
