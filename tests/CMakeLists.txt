function(add_cli_test name expected_exit expected_stdout expected_stderr)
    add_test(NAME "cli.${name}"
        COMMAND "${CMAKE_COMMAND}"
            "-Dexecutable=$<TARGET_FILE:rypath>"
            "-Darguments=${ARGN}"
            "-Dexpected_exit=${expected_exit}"
            "-Dexpected_stdout=${expected_stdout}"
            "-Dexpected_stderr=${expected_stderr}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli.cmake"
    )
    set_tests_properties("cli.${name}" PROPERTIES TIMEOUT 30)
endfunction()

set(help_text "Usage: rypath [--help | --version | --metal-info | --metal-smoke-test]\n")
set(error_text "rypath: unsupported arguments\nTry 'rypath --help' for usage.\n")

add_cli_test(no_arguments 0 "${help_text}" "")
add_cli_test(help 0 "${help_text}" "" --help)
add_cli_test(version 0 "rypath ${PROJECT_VERSION}\n" "" --version)
add_cli_test(unknown_argument 1 "" "${error_text}" --unknown)
add_cli_test(extra_argument 1 "" "${error_text}" --help --version)
add_cli_test(metal_combined_arguments 1 "" "${error_text}" --metal-info --metal-smoke-test)
add_cli_test(metal_extra_argument 1 "" "${error_text}" --metal-smoke-test extra)

set(metal_cli_directory "${CMAKE_CURRENT_BINARY_DIR}/metal-cli")
file(MAKE_DIRECTORY "${metal_cli_directory}")
foreach(command IN ITEMS metal-info metal-smoke-test)
    string(REPLACE "-" "_" test_name "cli.${command}")
    add_test(NAME "${test_name}"
        COMMAND "${CMAKE_COMMAND}"
            "-Dexecutable=$<TARGET_FILE:rypath>"
            "-Dargument=--${command}"
            "-Dmetal_enabled=${RYPATH_ENABLE_METAL}"
            "-Dallow_no_device=${RYPATH_ALLOW_NO_METAL_DEVICE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_metal_cli.cmake"
    )
    set_tests_properties("${test_name}" PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${metal_cli_directory}"
    )
endforeach()

add_executable(rypath_metal_tests metal_tests.cpp)
set_target_properties(rypath_metal_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_metal_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_metal_tests)

if(RYPATH_ENABLE_METAL)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/invalid.metallib"
        CONTENT "This is not a Metal library.\n")
    add_test(NAME metal.api_smoke COMMAND rypath_metal_tests smoke)
    add_test(NAME metal.missing_library COMMAND rypath_metal_tests missing-library
        "${CMAKE_CURRENT_BINARY_DIR}/missing.metallib")
    add_test(NAME metal.invalid_library COMMAND rypath_metal_tests invalid-library
        "${CMAKE_CURRENT_BINARY_DIR}/invalid.metallib")
    set_tests_properties(metal.api_smoke metal.missing_library metal.invalid_library
        PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        set_tests_properties(metal.api_smoke metal.missing_library metal.invalid_library
            PROPERTIES SKIP_RETURN_CODE 77)
        # The CMake CLI validator emits this marker only after checking the no-device result.
        set_tests_properties(cli.metal_smoke_test PROPERTIES
            SKIP_REGULAR_EXPRESSION "RYPATH_CTEST_SKIP_NO_METAL_DEVICE")
    endif()
else()
    add_test(NAME metal.api_stub COMMAND rypath_metal_tests stub)
    set_tests_properties(metal.api_stub PROPERTIES TIMEOUT 30)
endif()
