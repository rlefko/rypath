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
foreach(shader IN ITEMS smoke geometry analytic_probe packet_scattering_probe render gltf_texture_probe texture_probe hdr_texture_probe masked_probe texture_graph_probe surface_detail_probe image_lighting_probe light_sampling_probe conductor_probe gltf_probe dielectric_probe layered_probe table_bsdf_probe medium_probe)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.metal")
    if(shader STREQUAL "packet_scattering_probe")
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/tests/packet_scattering_probe.metal")
    endif()
    set(air "${rypath_runtime_directory}/CMakeFiles/rypath-${shader}.air")
    set(shader_options)
    set(shader_dependencies "${source}")
    if(shader STREQUAL "render" OR shader STREQUAL "medium_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/medium.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/conductor.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/medium_probe_wire.h")
    endif()
    if(shader STREQUAL "geometry" OR shader STREQUAL "render" OR shader STREQUAL "masked_probe" OR shader STREQUAL "analytic_probe")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle_candidate.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/triangle_product_guard.h")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies
            "${CMAKE_CURRENT_SOURCE_DIR}/src/geometry_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bilinear.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/analytic.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/intersection.h"
        )
    endif()
    if(shader STREQUAL "gltf_texture_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_texture.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gltf_texture_probe_wire.h")
    endif()
    if(shader STREQUAL "gltf_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gltf_bsdf.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/conductor.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gltf_probe_wire.h")
    endif()
    if(shader STREQUAL "conductor_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/conductor.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/conductor_probe_wire.h")
    endif()
    if(shader STREQUAL "render" OR shader STREQUAL "packet_scattering_probe")
        # Bound code growth in mixed-material rendering and the all-family probe.
        # Preserve strict FP while reducing runtime compiler input size.
        list(APPEND shader_options -Os)
    endif()
    if(shader STREQUAL "render")
        list(APPEND shader_dependencies
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_sampling.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_emission_sampling.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_geometry_sampling.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scalar_lighting.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/light_geometry_sampling_shared.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/checkpoint_contract.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/packet_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_transport.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_walk.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/conductor.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dielectric.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/sampling_contract.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/sobol_shared.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/qmc_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/qmc_probe_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pbrt/primes.inc")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/sobol_probe_wire.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/lens_probe_wire.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/texture_footprint.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_texture.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/texture_graph.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/masked_intersection.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/alpha_hash.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_lighting.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface_detail.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/texture_graph_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ewa_weights.h")
    endif()
    if(shader STREQUAL "render" OR shader STREQUAL "layered_probe" OR shader STREQUAL "table_bsdf_probe" OR shader STREQUAL "packet_scattering_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scattering.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_scattering.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_layered.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/packet_spectrum_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/layered_rng_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gltf_bsdf.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/layered.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/transmission.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/conductor.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dielectric.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/table_bsdf.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/bsdf_table_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/layered_probe_wire.h")
    endif()
    if(shader STREQUAL "render" OR shader STREQUAL "packet_scattering_probe")
        list(APPEND shader_dependencies
            "${CMAKE_CURRENT_SOURCE_DIR}/src/shading_normal_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/shading_normal_probe_wire.h")
    endif()
    if(shader STREQUAL "packet_scattering_probe")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/tests/layered_rng_oracles.h")
    endif()
    if(shader STREQUAL "dielectric_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dielectric.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/conductor.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/dielectric_probe_wire.h")
    endif()
    if(shader STREQUAL "texture_probe" OR shader STREQUAL "hdr_texture_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/hdr_probe_wire.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/texture_probe_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/texture_footprint.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_texture.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ewa_weights.h")
    endif()
    if(shader STREQUAL "texture_graph_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/texture_graph_probe_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/texture_graph_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/texture_graph.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_texture.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ewa_weights.h")
    endif()
    if(shader STREQUAL "surface_detail_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface_detail.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/texture_footprint.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/texture_graph_wire.h")
    endif()
    if(shader STREQUAL "image_lighting_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_lighting.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_texture.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/image_lighting_probe_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ewa_weights.h")
    endif()
    if(shader STREQUAL "light_sampling_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_sampling.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_emission_sampling.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_lighting.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image_texture.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/light_sampling_probe_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ewa_weights.h")
    endif()
    if(shader STREQUAL "masked_probe" OR shader STREQUAL "analytic_probe")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/masked_probe_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/masked_intersection.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/alpha_hash.h")
    endif()
    add_custom_command(
        OUTPUT "${air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${rypath_runtime_directory}/CMakeFiles"
        COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
            "-fmodules-cache-path=${rypath_metal_cache}" "-I${CMAKE_CURRENT_SOURCE_DIR}/shaders" ${shader_options} -c "${source}" -o "${air}"
        DEPENDS ${shader_dependencies}
        VERBATIM
    )
    list(APPEND rypath_metal_air_files "${air}")
    if(shader STREQUAL "render")
        set(rypath_render_shader_dependencies ${shader_dependencies})
    endif()
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

# This probe includes the actual renderer adapter; keep its duplicate renderer
# entry points in a separate library instead of relinking them into production.
set(rypath_light_geometry_library "${rypath_runtime_directory}/rypath-light-geometry-probe.metallib")
set(rypath_light_geometry_air "${rypath_runtime_directory}/CMakeFiles/rypath-light-geometry-probe.air")
add_custom_command(
    OUTPUT "${rypath_light_geometry_library}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${rypath_runtime_directory}/CMakeFiles"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
        "-fmodules-cache-path=${rypath_metal_cache}" "-I${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        -fno-fast-math -Os -c "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_geometry_sampling_probe.metal"
        -o "${rypath_light_geometry_air}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal "${rypath_metal_min_version}"
        -o "${rypath_light_geometry_library}" "${rypath_light_geometry_air}"
    DEPENDS ${rypath_render_shader_dependencies}
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_geometry_sampling_probe.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/light_geometry_sampling_probe_wire.h"
    VERBATIM
)
add_custom_target(rypath_light_geometry_shader DEPENDS "${rypath_light_geometry_library}")
