# cimgui (https://github.com/cimgui/cimgui), the C API of Dear ImGui that most bindings are made from (ImGui.NET,
# imgui-rs), for the C UI example (33_c_render_graph) and the ctypes test only: RASM's libraries never link it.
#
# Pinned to the commit of cimgui's tag 1.92.6, which is generated from Dear ImGui 1.92.6, RASM's version, with
# IMGUI_DISABLE_OBSOLETE_FUNCTIONS, as ports/imgui builds RASM's. cimgui.cpp is compiled against vcpkg's Dear ImGui and
# linked with its static library into a shared library, so the library holds its own copy of Dear ImGui with its own
# globals, as a binding's does: it reaches RASM's context only through rasm_get_imgui_context.
#
# Defines rasm_cimgui (the shared library "cimgui"). Its users include "cimgui.h" with CIMGUI_DEFINE_ENUMS_AND_STRUCTS.
include(FetchContent)

find_package(imgui CONFIG REQUIRED)

# The cimgui repository's CMakeLists.txt builds its own Dear ImGui from a submodule: SOURCE_SUBDIR names a folder
# without one, so FetchContent only downloads, and GIT_SUBMODULES "" leaves the submodule out.
FetchContent_Declare(rasm_cimgui_source
    GIT_REPOSITORY https://github.com/cimgui/cimgui.git
    GIT_TAG b6563e1b115a08e4e6651576420600df225e0cae # 1.92.6
    GIT_SUBMODULES ""
    SOURCE_SUBDIR rasm-no-cmake
)
FetchContent_MakeAvailable(rasm_cimgui_source)

# cimgui.cpp includes "./imgui/imgui.h": these forward to vcpkg's headers.
set(rasm_cimgui_shim "${CMAKE_CURRENT_BINARY_DIR}/cimgui-shim")
file(CONFIGURE OUTPUT "${rasm_cimgui_shim}/imgui/imgui.h" CONTENT "#include <imgui.h>\n")
file(CONFIGURE OUTPUT "${rasm_cimgui_shim}/imgui/imgui_internal.h" CONTENT "#include <imgui_internal.h>\n")

add_library(rasm_cimgui SHARED "${rasm_cimgui_source_SOURCE_DIR}/cimgui.cpp")
# Third-party code: no clang-tidy (SKIP_LINTING needs CMake 3.27, so the tidy command is cleared too).
set_target_properties(rasm_cimgui PROPERTIES OUTPUT_NAME cimgui SKIP_LINTING ON CXX_CLANG_TIDY "")
target_include_directories(rasm_cimgui PRIVATE "${rasm_cimgui_shim}")
target_include_directories(rasm_cimgui SYSTEM INTERFACE "${rasm_cimgui_source_SOURCE_DIR}")
target_link_libraries(rasm_cimgui PRIVATE imgui::imgui)
# Third-party code: none of RASM_WARNINGS.
target_compile_options(rasm_cimgui PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
