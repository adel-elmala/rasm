# LASzip (Apache-2.0), which RASM reads LAZ files with. Its vcpkg port installs no CMake package: this finds its header
# and library and makes the target laszip::laszip, with both include folders (laszip_api.h includes laszip_common.h
# without the folder). rasmConfig.cmake uses it too, as RASM is a static library.
find_path(laszip_INCLUDE_DIR laszip/laszip_api.h)
find_library(laszip_LIBRARY NAMES laszip3 laszip)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(laszip REQUIRED_VARS laszip_LIBRARY laszip_INCLUDE_DIR)

if (laszip_FOUND AND NOT TARGET laszip::laszip)
    add_library(laszip::laszip UNKNOWN IMPORTED)
    set_target_properties(laszip::laszip PROPERTIES
        IMPORTED_LOCATION "${laszip_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${laszip_INCLUDE_DIR};${laszip_INCLUDE_DIR}/laszip")
endif()
mark_as_advanced(laszip_INCLUDE_DIR laszip_LIBRARY)
