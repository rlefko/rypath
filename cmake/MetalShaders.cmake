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
foreach(shader IN ITEMS viewer frame temporal_denoiser smoke geometry analytic_probe packet_scattering_probe render gltf_texture_probe texture_probe hdr_texture_probe masked_probe texture_graph_probe surface_detail_probe image_lighting_probe light_sampling_probe conductor_probe gltf_probe dielectric_probe layered_probe table_bsdf_probe medium_probe)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.metal")
    if(shader STREQUAL "packet_scattering_probe")
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/tests/packet_scattering_probe.metal")
    endif()
    set(air "${rypath_runtime_directory}/CMakeFiles/rypath-${shader}.air")
    set(shader_options)
    set(shader_dependencies "${source}")
    if(shader STREQUAL "temporal_denoiser")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/temporal_denoiser_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h")
    endif()
    if(shader STREQUAL "frame")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/frame_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_sensor.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/render_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/packet_wire.h")
    endif()
    if(shader STREQUAL "render")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/camera_medium.metal"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/camera_medium_wire.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/reconstruction_guides.metal"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/reconstruction_guide_wire.h")
    endif()
    if(shader STREQUAL "render" OR shader STREQUAL "medium_probe")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/medium.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/conductor.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/medium_probe_wire.h")
    endif()
    if(shader STREQUAL "geometry" OR shader STREQUAL "render")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/opaque_shadow.h")
    endif()
    if(shader STREQUAL "geometry" OR shader STREQUAL "render" OR shader STREQUAL "masked_probe" OR shader STREQUAL "analytic_probe")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle_candidate.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/triangle_product_guard.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/triangle_extended.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/triangle_traversal_bounds.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle_traversal_bounds.h")
        list(APPEND shader_options -fno-fast-math)
        list(APPEND shader_dependencies
            "${CMAKE_CURRENT_SOURCE_DIR}/src/geometry_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bilinear.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/analytic.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/intersection.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_triangle_hint.h"
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
    if(shader STREQUAL "render")
        # The full-context material regression loses nested coating values at -Os
        # on the validated Metal toolchain; -O1 restores its exact physical controls.
        # Keep strict FP and qualify renderer performance separately.
        list(APPEND shader_options -O1)
    elseif(shader STREQUAL "packet_scattering_probe")
        # Bound code growth in the all-family probe while preserving strict FP.
        list(APPEND shader_options -Os)
    endif()
    if(shader STREQUAL "render")
        list(APPEND shader_dependencies
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_sampling.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_emission_sampling.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_geometry_sampling.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scalar_lighting.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scalar_transport.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wavefront.metal"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_sensor.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/wavefront_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/replay_sampler.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/area_reuse.metal"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/replay_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/reservoir_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/mutation_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/reuse_mis_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/hero_spectrum_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hero_scattering.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hero_pt_helpers.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/area_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/light_geometry_sampling_shared.h")
        list(APPEND shader_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/src/checkpoint_contract.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/offset_rounding_shared.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/packet_wire.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_transport.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_pt_step.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_pt_event_body.inc"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_shade_body.inc"
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
            "${CMAKE_CURRENT_SOURCE_DIR}/src/packet_roulette_shared.h"
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
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/packet_event_gate.h"
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
        -fno-fast-math -O1 -c "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_geometry_sampling_probe.metal"
        -o "${rypath_light_geometry_air}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal "${rypath_metal_min_version}"
        -o "${rypath_light_geometry_library}" "${rypath_light_geometry_air}"
    DEPENDS ${rypath_render_shader_dependencies}
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_geometry_sampling_probe.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/light_geometry_sampling_probe_wire.h"
    VERBATIM
)
add_custom_target(rypath_light_geometry_shader DEPENDS "${rypath_light_geometry_library}")

# Compile the unseeded control and hint regression routes separately so their
# inline solver definitions cannot be merged across the two test libraries.
set(native_hint_probe_libraries)
foreach(arm IN ITEMS control hint)
    set(probe_air "${rypath_runtime_directory}/CMakeFiles/rypath-native-hint-${arm}.air")
    set(probe_library "${rypath_runtime_directory}/rypath-native-hint-${arm}.metallib")
    add_custom_command(OUTPUT "${probe_air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${rypath_runtime_directory}/CMakeFiles"
        COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
            "-fmodules-cache-path=${rypath_metal_cache}" -fno-fast-math
            "-I${CMAKE_SOURCE_DIR}/shaders" "-I${CMAKE_SOURCE_DIR}/src"
            -c "${CMAKE_SOURCE_DIR}/tests/native_triangle_hint/${arm}.metal" -o "${probe_air}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/native_triangle_hint/${arm}.metal"
            "${CMAKE_SOURCE_DIR}/tests/native_triangle_hint/composition.metal"
            "${CMAKE_SOURCE_DIR}/tests/native_triangle_hint/seed.metal"
            "${CMAKE_SOURCE_DIR}/tests/native_triangle_hint/composition_wire.h"
            "${CMAKE_SOURCE_DIR}/tests/native_triangle_hint/seed_wire.h"
            "${CMAKE_SOURCE_DIR}/tests/native_triangle_hint/hint_probe_wire.h"
            "${CMAKE_SOURCE_DIR}/shaders/intersection.h"
            "${CMAKE_SOURCE_DIR}/shaders/native_triangle_hint.h"
            "${CMAKE_SOURCE_DIR}/shaders/triangle_candidate.h"
            "${CMAKE_SOURCE_DIR}/shaders/triangle_traversal_bounds.h"
            "${CMAKE_SOURCE_DIR}/shaders/analytic.h"
            "${CMAKE_SOURCE_DIR}/shaders/bilinear.h"
            "${CMAKE_SOURCE_DIR}/shaders/masked_intersection.h"
            "${CMAKE_SOURCE_DIR}/shaders/masked_probe.metal"
            "${CMAKE_SOURCE_DIR}/shaders/alpha_hash.h"
            "${CMAKE_SOURCE_DIR}/src/masked_probe_wire.h"
            "${CMAKE_SOURCE_DIR}/src/geometry_wire.h"
            "${CMAKE_SOURCE_DIR}/src/triangle_extended.h"
            "${CMAKE_SOURCE_DIR}/src/triangle_product_guard.h"
            "${CMAKE_SOURCE_DIR}/src/triangle_traversal_bounds.h"
        VERBATIM)
    add_custom_command(OUTPUT "${probe_library}"
        COMMAND "${RYPATH_XCRUN}" --sdk macosx metallib "${probe_air}" -o "${probe_library}"
        DEPENDS "${probe_air}" VERBATIM)
    list(APPEND native_hint_probe_libraries "${probe_library}")
    if(arm STREQUAL "control")
        set(rypath_native_hint_control_library "${probe_library}")
    else()
        set(rypath_native_hint_test_library "${probe_library}")
    endif()
endforeach()
add_custom_target(rypath_native_hint_shaders DEPENDS ${native_hint_probe_libraries})

# Standalone shared-math conformance; this library never enters production linking.
set(rypath_area_math_library "${rypath_runtime_directory}/rypath-area-math-probe.metallib")
set(rypath_area_math_air "${rypath_runtime_directory}/CMakeFiles/rypath-area-math-probe.air")
add_custom_command(OUTPUT "${rypath_area_math_air}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${rypath_runtime_directory}/CMakeFiles"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
        "-fmodules-cache-path=${rypath_metal_cache}" -fno-fast-math -O1
        -c "${CMAKE_SOURCE_DIR}/tests/area_math_probe.metal" -o "${rypath_area_math_air}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/area_math_probe.metal"
        "${CMAKE_SOURCE_DIR}/tests/area_math_probe_wire.h"
        "${CMAKE_SOURCE_DIR}/src/replay_shared.h"
        "${CMAKE_SOURCE_DIR}/src/reservoir_shared.h"
    VERBATIM)
add_custom_command(OUTPUT "${rypath_area_math_library}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metallib "${rypath_area_math_air}"
        -o "${rypath_area_math_library}"
    DEPENDS "${rypath_area_math_air}" VERBATIM)
add_custom_target(rypath_area_math_shader DEPENDS "${rypath_area_math_library}")

# Standalone shared-math conformance; this library never enters production linking.
set(rypath_mutation_math_library "${rypath_runtime_directory}/rypath-mutation-math-probe.metallib")
set(rypath_mutation_math_air "${rypath_runtime_directory}/CMakeFiles/rypath-mutation-math-probe.air")
add_custom_command(OUTPUT "${rypath_mutation_math_air}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${rypath_runtime_directory}/CMakeFiles"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
        "-fmodules-cache-path=${rypath_metal_cache}" -fno-fast-math -O1
        -c "${CMAKE_SOURCE_DIR}/tests/mutation_math_probe.metal" -o "${rypath_mutation_math_air}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/mutation_math_probe.metal"
        "${CMAKE_SOURCE_DIR}/tests/mutation_math_probe_wire.h"
        "${CMAKE_SOURCE_DIR}/src/replay_shared.h"
        "${CMAKE_SOURCE_DIR}/src/mutation_shared.h"
        "${CMAKE_SOURCE_DIR}/src/reservoir_shared.h"
    VERBATIM)
add_custom_command(OUTPUT "${rypath_mutation_math_library}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metallib "${rypath_mutation_math_air}"
        -o "${rypath_mutation_math_library}"
    DEPENDS "${rypath_mutation_math_air}" VERBATIM)
add_custom_target(rypath_mutation_math_shader DEPENDS "${rypath_mutation_math_library}")

# Source-target MIS and normalizer MH arithmetic without ray tracing requirements.
set(rypath_reuse_mis_math_library "${rypath_runtime_directory}/rypath-reuse-mis-math-probe.metallib")
set(rypath_reuse_mis_math_air "${rypath_runtime_directory}/CMakeFiles/rypath-reuse-mis-math-probe.air")
add_custom_command(OUTPUT "${rypath_reuse_mis_math_air}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${rypath_runtime_directory}/CMakeFiles"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
        "-fmodules-cache-path=${rypath_metal_cache}" -fno-fast-math -O1
        -c "${CMAKE_SOURCE_DIR}/tests/reuse_mis_math_probe.metal" -o "${rypath_reuse_mis_math_air}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/reuse_mis_math_probe.metal"
        "${CMAKE_SOURCE_DIR}/tests/reuse_mis_math_probe_wire.h"
        "${CMAKE_SOURCE_DIR}/src/reuse_mis_shared.h"
        "${CMAKE_SOURCE_DIR}/src/replay_shared.h"
        "${CMAKE_SOURCE_DIR}/src/mutation_shared.h"
        "${CMAKE_SOURCE_DIR}/src/reservoir_shared.h"
    VERBATIM)
add_custom_command(OUTPUT "${rypath_reuse_mis_math_library}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metallib "${rypath_reuse_mis_math_air}"
        -o "${rypath_reuse_mis_math_library}"
    DEPENDS "${rypath_reuse_mis_math_air}" VERBATIM)
add_custom_target(rypath_reuse_mis_math_shader DEPENDS "${rypath_reuse_mis_math_library}")

# Standalone hero BSDF conformance; no ray tracing or production-library linkage.
set(rypath_hero_scattering_library "${rypath_runtime_directory}/rypath-hero-scattering-probe.metallib")
set(rypath_hero_scattering_air "${rypath_runtime_directory}/CMakeFiles/rypath-hero-scattering-probe.air")
add_custom_command(OUTPUT "${rypath_hero_scattering_air}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${rypath_runtime_directory}/CMakeFiles"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
        "-fmodules-cache-path=${rypath_metal_cache}" -fno-fast-math -Os
        "-I${CMAKE_SOURCE_DIR}/shaders" "-I${CMAKE_SOURCE_DIR}/src"
        -c "${CMAKE_SOURCE_DIR}/tests/hero_scattering_probe.metal" -o "${rypath_hero_scattering_air}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/hero_scattering_probe.metal"
        "${CMAKE_SOURCE_DIR}/tests/hero_scattering_probe_wire.h"
        "${CMAKE_SOURCE_DIR}/shaders/hero_scattering.h"
        "${CMAKE_SOURCE_DIR}/shaders/packet_scattering.h"
        "${CMAKE_SOURCE_DIR}/shaders/packet_layered.h"
        "${CMAKE_SOURCE_DIR}/shaders/scattering.h"
        "${CMAKE_SOURCE_DIR}/shaders/conductor.h"
        "${CMAKE_SOURCE_DIR}/shaders/dielectric.h"
        "${CMAKE_SOURCE_DIR}/shaders/transmission.h"
        "${CMAKE_SOURCE_DIR}/shaders/gltf_bsdf.h"
        "${CMAKE_SOURCE_DIR}/shaders/layered.h"
        "${CMAKE_SOURCE_DIR}/shaders/image_texture.h"
        "${CMAKE_SOURCE_DIR}/shaders/table_bsdf.h"
        "${CMAKE_SOURCE_DIR}/src/hero_spectrum_shared.h"
        "${CMAKE_SOURCE_DIR}/src/packet_roulette_shared.h"
        "${CMAKE_SOURCE_DIR}/src/layered_rng_shared.h"
        "${CMAKE_SOURCE_DIR}/src/bsdf_table_wire.h"
        "${CMAKE_SOURCE_DIR}/src/ewa_weights.h"
        "${CMAKE_SOURCE_DIR}/src/render_wire.h"
    VERBATIM)
add_custom_command(OUTPUT "${rypath_hero_scattering_library}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metallib "${rypath_hero_scattering_air}"
        -o "${rypath_hero_scattering_library}"
    DEPENDS "${rypath_hero_scattering_air}" VERBATIM)
add_custom_target(rypath_hero_scattering_shader DEPENDS "${rypath_hero_scattering_library}")

# Native-capable legacy/mixed entry-point fixture for opaque capability checks.
set(rypath_opaque_capability_air "${rypath_runtime_directory}/CMakeFiles/rypath-opaque-capability.air")
set(rypath_opaque_capability_library "${rypath_runtime_directory}/rypath-opaque-capability.metallib")
add_custom_command(OUTPUT "${rypath_opaque_capability_air}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
        "-fmodules-cache-path=${rypath_metal_cache}" -c
        "${CMAKE_SOURCE_DIR}/tests/opaque_shadow_capability.metal" -o "${rypath_opaque_capability_air}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/opaque_shadow_capability.metal" VERBATIM)
add_custom_command(OUTPUT "${rypath_opaque_capability_library}"
    COMMAND "${RYPATH_XCRUN}" --sdk macosx metallib "${rypath_opaque_capability_air}"
        -o "${rypath_opaque_capability_library}"
    DEPENDS "${rypath_opaque_capability_air}" VERBATIM)
add_custom_target(rypath_opaque_capability_shader DEPENDS "${rypath_opaque_capability_library}")
