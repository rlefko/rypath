# SPDX-License-Identifier: GPL-3.0-only
file(MAKE_DIRECTORY "${test_directory}")
file(READ "${fixture}" source)
set(output "${test_directory}/result.exr")
set(sentinel "existing linear output")
set(cases
    "rgb eta|[-1 1 1]|1"
    "spectrum eta|[360 -1 830 -1]|1"
    "spectrum eta|[360 0 830 0] \"spectrum k\" [360 0 830 0]|1"
    "spectrum eta|\"missing-optical.spd\"|1"
    "rgb reflectance|[-0.5 0.5 0.5]|1"
    "texture roughness|\"undefined-roughness\"|1")
set(index 0)
foreach(case IN LISTS cases)
    string(REPLACE "|" ";" fields "${case}")
    list(GET fields 0 parameter)
    list(GET fields 1 value)
    list(GET fields 2 expected)
    string(REPLACE "\"float roughness\" [0]"
        "\"float roughness\" [0] \"${parameter}\" ${value}" variant "${source}")
    set(scene "${test_directory}/rejected-${index}.pbrt")
    file(WRITE "${scene}" "${variant}")
    file(WRITE "${output}" "${sentinel}")
    execute_process(COMMAND "${executable}" --render-pbrt "${scene}" --output "${output}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    file(READ "${output}" current)
    if(NOT result STREQUAL expected OR NOT current STREQUAL sentinel OR
       NOT stdout MATCHES "Rendering was not attempted" OR
       stdout MATCHES "Device:|Committed samples|Wrote")
        message(FATAL_ERROR "Rejected conductor case ${index}: ${result}: ${stdout}${stderr}")
    endif()
    math(EXPR index "${index}+1")
endforeach()
