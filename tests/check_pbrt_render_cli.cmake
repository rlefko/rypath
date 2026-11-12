file(MAKE_DIRECTORY "${test_directory}")
set(output "${test_directory}/result.png")
set(sentinel "existing output")
file(WRITE "${output}" "${sentinel}")
file(WRITE "${test_directory}/valid.pbrt" [=[
Film "rgb" "integer xresolution" 8 "integer yresolution" 6
    "string filename" "must-not-be-created.png" "bool savefp16" false
Camera "perspective" "float fov" 45
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 3 "integer seed" 0
Integrator "path" "integer maxdepth" 1
WorldBegin
LightSource "infinite" "spectrum L" [360 1 830 1]
Material "diffuse" "spectrum reflectance" [360 0.5 830 0.5]
Shape "trianglemesh" "point3 P" [-1 -1 3 1 -1 3 0 1 3]
]=])

function(expect_existing_output)
    file(READ "${output}" current)
    if(NOT current STREQUAL sentinel)
        message(FATAL_ERROR "Existing output changed after a rejected render")
    endif()
endfunction()

file(READ "${test_directory}/valid.pbrt" valid_scene)
string(REPLACE "\"spectrum reflectance\" [360 0.5 830 0.5]"
    "\"rgb reflectance\" [0.5 0.5 0.5]" unsupported_scene "${valid_scene}")
file(WRITE "${test_directory}/unsupported.pbrt" "${unsupported_scene}")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/unsupported.pbrt"
    --output "${output}" RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "2" OR NOT stderr MATCHES "unsupported" OR
   NOT stdout MATCHES "Rendering was not attempted" OR stdout MATCHES "Device:|Committed samples|Wrote PNG")
    message(FATAL_ERROR "Unexpected unsupported result ${exit}: ${stdout}${stderr}")
endif()
expect_existing_output()

execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/missing.pbrt"
    --output "${output}" RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "1" OR NOT stdout MATCHES "missing-file" OR stdout MATCHES "Device:|Committed samples|Wrote PNG")
    message(FATAL_ERROR "Unexpected missing input result ${exit}: ${stdout}${stderr}")
endif()
expect_existing_output()

file(REMOVE "${test_directory}/must-not-be-created.png")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/valid.pbrt"
    --output "${output}" --batch-size 2 WORKING_DIRECTORY "${test_directory}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(EXISTS "${test_directory}/must-not-be-created.png")
    message(FATAL_ERROR "Render wrote the overridden Film.filename")
endif()
if(exit STREQUAL "2")
    expect_existing_output()
    if(stdout MATCHES "Committed samples|Wrote PNG" OR NOT stderr MATCHES "^rypath: [^\n]+\n$")
        message(FATAL_ERROR "Unexpected unavailable-render diagnostics: ${stdout}${stderr}")
    endif()
    if(NOT metal_enabled AND stdout MATCHES "Render status: not_built\n")
        return()
    endif()
    if(metal_enabled AND ((allow_no_device AND stdout MATCHES "Render status: no_device\n") OR
       (allow_no_ray_tracing AND stdout MATCHES "Render status: no_ray_tracing\n")))
        message("RYPATH_CTEST_SKIP_RENDER_UNAVAILABLE\n${stdout}${stderr}")
        return()
    endif()
endif()
if(NOT metal_enabled OR NOT exit STREQUAL "0" OR NOT stderr STREQUAL "" OR
   NOT stdout MATCHES "Render profile: pbrt-spectral-v1\n" OR
   NOT stdout MATCHES "Resolution: 8x6\nSamples per pixel: 3\nBatch size: 2\nSeed: 0\n" OR
   NOT stdout MATCHES "Finite path maxdepth: 1\n" OR
   NOT stdout MATCHES "Committed samples per pixel: 2/3\nCommitted samples per pixel: 3/3\n" OR
   NOT stdout MATCHES "Device: [^\n]+\n" OR
   NOT stdout MATCHES "Total samples: 144\nWrote PNG: ")
    message(FATAL_ERROR "Unexpected PBRT render result ${exit}: ${stdout}${stderr}")
endif()
file(READ "${output}" signature LIMIT 8 HEX)
if(NOT signature STREQUAL "89504e470d0a1a0a")
    message(FATAL_ERROR "PBRT render output lacks the PNG signature")
endif()
