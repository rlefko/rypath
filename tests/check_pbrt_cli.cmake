function(check_report scene expected_exit)
    execute_process(COMMAND "${executable}" --check-pbrt "${scene}"
        WORKING_DIRECTORY "${test_directory}"
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT "${exit_code}" STREQUAL "${expected_exit}" OR NOT "${error}" STREQUAL "")
        message(FATAL_ERROR "Unexpected preflight result for ${scene}.\n"
            "Exit ${exit_code}, expected ${expected_exit}.\n${output}${error}")
    endif()
    foreach(expected IN LISTS ARGN)
        string(FIND "${output}" "${expected}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "Missing '${expected}' for ${scene}.\n${output}")
        endif()
    endforeach()
    if(NOT output MATCHES "Rendering was not attempted\\.\n$")
        message(FATAL_ERROR "Missing final rendering status for ${scene}.\n${output}")
    endif()
endfunction()

file(MAKE_DIRECTORY "${test_directory}")
file(REMOVE "${test_directory}/preflight-only.png")
file(WRITE "${test_directory}/local scene.pbrt" [=[
Film "rgb" "string filename" "preflight-only.png"
Camera "perspective"
WorldBegin
Material "diffuse"
Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0] "integer indices" [0 1 2]
]=])
check_report("local scene.pbrt" 0 "Syntax: complete" "Checked scene structure: complete"
    "Dependency inventory: complete" "Input dependencies: 0" "Unsupported features: 0"
    "Inventory: 0 named materials, 1 meshes, 1 triangles, 0 object placements")
if(EXISTS "${test_directory}/preflight-only.png")
    message(FATAL_ERROR "Preflight created a film output")
endif()

set(cornell "${fixture_directory}/cornell-box.pbrt")
file(SHA256 "${cornell}" scene_hash)
file(SHA256 "${fixture_directory}/LICENSE.txt" notice_hash)
if(NOT scene_hash STREQUAL "b2fc6654c966468119187d169a76ecec82e169a1058f3344e27e5103189d4e43"
   OR NOT notice_hash STREQUAL "520b18c04d27fd9abca81364746178a9a3d52cff3c6204de05be5b68e547ee43")
    message(FATAL_ERROR "Pinned Cornell Box fixture or notice has changed")
endif()
check_report("${cornell}" 2 "Syntax: complete" "Sampler type \"sobol\"" "Input dependencies: 0"
    "Inventory: 8 named materials, 8 meshes, 36 triangles, 0 object placements"
    "Unsupported features: 1\n")

file(WRITE "${test_directory}/mixed.pbrt" [=[
Camera "perspective"
WorldBegin
Include "missing dependency.pbrt"
AreaLightSource "diffuse" "string filename" "missing emission.exr"
]=])
check_report("mixed.pbrt" 1 ":3:9: error [missing-file]" "missing emission.exr"
    "Dependency inventory: incomplete")

file(WRITE "${test_directory}/escaped.pbrt" [=[
Camera "perspective"
WorldBegin
Include "missing\nline.pbrt"
]=])
check_report("escaped.pbrt" 1 [=[include: missing\x0aline.pbrt]=])
