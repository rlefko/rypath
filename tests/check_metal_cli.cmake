set(smoke OFF)
if(argument STREQUAL "--metal-smoke-test")
    set(smoke ON)
endif()

if(metal_enabled AND smoke)
    # There is deliberately no companion library beside this link or in the working directory.
    set(link "${CMAKE_CURRENT_BINARY_DIR}/rypath-via-link")
    file(CREATE_LINK "${executable}" "${link}" SYMBOLIC)
    set(executable "${link}")
endif()

execute_process(
    COMMAND "${executable}" "${argument}"
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
    TIMEOUT 25
)

if(NOT metal_enabled)
    set(expected_exit 0)
    if(smoke)
        set(expected_exit 2)
    endif()
    if(actual_exit STREQUAL "${expected_exit}" AND actual_stderr STREQUAL "" AND
       actual_stdout STREQUAL "Metal status: not_built\nReason: Metal backend was not built\n")
        return()
    endif()
elseif(actual_stderr STREQUAL "")
    set(no_device_output "Metal status: no_device\nReason: No system-default Metal device\n")
    if(actual_stdout STREQUAL "${no_device_output}")
        if(NOT smoke AND actual_exit STREQUAL "0")
            return()
        endif()
        if(smoke AND allow_no_device AND actual_exit STREQUAL "2")
            message("RYPATH_CTEST_SKIP_NO_METAL_DEVICE")
            return()
        endif()
    endif()

    string(CONCAT success_pattern
        "^Metal status: success\n"
        "Device selection: system default\n"
        "Device: [^\n]+\n"
        "Registry ID: [0-9]+\n"
        "Architecture: [^\n]+\n"
        "Unified memory: (yes|no)\n"
        "Ray tracing API [(]compute[)]: (yes|no)\n"
        "Ray tracing API [(]render[)]: (yes|no)\n"
        "Hardware ray tracing execution: not verified\n"
    )
    if(smoke)
        string(APPEND success_pattern
            "Dispatches verified: 2\nElements verified: 258\nGuard elements verified: [1-9][0-9]*\n")
    endif()
    if(actual_exit STREQUAL "0" AND actual_stdout MATCHES "${success_pattern}$")
        return()
    endif()
endif()

message(FATAL_ERROR
    "Unexpected CLI result for ${argument}\n"
    "Exit status: [${actual_exit}]\n"
    "stdout: [${actual_stdout}]\n"
    "stderr: [${actual_stderr}]"
)
