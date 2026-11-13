cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/cli_availability.cmake")

file(MAKE_DIRECTORY "${test_directory}")
set(testfile "${test_directory}/CTestTestfile.cmake")
file(WRITE "${testfile}" "")

function(add_case name validator expected metal_enabled allow_no_device allow_no_ray_tracing)
    set(directory "${test_directory}/${name}")
    file(MAKE_DIRECTORY "${directory}")
    if(validator STREQUAL "metal")
        set(marker "${metal_skip_marker}")
    else()
        set(marker "${render_skip_marker}")
    endif()
    get_cli_skip_regex(skip_regex "${marker}" "${metal_enabled}"
        "${allow_no_device}" "${allow_no_ray_tracing}")
    file(WRITE "${directory}/marker.txt" "${marker}\n")
    file(APPEND "${testfile}" "add_test(${name} [==[${CMAKE_COMMAND}]==]
        [==[-Dexecutable=${executable}]==] -Dargument=--metal-smoke-test
        [==[-Dtest_directory=${directory}]==] [==[-Doutput=${directory}/demo.png]==]
        -Dmetal_enabled=${metal_enabled} -Dallow_no_device=${allow_no_device}
        -Dallow_no_ray_tracing=${allow_no_ray_tracing}
        -P [==[${CMAKE_CURRENT_LIST_DIR}/check_${validator}_cli.cmake]==])
set_tests_properties(${name} PROPERTIES
    WORKING_DIRECTORY [==[${directory}]==] TIMEOUT 10
    SKIP_REGULAR_EXPRESSION [==[${skip_regex}]==])
")
    set("${name}_expected" "${expected}" PARENT_SCOPE)
    list(APPEND cases "${name}")
    set(cases "${cases}" PARENT_SCOPE)
endfunction()

set(cases)
add_case(demo-success render run ON ON ON)
add_case(demo-portable render run OFF ON ON)
add_case(demo-device render notrun ON ON OFF)
add_case(demo-ray-tracing render notrun ON OFF ON)
add_case(demo-device-wrong-flag render fail ON OFF ON)
add_case(demo-ray-tracing-wrong-flag render fail ON ON OFF)
add_case(demo-strict render fail ON OFF OFF)
add_case(demo-portable-device render fail OFF ON ON)
foreach(scenario IN ITEMS marker prefixed-status suffixed-status incomplete-status duplicate-status
        conflicting-status extra-status missing-status status-in-stderr unexpected-not-built
        wrong-exit changed-output progress total-samples wrong-diagnostic extra-stderr)
    add_case("demo-${scenario}" render fail ON ON ON)
endforeach()

add_case(metal-device metal notrun ON ON OFF)
add_case(metal-strict metal fail ON OFF OFF)
add_case(metal-marker metal fail ON ON OFF)

add_case(pbrt-success pbrt_render run ON ON ON)
add_case(pbrt-portable pbrt_render run OFF ON ON)
add_case(pbrt-device pbrt_render notrun ON ON OFF)
add_case(pbrt-ray-tracing pbrt_render notrun ON OFF ON)
add_case(pbrt-marker pbrt_render fail ON ON OFF)
add_case(pbrt-independent-success pbrt_render fail ON ON OFF)
add_case(pbrt-sobol-success pbrt_render fail ON ON OFF)
add_case(pbrt-conflicting-status pbrt_render fail ON ON ON)
add_case(pbrt-total-samples pbrt_render fail ON ON OFF)
add_case(pbrt-wrong-diagnostic pbrt_render fail ON ON OFF)

set(report_path "${test_directory}/results.xml")
file(REMOVE "${report_path}")
execute_process(COMMAND "${ctest}" --test-dir "${test_directory}" --parallel 2
    --output-junit "${report_path}" --no-tests=error
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "8" OR NOT EXISTS "${report_path}")
    message(FATAL_ERROR "Unexpected nested CTest exit ${exit}:\n${stdout}${stderr}")
endif()

file(READ "${report_path}" report)
foreach(name IN LISTS cases)
    string(FIND "${report}" "<testcase name=\"${name}\"" start)
    if(start EQUAL -1)
        message(FATAL_ERROR "Missing nested CTest result for ${name}:\n${stdout}${stderr}")
    endif()
    string(SUBSTRING "${report}" "${start}" -1 testcase)
    string(FIND "${testcase}" "</testcase>" end)
    if(end EQUAL -1)
        message(FATAL_ERROR "Incomplete nested CTest result for ${name}:\n${testcase}")
    endif()
    string(SUBSTRING "${testcase}" 0 "${end}" testcase)
    set(expected "${${name}_expected}")
    if(NOT testcase MATCHES "^[^>]+ status=\"${expected}\"" OR
       (expected STREQUAL "notrun" AND NOT testcase MATCHES
           "<skipped message=\"SKIP_REGULAR_EXPRESSION_MATCHED\"") OR
       (expected STREQUAL "fail" AND NOT testcase MATCHES "<system-out>CMake Error"))
        message(FATAL_ERROR "Expected ${name} to report ${expected}:\n${testcase}")
    endif()
    if(name MATCHES "-marker$")
        file(READ "${test_directory}/${name}/marker.txt" marker)
        string(STRIP "${marker}" marker)
        if(NOT testcase MATCHES "${marker}")
            message(FATAL_ERROR "Missing simulated child marker for ${name}:\n${testcase}")
        endif()
    endif()
endforeach()
