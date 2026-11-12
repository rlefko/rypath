find_program(RYPATH_XCRUN xcrun)
if(NOT RYPATH_XCRUN)
    message(FATAL_ERROR "The Metal backend requires xcrun from a full Xcode installation.")
endif()
execute_process(
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal --version
    RESULT_VARIABLE rypath_metal_tool_result
    OUTPUT_QUIET
    ERROR_VARIABLE rypath_metal_tool_error
)
if(NOT rypath_metal_tool_result STREQUAL "0")
    message(FATAL_ERROR
        "The Metal backend requires the Xcode Metal Toolchain. Select a full Xcode "
        "installation and install the Metal Toolchain if needed with "
        "'xcodebuild -downloadComponent metalToolchain'.\n${rypath_metal_tool_error}"
    )
endif()

set(rypath_metal_library "${rypath_runtime_directory}/rypath.metallib")
set(rypath_metal_cache "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/rypath-metal-cache")
set(rypath_metal_min_version "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")

set(rypath_metal_air_files)
foreach(shader IN ITEMS smoke geometry render)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.metal")
    set(air "${rypath_runtime_directory}/CMakeFiles/rypath-${shader}.air")
    set(shader_options)
    set(shader_dependencies "${source}")
    if(shader STREQUAL "geometry" OR shader STREQUAL "render")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/${shader}_wire.h")
    endif()
    add_custom_command(
        OUTPUT "${air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${rypath_runtime_directory}/CMakeFiles"
        COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
            "-fmodules-cache-path=${rypath_metal_cache}" ${shader_options} -c "${source}" -o "${air}"
        DEPENDS ${shader_dependencies}
        VERBATIM
    )
    list(APPEND rypath_metal_air_files "${air}")
endforeach()
add_custom_command(
    OUTPUT "${rypath_metal_library}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal "${rypath_metal_min_version}"
        -o "${rypath_metal_library}" ${rypath_metal_air_files}
    DEPENDS ${rypath_metal_air_files}
    VERBATIM
)
add_custom_target(rypath_metal_shaders DEPENDS "${rypath_metal_library}")
add_dependencies(rypath_metal_backend rypath_metal_shaders)
