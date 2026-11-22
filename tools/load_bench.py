"""Load-time benchmark: the numbers of P14, P21 (docs/guides/job-system.html) and D13's transfer queue
(docs/guides/transfer-queue.html).

    python tools/load_bench.py --preset x64-release --runs 3 [--transfer-queue on|off|both]

Fetches (once) the PNG Flight Helmet and Sponza from a pinned commit of KhronosGroup/glTF-Sample-Assets into
build/test-assets/, each file checked by SHA-256, and makes build/test-assets/burst/: 300 copies of Sponza's images
under their own names, so each is its own load. Then it runs, headless:

    10_ktx2_textures --measure --model <the KTX2 helmet, the PNG helmet, Sponza>
    17_streaming --burst build/test-assets/burst

and prints the median of each line: the load time, the longest frame, the longest GPU frame and the GPU time of the upload
copies while it loads, once with the copies on the transfer queue and once on the graphics queue (EngineConfig::
transferQueue, the examples' --transfer-queue). On Windows it also samples the process while it runs (every 5 ms): the
most threads it had at once and its peak private memory, so builds without Engine::getLoadStats can be compared too.
"""

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from image_tests import ROOT, SPONZA_COMMIT, TEST_ASSETS, download, fetch_sponza, sha256  # noqa: E402

HELMET_URL = f"https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/{SPONZA_COMMIT}/Models/FlightHelmet/glTF/"
HELMET_GLTF_SHA256 = "c9c16c8b85749f62f38ec6069700e5388c6894d8e566f51a35cc270255402cd3"
# The SHA-256 of the lines "<file> <sha256>" of every file FlightHelmet.gltf names, sorted by file name.
HELMET_FILES_SHA256 = "7fe73961ebdffa00e4f936a3833ecff335784ed9e086857599995efeada48c48"
BURST_FILES = 300


def fetch_helmet():
    folder = TEST_ASSETS / "FlightHelmet" / "glTF"
    gltf = folder / "FlightHelmet.gltf"
    if not gltf.exists() or sha256(gltf) != HELMET_GLTF_SHA256:
        download(HELMET_URL + "FlightHelmet.gltf", gltf)
    if sha256(gltf) != HELMET_GLTF_SHA256:
        sys.exit(f"FlightHelmet.gltf: SHA-256 {sha256(gltf)}, expected {HELMET_GLTF_SHA256}. Not used.")
    model = json.loads(gltf.read_text(encoding="utf-8"))
    names = sorted({b["uri"] for b in model.get("buffers", [])} | {i["uri"] for i in model.get("images", []) if "uri" in i})
    for name in names:
        if "/" in name or "\\" in name or name.startswith("."):
            sys.exit(f"FlightHelmet.gltf names an unexpected file: {name}")
        if not (folder / name).exists():
            download(HELMET_URL + name, folder / name)
    lines = "".join(f"{name} {sha256(folder / name)}\n" for name in names)
    found = hashlib.sha256(lines.encode()).hexdigest()
    if found != HELMET_FILES_SHA256:
        sys.exit(f"The Flight Helmet's files hash to {found}, expected {HELMET_FILES_SHA256}. Delete {folder} to fetch them again.")
    return gltf


def make_burst(sponza):
    folder = TEST_ASSETS / "burst"
    images = sorted(p for p in sponza.parent.iterdir() if p.suffix.lower() in (".png", ".jpg", ".jpeg"))
    if len(list(folder.glob("tile_*"))) != BURST_FILES:
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
        for i in range(BURST_FILES):
            source = images[i % len(images)]
            shutil.copyfile(source, folder / f"tile_{i:03}{source.suffix.lower()}")
    return folder


class Sampler:
    """The most threads and the peak private bytes of a running process (Windows only)."""

    def __init__(self, pid):
        self.pid = pid
        self.threads = 0
        self.peak_private = 0
        self.stop = False
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        if os.name != "nt":
            return
        kernel = ctypes.windll.kernel32
        psapi = ctypes.windll.psapi

        class THREADENTRY32(ctypes.Structure):
            _fields_ = [("dwSize", ctypes.c_ulong), ("cntUsage", ctypes.c_ulong), ("th32ThreadID", ctypes.c_ulong),
                        ("th32OwnerProcessID", ctypes.c_ulong), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                        ("dwFlags", ctypes.c_ulong)]

        class COUNTERS(ctypes.Structure):
            _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong), ("PeakWorkingSetSize", ctypes.c_size_t),
                        ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                        ("PeakPagefileUsage", ctypes.c_size_t)]

        kernel.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
        kernel.OpenProcess.restype = ctypes.c_void_p
        process = kernel.OpenProcess(0x0410, False, self.pid)
        while not self.stop:
            snapshot = kernel.CreateToolhelp32Snapshot(0x4, 0)
            entry = THREADENTRY32()
            entry.dwSize = ctypes.sizeof(entry)
            count = 0
            ok = kernel.Thread32First(ctypes.c_void_p(snapshot), ctypes.byref(entry))
            while ok:
                if entry.th32OwnerProcessID == self.pid:
                    count += 1
                ok = kernel.Thread32Next(ctypes.c_void_p(snapshot), ctypes.byref(entry))
            kernel.CloseHandle(ctypes.c_void_p(snapshot))
            self.threads = max(self.threads, count)
            counters = COUNTERS()
            counters.cb = ctypes.sizeof(counters)
            if process and psapi.GetProcessMemoryInfo(ctypes.c_void_p(process), ctypes.byref(counters), counters.cb):
                self.peak_private = max(self.peak_private, counters.PeakPagefileUsage)
            time.sleep(0.005)
        if process:
            kernel.CloseHandle(ctypes.c_void_p(process))

    def finish(self):
        self.stop = True
        self.thread.join()


def run(exe, args, pattern):
    process = subprocess.Popen([str(exe)] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    sampler = Sampler(process.pid)
    output, _ = process.communicate()
    sampler.finish()
    match = re.search(pattern, output)
    if not match:
        sys.exit(f"{exe.name} {' '.join(args)}: no result line.\n{output[-3000:]}")
    return [float(v) if v is not None else float("nan") for v in match.groups()], sampler.threads, sampler.peak_private / (1 << 20)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preset", default="x64-release")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--build-dir", help="another build's folder (for a before/after), default build/<preset>")
    parser.add_argument("--transfer-queue", choices=["on", "off", "both"], default="both", help="the queue the upload copies run on")
    args = parser.parse_args()

    sponza = fetch_sponza()
    helmet = fetch_helmet()
    burst = make_burst(sponza)
    examples = Path(args.build_dir) if args.build_dir else ROOT / "build" / args.preset / "examples"
    suffix = ".exe" if os.name == "nt" else ""
    headless = ["--headless", "--frames", "100000"]
    # A build from before the GPU times prints none, and they read nan.
    gpu = r"(?:, longest GPU frame ([\d.]+) ms, ([\d.]+) ms of upload copies on the GPU)?"
    measure = r"loaded in (\d+) ms over \d+ frames, longest frame ([\d.]+) ms" + gpu
    cases = [
        ("Flight Helmet, KTX2", examples / f"10_ktx2_textures{suffix}", ["--measure"] + headless, measure),
        ("Flight Helmet, PNG", examples / f"10_ktx2_textures{suffix}", ["--measure", "--model", str(helmet)] + headless, measure),
        ("Sponza", examples / f"10_ktx2_textures{suffix}", ["--measure", "--model", str(sponza)] + headless, measure),
        (f"Burst of {BURST_FILES} images", examples / f"17_streaming{suffix}", ["--burst", str(burst)] + headless,
         measure),
    ]
    queues = ["on", "off"] if args.transfer_queue == "both" else [args.transfer_queue]
    print(f"{'case':24} {'copies on':>9} {'load ms':>8} {'longest frame ms':>17} {'longest GPU ms':>15} {'upload GPU ms':>14} {'threads':>8} {'peak private MiB':>17}")
    for name, exe, case_args, pattern in cases:
        for queue in queues:
            columns = [[] for _ in range(6)]
            for _ in range(args.runs):
                values, most_threads, peak = run(exe, case_args + ["--transfer-queue", queue], pattern)
                for i, value in enumerate(values + [most_threads, peak]):
                    columns[i].append(value)
            load, frame, gpu_frame, upload, threads, private = (statistics.median(c) for c in columns)
            where = "transfer" if queue == "on" else "graphics"
            print(f"{name:24} {where:>9} {load:8.0f} {frame:17.1f} {gpu_frame:15.2f} {upload:14.1f} {threads:8.0f} {private:17.0f}")


if __name__ == "__main__":
    main()
