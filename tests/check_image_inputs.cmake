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
Texture "paint" "spectrum" "imagemap" "string filename" "input.png" "string filter" "trilinear"
Material "diffuse" "texture reflectance" "paint"
Shape "trianglemesh" "point3 P" [-1 -1 2 1 -1 2 0 1 2]
]=])
file(WRITE "${input}" "${scene}")
execute_process(COMMAND "${executable}" --check-pbrt "${input}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "0" OR NOT stderr STREQUAL "")
    message(FATAL_ERROR "Image preflight failed: ${exit}: ${stdout}${stderr}")
endif()
foreach(image IN ITEMS s35n3p04.png tp1n3p08.png basn2c16.png basn0g16.png)
    file(COPY_FILE "${fixture_directory}/image-assets/${image}" "${asset}")
    execute_process(COMMAND "${executable}" --check-pbrt "${input}"
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT exit STREQUAL "0" OR NOT stderr STREQUAL "")
        message(FATAL_ERROR "Image preflight failed for ${image}: ${exit}: ${stdout}${stderr}")
    endif()
endforeach()
string(REPLACE "\"trilinear\"" "\"ewa\"" ewa "${scene}")
file(WRITE "${input}" "${ewa}")
execute_process(COMMAND "${executable}" --check-pbrt "${input}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "0" OR NOT stderr STREQUAL "")
    message(FATAL_ERROR "EWA preflight failed: ${exit}: ${stdout}${stderr}")
endif()
file(APPEND "${input}"
    "\nTexture \"unused\" \"spectrum\" \"imagemap\" \"string filename\" \"input.png\" \"string filter\" \"EWA\" \"float maxanisotropy\" 17\n")
foreach(mode IN ITEMS --check-pbrt --render-pbrt)
    set(arguments "${mode}" "${input}")
    if(mode STREQUAL "--render-pbrt")
        list(APPEND arguments --output "${output}")
    endif()
    execute_process(COMMAND "${executable}" ${arguments}
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT exit STREQUAL "2" OR NOT stdout MATCHES "EWA maxanisotropy" OR
        stdout MATCHES "Device:|Committed samples|Wrote PNG")
        message(FATAL_ERROR "Unsupported EWA cap reached execution: ${exit}: ${stdout}${stderr}")
    endif()
    file(READ "${output}" current)
    if(NOT current STREQUAL sentinel)
        message(FATAL_ERROR "Unsupported image render replaced existing output")
    endif()
endforeach()
file(WRITE "${input}" "${scene}")
# Invalid or over-limit inputs must fail even in an unused definition.
file(COPY_FILE "${fixture_directory}/image-assets/basn2c16.png" "${test_directory}/broken16.png")
file(APPEND "${test_directory}/broken16.png" "invalid trailing bytes")
file(COPY "${fixture_directory}/image-assets/invalid-index.png"
          "${fixture_directory}/image-assets/over-limit-indexed.png"
    DESTINATION "${test_directory}")
file(APPEND "${input}" "\nInclude \"unused.pbrt\"\n")
foreach(image IN ITEMS broken16.png invalid-index.png over-limit-indexed.png)
    set(code invalid-image)
    if(image STREQUAL "over-limit-indexed.png")
        set(code limit-exceeded)
    endif()
    file(WRITE "${test_directory}/unused.pbrt"
        "# Unused image definition\nTexture \"unused\" \"spectrum\" \"imagemap\" \"string filename\" \"${image}\"\n")
    foreach(mode IN ITEMS --check-pbrt --render-pbrt)
        set(arguments "${mode}" "${input}")
        if(mode STREQUAL "--render-pbrt")
            list(APPEND arguments --output "${output}")
        endif()
        execute_process(COMMAND "${executable}" ${arguments}
            RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
        if(NOT exit STREQUAL "1" OR
            NOT stdout MATCHES "unused[.]pbrt:2:[0-9]+: error [[]${code}[]]" OR
            stdout MATCHES "Device:|Committed samples|Wrote PNG")
            message(FATAL_ERROR "${image} was not rejected at its definition: ${exit}: ${stdout}${stderr}")
        endif()
        file(READ "${output}" current)
        if(NOT current STREQUAL sentinel)
            message(FATAL_ERROR "Invalid image render replaced existing output")
        endif()
    endforeach()
endforeach()
