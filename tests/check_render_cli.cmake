include("${CMAKE_CURRENT_LIST_DIR}/cli_availability.cmake")

get_filename_component(output_directory "${output}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
set(sentinel "existing output")
file(WRITE "${output}" "${sentinel}")

function(expect_existing_output)
    file(READ "${output}" current)
    if(NOT current STREQUAL sentinel)
        message(FATAL_ERROR "Existing output changed after a failed render")
    endif()
endfunction()

execute_process(
    COMMAND "${executable}" --render-demo --output "${output}" --width 0
    RESULT_VARIABLE invalid_exit
    OUTPUT_VARIABLE invalid_stdout
    ERROR_VARIABLE invalid_stderr
)
if(NOT invalid_exit STREQUAL "1" OR NOT invalid_stdout STREQUAL "" OR
   NOT invalid_stderr MATCHES "^rypath: [^\n]+\n$")
    message(FATAL_ERROR "Render preflight returned ${invalid_exit}: ${invalid_stdout}${invalid_stderr}")
endif()
expect_existing_output()

# EXR is portable output, while backend availability retains its existing status.
set(exr_output "${output_directory}/demo.exr")
file(WRITE "${exr_output}" "${sentinel}")
execute_process(COMMAND "${executable}" --render-demo --output "${exr_output}"
    --width 8 --height 6 --samples 3 --batch-size 2 --seed 0
    RESULT_VARIABLE exr_exit OUTPUT_VARIABLE exr_stdout ERROR_VARIABLE exr_stderr)
if(NOT exr_stdout MATCHES "EXR; linear sRGB; FLOAT")
    message(FATAL_ERROR "Demo EXR precision missing: ${exr_stdout}${exr_stderr}")
endif()
if(exr_exit STREQUAL "0")
    if(NOT exr_stdout MATCHES "Total samples: 144\nWrote EXR: " OR NOT exr_stderr STREQUAL "")
        message(FATAL_ERROR "Unexpected demo EXR result: ${exr_stdout}${exr_stderr}")
    endif()
    file(READ "${exr_output}" signature LIMIT 4 HEX)
    if(NOT signature STREQUAL "762f3101")
        message(FATAL_ERROR "Demo output lacks EXR signature")
    endif()
elseif(exr_exit STREQUAL "2")
    get_render_availability(exr_availability "${exr_stdout}" "${exr_stderr}" "")
    file(READ "${exr_output}" current)
    if(exr_availability STREQUAL "" OR NOT current STREQUAL sentinel)
        message(FATAL_ERROR "Unexpected demo EXR availability result")
    endif()
else()
    message(FATAL_ERROR "Unexpected demo EXR status ${exr_exit}: ${exr_stdout}${exr_stderr}")
endif()

execute_process(
    COMMAND "${executable}" --render-demo --output "${output}"
        --width 8 --height 6 --samples 3 --batch-size 2 --seed 0
    RESULT_VARIABLE exit
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(exit STREQUAL "2")
    expect_existing_output()
    get_render_availability(availability "${stdout}" "${stderr}" "")
    if(availability STREQUAL "not_built")
        return()
    endif()
    if(NOT availability STREQUAL "")
        message("${render_skip_marker}\n${stdout}${stderr}")
        return()
    endif()
endif()
if(NOT metal_enabled OR NOT exit STREQUAL "0" OR NOT stderr STREQUAL "" OR
   NOT stdout MATCHES "Resolution: 8x6\nSamples per pixel: 3\nBatch size: 2\nSeed: 0\n" OR
   NOT stdout MATCHES "Committed samples per pixel: 2/3\nCommitted samples per pixel: 3/3\n" OR
   NOT stdout MATCHES "Device: [^\n]+\n" OR
   NOT stdout MATCHES "Total samples: 144\nWrote PNG: ")
    message(FATAL_ERROR "Unexpected demo result ${exit}: ${stdout}${stderr}")
endif()
file(READ "${output}" signature LIMIT 8 HEX)
if(NOT signature STREQUAL "89504e470d0a1a0a")
    message(FATAL_ERROR "Demo output lacks the PNG signature")
endif()
