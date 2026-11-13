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

string(CONCAT help_text
    "Usage: rypath [--help | --version | --metal-info | --metal-smoke-test]\n"
    "       rypath --check-pbrt SCENE\n"
    "       rypath --render-pbrt SCENE --output IMAGE.png [--batch-size 8]\n"
    "       rypath --render-demo --output PATH [--width 256] [--height 192]\n"
    "              [--samples 128] [--batch-size 8] [--seed 1]\n")
set(error_text "rypath: unsupported arguments\nTry 'rypath --help' for usage.\n")

add_cli_test(no_arguments 0 "${help_text}" "")
add_cli_test(help 0 "${help_text}" "" --help)
add_cli_test(version 0 "rypath ${PROJECT_VERSION}\n" "" --version)
add_cli_test(unknown_argument 1 "" "${error_text}" --unknown)
add_cli_test(extra_argument 1 "" "${error_text}" --help --version)
add_cli_test(metal_combined_arguments 1 "" "${error_text}" --metal-info --metal-smoke-test)
add_cli_test(metal_extra_argument 1 "" "${error_text}" --metal-smoke-test extra)
add_cli_test(pbrt_missing_scene 1 "" "${error_text}" --check-pbrt)
add_cli_test(pbrt_extra_argument 1 "" "${error_text}" --check-pbrt input.pbrt --version)
add_cli_test(pbrt_option_as_scene 1 "" "${error_text}" --check-pbrt --version)
add_cli_test(pbrt_combined_modes 1 "" "${error_text}" --version --check-pbrt input.pbrt)
add_cli_test(render_missing_output 1 "" "rypath: --render-demo requires --output PATH\n"
    --render-demo)
add_cli_test(render_duplicate 1 "" "rypath: duplicate option --seed\n"
    --render-demo --output unused.png --seed 1 --seed 2)
add_cli_test(render_unknown 1 "" "${error_text}"
    --render-demo --output unused.png --unknown value)
add_cli_test(render_combined_modes 1 "" "${error_text}"
    --metal-info --render-demo --output unused.png)
add_cli_test(render_malformed_width 1 "" "rypath: invalid unsigned integer for --width\n"
    --render-demo --output unused.png --width 12px)
add_cli_test(render_width_overflow 1 "" "rypath: invalid unsigned integer for --width\n"
    --render-demo --output unused.png --width 4294967296)
add_cli_test(render_seed_overflow 1 "" "rypath: invalid unsigned integer for --seed\n"
    --render-demo --output unused.png --seed 18446744073709551616)
add_cli_test(render_total_overflow 1 "" "rypath: Total render sample count exceeds 64-bit capacity\n"
    --render-demo --output unused.png --width 2 --height 1 --samples 18446744073709551615)
add_cli_test(render_signed_samples 1 "" "rypath: invalid unsigned integer for --samples\n"
    --render-demo --output unused.png --samples -1)
add_cli_test(render_pbrt_missing_scene 1 "" "${error_text}" --render-pbrt)
add_cli_test(render_pbrt_missing_output 1 "" "rypath: --render-pbrt requires --output IMAGE.png\n"
    --render-pbrt unused.pbrt)
add_cli_test(render_pbrt_duplicate 1 "" "rypath: duplicate option --batch-size\n"
    --render-pbrt unused.pbrt --output unused.png --batch-size 1 --batch-size 2)
add_cli_test(render_pbrt_zero_batch 1 "" "rypath: --batch-size must be a positive unsigned integer\n"
    --render-pbrt unused.pbrt --output unused.png --batch-size 0)
add_cli_test(render_pbrt_output_format 1 "" "rypath: --render-pbrt output must have the .png extension\n"
    --render-pbrt unused.pbrt --output unused.exr)
add_cli_test(render_pbrt_unknown 1 "" "${error_text}"
    --render-pbrt unused.pbrt --output unused.png --samples 4)

add_test(NAME cli.render_demo
    COMMAND "${CMAKE_COMMAND}"
        "-Dexecutable=$<TARGET_FILE:rypath>"
        "-Doutput=${CMAKE_CURRENT_BINARY_DIR}/render-demo/$<CONFIG>/demo.png"
        "-Dmetal_enabled=${RYPATH_ENABLE_METAL}"
        "-Dallow_no_device=${RYPATH_ALLOW_NO_METAL_DEVICE}"
        "-Dallow_no_ray_tracing=${RYPATH_ALLOW_NO_METAL_RAY_TRACING}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_render_cli.cmake"
)
set_tests_properties(cli.render_demo PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL AND (RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING))
    set_tests_properties(cli.render_demo PROPERTIES SKIP_REGULAR_EXPRESSION
        "RYPATH_CTEST_SKIP_RENDER_UNAVAILABLE")
endif()

set(pbrt_fixture_directory "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt")
set(pbrt_test_directory "${CMAKE_CURRENT_BINARY_DIR}/pbrt")
add_test(NAME cli.pbrt_preflight
    COMMAND "${CMAKE_COMMAND}"
        "-Dexecutable=$<TARGET_FILE:rypath>"
        "-Dfixture_directory=${pbrt_fixture_directory}"
        "-Dtest_directory=${pbrt_test_directory}/$<CONFIG>/cli"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_pbrt_cli.cmake"
)
set_tests_properties(cli.pbrt_preflight PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_tests pbrt_tests.cpp)
target_link_libraries(rypath_pbrt_tests PRIVATE rypath_pbrt)
rypath_configure_host_target(rypath_pbrt_tests)
foreach(group IN ITEMS syntax state dependencies limits geometry corpus)
    add_test(NAME "pbrt.${group}" COMMAND rypath_pbrt_tests "${group}"
        "${pbrt_test_directory}/$<CONFIG>/${group}" "${pbrt_fixture_directory}")
    set_tests_properties("pbrt.${group}" PROPERTIES TIMEOUT 30)
endforeach()

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

add_executable(rypath_render_tests render_tests.cpp render_reference.cpp)
set_target_properties(rypath_render_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_include_directories(rypath_render_tests PRIVATE "${rypath_generated_directory}")
target_link_libraries(rypath_render_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_render_tests)
add_test(NAME render.cpu_reference COMMAND rypath_render_tests cpu)
set_tests_properties(render.cpu_reference PROPERTIES TIMEOUT 30)

add_executable(rypath_sampling_tests sampling_tests.cpp)
target_link_libraries(rypath_sampling_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_sampling_tests)
add_test(NAME render.sobol_sampling COMMAND rypath_sampling_tests)
set_tests_properties(render.sobol_sampling PROPERTIES TIMEOUT 30)

set(ray_tracing_test_options)
if(RYPATH_ALLOW_NO_METAL_DEVICE)
    list(APPEND ray_tracing_test_options --allow-no-device)
endif()
if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
    list(APPEND ray_tracing_test_options --allow-no-ray-tracing)
endif()

set(pbrt_render_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt-render")
add_executable(rypath_rgb_spectrum_tests rgb_spectrum_tests.cpp)
target_link_libraries(rypath_rgb_spectrum_tests PRIVATE rypath_rgb_spectrum)
rypath_configure_host_target(rypath_rgb_spectrum_tests)
add_test(NAME render.rgb_spectrum COMMAND rypath_rgb_spectrum_tests)
set_tests_properties(render.rgb_spectrum PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_render_tests pbrt_render_tests.cpp)
target_link_libraries(rypath_pbrt_render_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_pbrt_render_tests)
add_test(NAME pbrt.render_translation COMMAND rypath_pbrt_render_tests
    "${CMAKE_CURRENT_BINARY_DIR}/pbrt-render/$<CONFIG>" "${pbrt_render_fixtures}")
set_tests_properties(pbrt.render_translation PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_reference pbrt_reference.cpp)
set_target_properties(rypath_pbrt_reference PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_pbrt_reference PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_reference)

add_executable(rypath_pbrt_render_gpu_tests pbrt_render_gpu_tests.cpp)
set_target_properties(rypath_pbrt_render_gpu_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_include_directories(rypath_pbrt_render_gpu_tests PRIVATE "${rypath_generated_directory}")
target_link_libraries(rypath_pbrt_render_gpu_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_render_gpu_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.pbrt_render COMMAND rypath_pbrt_render_gpu_tests
        "${pbrt_render_fixtures}" ${ray_tracing_test_options})
    set_tests_properties(metal.pbrt_render PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.pbrt_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_pbrt_area_tests pbrt_area_tests.cpp area_reference.cpp)
set_target_properties(rypath_pbrt_area_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_include_directories(rypath_pbrt_area_tests PRIVATE "${rypath_generated_directory}")
target_link_libraries(rypath_pbrt_area_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_area_tests)
add_test(NAME render.area_reference COMMAND rypath_pbrt_area_tests
    --reference "${pbrt_render_fixtures}")
set_tests_properties(render.area_reference PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.pbrt_area COMMAND rypath_pbrt_area_tests --metal ${ray_tracing_test_options})
    set_tests_properties(metal.pbrt_area PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.pbrt_area PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_pbrt_filter_tests pbrt_filter_tests.cpp)
set_target_properties(rypath_pbrt_filter_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_pbrt_filter_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_filter_tests)
add_test(NAME render.pbrt_filter_reference COMMAND rypath_pbrt_filter_tests
    --reference "${pbrt_render_fixtures}")
set_tests_properties(render.pbrt_filter_reference PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.pbrt_filter COMMAND rypath_pbrt_filter_tests --metal ${ray_tracing_test_options})
    set_tests_properties(metal.pbrt_filter PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.pbrt_filter PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_triangle_attributes_tests triangle_attributes_tests.cpp)
target_link_libraries(rypath_triangle_attributes_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_triangle_attributes_tests)
add_test(NAME render.triangle_attributes COMMAND rypath_triangle_attributes_tests)
set_tests_properties(render.triangle_attributes PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_attribute_tests pbrt_attribute_tests.cpp attribute_reference.cpp)
set_target_properties(rypath_pbrt_attribute_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_pbrt_attribute_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_attribute_tests)
add_test(NAME render.pbrt_attribute_reference COMMAND rypath_pbrt_attribute_tests
    --reference "${pbrt_render_fixtures}")
set_tests_properties(render.pbrt_attribute_reference PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.pbrt_attributes COMMAND rypath_pbrt_attribute_tests
        --metal ${ray_tracing_test_options})
    set_tests_properties(metal.pbrt_attributes PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.pbrt_attributes PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_test(NAME cli.render_pbrt COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath>"
    "-Dfixture_directory=${pbrt_render_fixtures}"
    "-Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/pbrt-render/$<CONFIG>/cli"
    "-Dmetal_enabled=${RYPATH_ENABLE_METAL}"
    "-Dallow_no_device=${RYPATH_ALLOW_NO_METAL_DEVICE}"
    "-Dallow_no_ray_tracing=${RYPATH_ALLOW_NO_METAL_RAY_TRACING}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_pbrt_render_cli.cmake")
set_tests_properties(cli.render_pbrt PROPERTIES TIMEOUT 60)
if(RYPATH_ENABLE_METAL AND (RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING))
    set_tests_properties(cli.render_pbrt PROPERTIES SKIP_REGULAR_EXPRESSION
        "RYPATH_CTEST_SKIP_RENDER_UNAVAILABLE")
endif()

if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.render COMMAND rypath_render_tests metal ${ray_tracing_test_options})
    set_tests_properties(metal.render PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.render PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    add_executable(rypath_sobol_render_tests sobol_render_tests.cpp sobol_probe.mm)
    set_target_properties(rypath_sobol_render_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_sobol_render_tests PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_compile_definitions(rypath_sobol_render_tests PRIVATE
        "$<$<BOOL:${RYPATH_ALLOW_NO_METAL_DEVICE}>:RYPATH_ALLOW_NO_METAL_DEVICE=1>"
        "$<$<BOOL:${RYPATH_ALLOW_NO_METAL_RAY_TRACING}>:RYPATH_ALLOW_NO_METAL_RAY_TRACING=1>")
    target_link_libraries(rypath_sobol_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
    rypath_configure_host_target(rypath_sobol_render_tests)
    add_test(NAME metal.sobol_render COMMAND rypath_sobol_render_tests)
    set_tests_properties(metal.sobol_render PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.sobol_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    add_executable(rypath_image_output_tests image_output_tests.mm)
    set_target_properties(rypath_image_output_tests PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_image_output_tests PRIVATE -fobjc-arc)
    target_link_libraries(rypath_image_output_tests PRIVATE rypath_metal_backend)
    rypath_configure_host_target(rypath_image_output_tests)
    add_test(NAME render.png COMMAND rypath_image_output_tests)
    set_tests_properties(render.png PROPERTIES TIMEOUT 30)
else()
    add_test(NAME render.stub COMMAND rypath_render_tests stub)
    set_tests_properties(render.stub PROPERTIES TIMEOUT 30)
endif()

add_executable(rypath_geometry_tests geometry_tests.cpp geometry_reference.cpp)
set_target_properties(rypath_geometry_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_geometry_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_geometry_tests)

add_test(NAME geometry.cpu_reference COMMAND rypath_geometry_tests cpu)
set_tests_properties(geometry.cpu_reference PROPERTIES TIMEOUT 30)

if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.geometry COMMAND rypath_geometry_tests metal ${ray_tracing_test_options})
    set_tests_properties(metal.geometry PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.geometry PROPERTIES SKIP_RETURN_CODE 77)
    endif()
else()
    add_test(NAME geometry.stub COMMAND rypath_geometry_tests stub)
    set_tests_properties(geometry.stub PROPERTIES TIMEOUT 30)
endif()
