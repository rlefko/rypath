file(MAKE_DIRECTORY "${test_directory}")
set(output "${test_directory}/result.png")
set(sentinel "existing output")
file(WRITE "${output}" "${sentinel}")
file(WRITE "${test_directory}/valid.pbrt" [=[
Film "rgb" "integer xresolution" 8 "integer yresolution" 6
    "string filename" "must-not-be-created.png" "bool savefp16" false
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

file(READ "${test_directory}/valid.pbrt" valid_scene)
string(REPLACE "Material \"diffuse\""
    "ColorSpace \"rec2020\"\nMaterial \"diffuse\"" unsupported_scene "${valid_scene}")
file(WRITE "${test_directory}/unsupported.pbrt" "${unsupported_scene}")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/unsupported.pbrt"
    --output "${output}" RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "2" OR NOT stderr MATCHES "unsupported" OR
   NOT stdout MATCHES "Rendering was not attempted" OR stdout MATCHES "Device:|Committed samples|Wrote PNG")
    message(FATAL_ERROR "Unexpected unsupported result ${exit}: ${stdout}${stderr}")
endif()
expect_existing_output()

string(REPLACE "Sampler \"independent\"" "Sampler \"halton\"" unsupported_sampler "${valid_scene}")
file(WRITE "${test_directory}/unsupported-sampler.pbrt" "${unsupported_sampler}")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/unsupported-sampler.pbrt"
    --output "${output}" RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "2" OR NOT stdout MATCHES "Sampler type \"halton\"" OR
   NOT stdout MATCHES "Unsupported features: 1\n" OR NOT stderr STREQUAL "" OR
   NOT stdout MATCHES "Rendering was not attempted" OR stdout MATCHES "Device:|Committed samples|Wrote PNG")
    message(FATAL_ERROR "Unexpected unsupported sampler result ${exit}: ${stdout}${stderr}")
endif()
expect_existing_output()

string(REPLACE "\"float xradius\" 1" "\"float xradius\" 1e-40" unsafe_filter "${valid_scene}")
file(WRITE "${test_directory}/unsafe-filter.pbrt" "${unsafe_filter}")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/unsafe-filter.pbrt"
    --output "${output}" RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "1" OR NOT stderr MATCHES "xradius must be positive and normal in binary32" OR
   NOT stdout MATCHES "Rendering was not attempted" OR stdout MATCHES "Device:|Committed samples|Wrote PNG")
    message(FATAL_ERROR "Unexpected triangle filter failure ${exit}: ${stdout}${stderr}")
endif()
expect_existing_output()

execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/missing.pbrt"
    --output "${output}" RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "1" OR NOT stdout MATCHES "missing-file" OR stdout MATCHES "Device:|Committed samples|Wrote PNG")
    message(FATAL_ERROR "Unexpected missing input result ${exit}: ${stdout}${stderr}")
endif()
expect_existing_output()

function(check_render input sampler seed warning_pattern)
    file(WRITE "${output}" "${sentinel}")
    file(REMOVE "${test_directory}/must-not-be-created.png")
    execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/${input}"
        --output "${output}" --batch-size 2 WORKING_DIRECTORY "${test_directory}"
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(EXISTS "${test_directory}/must-not-be-created.png")
        message(FATAL_ERROR "Render wrote the overridden Film.filename")
    endif()
    string(FIND "${stdout}" "Sampler: ${sampler}\n" sampler_position)
    if(sampler_position EQUAL -1 OR NOT stdout MATCHES "Render profile: pbrt-spectral-v1\n" OR
       NOT stdout MATCHES "Pixel filter: triangle \\(x radius 1, y radius 1.5\\)" OR
       NOT stdout MATCHES "Light sampling: separate environment and uniform triangle area" OR
       NOT stdout MATCHES "Resolution: 8x6\nSamples per pixel: 3\nBatch size: 2\nSeed: ${seed}\n" OR
       NOT stdout MATCHES "Finite path maxdepth: 1\n")
        message(FATAL_ERROR "Unexpected PBRT settings for ${input}: ${stdout}${stderr}")
    endif()
    if(exit STREQUAL "2")
        expect_existing_output()
        if(stdout MATCHES "Committed samples|Wrote PNG" OR
           NOT stderr MATCHES "^${warning_pattern}rypath: [^\n]+\n$")
            message(FATAL_ERROR "Unexpected unavailable-render diagnostics: ${stdout}${stderr}")
        endif()
        if(NOT metal_enabled AND stdout MATCHES "Render status: not_built\n")
            return()
        endif()
        if(metal_enabled AND ((allow_no_device AND stdout MATCHES "Render status: no_device\n") OR
           (allow_no_ray_tracing AND stdout MATCHES "Render status: no_ray_tracing\n")))
            set(skip_render_unavailable "${stdout}${stderr}" PARENT_SCOPE)
            return()
        endif()
    endif()
    if(NOT metal_enabled OR NOT exit STREQUAL "0" OR NOT stderr MATCHES "^${warning_pattern}$" OR
       NOT stdout MATCHES "Committed samples per pixel: 2/3\nCommitted samples per pixel: 3/3\n" OR
       NOT stdout MATCHES "Device: [^\n]+\n" OR
       NOT stdout MATCHES "Total samples: 144\nWrote PNG: ")
        message(FATAL_ERROR "Unexpected PBRT render result ${exit} for ${input}: ${stdout}${stderr}")
    endif()
    file(READ "${output}" signature LIMIT 8 HEX)
    if(NOT signature STREQUAL "89504e470d0a1a0a")
        message(FATAL_ERROR "PBRT render output lacks the PNG signature")
    endif()
endfunction()

check_render("valid.pbrt" "philox4x32-10-v1 (sequence differs from PBRT)" 0 "")

string(REPLACE "Sampler \"independent\" \"integer pixelsamples\" 3 \"integer seed\" 0"
    "Sampler \"sobol\" \"integer pixelsamples\" 3 \"integer seed\" -17 \"string randomization\" \"owen\""
    sobol_scene "${valid_scene}")
file(WRITE "${test_directory}/sobol.pbrt" "${sobol_scene}")

string(REPLACE "\"integer maxdepth\" 1" "\"integer maxdepth\" 128" deep_sobol "${sobol_scene}")
file(WRITE "${test_directory}/sobol-depth.pbrt" "${deep_sobol}")
file(WRITE "${output}" "${sentinel}")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/sobol-depth.pbrt"
    --output "${output}" RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "2" OR NOT stderr MATCHES "sobol-depth\\.pbrt:6:[0-9]+: unsupported.*maxdepth.*127" OR
   NOT stdout MATCHES "Rendering was not attempted" OR stdout MATCHES "Device:|Committed samples|Wrote PNG")
    message(FATAL_ERROR "Unexpected Sobol depth result ${exit}: ${stdout}${stderr}")
endif()
expect_existing_output()

check_render("sobol.pbrt" "sobol-v1 / owen (PBRT camera variates; rypath transport dimensions)" -17
    "[^\n]+:5:[0-9]+: warning \\[sobol-non-power-of-two\\][^\n]*requested 3 samples per pixel\n")
if(DEFINED skip_render_unavailable)
    message("RYPATH_CTEST_SKIP_RENDER_UNAVAILABLE\n${skip_render_unavailable}")
endif()
