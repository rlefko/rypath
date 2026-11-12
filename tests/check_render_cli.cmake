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

execute_process(
    COMMAND "${executable}" --render-demo --output "${output}"
        --width 8 --height 6 --samples 3 --batch-size 2 --seed 0
    RESULT_VARIABLE exit
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
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
