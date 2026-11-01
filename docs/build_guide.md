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
Examples executables can be found in `/build` folder under the `x64-debug` preset folder, in `examples/`. They can run from any directory. Each build copies `shaders/` next to them, so run `cmake --build` after editing a shader:

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
Examples executables can be found in `/build` folder under the `macOS` preset folder, in `examples/`. They can run from any directory. Each build copies `shaders/` next to them, so run `cmake --build` after editing a shader:

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

## Using RASM from another project

`cmake --install` copies the library, public headers and shaders to `install/` in the repo. Debug and Release share that prefix, so install both if the project builds both:

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
- Shaders are compiled at runtime, so the application needs them when it ships. By default the engine looks in a `shaders/` folder next to the executable, so copy `share/rasm/shaders` from the install prefix there. To keep them elsewhere, set `EngineConfig::shaderDir`.
- Set `EngineConfig::assetRoot` to have relative `loadMesh` and `loadTexture` paths resolve against a folder of your choice. A relative `shaderDir` or `assetRoot` is resolved against the executable's directory.
- To use a build tree without installing, set `rasm_DIR` to it, for example `-Drasm_DIR=<repo>/build/x64-debug`.
- Pass `-DRASM_INSTALL=OFF` to skip the install rules, which is the default when RASM is added with `add_subdirectory`.

## Profiling with Tracy

Every build is instrumented with [Tracy](https://github.com/wolfpld/tracy) in on-demand mode: nothing is recorded until a profiler connects, so an unprofiled run does not buffer events. Zones from before the connection are lost, so start the profiler first to see startup work such as `prepareScene`, `createPipeline` and `fillTexture`.

The profiler must be the same version as the client that vcpkg installed (0.13.1 today), or it refuses the connection. Use the matching Tracy release, or build it from the port's `gui-tools` feature. For a capture without the GUI, the `cli-tools` feature builds `tracy-capture` and `tracy-csvexport`:

```
tracy-capture -a 127.0.0.1 -o trace.tracy -f -s 8
tracy-csvexport trace.tracy
```

While an engine process runs, Tracy listens on TCP port 8086 on all network interfaces and announces itself by UDP broadcast, in Release builds too. Windows may ask for a firewall exception the first time.