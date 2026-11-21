# vcpkg's x64-osx triplet, plus build options for single ports. CMakePresets.json points VCPKG_OVERLAY_TRIPLETS here.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)

# Tracy listens on 127.0.0.1 only. Listening on every interface makes the Windows (and macOS) firewall ask for
# network access the first time each executable runs. The profiler and tracy-capture on the same machine still connect.
if(PORT STREQUAL "tracy")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DTRACY_ONLY_LOCALHOST=ON)
endif()
