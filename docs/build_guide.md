# Build Guide

## Common prerequisites
- Git.
- CMake 3.23 or newer.
- Ninja.
- A C++20 compiler: LLVM Clang on Windows, Apple Clang on macOS. On Windows, the MSVC STL of Visual Studio 18 needs Clang 22 or newer; CI uses 22.1.8.
- A recent Vulkan SDK (version 1.4).
- vcpkg.

## Windows (x64)

### 1. Install tools and dependencies

#### Install `cmake`, `ninja`, and `clang++` :
 Check that `cmake`, `ninja`, and `clang++` are on PATH

 ```ps
    Get-Command cmake, ninja, clang++
 ```

you can use `winget` if any of the previour tools are missing on your system.

For example, to install `ninja` :
```powershell
winget install ninja
```
> Make sure system `$PATH` varaible is updated and contians these tools bin directory

#### Install `vcpkg` :
```ps
git clone https://github.com/microsoft/vcpkg.git "$HOME\vcpkg"

cd $HOME\vcpkg

.\bootstrap-vcpkg.bat
```
Update enviroment variables (system-wide):
```ps
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$HOME\vcpkg", [System.EnvironmentVariableTarget]::Machine)
```

#### Install `Vulkan`:

Download [LunarG's Vulkan SDK](https://vulkan.lunarg.com/sdk/home).

And update you graphics drivers to be the latest.


### 2. Configure and build

from the engine repo. root:

```ps
cmake --preset x64-debug
cmake --build --preset x64-debug
```
other presets are also available, check `cmakepresets.json`.

### 3. Run
Examples executables can be found in `/build` folder under the `x64-debug` preset folder, in `examples/`. They can run from any directory, and read `shaders/` and `assets/` from the repo, so a shader edit shows up the next time an example starts:

```
.\build\x64-debug\examples\00_hello_scene.exe
.\build\x64-debug\examples\01_render_graph.exe
.\build\x64-debug\examples\02_gltf.exe
```

## macOS (Apple Silicon or Intel)


### 1. Install tools and dependencies

#### Install `cmake`, `ninja`, and `clang++` :

```bash
xcode-select --install
brew install cmake ninja git
```

> Make sure system `$PATH` varaible is updated and contians these tools bin directory


#### Install `vcpkg` :
```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"

$Home/vcpkg/bootstrap-vcpkg.sh"
```

Update enviroment variables:

1. open up your shell config file.
```bash
sudo nano /etc/zshrc
```
2. add the following line:
```bash
export VCPKG_ROOT="$HOME/vcpkg"
```

#### Install `Vulkan` and `MoltenVK`:

Download [LunarG's Vulkan SDK](https://vulkan.lunarg.com/sdk/home).

And update you graphics drivers to be the latest.



### 2. Configure and build

from the engine repo. root:

```bash
cmake --preset macOs
cmake --build --preset macOs
```
other presets are also available, check `cmakepresets.json`.

### 3. Run
Examples executables can be found in `/build` folder under the `macOS` preset folder, in `examples/`. They can run from any directory, and read `shaders/` and `assets/` from the repo, so a shader edit shows up the next time an example starts:

```
./build/macOS/examples/00_hello_scene
./build/macOS/examples/01_render_graph
./build/macOS/examples/02_gltf
```

## Unit tests

The tests use [doctest](https://github.com/doctest/doctest) and need no GPU. Run them with the same preset you built:

```
ctest --preset x64-debug
```

To see the test output, or to run a subset, run the executable directly, for example `.\build\x64-debug\tests\rasm_tests.exe -tc="*trackball*"`. The tests that build an `Engine` log an expected `Window dimensions must be positive` error, because that is how they skip the window and GPU setup.

Pass `-DRASM_BUILD_TESTS=OFF` to leave them out.

## Image tests

`rasm_image_tests` renders 15 scenes headless at 512 × 288 and compares each frame with a golden image in `tests/golden/lavapipe/` ([Image tests](guides/image-tests.html)). The goldens come from lavapipe, Mesa's CPU Vulkan driver, so the tests run on that driver, not on your GPU. `tools/image_tests.py` runs them (Windows):

```
python tools/image_tests.py --lavapipe                          # run every test, print a table
python tools/image_tests.py --lavapipe --scenes sponza,taa      # only these
python tools/image_tests.py --lavapipe --update --scenes taa    # render new goldens for these
```

**`--lavapipe` downloads (once) and runs a third-party Mesa build**: lavapipe from [mesa-dist-win](https://github.com/pal1000/mesa-dist-win) 26.3.0, checked by its SHA-256, into `build/test-assets/`. Nothing is downloaded or run without that flag. Extracting it needs 7-Zip (`7z`) on the `PATH`. The script also fetches Sponza from a pinned commit of KhronosGroup/glTF-Sample-Assets (SHA-256 checked, into `build/test-assets/`): its license, the CRYENGINE Limited License, does not allow it in this repository. It runs `build/x64-release/tests/rasm_image_tests.exe` (`--preset`, `--build`), points the Vulkan loader at lavapipe with `VK_DRIVER_FILES`, and fails when the log names another device or has a validation or engine error. A failed test leaves `actual.png`, `expected.png` and `diff.png` (red: over the threshold) in `build/x64-release/image-tests/<test>/`.

The default `ctest` presets leave the tests out (label `image`); `ctest --preset x64-release -L image` runs them on whatever driver the loader picks. An elevated process's loader ignores `VK_DRIVER_FILES`, so run the script from a normal prompt.

A change that changes a golden says why in its pull request, with the image before and after. `verify-change` runs the tests with `--images`, only when asked.

## Dear ImGui

`RASM_IMGUI` (on by default) builds RASM with [Dear ImGui](https://github.com/ocornut/imgui) for `EngineConfig::ui`: the RASM window on F1 and the application's own windows (`Engine::setUiCallback`). The option adds the `imgui` feature of `vcpkg.json` to `VCPKG_MANIFEST_FEATURES` before `project()`, so vcpkg installs the `imgui` port with its `sdl3-binding` feature (SDL3 input; RASM draws ImGui itself through its RHI, so no `vulkan-binding`). `-DRASM_IMGUI=OFF` installs nothing, compiles no UI code and links no ImGui; `EngineConfig::ui` then logs a warning, and `examples/15_debug_ui` is not built. Change the option in a new build folder or delete `CMakeCache.txt`, so vcpkg installs again.

RASM links ImGui PUBLIC as a static library and defines `RASM_IMGUI` for the application too. RASM owns the ImGui context, and the application's `ImGui::` calls go to the same library: use the one `find_package(rasm)` brings, never a second copy of ImGui.

## Static and dynamic analysis

All of these run locally; CI does not run them. Each tool only looks at RASM's own code (`rasm`, `rasm_tests`, the examples), never at the vcpkg dependencies.

| Tool | Finds | How to run |
|---|---|---|
| Compiler warnings | conversions, shadowing, unused code | on by default (`RASM_WARNINGS`) in every preset |
| clang-tidy + clang static analyzer | bug patterns, null and leak paths, slow copies, naming | `x64-tidy` / `macOS-tidy` preset |
| AddressSanitizer + UndefinedBehaviorSanitizer | out-of-bounds, use-after-free, undefined behaviour | `x64-asan` / `macOS-asan` preset |
| ThreadSanitizer | data races | `macOS-tsan` preset |
| Vulkan synchronization validation, GPU-AV | missing barriers; bad bindless, indirect and BDA accesses in shaders | `EngineConfig` switches or environment variables |

A new build should have no warnings and no clang-tidy findings. Fix new ones, or turn a check off in `.clang-tidy` with a reason, rather than letting them pile up.

### Compiler warnings

`RASM_WARNINGS` (on when RASM is the top-level project) adds `-Wall -Wextra -Wshadow -Wconversion` with clang and GCC, and `/W4` with MSVC. Two are turned off: sign conversions (nearly all are tinygltf's `int` indices; `-Wsign-compare` still reports the comparisons), and missing field initializers (`{VK_STRUCTURE_TYPE_...}` and designated initializers leave the rest at zero or the default on purpose).

### clang-tidy

```
cmake --preset x64-tidy
cmake --build --preset x64-tidy
```

The tidy preset runs clang-tidy on every file it compiles, so findings show up as build warnings and a rebuild only checks the files that changed. It needs `clang-tidy` 19 or newer on `PATH` (it ships with LLVM; `.clang-tidy` uses `ExcludeHeaderFilterRegex`) and CMake 3.27 or newer (for `SKIP_LINTING`); configure stops with an error on an older CMake. The checks are in `.clang-tidy`: `bugprone-*`, `clang-analyzer-*` (the clang static analyzer), `performance-*` and `readability-identifier-naming`, which checks the [naming table](coding_style.md#2-naming). The file lists each check that is turned off and why. `src/thirdparty_impl.cpp` is skipped.

Every build writes `compile_commands.json`, so clang-tidy also runs on an existing build without the preset, for example on one file: `clang-tidy -p build/x64-debug src/engine.cpp`. To silence one finding that is wrong, add `// NOLINT(<check>): <reason>` on its line.

### Sanitizers

```
cmake --preset x64-asan
cmake --build --preset x64-asan
ctest --preset x64-asan
python .claude/skills/verify-change/scripts/verify.py run --preset x64-asan
```

The sanitizer presets set `RASM_SANITIZERS` (`address;undefined`, or `thread`). The first ASan or UBSan error stops the program with a report (`-fno-sanitize-recover=all`). The `x64-asan` / `macOS-asan` test presets add `UBSAN_OPTIONS=print_stacktrace=1` for a stack trace, and `macOS-tsan` sets `TSAN_OPTIONS=halt_on_error=1`, because TSan keeps running after a report by default. Set these yourself when running an example by hand.

- **Windows:** clang's ASan does not support the Debug C runtime, so `x64-asan` builds `RelWithDebInfo` against the release runtime and libraries, but with `-O1` and asserts still on. The ASan runtime DLL is copied next to each executable.
- **UBSan** skips four checks in `src/thirdparty_impl.cpp`, all for stb_image_resize2's deliberate tricks: `array-bounds` (it indexes before its sRGB table), `null` and `object-size` (`STBIR__FREE_AND_CLEAR` reads `info->user_data` after clearing `info`) and `alignment` (`STBIR_MOVE_*` copies coefficients with unaligned 8-byte stores). Every other UBSan check (shifts, overflow, enums, ...) and ASan still cover the file, so tinygltf, stb_image and tinyobjloader parsing stays checked.
- **Installing a sanitized build:** `rasm` then carries `-fsanitize=...` in its link interface, so a `find_package(rasm)` consumer links the sanitizer runtime too and must use clang.
- **GFXReconstruct:** its default page-guard memory tracking works through access violations, which ASan treats as crashes. `verify.py` sets `GFXRECON_MEMORY_TRACKING_MODE=unassisted` for `-asan` presets; do the same when capturing by hand.
- **Leaks:** LeakSanitizer is part of ASan on Linux. It does not exist on Windows, and Apple clang does not support it on macOS; with Homebrew LLVM, set `ASAN_OPTIONS=detect_leaks=1`. With validation on, the Vulkan layer reports every Vulkan object still alive at `vkDestroyDevice`, and VMA asserts on leaked allocations in builds with asserts on.
- **MemorySanitizer** is not set up. It only runs on Linux, and only when every library, the C++ standard library included, is built with it.
- **ThreadSanitizer** (`macOS-tsan`) cannot be combined with ASan, so it has its own preset. Windows has no TSan.

### Vulkan validation

`EngineConfig::enableValidation` turns on the Khronos validation layer with its core checks. Two slower checks can be added on top:

| `EngineConfig` field | Environment variable | Finds |
|---|---|---|
| `enableSyncValidation` | `VK_LAYER_VALIDATE_SYNC=1` | missing or wrong barriers, read/write hazards between passes |
| `enableGpuValidation` | `VK_LAYER_GPUAV_ENABLE=1` | out-of-bounds bindless indices, bad indirect draw arguments, bad buffer device address reads, in shaders as they run |

Both fields need `enableValidation` and installed validation layers; without them the engine logs a warning and ignores the fields. The engine enables `VK_EXT_layer_settings` (from the validation layer) to pass them. The environment variables need no rebuild; `verify.py --sync --gpu-av` sets them. Use the environment variables when the GFXReconstruct capture layer is loaded (as `verify.py` does): it drops the layer settings that the two fields pass to `vkCreateInstance`, so the fields have no effect under it. GPU-AV logs three expected warnings at startup: that it is slow next to core checks, which device features it turned on for itself, and which of its checks it turned off because the GPU lacks the feature (ray tracing, mesh shading).

## Continuous integration

`.github/workflows/ci.yml` runs on every pull request and every push to `main`. It has one job per OS: Windows builds `x64-debug` then `x64-release`, macOS builds `macOS` then `macOS-release`, examples included, and each preset runs `ctest`. The runners have no GPU, so CI does not run the examples. It still installs the Vulkan runtime on Windows, because `rasm_tests.exe` loads `vulkan-1.dll`. After the Release tests, the Windows job runs the [image tests](#image-tests) on lavapipe (about 1.7 minutes: 20 s to fetch and register lavapipe, 80 s for the tests): it caches `build/test-assets/`, registers lavapipe and the validation layer in the registry (the runner's processes are elevated, and the loader of an elevated process ignores `VK_DRIVER_FILES` and `VK_LAYER_PATH`), runs `tools/image_tests.py --lavapipe`, and uploads `build/x64-release/image-tests/` as the `image-test-failures` artifact when a test fails.

vcpkg.json has no `builtin-baseline`, so the vcpkg commit picks the dependency versions. CI pins it in `VCPKG_COMMIT`. When you update your local vcpkg, put its commit (`git -C $VCPKG_ROOT rev-parse HEAD`) there too, so CI builds the same versions you do. Built packages are cached per OS and vcpkg commit. A run restores the newest cache and saves a new one only when it built packages, for example after a runner toolchain update. A new vcpkg commit makes the next run build all dependencies again.

`ports/` holds overlay ports, which `vcpkg-configuration.json` puts in front of the vcpkg commit's ports. `ports/tinygltf` is vcpkg's 2.9.7 port with a new SHA512, because GitHub regenerated the `v2.9.7` archive. Delete it when `VCPKG_COMMIT` moves past microsoft/vcpkg#53226. That commit also updates tinygltf to 3.0.0.

`cmake/triplets/` holds overlay triplets (`VCPKG_OVERLAY_TRIPLETS` in `CMakePresets.json`): vcpkg's `x64-windows`, `arm64-osx` and `x64-osx` triplets, plus build options for single ports. `tracy` gets `TRACY_ONLY_LOCALHOST` (see [Profiling with Tracy](#profiling-with-tracy)). `ktx` gets `KTX_FEATURE_ETC_UNPACK=OFF` and `KTX_FEATURE_GL_UPLOAD=OFF`: by default libktx compiles Ericsson's ETC decoder (`lib/etcdec.cxx`), whose license covers only OpenGL, OpenGL ES and WebGL products, and RASM transcodes on the CPU and never uploads through OpenGL. vcpkg hashes the triplet file into every package, so a change to it rebuilds all dependencies once.

## Using RASM from another project

`cmake --install` copies the library, public headers and shaders to `install/` in the repo. Debug and Release share that prefix, so install both if the project builds both. A project building `RelWithDebInfo` or `MinSizeRel` links the Release library:

```
cmake --install build/x64-debug
cmake --install build/x64-release
```

Then point the other project's `CMAKE_PREFIX_PATH` at that prefix:

```cmake
find_package(rasm 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE rasm::rasm)
```

- RASM is a static library, so the project also needs RASM's link dependencies: Vulkan SDK, vk-bootstrap, VulkanMemoryAllocator, glm, SDL3, Tracy, spdlog, tinyobjloader, meshoptimizer, Slang, libktx and, with `RASM_IMGUI`, Dear ImGui. With vcpkg, add `vk-bootstrap`, `vulkan-memory-allocator`, `glm`, `sdl3` with the `vulkan` feature, `tracy` with the `on-demand` feature, `spdlog`, `tinyobjloader`, `meshoptimizer`, `shader-slang` and `ktx` to its manifest (and `imgui` with the `sdl3-binding` feature for a RASM built with `RASM_IMGUI`, the default; `find_package(rasm)` then finds it with `find_dependency`), and copy the `ktx` block of `cmake/triplets/` into its own triplets to leave out the ETC decoder.
- Shaders are compiled at runtime, so the application needs them when it ships. By default the engine looks in a `shaders/` folder next to the executable. `find_package(rasm)` sets `rasm_SHADER_DIR` to the folder holding them, so a build step can copy them there:

  ```cmake
  add_custom_command(TARGET my_app POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${rasm_SHADER_DIR}" "$<TARGET_FILE_DIR:my_app>/shaders")
  ```

  To keep them elsewhere, set `EngineConfig::shaderDir`.
- Set `EngineConfig::assetRoot` to have relative `loadMesh` and `loadTexture` paths resolve against a folder of your choice. A relative `shaderDir` or `assetRoot` is resolved against the executable's directory.
- To use a build tree without installing, set `rasm_DIR` to it, for example `-Drasm_DIR=<repo>/build/x64-debug`.
- Pass `-DRASM_INSTALL=OFF` to skip the install rules, which is the default when RASM is added with `add_subdirectory`.

## Profiling with Tracy

Every build is instrumented with [Tracy](https://github.com/wolfpld/tracy) in on-demand mode: nothing is recorded until a profiler connects, so an unprofiled run does not buffer events. The client listens on `127.0.0.1` only (`TRACY_ONLY_LOCALHOST`, set for the `tracy` port in the overlay triplets in `cmake/triplets/`), so running an example does not make the firewall ask for network access. Connect from the same machine; to profile from another one, remove that option and rebuild. Zones from before the connection are lost, so start the profiler first to see startup work such as `createPipeline`, `fillTexture` and the first `syncGpuScene`.

The profiler must be the same version as the client that vcpkg installed (0.13.1 today), or it refuses the connection. Use the matching Tracy release, or build it from the port's `gui-tools` feature. For a capture without the GUI, the `cli-tools` feature builds `tracy-capture` and `tracy-csvexport`:

```
tracy-capture -a 127.0.0.1 -o trace.tracy -f -s 8
tracy-csvexport trace.tracy
```

`python .claude/skills/tracy-profile/scripts/tracy.py setup` builds both into `build/tracy-tools/` from the same vcpkg checkout, so their version always matches the client. The same script also captures an example and summarizes the trace.

While an engine process runs, Tracy listens on TCP port 8086 on all network interfaces and announces itself by UDP broadcast, in Release builds too. Windows may ask for a firewall exception the first time.