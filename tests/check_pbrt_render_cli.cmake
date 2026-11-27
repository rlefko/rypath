include("${CMAKE_CURRENT_LIST_DIR}/cli_availability.cmake")

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

string(REPLACE "Sampler \"independent\"" "Sampler \"pmj02bn\"" unsupported_sampler "${valid_scene}")
file(WRITE "${test_directory}/unsupported-sampler.pbrt" "${unsupported_sampler}")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/unsupported-sampler.pbrt"
    --output "${output}" RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "2" OR NOT stdout MATCHES "Sampler type \"pmj02bn\"" OR
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

function(check_render input sampler seed warning_pattern result selection transport maxdepth)
    set(spectral_arguments)
    if(NOT selection STREQUAL "auto")
        list(APPEND spectral_arguments --spectral-transport "${selection}")
    endif()
    set(expected_sampler "${sampler}")
    if(transport STREQUAL "hero")
        string(APPEND expected_sampler "; hero PT fixed packet dimension blocks; keyed material-local Philox streams; independent coating hash/PCG domains")
        set(spectral_description "Hero sampling: four visible wavelengths; cumulative spectral and direct-light balance MIS")
    elseif(transport STREQUAL "packet")
        string(APPEND expected_sampler "; packet fixed dimension allocation [0,920); keyed material-local Philox streams; independent coating hash/PCG domains")
        set(spectral_description "Packet sampling: fixed dimension allocation [0,920); keyed material-local Philox streams; independent coating hash/PCG domains")
    elseif(NOT transport STREQUAL "scalar")
        message(FATAL_ERROR "Unknown expected spectral mode: ${transport}")
    endif()
    set(mode_provenance "effective spectral transport ${transport}")
    if(selection STREQUAL "auto")
        string(APPEND mode_provenance "; automatic selection preserves scene-selected ${transport} transport")
    endif()
    file(WRITE "${output}" "${sentinel}")
    file(REMOVE "${test_directory}/must-not-be-created.png")
    execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/${input}"
        --output "${output}" --batch-size 2 ${spectral_arguments} WORKING_DIRECTORY "${test_directory}"
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(EXISTS "${test_directory}/must-not-be-created.png")
        message(FATAL_ERROR "Render wrote the overridden Film.filename")
    endif()
    string(FIND "${stdout}" "Sampler: ${expected_sampler}\n" sampler_position)
    string(FIND "${stdout}" "${mode_provenance}\n" mode_position)
    string(REGEX MATCHALL "effective spectral transport " mode_records "${stdout}")
    list(LENGTH mode_records mode_count)
    if(NOT mode_count EQUAL 1 OR mode_position EQUAL -1)
        message(FATAL_ERROR "Expected exactly one precise spectral mode provenance: ${stdout}${stderr}")
    endif()
    if(transport STREQUAL "scalar")
        if(stdout MATCHES "Hero sampling:|Packet sampling:")
            message(FATAL_ERROR "Scalar mode contains packet/hero provenance: ${stdout}${stderr}")
        endif()
    else()
        string(FIND "${stdout}" "${spectral_description}\n" description_position)
        string(REGEX MATCHALL "(Hero|Packet) sampling:" spectral_records "${stdout}")
        list(LENGTH spectral_records description_count)
        if(description_position EQUAL -1 OR NOT description_count EQUAL 1)
            message(FATAL_ERROR "Expected one precise spectral sampling description: ${stdout}${stderr}")
        endif()
    endif()
    if(sampler_position EQUAL -1 OR NOT stdout MATCHES "Render profile: pbrt-spectral-v1\n" OR
       NOT stdout MATCHES "Pixel filter: triangle \\(x radius 1, y radius 1.5\\)" OR
       NOT stdout MATCHES "Light sampling: bvh; matched forward and reverse probabilities" OR
       NOT stdout MATCHES "Resolution: 8x6\nSamples per pixel: 3\nBatch size: 2\nSeed: ${seed}\n" OR
       NOT stdout MATCHES "Finite path maxdepth: ${maxdepth}\n")
        message(FATAL_ERROR "Unexpected PBRT settings for ${input}: ${stdout}${stderr}")
    endif()
    if(exit STREQUAL "2")
        expect_existing_output()
        get_render_availability(availability "${stdout}" "${stderr}" "${warning_pattern}")
        if(NOT availability STREQUAL "")
            set("${result}" "${availability}" PARENT_SCOPE)
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
    set("${result}" "success" PARENT_SCOPE)
endfunction()

# Automatic selection preserves historical scalar PT sampler/provenance.
check_render("valid.pbrt" "philox4x32-10-v1 (sequence differs from PBRT)" 0 "" independent_status auto scalar 1)
# Explicit selectors report each estimator's own sampler description.
check_render("valid.pbrt" "philox4x32-10-v1 (sequence differs from PBRT)" 0 "" scalar_status scalar scalar 1)
check_render("valid.pbrt" "philox4x32-10-v1 (sequence differs from PBRT)" 0 "" packet_status packet packet 1)
check_render("valid.pbrt" "philox4x32-10-v1 (sequence differs from PBRT)" 0 "" hero_status hero hero 1)
# Independent scalar transport keeps its historical depth admission.
string(REPLACE "\"integer maxdepth\" 1" "\"integer maxdepth\" 66" deep_independent "${valid_scene}")
file(WRITE "${test_directory}/deep-independent.pbrt" "${deep_independent}")
check_render("deep-independent.pbrt" "philox4x32-10-v1 (sequence differs from PBRT)" 0 "" deep_status auto scalar 66)
foreach(status IN ITEMS scalar_status packet_status hero_status deep_status)
    if(NOT "${${status}}" STREQUAL "${independent_status}")
        message(FATAL_ERROR "Spectral selectors differ in backend availability: ${status}=${${status}}, auto=${independent_status}")
    endif()
endforeach()

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

# A material seed is separate from Sampler.seed, accepts the full signed32 domain,
# and is retained as an EXR string attribute.
foreach(material_seed IN ITEMS -2147483649 2147483648 1.5 invalid)
    file(WRITE "${output}" "${sentinel}")
    execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/valid.pbrt"
        --output "${output}" --material-seed "${material_seed}"
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT exit STREQUAL "1" OR NOT stderr MATCHES "--material-seed must be a signed 32-bit integer" OR
       stdout MATCHES "Device:|Committed samples|Wrote PNG")
        message(FATAL_ERROR "Unexpected material seed rejection ${exit}: ${stdout}${stderr}")
    endif()
    expect_existing_output()
endforeach()
foreach(material_seed IN ITEMS -2147483648 0 2147483647)
    set(seed_output "${test_directory}/material-seed-${material_seed}.exr")
    execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/valid.pbrt"
        --output "${seed_output}" --batch-size 2 --material-seed "${material_seed}"
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT stdout MATCHES "Material seed: ${material_seed}\n")
        message(FATAL_ERROR "Material seed missing from render settings: ${stdout}${stderr}")
    endif()
    if(exit STREQUAL "0")
        file(READ "${seed_output}" seed_exr HEX)
        string(HEX "${material_seed}" seed_hex)
        if(material_seed STREQUAL "-2147483648")
            set(seed_size "0b000000")
        elseif(material_seed STREQUAL "2147483647")
            set(seed_size "0a000000")
        else()
            set(seed_size "01000000")
        endif()
        string(FIND "${seed_exr}" "7279706174682e6d6174657269616c5365656400737472696e6700${seed_size}${seed_hex}" seed_attribute)
        if(seed_attribute EQUAL -1)
            message(FATAL_ERROR "Material seed EXR attribute has incorrect value/type")
        endif()
    elseif(exit STREQUAL "2")
        get_render_availability(availability "${stdout}" "${stderr}" "")
        if(availability STREQUAL "")
            message(FATAL_ERROR "Unexpected unavailable material seed render: ${stdout}${stderr}")
        endif()
    else()
        message(FATAL_ERROR "Material seed render failed ${exit}: ${stdout}${stderr}")
    endif()
endforeach()
expect_existing_output()

check_render("sobol.pbrt" "sobol-v1 / owen (PBRT camera variates; rypath transport dimensions)" -17
    "[^\n]+:5:[0-9]+: warning \\[sobol-non-power-of-two\\][^\n]*requested 3 samples per pixel\n"
    sobol_status auto scalar 1)
if(NOT independent_status STREQUAL sobol_status)
    message(FATAL_ERROR "PBRT render availability differs between independent (${independent_status}) and Sobol (${sobol_status})")
endif()

# EXR precision follows Film.savefp16, including the authored default.
function(check_exr input precision result)
    set(output "${test_directory}/result.exr")
    file(WRITE "${output}" "${sentinel}")
    execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/${input}"
        --output "${output}" --batch-size 2
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT stdout MATCHES "EXR; linear sRGB; ${precision}; Film.savefp16 honored")
        message(FATAL_ERROR "Wrong EXR precision: ${stdout}${stderr}")
    endif()
    if(exit STREQUAL "2")
        expect_existing_output()
        get_render_availability(availability "${stdout}" "${stderr}" "")
        if(NOT availability STREQUAL "")
            set("${result}" "${availability}" PARENT_SCOPE)
            return()
        endif()
    endif()
    if(NOT exit STREQUAL "0" OR NOT stdout MATCHES "Wrote EXR: " OR NOT stderr STREQUAL "")
        message(FATAL_ERROR "Unexpected EXR result ${exit}: ${stdout}${stderr}")
    endif()
    file(READ "${output}" signature LIMIT 4 HEX)
    if(NOT signature STREQUAL "762f3101")
        message(FATAL_ERROR "EXR output lacks the OpenEXR signature")
    endif()
    set("${result}" "success" PARENT_SCOPE)
endfunction()
check_exr("valid.pbrt" "FLOAT" float_status)
string(REPLACE " \"bool savefp16\" false" "" default_precision_scene "${valid_scene}")
file(WRITE "${test_directory}/half-default.pbrt" "${default_precision_scene}")
check_exr("half-default.pbrt" "HALF" half_default_status)
string(REPLACE "\"bool savefp16\" false" "\"bool savefp16\" true" half_scene "${valid_scene}")
file(WRITE "${test_directory}/half.pbrt" "${half_scene}")
check_exr("half.pbrt" "HALF" half_status)
if(NOT float_status STREQUAL independent_status OR NOT half_status STREQUAL float_status OR
   NOT half_default_status STREQUAL float_status)
    message(FATAL_ERROR "EXR and PNG backend availability differs")
endif()
# Publication failure reports status 1 after rendering; unavailable backends keep status 2.
set(blocked_output "${test_directory}/blocked.exr")
file(MAKE_DIRECTORY "${blocked_output}")
file(WRITE "${blocked_output}/sentinel" "preserve")
execute_process(COMMAND "${executable}" --render-pbrt "${test_directory}/valid.pbrt"
    --output "${blocked_output}" --batch-size 2
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(float_status STREQUAL "success")
    if(NOT exit STREQUAL "1" OR NOT stderr MATCHES "could not publish EXR image" OR stdout MATCHES "Wrote EXR")
        message(FATAL_ERROR "Unexpected EXR publication failure ${exit}: ${stdout}${stderr}")
    endif()
elseif(NOT exit STREQUAL "2")
    message(FATAL_ERROR "EXR unavailable backend status changed: ${exit}")
endif()
file(READ "${blocked_output}/sentinel" preserved)
if(NOT preserved STREQUAL "preserve")
    message(FATAL_ERROR "EXR failure modified destination")
endif()
file(GLOB leaked "${test_directory}/.rypath-exr*")
if(leaked)
    message(FATAL_ERROR "EXR publication leaked temporary files: ${leaked}")
endif()

if(independent_status STREQUAL "no_device" OR independent_status STREQUAL "no_ray_tracing")
    message("${render_skip_marker}\nRender status: ${independent_status}")
endif()
