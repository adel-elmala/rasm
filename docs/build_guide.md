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
Examples executables can be found in `/build` folder under the `x64-debug` preset folder

```
.\build\x64-debug\Example_0.exe
.\build\x64-debug\Example_1.exe
.\build\x64-debug\Example_2.exe
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
Examples executables can be found in `/build` folder under the `macOs` preset folder

```
./build/macOs/Example_0
./build/macOs/Example_1
./build/macOs/Example_2
```