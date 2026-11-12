execute_process(
    COMMAND "${executable}" ${arguments}
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
)

if(NOT "${actual_exit}" STREQUAL "${expected_exit}" OR
   NOT "${actual_stdout}" STREQUAL "${expected_stdout}" OR
   NOT "${actual_stderr}" STREQUAL "${expected_stderr}")
    message(FATAL_ERROR
        "Unexpected CLI result for [${arguments}]\n"
        "Exit status: [${actual_exit}], expected [${expected_exit}]\n"
        "stdout: [${actual_stdout}], expected [${expected_stdout}]\n"
        "stderr: [${actual_stderr}], expected [${expected_stderr}]"
    )
endif()
