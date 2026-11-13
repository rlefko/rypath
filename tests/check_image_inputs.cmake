# SPDX-License-Identifier: GPL-3.0-only
file(MAKE_DIRECTORY "${test_directory}")
set(output "${test_directory}/result.png")
set(input "${test_directory}/input.pbrt")
set(asset "${test_directory}/input.png")
set(sentinel "preserved output")
file(WRITE "${output}" "${sentinel}")
file(COPY_FILE "${fixture_directory}/image-assets/asymmetric.png" "${asset}")
set(scene [=[
Camera "perspective"
Film "rgb" "integer xresolution" 2 "integer yresolution" 2
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 1
Integrator "path" "integer maxdepth" 1
WorldBegin
Texture "paint" "spectrum" "imagemap" "string filename" "input.png"
Material "diffuse" "texture reflectance" "paint"
Shape "trianglemesh" "point3 P" [-1 -1 2 1 -1 2 0 1 2]
]=])
file(WRITE "${input}" "${scene}")
execute_process(COMMAND "${executable}" --check-pbrt "${input}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "0" OR NOT stderr STREQUAL "")
    message(FATAL_ERROR "Image preflight failed: ${exit}: ${stdout}${stderr}")
endif()
string(REPLACE "\"imagemap\"" "\"imagemap\" \"string filter\" \"ewa\"" unsupported "${scene}")
file(WRITE "${input}" "${unsupported}")
execute_process(COMMAND "${executable}" --render-pbrt "${input}" --output "${output}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "2" OR NOT stdout MATCHES "unsupported" OR
    stdout MATCHES "Device:|Committed samples|Wrote PNG")
    message(FATAL_ERROR "Unsupported image render reached execution: ${exit}: ${stdout}${stderr}")
endif()
file(READ "${output}" current)
if(NOT current STREQUAL sentinel)
    message(FATAL_ERROR "Unsupported image render replaced existing output")
endif()
file(WRITE "${input}" "${scene}")
file(WRITE "${asset}" "invalid PNG")
foreach(mode IN ITEMS --check-pbrt --render-pbrt)
    set(arguments "${mode}" "${input}")
    if(mode STREQUAL "--render-pbrt")
        list(APPEND arguments --output "${output}")
    endif()
    execute_process(COMMAND "${executable}" ${arguments}
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT exit STREQUAL "1" OR NOT stdout MATCHES "invalid-image" OR
        stdout MATCHES "Device:|Committed samples|Wrote PNG")
        message(FATAL_ERROR "Malformed image was not rejected before execution: ${exit}: ${stdout}${stderr}")
    endif()
    file(READ "${output}" current)
    if(NOT current STREQUAL sentinel)
        message(FATAL_ERROR "Malformed image render replaced existing output")
    endif()
endforeach()
