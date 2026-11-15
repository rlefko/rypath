# SPDX-License-Identifier: GPL-3.0-only
file(MAKE_DIRECTORY "${test_directory}")
set(output "${test_directory}/result.exr")
set(sentinel "existing linear output")
set(options [=[Camera "perspective"
Film "rgb" "integer xresolution" 8 "integer yresolution" 8
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 1
Integrator "path"
WorldBegin
]=])
set(shape [=[
Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1]
]=])
set(cases
    "rgb eta|[-1 1 1]|1"
    "blackbody eta|-3000|1"
    "float eta|-1|1"
    "float eta|1e100|1"
    "float eta|1e-100|1"
    "spectrum eta|[360 -1 830 1]|1"
    "spectrum eta|[360 1e100 830 1]|1"
    "spectrum eta|\"missing-optical.spd\"|1"
    "texture roughness|\"undefined-roughness\"|1")
set(index 0)
foreach(case IN LISTS cases)
    string(REPLACE "|" ";" fields "${case}")
    list(GET fields 0 parameter)
    list(GET fields 1 value)
    list(GET fields 2 expected)
    set(variant "${options}Material \"dielectric\" \"${parameter}\" ${value}\n${shape}")
    set(scene "${test_directory}/rejected-${index}.pbrt")
    file(WRITE "${scene}" "${variant}")
    file(WRITE "${output}" "${sentinel}")
    execute_process(COMMAND "${executable}" --render-pbrt "${scene}" --output "${output}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    file(READ "${output}" current)
    set(diagnostic "${stdout}${stderr}")
    if(NOT result STREQUAL expected OR NOT current STREQUAL sentinel OR
       NOT stdout MATCHES "Rendering was not attempted" OR
       stdout MATCHES "Device:|Committed samples|Wrote" OR
       NOT diagnostic MATCHES "rejected-${index}\\.pbrt:[0-9]+:[0-9]+")
        message(FATAL_ERROR "Rejected dielectric case ${index}: ${result}: ${diagnostic}")
    endif()
    math(EXPR index "${index}+1")
endforeach()

string(REPLACE "Sampler \"independent\"" "Sampler \"sobol\"" options "${options}")
string(REPLACE "Integrator \"path\"" "Integrator \"path\" \"integer maxdepth\" 114"
    options "${options}")
set(scene "${test_directory}/depth.pbrt")
file(WRITE "${scene}" "${options}MakeNamedMaterial \"unused\" \"string type\" \"dielectric\"\n${shape}")
file(WRITE "${output}" "${sentinel}")
execute_process(COMMAND "${executable}" --render-pbrt "${scene}" --output "${output}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
file(READ "${output}" current)
if(NOT result STREQUAL "2" OR NOT current STREQUAL sentinel OR
   NOT stdout MATCHES "Rendering was not attempted" OR
   stdout MATCHES "Device:|Committed samples|Wrote" OR
   NOT "${stdout}${stderr}" MATCHES "depth\\.pbrt:5:[0-9]+" OR
   NOT "${stdout}${stderr}" MATCHES "limit of 113")
    message(FATAL_ERROR "Dielectric Sobol depth rejection: ${result}: ${stdout}${stderr}")
endif()
