include("${CMAKE_CURRENT_LIST_DIR}/cli_availability.cmake")

file(MAKE_DIRECTORY "${test_directory}")
set(output "${test_directory}/result.png")
set(sentinel "existing output")
file(WRITE "${output}" "${sentinel}")
file(WRITE "${test_directory}/valid.pbrt" [=[
Film "rgb" "integer xresolution" 8 "integer yresolution" 6
    "string filename" "must-not-be-created.png" "bool savefp16" true
Camera "perspective" "float fov" 45
PixelFilter "triangle" "float xradius" 1 "float yradius" 1.5
Sampler "independent" "integer pixelsamples" 3 "integer seed" 0
Integrator "path" "integer maxdepth" 1
WorldBegin
LightSource "infinite" "rgb L" [1 0.7 0.2]
Material "diffuse" "rgb reflectance" [0.1 0.5 0.3]
AreaLightSource "diffuse" "bool twosided" true
Shape "trianglemesh" "point3 P" [-1 -1 3 1 -1 3 0 1 3]
    "normal N" [-1 0 2 1 0 2 0 1 3] "point2 uv" [-1 0 1 0 0 2]
]=])

function(expect_existing_output)
    file(READ "${output}" current)
    if(NOT current STREQUAL sentinel)
        message(FATAL_ERROR "Existing output changed after a rejected render")
    endif()
endfunction()

# Raw output aliases must fail before GPU acquisition and preserve every input.
set(raw_alias "${test_directory}/alias.exr")
file(WRITE "${raw_alias}" "${sentinel}")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/valid.pbrt"
    --output "${raw_alias}" --raw-output "${raw_alias}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "1" OR NOT stderr MATCHES "raw output must not alias" OR
   stdout MATCHES "Device:|Committed samples|Wrote")
    message(FATAL_ERROR "Raw output alias was not rejected: ${exit} ${stdout}${stderr}")
endif()
file(READ "${raw_alias}" preserved)
if(NOT preserved STREQUAL sentinel)
    message(FATAL_ERROR "Raw output alias changed its destination")
endif()
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/valid.pbrt"
    --output "${output}" --raw-output "${raw_alias}" --checkpoint "${raw_alias}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "1" OR stdout MATCHES "Device:|Committed samples|Wrote")
    message(FATAL_ERROR "Raw/checkpoint alias was not rejected: ${exit} ${stdout}${stderr}")
endif()
expect_existing_output()

# Authored Film.savefp16 does not change the raw FLOAT export.
set(raw_output "${test_directory}/raw.exr")
file(WRITE "${raw_output}" "${sentinel}")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/valid.pbrt"
    --output "${output}" --raw-output "${raw_output}" --batch-size 2
    --checkpoint "${test_directory}/raw.rycp"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(exit STREQUAL "2")
    get_render_availability(availability "${stdout}" "${stderr}" "")
    if(NOT availability STREQUAL "")
        expect_existing_output()
        file(READ "${raw_output}" preserved)
        if(NOT preserved STREQUAL sentinel)
            message(FATAL_ERROR "Unavailable backend modified raw output")
        endif()
        if(availability STREQUAL "no_device" OR availability STREQUAL "no_ray_tracing")
            message("${render_skip_marker}\nRender status: ${availability}")
        endif()
        return()
    endif()
endif()
if(NOT exit STREQUAL "0" OR NOT stdout MATCHES "Raw output: .*EXR FLOAT; unexposed; unclamped" OR
   NOT stderr STREQUAL "")
    message(FATAL_ERROR "Unexpected raw export: ${exit} ${stdout}${stderr}")
endif()
file(READ "${raw_output}" signature LIMIT 4 HEX)
file(STRINGS "${raw_output}" raw_strings)
if(NOT signature STREQUAL "762f3101" OR NOT raw_strings MATCHES "rypath.filmOutput" OR
   NOT raw_strings MATCHES "unclamped unexposed raw Monte Carlo estimate" OR
   NOT raw_strings MATCHES "rypath.checkpointIdentity")
    message(FATAL_ERROR "Raw EXR signature or provenance missing")
endif()
