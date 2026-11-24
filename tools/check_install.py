"""Check RASM as an application gets it: install a build, then build programs against the install.

    python tools/check_install.py --preset x64-release [--ctypes] [--validation]

1. `cmake --install build/<preset>` into build/<preset>-install (emptied first).
2. tests/consumer, configured once per component in a folder of its own:
   - renderer: C++, find_package(rasm COMPONENTS renderer), linked to rasm::renderer;
   - rasm:     C++, find_package(rasm COMPONENTS rasm), linked to rasm::rasm;
   - c:        C, find_package(rasm COMPONENTS c) with only the install folder (no vcpkg), linked to rasm::c and to
               rasm::c_renderer, and both programs run.
   The C++ ones find their dependencies in the build's vcpkg_installed folder.
3. The C libraries export only rasm_* functions, import no Vulkan loader, and rasm_c_renderer holds no SDL3 or libktx.
4. --ctypes: tests/c_api/ctypes_smoke.py renders with the installed rasm_c from Python, with no shader_dir: the
   shaders installed beside the library must be found. It needs a Vulkan device.

Exit code 0 when everything passes.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(command, env=None):
    print("+ " + " ".join(command), flush=True)
    result = subprocess.run(command, env=env)
    if result.returncode != 0:
        sys.exit(f"FAIL: {' '.join(command)} exited with {result.returncode}")


def output(command):
    return subprocess.run(command, capture_output=True, text=True, check=True).stdout


def read_cache(build, name):
    with open(os.path.join(build, "CMakeCache.txt"), encoding="utf-8") as f:
        for line in f:
            match = re.match(rf"^{name}:[A-Z]+=(.*)$", line.strip())
            if match:
                return match.group(1)
    return ""


def c_libraries(prefix, debug):
    suffix = "d" if debug else ""
    if sys.platform == "win32":
        return {name: os.path.join(prefix, "bin", f"{name}{suffix}.dll") for name in ("rasm_c", "rasm_c_renderer")}
    extension = ".dylib" if sys.platform == "darwin" else ".so"
    return {name: os.path.join(prefix, "lib", f"lib{name}{suffix}{extension}") for name in ("rasm_c", "rasm_c_renderer")}


def exports_and_imports(path):
    """The library's exported function names, the libraries it imports, and every symbol it defines."""
    if sys.platform == "win32":
        dump = output(["llvm-objdump", "-p", path])
        table = dump.split("Export Table:", 1)[1] if "Export Table:" in dump else ""
        exports = re.findall(r"^\s+\d+\s+0x[0-9a-f]+\s+(\S+)$", table, flags=re.M)
        imports = re.findall(r"DLL Name: (\S+)", dump)
        return exports, imports, exports
    if sys.platform == "darwin":
        # The linker's own header symbol is always there.
        exports = [s[1:] for s in output(["nm", "-gUj", path]).split() if s != "__mh_dylib_header"]
        imports = output(["otool", "-L", path]).splitlines()[1:]
        defined = [s[1:] for s in output(["nm", "-Uj", path]).split()]
        return exports, imports, defined
    exports = output(["nm", "-D", "--defined-only", "--format=just-symbols", path]).split()
    imports = re.findall(r"NEEDED\s+(\S+)", output(["readelf", "-d", path]))
    defined = output(["nm", "--defined-only", "--format=just-symbols", path]).split()
    return exports, imports, defined


def check_library(name, path):
    exports, imports, defined = exports_and_imports(path)
    problems = []
    if not any(e.startswith("rasm_") for e in exports):
        problems.append("exports no rasm_ function")
    problems += [f"exports {e}" for e in exports if not e.startswith("rasm_")]
    problems += [f"imports {i.strip()}" for i in imports if "vulkan" in i.lower()]
    if name == "rasm_c_renderer":
        problems += [f"imports {i.strip()}" for i in imports if re.search(r"sdl|ktx", i, re.I)]
        if any(s.startswith("SDL_Init") or s.startswith("ktxTexture") for s in defined):
            problems.append("holds SDL3 or libktx code")
        problems += [f"exports {e}" for e in exports if e.startswith("rasm_engine_") or e.startswith("rasm_asset_loader_")]
    elif not any(e.startswith("rasm_engine_create") for e in exports):
        problems.append("has no rasm_engine_create")
    for problem in problems:
        print(f"  {name}: {problem}")
    print(f"{name}: {len(exports)} exports, {'OK' if not problems else 'FAIL'}")
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preset", required=True)
    parser.add_argument("--ctypes", action="store_true", help="also render from Python with the installed rasm_c")
    parser.add_argument("--validation", action="store_true", help="the ctypes run with Vulkan validation")
    args = parser.parse_args()

    build = os.path.join(REPO, "build", args.preset)
    prefix = os.path.join(REPO, "build", args.preset + "-install")
    build_type = read_cache(build, "CMAKE_BUILD_TYPE") or "Debug"
    debug = build_type == "Debug"
    triplet = read_cache(build, "VCPKG_TARGET_TRIPLET")
    vcpkg = os.path.join(build, "vcpkg_installed", triplet)
    if not triplet or not os.path.isdir(vcpkg):
        sys.exit(f"FAIL: no vcpkg_installed folder for triplet '{triplet}' in {build}")

    shutil.rmtree(prefix, ignore_errors=True)
    run(["cmake", "--install", build, "--prefix", prefix])

    for component in ("renderer", "rasm", "c"):
        consumer = os.path.join(REPO, "build", f"{args.preset}-consumer-{component}")
        shutil.rmtree(consumer, ignore_errors=True)
        prefix_path = prefix if component == "c" else f"{prefix};{vcpkg}"
        command = ["cmake", "-S", os.path.join(REPO, "tests", "consumer"), "-B", consumer, "-G", "Ninja",
                   f"-DCMAKE_BUILD_TYPE={build_type}", f"-DRASM_CONSUMER={component}", f"-DCMAKE_PREFIX_PATH={prefix_path}",
                   "-DCMAKE_C_COMPILER=clang" if component == "c" else "-DCMAKE_CXX_COMPILER=clang++"]
        run(command)
        run(["cmake", "--build", consumer])
        if component == "c":
            env = dict(os.environ)
            if sys.platform == "win32":
                env["PATH"] = os.path.join(prefix, "bin") + os.pathsep + env["PATH"]
            for program in ("consumer_c", "consumer_c_renderer"):
                run([os.path.join(consumer, program + (".exe" if sys.platform == "win32" else ""))], env=env)

    libraries = c_libraries(prefix, debug)
    ok = all([check_library(name, path) for name, path in libraries.items()])
    if not ok:
        sys.exit("FAIL: the C libraries")

    if args.ctypes:
        command = [sys.executable, os.path.join(REPO, "tests", "c_api", "ctypes_smoke.py"), libraries["rasm_c"]]
        if args.validation:
            command.append("--validation")
        # The validation layer prints through vk-bootstrap's messenger, not RASM's log, so its lines are read here.
        print("+ " + " ".join(command), flush=True)
        result = subprocess.run(command, capture_output=True, text=True, errors="replace")
        print(result.stdout + result.stderr)
        messages = re.findall(r"\[(?:ERROR|WARNING): Validation\]", result.stdout + result.stderr)
        if result.returncode != 0 or messages:
            sys.exit(f"FAIL: the ctypes run exited with {result.returncode}, {len(messages)} validation messages")
    print("OK: the install works for C++ and C consumers.")


if __name__ == "__main__":
    main()
