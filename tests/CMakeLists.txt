function(add_cli_test name expected_exit expected_stdout expected_stderr)
    add_test(NAME "cli.${name}"
        COMMAND "${CMAKE_COMMAND}"
            "-Dexecutable=$<TARGET_FILE:rypath>"
            "-Darguments=${ARGN}"
            "-Dexpected_exit=${expected_exit}"
            "-Dexpected_stdout=${expected_stdout}"
            "-Dexpected_stderr=${expected_stderr}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli.cmake"
    )
endfunction()

set(help_text "Usage: rypath [--help | --version]\n")
set(error_text "rypath: unsupported arguments\nTry 'rypath --help' for usage.\n")

add_cli_test(no_arguments 0 "${help_text}" "")
add_cli_test(help 0 "${help_text}" "" --help)
add_cli_test(version 0 "rypath ${PROJECT_VERSION}\n" "" --version)
add_cli_test(unknown_argument 1 "" "${error_text}" --unknown)
add_cli_test(extra_argument 1 "" "${error_text}" --help --version)
