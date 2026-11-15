# SPDX-License-Identifier: GPL-3.0-only
if(NOT DEFINED executable OR NOT DEFINED fixture OR NOT DEFINED test_directory)
    message(FATAL_ERROR "Missing glTF CLI test arguments")
endif()
file(REMOVE_RECURSE "${test_directory}")
file(MAKE_DIRECTORY "${test_directory}")
execute_process(COMMAND "${executable}" --check-gltf "${fixture}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT output MATCHES "glTF conversion: supported" OR NOT error STREQUAL "")
    message(FATAL_ERROR "glTF preflight failed: ${status}\n${output}\n${error}")
endif()
if(EXISTS "${test_directory}/bundle")
    message(FATAL_ERROR "glTF preflight published output")
endif()
execute_process(COMMAND "${executable}" --convert-gltf "${fixture}" --output "${test_directory}/bundle"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT EXISTS "${test_directory}/bundle/model.pbrt"
   OR NOT EXISTS "${test_directory}/bundle/conversion.json" OR NOT error STREQUAL "")
    message(FATAL_ERROR "glTF conversion failed: ${status}\n${output}\n${error}")
endif()
file(SHA256 "${test_directory}/bundle/conversion.json" original)
execute_process(COMMAND "${executable}" --convert-gltf "${fixture}" --output "${test_directory}/bundle"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(SHA256 "${test_directory}/bundle/conversion.json" after)
if(NOT status EQUAL 1 OR NOT original STREQUAL after)
    message(FATAL_ERROR "glTF conversion replaced existing bundle: ${status}\n${output}\n${error}")
endif()
file(WRITE "${test_directory}/malformed.gltf" "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[")
execute_process(COMMAND "${executable}" --convert-gltf "${test_directory}/malformed.gltf"
    --output "${test_directory}/rejected" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 1 OR EXISTS "${test_directory}/rejected")
    message(FATAL_ERROR "Malformed glTF did not fail before publication: ${status}\n${output}\n${error}")
endif()
# Reuse the unchanged licensed triangle bytes while authoring a labeled test wrapper.
get_filename_component(fixture_directory "${fixture}" DIRECTORY)
file(COPY "${fixture_directory}/Triangle.bin" DESTINATION "${test_directory}")
file(READ "${fixture}" multiple)
string(REGEX REPLACE "\"scene\"[ \t\r\n]*:[ \t\r\n]*0[ \t\r\n]*," "" multiple "${multiple}")
string(REPLACE "\"scenes\" : [" "\"scenes\" : [{\"name\":\"first\",\"nodes\":[0]}," multiple "${multiple}")
file(WRITE "${test_directory}/multiple.gltf" "${multiple}")
execute_process(COMMAND "${executable}" --check-gltf "${test_directory}/multiple.gltf"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 1 OR NOT output MATCHES "explicit --gltf-scene")
    message(FATAL_ERROR "Ambiguous scene did not require selection: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${executable}" --convert-gltf "${test_directory}/multiple.gltf"
    --gltf-scene 1 --output "${test_directory}/selected"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT EXISTS "${test_directory}/selected/model.pbrt"
   OR NOT output MATCHES "scene 1" OR NOT error STREQUAL "")
    message(FATAL_ERROR "Explicit scene conversion failed: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${executable}" --convert-gltf "${test_directory}/multiple.gltf"
    --output "${test_directory}/out-of-range" --gltf-scene 2
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 1 OR EXISTS "${test_directory}/out-of-range")
    message(FATAL_ERROR "Invalid scene selection published output: ${status}\n${output}\n${error}")
endif()
file(GLOB stages "${test_directory}/.rypath-*")
if(stages)
    message(FATAL_ERROR "glTF conversion left staging directories: ${stages}")
endif()
file(REMOVE_RECURSE "${test_directory}")
