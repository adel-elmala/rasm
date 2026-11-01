include(RasmRuntimeDlls)

find_package(doctest CONFIG REQUIRED)

add_executable(rasm_tests
            "main.cpp"
            "cameraTests.cpp"
            "handleTests.cpp"
            "renderGraphTests.cpp"
            "resourceDescTests.cpp"
            "transformTests.cpp"
    )

target_link_libraries(rasm_tests PRIVATE rasm::rasm doctest::doctest)
rasm_copy_runtime_dlls(rasm_tests)

add_test(NAME rasm_tests COMMAND rasm_tests)
