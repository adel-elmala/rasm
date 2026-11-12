# Build Guide

## Common prerequisites
- Git.
- CMake 3.21 or newer.
- Ninja.
- A C++20 compiler: LLVM Clang on Windows, Apple Clang on macOS.
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

The tidy preset runs clang-tidy on every file it compiles, so findings show up as build warnings and a rebuild only checks the files that changed. It needs `clang-tidy` on `PATH` (it ships with LLVM). The checks are in `.clang-tidy`: `bugprone-*`, `clang-analyzer-*` (the clang static analyzer), `performance-*` and `readability-identifier-naming`, which checks the [naming table](coding_style.md#2-naming). The file lists each check that is turned off and why. `src/thirdparty_impl.cpp` is skipped.

Every build writes `compile_commands.json`, so clang-tidy also runs on an existing build without the preset, for example on one file: `clang-tidy -p build/x64-debug src/engine.cpp`. To silence one finding that is wrong, add `// NOLINT(<check>): <reason>` on its line.

### Sanitizers

```
cmake --preset x64-asan
cmake --build --preset x64-asan
ctest --preset x64-asan
python .claude/skills/verify-change/scripts/verify.py run --preset x64-asan
```

The sanitizer presets set `RASM_SANITIZERS` (`address;undefined`, or `thread`). The first error stops the program with a report and a stack trace. The test presets set `halt_on_error=1` and `print_stacktrace=1`; set `ASAN_OPTIONS` / `UBSAN_OPTIONS` yourself to change that when running an example by hand.

- **Windows:** clang's ASan does not support the Debug C runtime, so `x64-asan` builds `RelWithDebInfo` against the release runtime and libraries, but with `-O1` and asserts still on. The ASan runtime DLL is copied next to each executable.
- **UBSan** is off for `src/thirdparty_impl.cpp`: stb_image_resize2 indexes from a pointer before its table on purpose. ASan still checks that file.
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

Both fields need `enableValidation`. The environment variables need no rebuild; `verify.py --sync --gpu-av` sets them. Use the environment variables when the GFXReconstruct capture layer is loaded (as `verify.py` does): it drops the layer settings that the two fields pass to `vkCreateInstance`, so the fields have no effect under it. GPU-AV logs three expected warnings at startup: that it is slow next to core checks, which device features it turned on for itself, and which of its checks it turned off because the GPU lacks the feature (ray tracing, mesh shading).

## Continuous integration

`.github/workflows/ci.yml` runs on every pull request and every push to `main`. It has one job per OS: Windows builds `x64-debug` then `x64-release`, macOS builds `macOS` then `macOS-release`, examples included, and each preset runs `ctest`. The runners have no GPU, so CI does not run the examples. It still installs the Vulkan runtime on Windows, because `rasm_tests.exe` loads `vulkan-1.dll`.

vcpkg.json has no `builtin-baseline`, so the vcpkg commit picks the dependency versions. CI pins it in `VCPKG_COMMIT`. When you update your local vcpkg, put its commit (`git -C $VCPKG_ROOT rev-parse HEAD`) there too, so CI builds the same versions you do. Built packages are cached per OS and vcpkg commit. A run restores the newest cache and saves a new one only when it built packages, for example after a runner toolchain update. A new vcpkg commit makes the next run build all dependencies again.

`ports/` holds overlay ports, which `vcpkg-configuration.json` puts in front of the vcpkg commit's ports. `ports/tinygltf` is vcpkg's 2.9.7 port with a new SHA512, because GitHub regenerated the `v2.9.7` archive. Delete it when `VCPKG_COMMIT` moves past microsoft/vcpkg#53226. That commit also updates tinygltf to 3.0.0.

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

- RASM is a static library, so the project also needs RASM's link dependencies: Vulkan SDK, vk-bootstrap, VulkanMemoryAllocator, glm, SDL3, Tracy, spdlog, tinyobjloader and Slang. With vcpkg, add `vk-bootstrap`, `vulkan-memory-allocator`, `glm`, `sdl3` with the `vulkan` feature, `tracy` with the `on-demand` feature, `spdlog`, `tinyobjloader` and `shader-slang` to its manifest.
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

Every build is instrumented with [Tracy](https://github.com/wolfpld/tracy) in on-demand mode: nothing is recorded until a profiler connects, so an unprofiled run does not buffer events. Zones from before the connection are lost, so start the profiler first to see startup work such as `createPipeline`, `fillTexture` and the first `syncGpuScene`.

The profiler must be the same version as the client that vcpkg installed (0.13.1 today), or it refuses the connection. Use the matching Tracy release, or build it from the port's `gui-tools` feature. For a capture without the GUI, the `cli-tools` feature builds `tracy-capture` and `tracy-csvexport`:

```
tracy-capture -a 127.0.0.1 -o trace.tracy -f -s 8
tracy-csvexport trace.tracy
```

`python .claude/skills/tracy-profile/scripts/tracy.py setup` builds both into `build/tracy-tools/` from the same vcpkg checkout, so their version always matches the client. The same script also captures an example and summarizes the trace.

While an engine process runs, Tracy listens on TCP port 8086 on all network interfaces and announces itself by UDP broadcast, in Release builds too. Windows may ask for a firewall exception the first time.