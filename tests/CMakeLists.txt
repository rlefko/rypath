add_executable(rypath_frame_session_tests frame_session_tests.cpp)
target_link_libraries(rypath_frame_session_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
target_compile_definitions(rypath_frame_session_tests PRIVATE
    RYPATH_TEST_FRAME_METAL=$<BOOL:${RYPATH_ENABLE_METAL}>
    RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(rypath_frame_session_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_frame_session_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.frame_session COMMAND rypath_frame_session_tests)
    set_tests_properties(metal.frame_session PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
    foreach(capability IN ITEMS DEVICE RAY_TRACING)
        if(RYPATH_ALLOW_NO_METAL_${capability})
            target_compile_definitions(rypath_frame_session_tests PRIVATE RYPATH_ALLOW_NO_METAL_${capability}=1)
        endif()
    endforeach()
else()
    add_test(NAME frame.unavailable COMMAND rypath_frame_session_tests)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME infrastructure.canonical_probe_generation COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_generate_canonical_probe.py")
if(RYPATH_ENABLE_METAL)
    add_test(NAME cli.reconstruction_diagnostics COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/reconstruction_diagnostics_cli_tests.py"
        "$<TARGET_FILE:rypath_reconstruction_diagnostics>")
endif()
add_test(NAME cli.frame_benchmark COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_benchmark_cli_tests.py" "$<TARGET_FILE:rypath_frame_benchmark>")
add_test(NAME cli.pipeline_qualification COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_qualification_cli_tests.py"
    "$<TARGET_FILE:rypath_pipeline_qualification>")
add_executable(rypath_frame_statistics_tests frame_statistics_tests.cpp)
target_include_directories(rypath_frame_statistics_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_frame_statistics_tests)
add_test(NAME frame.statistics COMMAND rypath_frame_statistics_tests)

add_executable(rypath_frame_motion_tests frame_motion_tests.cpp)
target_include_directories(rypath_frame_motion_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_frame_motion_tests)
add_test(NAME frame.motion COMMAND rypath_frame_motion_tests)

add_executable(rypath_shading_normal_tests shading_normal_tests.cpp)
target_include_directories(rypath_shading_normal_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_shading_normal_tests)
add_test(NAME transport.shading_normal COMMAND rypath_shading_normal_tests)

add_executable(rypath_environment_sampling_tests environment_sampling_tests.cpp)
target_link_libraries(rypath_environment_sampling_tests PRIVATE rypath_image)
rypath_configure_host_target(rypath_environment_sampling_tests)
add_test(NAME pbrt.environment_sampling COMMAND rypath_environment_sampling_tests)

add_executable(rypath_layered_rng_tests layered_rng_tests.cpp)
target_include_directories(rypath_layered_rng_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_layered_rng_tests)
add_test(NAME sampling.layered_rng COMMAND rypath_layered_rng_tests)

add_executable(rypath_light_sampling_tests light_sampling_tests.cpp)
target_link_libraries(rypath_light_sampling_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_light_sampling_tests)
add_test(NAME sampling.lights COMMAND rypath_light_sampling_tests)

add_executable(rypath_light_geometry_sampling_tests light_geometry_sampling_tests.cpp)
target_include_directories(rypath_light_geometry_sampling_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_light_geometry_sampling_tests)
add_test(NAME sampling.light_geometry COMMAND rypath_light_geometry_sampling_tests)

find_package(Threads REQUIRED)
add_executable(rypath_pavilion_quality_metrics pavilion_quality_metrics.cpp)
target_link_libraries(rypath_pavilion_quality_metrics PRIVATE rypath_image Threads::Threads)
set_target_properties(rypath_pavilion_quality_metrics PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_pavilion_quality_metrics)
add_executable(rypath_pavilion_quality_metrics_tests pavilion_quality_metrics_tests.cpp)
rypath_configure_host_target(rypath_pavilion_quality_metrics_tests)
add_test(NAME pbrt.pavilion_quality_metrics COMMAND rypath_pavilion_quality_metrics_tests)

add_executable(rypath_pavilion_quality_metrics_v2 pavilion_quality_metrics_v2.cpp)
target_link_libraries(rypath_pavilion_quality_metrics_v2 PRIVATE rypath_image Threads::Threads)
set_target_properties(rypath_pavilion_quality_metrics_v2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_pavilion_quality_metrics_v2)
add_executable(rypath_pavilion_quality_metrics_v2_tests pavilion_quality_metrics_v2_tests.cpp)
target_link_libraries(rypath_pavilion_quality_metrics_v2_tests PRIVATE rypath_image Threads::Threads)
rypath_configure_host_target(rypath_pavilion_quality_metrics_v2_tests)
add_test(NAME pbrt.pavilion_quality_metrics_v2 COMMAND rypath_pavilion_quality_metrics_v2_tests)

add_executable(rypath_layered_schedule_tests layered_schedule_tests.cpp)
rypath_configure_host_target(rypath_layered_schedule_tests)
add_test(NAME bsdf.layered_proposal_partition COMMAND rypath_layered_schedule_tests)

add_executable(rypath_pbrt_packet_tests pbrt_packet_tests.cpp)
target_link_libraries(rypath_pbrt_packet_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_pbrt_packet_tests)
add_test(NAME pbrt.packet COMMAND rypath_pbrt_packet_tests)

if(RYPATH_ENABLE_METAL)
    # Isolate arithmetic probes from unrelated reconstruction-guide entrypoints.
    # The whole-production-TU canonical_full_context test below stays unchanged.
    set(canonical_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/canonical-probe")
    set(canonical_probe_source "${canonical_probe_dir}/canonical_probe.metal")
    set(canonical_render_source "${canonical_probe_dir}/canonical_render.metal")
    set(canonical_probe_manifest "${canonical_probe_dir}/generation.json")
    set(canonical_probe_air "${canonical_probe_dir}/canonical_probe.air")
    set(canonical_probe_library "${rypath_runtime_directory}/rypath-canonical-probe.metallib")
    add_custom_command(
        OUTPUT "${canonical_probe_source}" "${canonical_render_source}" "${canonical_probe_manifest}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_canonical_probe.py"
            --render "${CMAKE_SOURCE_DIR}/shaders/render.metal"
            --probe "${CMAKE_SOURCE_DIR}/shaders/light_geometry_sampling_probe.metal"
            --output-dir "${canonical_probe_dir}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate_canonical_probe.py"
            "${CMAKE_SOURCE_DIR}/shaders/render.metal"
            "${CMAKE_SOURCE_DIR}/shaders/light_geometry_sampling_probe.metal"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${canonical_probe_library}"
        COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1 "${rypath_metal_min_version}"
            "-fmodules-cache-path=${rypath_metal_cache}" "-I${CMAKE_SOURCE_DIR}/shaders"
            -fno-fast-math -O1 -c "${canonical_probe_source}" -o "${canonical_probe_air}"
        COMMAND "${RYPATH_XCRUN}" --sdk macosx metal "${rypath_metal_min_version}"
            -o "${canonical_probe_library}" "${canonical_probe_air}"
        DEPENDS "${canonical_probe_source}" "${canonical_render_source}" "${canonical_probe_manifest}"
            ${rypath_render_shader_dependencies}
            "${CMAKE_CURRENT_SOURCE_DIR}/light_geometry_sampling_probe_wire.h"
        VERBATIM
    )
    add_custom_target(rypath_canonical_probe_shader DEPENDS "${canonical_probe_library}")
    foreach(suite IN ITEMS light_geometry_sampling light_sampling)
        add_executable(rypath_${suite}_probe ${suite}_probe.mm)
        target_link_libraries(rypath_${suite}_probe PRIVATE rypath_metal_backend rypath_render "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
        set_target_properties(rypath_${suite}_probe PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
        target_compile_options(rypath_${suite}_probe PRIVATE -fobjc-arc)
        rypath_configure_host_target(rypath_${suite}_probe)
        if(suite STREQUAL "light_geometry_sampling")
            add_dependencies(rypath_${suite}_probe rypath_light_geometry_shader rypath_canonical_probe_shader)
            add_test(NAME metal.${suite} COMMAND rypath_${suite}_probe "${canonical_probe_library}")
            add_test(NAME metal.canonical_full_context COMMAND rypath_${suite}_probe
                "${rypath_light_geometry_library}" --canonical-full-context "${rypath_metal_library}")
            set_tests_properties(metal.canonical_full_context PROPERTIES TIMEOUT 120)
            if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
                target_compile_definitions(rypath_${suite}_probe PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
            endif()
            if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
                set_tests_properties(metal.canonical_full_context PROPERTIES SKIP_RETURN_CODE 77)
            endif()
        else()
            add_test(NAME metal.${suite} COMMAND rypath_${suite}_probe "${rypath_metal_library}")
        endif()
        set_tests_properties(metal.${suite} PROPERTIES TIMEOUT 120)
        if(RYPATH_ALLOW_NO_METAL_DEVICE)
            target_compile_definitions(rypath_${suite}_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
            set_tests_properties(metal.${suite} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
    add_executable(rypath_packet_render_tests packet_render_tests.cpp)
    target_link_libraries(rypath_packet_render_tests PRIVATE rypath_metal_backend)
    set_target_properties(rypath_packet_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    rypath_configure_host_target(rypath_packet_render_tests)
    add_test(NAME metal.packet_render COMMAND rypath_packet_render_tests)
    add_test(NAME metal.packet_roulette_dispersion COMMAND rypath_packet_render_tests --roulette-dispersion)
    set_tests_properties(metal.packet_roulette_dispersion PROPERTIES TIMEOUT 600)
    add_test(NAME metal.shading_normal COMMAND rypath_packet_render_tests --shading-normal)
    set_tests_properties(metal.shading_normal PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    set_tests_properties(metal.packet_render PROPERTIES TIMEOUT 300)
    set(packet_surface_tests)
    foreach(family thin_dielectric diffuse_transmission measured fourier)
        set(test_name "metal.packet_surface_${family}")
        add_test(NAME "${test_name}" COMMAND rypath_packet_render_tests --surface-family "${family}")
        # Cold Metal pipeline compilation can exceed five minutes on M4 Max.
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 600)
        list(APPEND packet_surface_tests "${test_name}")
    endforeach()
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_packet_render_tests PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        target_compile_definitions(rypath_packet_render_tests PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.packet_render metal.packet_roulette_dispersion ${packet_surface_tests} PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_degenerate_triangle_tests degenerate_triangle_tests.cpp)
target_link_libraries(rypath_degenerate_triangle_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_degenerate_triangle_tests)
add_test(NAME geometry.zero_measure COMMAND rypath_degenerate_triangle_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.zero_measure COMMAND rypath_degenerate_triangle_tests --native "${rypath_metal_library}")
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_degenerate_triangle_tests PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        target_compile_definitions(rypath_degenerate_triangle_tests PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.zero_measure PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_pbrt_analytic_tests pbrt_analytic_tests.cpp)
target_link_libraries(rypath_pbrt_analytic_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_pbrt_analytic_tests)
add_test(NAME pbrt.analytic COMMAND rypath_pbrt_analytic_tests)

add_executable(rypath_packet_driver_tests packet_driver_tests.cpp)
target_link_libraries(rypath_packet_driver_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_packet_driver_tests)
add_test(NAME render.packet_driver COMMAND rypath_packet_driver_tests)

add_executable(rypath_analytic_geometry_tests analytic_geometry_tests.cpp)
target_link_libraries(rypath_analytic_geometry_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_analytic_geometry_tests)
add_test(NAME geometry.analytic COMMAND rypath_analytic_geometry_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.analytic_geometry COMMAND rypath_analytic_geometry_tests --native "${rypath_metal_library}")
    add_executable(rypath_analytic_render_tests analytic_render_tests.cpp)
    target_link_libraries(rypath_analytic_render_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
    rypath_configure_host_target(rypath_analytic_render_tests)
    set_target_properties(rypath_analytic_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    add_test(NAME metal.analytic_render COMMAND rypath_analytic_render_tests)
    set_tests_properties(metal.analytic_render PROPERTIES TIMEOUT 120)

    foreach(suite IN ITEMS analytic_masked packet_scattering)
        add_executable(rypath_${suite}_probe ${suite}_probe.mm)
        target_link_libraries(rypath_${suite}_probe PRIVATE rypath_metal_backend "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
        set_target_properties(rypath_${suite}_probe PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
        target_compile_options(rypath_${suite}_probe PRIVATE -fobjc-arc)
        rypath_configure_host_target(rypath_${suite}_probe)
        add_test(NAME metal.${suite} COMMAND rypath_${suite}_probe "${rypath_metal_library}")
        if(RYPATH_ALLOW_NO_METAL_DEVICE)
            target_compile_definitions(rypath_${suite}_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
            set_tests_properties(metal.${suite} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
    foreach(target IN ITEMS rypath_analytic_geometry_tests rypath_analytic_masked_probe rypath_analytic_render_tests)
        if(RYPATH_ALLOW_NO_METAL_DEVICE)
            target_compile_definitions(${target} PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        endif()
        if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
            target_compile_definitions(${target} PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
        endif()
    endforeach()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.analytic_geometry metal.analytic_masked metal.analytic_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

foreach(suite IN ITEMS resource_limits packet_spectrum)
    add_executable(rypath_${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(rypath_${suite}_tests PRIVATE rypath_pbrt)
    rypath_configure_host_target(rypath_${suite}_tests)
    add_test(NAME core.${suite} COMMAND rypath_${suite}_tests)
endforeach()

target_link_libraries(rypath_resource_limits_tests PRIVATE rypath_render rypath_pbrt_render)

# glTF portable image and scene contracts.
foreach(suite IN ITEMS gltf_texture gltf_scene)
    add_executable(rypath_${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(rypath_${suite}_tests PRIVATE rypath_pbrt_render rypath_checkpoint rypath_mesh_bundle)
    rypath_configure_host_target(rypath_${suite}_tests)
    add_test(NAME gltf.${suite} COMMAND rypath_${suite}_tests)
    set_tests_properties(gltf.${suite} PROPERTIES TIMEOUT 30)
endforeach()

include(cli_availability.cmake)

foreach(suite IN ITEMS stl stl_bundle)
    add_executable(rypath_${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(rypath_${suite}_tests PRIVATE rypath_stl)
    target_compile_definitions(rypath_${suite}_tests PRIVATE RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    rypath_configure_host_target(rypath_${suite}_tests)
    add_test(NAME import.${suite} COMMAND rypath_${suite}_tests)
    set_tests_properties(import.${suite} PROPERTIES TIMEOUT 60)
endforeach()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_stl_render_tests stl_render_tests.cpp)
    target_link_libraries(rypath_stl_render_tests PRIVATE rypath_stl rypath_metal_backend)
    set_target_properties(rypath_stl_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    rypath_configure_host_target(rypath_stl_render_tests)
    add_test(NAME metal.stl_import COMMAND rypath_stl_render_tests)
    set_tests_properties(metal.stl_import PROPERTIES TIMEOUT 60)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_stl_render_tests PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        target_compile_definitions(rypath_stl_render_tests PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.stl_import PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()


foreach(suite IN ITEMS obj obj_material obj_bundle)
    add_executable(rypath_${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(rypath_${suite}_tests PRIVATE rypath_obj)
    target_compile_definitions(rypath_${suite}_tests PRIVATE RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    rypath_configure_host_target(rypath_${suite}_tests)
    add_test(NAME import.${suite} COMMAND rypath_${suite}_tests)
    set_tests_properties(import.${suite} PROPERTIES TIMEOUT 60)
endforeach()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_obj_render_tests obj_render_tests.cpp)
    target_link_libraries(rypath_obj_render_tests PRIVATE rypath_obj rypath_metal_backend)
    target_compile_definitions(rypath_obj_render_tests PRIVATE RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    set_target_properties(rypath_obj_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    rypath_configure_host_target(rypath_obj_render_tests)
    add_test(NAME metal.obj_import COMMAND rypath_obj_render_tests)
    # API validation exceeds a minute even on the unchanged M4 Max baseline.
    set_tests_properties(metal.obj_import PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_obj_render_tests PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        target_compile_definitions(rypath_obj_render_tests PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.obj_import PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_checkpoint_tests checkpoint_tests.cpp ../src/checkpoint.cpp ../src/sha256.cpp)
target_compile_definitions(rypath_checkpoint_tests PRIVATE RYPATH_CHECKPOINT_TESTING=1)
target_link_libraries(rypath_checkpoint_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_checkpoint_tests)
add_test(NAME render.checkpoint_codec COMMAND rypath_checkpoint_tests)
set_tests_properties(render.checkpoint_codec PROPERTIES TIMEOUT 30)

foreach(implementation IN ITEMS native portable)
    add_executable(rypath_sha256_${implementation}_tests sha256_stream_tests.cpp ../src/sha256.cpp)
    target_include_directories(rypath_sha256_${implementation}_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    if(implementation STREQUAL "portable")
        target_compile_definitions(rypath_sha256_${implementation}_tests PRIVATE RYPATH_SHA256_FORCE_PORTABLE=1)
    endif()
    rypath_configure_host_target(rypath_sha256_${implementation}_tests)
    add_test(NAME render.sha256_${implementation} COMMAND rypath_sha256_${implementation}_tests)
    set_tests_properties(render.sha256_${implementation} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(rypath_render_job_tests render_job_tests.cpp)
target_link_libraries(rypath_render_job_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_render_job_tests)
add_test(NAME render.checkpoint_job COMMAND rypath_render_job_tests)
set_tests_properties(render.checkpoint_job PROPERTIES TIMEOUT 30)

add_executable(rypath_render_timings_tests render_timings_tests.cpp)
target_link_libraries(rypath_render_timings_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_render_timings_tests)
add_test(NAME render.timings COMMAND rypath_render_timings_tests)
set_tests_properties(render.timings PROPERTIES TIMEOUT 30)

add_executable(rypath_render_identity_tests render_identity_tests.cpp)
target_link_libraries(rypath_render_identity_tests PRIVATE rypath_checkpoint rypath_metal_backend)
set_target_properties(rypath_render_identity_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_render_identity_tests)
add_test(NAME render.checkpoint_identity COMMAND rypath_render_identity_tests)
set_tests_properties(render.checkpoint_identity PROPERTIES TIMEOUT 30)

add_executable(rypath_resume_render_tests resume_render_tests.cpp)
target_link_libraries(rypath_resume_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render rypath_checkpoint)
target_compile_definitions(rypath_resume_render_tests PRIVATE RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(rypath_resume_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_resume_render_tests)
add_test(NAME render.resume_driver COMMAND rypath_resume_render_tests --portable)
set_tests_properties(render.resume_driver PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL)
    foreach(variant IN ITEMS bad missing)
        set(contract_air "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-contract-${variant}.air")
        set(contract_library "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-contract-${variant}.metallib")
        add_custom_command(OUTPUT "${contract_library}"
            COMMAND "${RYPATH_XCRUN}" --sdk macosx metal -std=metal3.1
                "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
                -c "${CMAKE_CURRENT_SOURCE_DIR}/checkpoint_contract_${variant}.metal" -o "${contract_air}"
            COMMAND "${RYPATH_XCRUN}" --sdk macosx metallib "${contract_air}" -o "${contract_library}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/checkpoint_contract_${variant}.metal" VERBATIM)
        add_custom_target(rypath_contract_${variant} DEPENDS "${contract_library}")
        add_dependencies(rypath_render_identity_tests rypath_contract_${variant})
        set(contract_expected mismatch)
        if(variant STREQUAL "missing")
            set(contract_expected unavailable)
        endif()
        set(contract_availability)
        if(RYPATH_ALLOW_NO_METAL_DEVICE)
            list(APPEND contract_availability --allow-no-device)
        endif()
        if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
            list(APPEND contract_availability --allow-no-ray-tracing)
        endif()
        add_test(NAME metal.checkpoint_contract_${variant} COMMAND rypath_render_identity_tests
            --shader-contract "${contract_library}" ${contract_expected} ${contract_availability})
        set_tests_properties(metal.checkpoint_contract_${variant} PROPERTIES TIMEOUT 30)
        if(contract_availability)
            set_tests_properties(metal.checkpoint_contract_${variant} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
    add_test(NAME metal.resume_render COMMAND rypath_resume_render_tests --native)
    set_tests_properties(metal.resume_render PROPERTIES TIMEOUT 60)
    foreach(target IN ITEMS rypath_resume_render_tests rypath_render_identity_tests)
        if(RYPATH_ALLOW_NO_METAL_DEVICE)
            target_compile_definitions(${target} PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        endif()
        if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
            target_compile_definitions(${target} PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
        endif()
    endforeach()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.resume_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
add_executable(rypath_medium_scene_tests medium_scene_tests.cpp)
target_link_libraries(rypath_medium_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_medium_scene_tests)
add_test(NAME pbrt.medium_scene COMMAND rypath_medium_scene_tests "${CMAKE_CURRENT_BINARY_DIR}/media/$<CONFIG>")
set_tests_properties(pbrt.medium_scene PROPERTIES TIMEOUT 30)
get_cli_skip_regex(metal_skip_regex "${metal_skip_marker}" "${RYPATH_ENABLE_METAL}"
    "${RYPATH_ALLOW_NO_METAL_DEVICE}" OFF)
get_cli_skip_regex(render_skip_regex "${render_skip_marker}" "${RYPATH_ENABLE_METAL}"
    "${RYPATH_ALLOW_NO_METAL_DEVICE}" "${RYPATH_ALLOW_NO_METAL_RAY_TRACING}")

add_executable(rypath_availability_fake_cli availability_fake_cli.cpp)
rypath_configure_host_target(rypath_availability_fake_cli)
add_test(NAME cli.availability_policy COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath_availability_fake_cli>"
    "-Dctest=${CMAKE_CTEST_COMMAND}"
    "-Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/cli-availability/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli_availability.cmake")
set_tests_properties(cli.availability_policy PROPERTIES TIMEOUT 30)

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
    set_tests_properties("cli.${name}" PROPERTIES TIMEOUT 30)
endfunction()

string(CONCAT help_text
    "Usage: rypath [--help | --version | --metal-info | --metal-smoke-test]\n"
    "       rypath --check-pbrt SCENE [--resource-profile standard|large]\n"
    "       rypath --check-obj MODEL.obj\n"
    "       rypath --convert-obj MODEL.obj --output DIRECTORY\n"
    "       rypath --check-gltf MODEL [--gltf-scene INDEX]\n"
    "       rypath --convert-gltf MODEL --output DIRECTORY [--gltf-scene INDEX]\n"
    "       rypath --check-stl MODEL.stl [--stl-colors auto|viscam|magics]\n"
    "       rypath --convert-stl MODEL.stl --output DIRECTORY\n"
    "              [--stl-colors auto|viscam|magics]\n"
    "       rypath --render-pbrt SCENE --output IMAGE.{png,exr} [--batch-size 8]\n"
    "           [--resource-profile standard|large] [--raw-output PATH.exr] [--timings PATH.json]\n"
    "           [--material-seed 0]\n"
    "       rypath --render-demo --output PATH [--width 256] [--height 192]\n"
    "              [--samples 128] [--batch-size 8] [--seed 1]\n"
    "       Render options: [--checkpoint PATH] [--resume PATH]\n"
    "                       [--checkpoint-interval SECONDS (default: 60)]\n")
set(error_text "rypath: unsupported arguments\nTry 'rypath --help' for usage.\n")

add_cli_test(obj_missing_model 1 "" "${error_text}" --check-obj)
add_cli_test(obj_extra_argument 1 "" "${error_text}" --check-obj input.obj --version)
add_cli_test(obj_option_as_model 1 "" "${error_text}" --check-obj --version)
add_cli_test(obj_convert_missing_output 1 "" "${error_text}" --convert-obj input.obj)
add_cli_test(obj_convert_empty_output 1 "" "${error_text}" --convert-obj input.obj --output)
add_cli_test(obj_convert_unknown_option 1 "" "${error_text}" --convert-obj input.obj --foo value)
add_test(NAME cli.obj_import COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath>"
    "-Dfixture=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obj/external/cube.obj"
    "-Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/obj-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_obj_cli.cmake")
set_tests_properties(cli.obj_import PROPERTIES TIMEOUT 30)

add_cli_test(stl_missing_model 1 "" "${error_text}" --check-stl)
add_cli_test(stl_option_as_model 1 "" "${error_text}" --check-stl --version)
add_cli_test(stl_extra_argument 1 "" "${error_text}" --check-stl input.stl --version)
add_cli_test(stl_check_output 1 "" "${error_text}" --check-stl input.stl --output unused)
add_cli_test(stl_convert_missing_output 1 "" "${error_text}" --convert-stl input.stl)
add_cli_test(stl_convert_empty_output 1 "" "${error_text}" --convert-stl input.stl --output)
add_cli_test(stl_convert_unknown_option 1 "" "${error_text}" --convert-stl input.stl --foo value)
add_cli_test(stl_duplicate_output 1 "" "${error_text}" --convert-stl input.stl --output a --output b)
add_cli_test(stl_invalid_color_mode 1 "" "${error_text}" --check-stl input.stl --stl-colors none)
add_cli_test(stl_missing_color_mode 1 "" "${error_text}" --check-stl input.stl --stl-colors)
add_cli_test(stl_duplicate_color_mode 1 "" "${error_text}" --check-stl input.stl --stl-colors auto --stl-colors magics)
add_test(NAME cli.stl_import COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath>"
    "-Dfixture=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stl/external/triangle.stl"
    "-Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/stl-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_stl_cli.cmake")
set_tests_properties(cli.stl_import PROPERTIES TIMEOUT 30)

add_cli_test(gltf_missing_model 1 "" "${error_text}" --check-gltf)
add_cli_test(gltf_option_as_model 1 "" "${error_text}" --check-gltf --version)
add_cli_test(gltf_extra_argument 1 "" "${error_text}" --check-gltf input.gltf --version)
add_cli_test(gltf_check_output 1 "" "${error_text}" --check-gltf input.gltf --output unused)
add_cli_test(gltf_convert_missing_output 1 "" "${error_text}" --convert-gltf input.gltf)
add_cli_test(gltf_convert_empty_output 1 "" "${error_text}" --convert-gltf input.gltf --output)
add_cli_test(gltf_convert_unknown_option 1 "" "${error_text}" --convert-gltf input.gltf --foo value)
add_cli_test(gltf_duplicate_output 1 "" "${error_text}" --convert-gltf input.gltf --output a --output b)
add_cli_test(gltf_missing_scene_index 1 "" "${error_text}" --check-gltf input.gltf --gltf-scene)
add_cli_test(gltf_malformed_scene_index 1 "" "${error_text}" --check-gltf input.gltf --gltf-scene 1px)
add_cli_test(gltf_negative_scene_index 1 "" "${error_text}" --check-gltf input.gltf --gltf-scene -1)
add_cli_test(gltf_overflow_scene_index 1 "" "${error_text}" --check-gltf input.gltf --gltf-scene 18446744073709551616)
add_cli_test(gltf_duplicate_scene_index 1 "" "${error_text}" --check-gltf input.gltf --gltf-scene 0 --gltf-scene 1)
add_test(NAME cli.gltf_import COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath>"
    "-Dfixture=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gltf/external/Triangle/glTF/Triangle.gltf"
    "-Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/gltf-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_gltf_cli.cmake")
set_tests_properties(cli.gltf_import PROPERTIES TIMEOUT 30)

add_cli_test(no_arguments 0 "${help_text}" "")
add_cli_test(help 0 "${help_text}" "" --help)
add_cli_test(version 0 "rypath ${PROJECT_VERSION}\n" "" --version)
add_cli_test(unknown_argument 1 "" "${error_text}" --unknown)
add_cli_test(extra_argument 1 "" "${error_text}" --help --version)
add_cli_test(metal_combined_arguments 1 "" "${error_text}" --metal-info --metal-smoke-test)
add_cli_test(metal_extra_argument 1 "" "${error_text}" --metal-smoke-test extra)
add_cli_test(pbrt_missing_scene 1 "" "${error_text}" --check-pbrt)
add_cli_test(pbrt_extra_argument 1 "" "${error_text}" --check-pbrt input.pbrt --version)
add_cli_test(pbrt_option_as_scene 1 "" "${error_text}" --check-pbrt --version)
add_cli_test(pbrt_combined_modes 1 "" "${error_text}" --version --check-pbrt input.pbrt)
add_cli_test(render_missing_output 1 "" "rypath: --render-demo requires --output PATH\n"
    --render-demo)
add_cli_test(render_duplicate 1 "" "rypath: duplicate option --seed\n"
    --render-demo --output unused.png --seed 1 --seed 2)
add_cli_test(render_unknown 1 "" "${error_text}"
    --render-demo --output unused.png --unknown value)
add_cli_test(render_combined_modes 1 "" "${error_text}"
    --metal-info --render-demo --output unused.png)
add_cli_test(render_malformed_width 1 "" "rypath: invalid unsigned integer for --width\n"
    --render-demo --output unused.png --width 12px)
add_cli_test(render_width_overflow 1 "" "rypath: invalid unsigned integer for --width\n"
    --render-demo --output unused.png --width 4294967296)
add_cli_test(render_seed_overflow 1 "" "rypath: invalid unsigned integer for --seed\n"
    --render-demo --output unused.png --seed 18446744073709551616)
add_cli_test(render_total_overflow 1 "" "rypath: Total render sample count exceeds 64-bit capacity\n"
    --render-demo --output unused.png --width 2 --height 1 --samples 18446744073709551615)
add_cli_test(render_signed_samples 1 "" "rypath: invalid unsigned integer for --samples\n"
    --render-demo --output unused.png --samples -1)
add_cli_test(checkpoint_missing_path 1 "" "rypath: missing value for --checkpoint\n"
    --render-demo --output unused.png --checkpoint)
add_cli_test(checkpoint_zero_interval 1 "" "rypath: --checkpoint-interval must be a positive unsigned integer\n"
    --render-demo --output unused.png --checkpoint-interval 0)
add_cli_test(checkpoint_orphan_interval 1 "" "rypath: --checkpoint-interval requires --checkpoint or --resume\n"
    --render-demo --output unused.png --checkpoint-interval 1)
add_cli_test(checkpoint_duplicate 1 "" "rypath: duplicate option --resume\n"
    --render-demo --output unused.png --resume a --resume b)
add_cli_test(checkpoint_alias 1 "" "rypath: checkpoint and image output paths must not alias\n"
    --render-demo --output unused.png --checkpoint unused.png)
add_cli_test(checkpoint_pbrt_interval 1 "" "rypath: --checkpoint-interval must be a positive unsigned integer\n"
    --render-pbrt unused.pbrt --output unused.png --checkpoint-interval -1)
add_cli_test(render_pbrt_missing_scene 1 "" "${error_text}" --render-pbrt)
add_cli_test(render_pbrt_missing_output 1 "" "rypath: --render-pbrt requires --output IMAGE.{png,exr}\n"
    --render-pbrt unused.pbrt)
add_cli_test(render_pbrt_duplicate 1 "" "rypath: duplicate option --batch-size\n"
    --render-pbrt unused.pbrt --output unused.png --batch-size 1 --batch-size 2)
add_cli_test(render_pbrt_zero_batch 1 "" "rypath: --batch-size must be a positive unsigned integer\n"
    --render-pbrt unused.pbrt --output unused.png --batch-size 0)
add_cli_test(render_pbrt_output_format 1 "" "rypath: --render-pbrt output must have the .png or .exr extension\n"
    --render-pbrt unused.pbrt --output unused.jpg)
add_cli_test(render_pbrt_unknown 1 "" "${error_text}"
    --render-pbrt unused.pbrt --output unused.png --samples 4)

add_test(NAME cli.render_demo
    COMMAND "${CMAKE_COMMAND}"
        "-Dexecutable=$<TARGET_FILE:rypath>"
        "-Doutput=${CMAKE_CURRENT_BINARY_DIR}/render-demo/$<CONFIG>/demo.png"
        "-Dmetal_enabled=${RYPATH_ENABLE_METAL}"
        "-Dallow_no_device=${RYPATH_ALLOW_NO_METAL_DEVICE}"
        "-Dallow_no_ray_tracing=${RYPATH_ALLOW_NO_METAL_RAY_TRACING}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_render_cli.cmake"
)
set_tests_properties(cli.render_demo PROPERTIES TIMEOUT 30
    SKIP_REGULAR_EXPRESSION "${render_skip_regex}")

set(pbrt_fixture_directory "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt")
set(pbrt_test_directory "${CMAKE_CURRENT_BINARY_DIR}/pbrt")
add_executable(rypath_image_texture_tests image_texture_tests.cpp)
target_link_libraries(rypath_image_texture_tests PRIVATE rypath_image)
rypath_configure_host_target(rypath_image_texture_tests)
add_test(NAME pbrt.png_mipmaps COMMAND rypath_image_texture_tests
    "${pbrt_test_directory}/$<CONFIG>/png")
set_tests_properties(pbrt.png_mipmaps PROPERTIES TIMEOUT 30)
add_executable(rypath_image16_probe image16_probe.cpp)
target_link_libraries(rypath_image16_probe PRIVATE rypath_image)
rypath_configure_host_target(rypath_image16_probe)
add_executable(rypath_pbrt_image_tests pbrt_image_tests.cpp)
target_link_libraries(rypath_pbrt_image_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_pbrt_image_tests)
add_test(NAME pbrt.image_textures COMMAND rypath_pbrt_image_tests
    "${pbrt_test_directory}/$<CONFIG>/image")
set_tests_properties(pbrt.image_textures PROPERTIES TIMEOUT 30)

add_executable(rypath_texture_footprint_tests texture_footprint_tests.cpp)
target_link_libraries(rypath_texture_footprint_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_texture_footprint_tests)
add_test(NAME render.texture_footprints COMMAND rypath_texture_footprint_tests)
set_tests_properties(render.texture_footprints PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL)
    add_executable(rypath_blackbody_probe blackbody_probe.mm)
    target_link_libraries(rypath_blackbody_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_blackbody_probe)
    set_target_properties(rypath_blackbody_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_blackbody_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.blackbody_reflectance COMMAND rypath_blackbody_probe)
    set_tests_properties(metal.blackbody_reflectance PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_blackbody_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.blackbody_reflectance PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(rypath_texture_footprint_probe texture_footprint_probe.mm)
    target_link_libraries(rypath_texture_footprint_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_texture_footprint_probe)
    set_target_properties(rypath_texture_footprint_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_texture_footprint_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.texture_footprint COMMAND rypath_texture_footprint_probe)
    set_tests_properties(metal.texture_footprint PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_texture_footprint_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.texture_footprint PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(rypath_masked_intersection_probe masked_intersection_probe.mm)
    target_link_libraries(rypath_masked_intersection_probe PRIVATE rypath_metal_backend rypath_geometry)
    rypath_configure_host_target(rypath_masked_intersection_probe)
    set_target_properties(rypath_masked_intersection_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_masked_intersection_probe PRIVATE -fobjc-arc)
    add_executable(rypath_texture_graph_probe texture_graph_probe.mm)
    target_link_libraries(rypath_texture_graph_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_texture_graph_probe)
    set_target_properties(rypath_texture_graph_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_texture_graph_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.texture_graph COMMAND rypath_texture_graph_probe)
    set_tests_properties(metal.texture_graph PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_texture_graph_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.texture_graph PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    add_executable(rypath_surface_detail_probe surface_detail_probe.mm)
    target_link_libraries(rypath_surface_detail_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_surface_detail_probe)
    set_target_properties(rypath_surface_detail_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_surface_detail_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.surface_detail COMMAND rypath_surface_detail_probe "${rypath_metal_library}")
    set_tests_properties(metal.surface_detail PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_surface_detail_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.surface_detail PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    add_executable(rypath_image_lighting_probe image_lighting_probe.mm)
    target_link_libraries(rypath_image_lighting_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_image_lighting_probe)
    set_target_properties(rypath_image_lighting_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_image_lighting_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.image_lighting COMMAND rypath_image_lighting_probe)
    set_tests_properties(metal.image_lighting PROPERTIES TIMEOUT 120)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_image_lighting_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.image_lighting PROPERTIES SKIP_RETURN_CODE 77)
    endif()

endif()
add_test(NAME cli.pbrt_preflight
    COMMAND "${CMAKE_COMMAND}"
        "-Dexecutable=$<TARGET_FILE:rypath>"
        "-Dfixture_directory=${pbrt_fixture_directory}"
        "-Dtest_directory=${pbrt_test_directory}/$<CONFIG>/cli"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_pbrt_cli.cmake"
)
set_tests_properties(cli.pbrt_preflight PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_tests pbrt_tests.cpp)
target_link_libraries(rypath_pbrt_tests PRIVATE rypath_pbrt)
rypath_configure_host_target(rypath_pbrt_tests)

add_executable(rypath_loop_subdivision_tests loop_subdivision_tests.cpp)
target_link_libraries(rypath_loop_subdivision_tests PRIVATE rypath_pbrt)
rypath_configure_host_target(rypath_loop_subdivision_tests)
add_test(NAME geometry.loop_subdivision COMMAND rypath_loop_subdivision_tests)
set_tests_properties(geometry.loop_subdivision PROPERTIES TIMEOUT 30)

add_executable(rypath_loop_subdivision_dump loop_subdivision_dump.cpp)
target_link_libraries(rypath_loop_subdivision_dump PRIVATE rypath_pbrt)
set_target_properties(rypath_loop_subdivision_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_loop_subdivision_dump)

add_executable(rypath_pbrt_mesh_tests pbrt_mesh_tests.cpp)
target_link_libraries(rypath_pbrt_mesh_tests PRIVATE rypath_pbrt_render rypath_checkpoint)
target_compile_definitions(rypath_pbrt_mesh_tests PRIVATE RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
rypath_configure_host_target(rypath_pbrt_mesh_tests)
add_test(NAME pbrt.mesh_shapes COMMAND rypath_pbrt_mesh_tests)
set_tests_properties(pbrt.mesh_shapes PROPERTIES TIMEOUT 60)

add_executable(rypath_mesh_render_tests mesh_render_tests.cpp)
target_link_libraries(rypath_mesh_render_tests PRIVATE rypath_pbrt_render rypath_metal_backend rypath_checkpoint)
target_compile_definitions(rypath_mesh_render_tests PRIVATE RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(rypath_mesh_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_mesh_render_tests)
foreach(group IN ITEMS syntax state dependencies limits geometry corpus)
    add_test(NAME "pbrt.${group}" COMMAND rypath_pbrt_tests "${group}"
        "${pbrt_test_directory}/$<CONFIG>/${group}" "${pbrt_fixture_directory}")
    set_tests_properties("pbrt.${group}" PROPERTIES TIMEOUT 30)
endforeach()

add_executable(rypath_ply_tests ply_tests.cpp)
target_link_libraries(rypath_ply_tests PRIVATE rypath_pbrt)
rypath_configure_host_target(rypath_ply_tests)
add_test(NAME pbrt.ply COMMAND rypath_ply_tests "${pbrt_test_directory}/$<CONFIG>/ply")
set_tests_properties(pbrt.ply PROPERTIES TIMEOUT 30)

set(metal_cli_directory "${CMAKE_CURRENT_BINARY_DIR}/metal-cli")
file(MAKE_DIRECTORY "${metal_cli_directory}")
foreach(command IN ITEMS metal-info metal-smoke-test)
    string(REPLACE "-" "_" test_name "cli.${command}")
    add_test(NAME "${test_name}"
        COMMAND "${CMAKE_COMMAND}"
            "-Dexecutable=$<TARGET_FILE:rypath>"
            "-Dargument=--${command}"
            "-Dmetal_enabled=${RYPATH_ENABLE_METAL}"
            "-Dallow_no_device=${RYPATH_ALLOW_NO_METAL_DEVICE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_metal_cli.cmake"
    )
    set_tests_properties("${test_name}" PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${metal_cli_directory}"
    )
endforeach()
set_tests_properties(cli.metal_smoke_test PROPERTIES SKIP_REGULAR_EXPRESSION "${metal_skip_regex}")

add_executable(rypath_metal_tests metal_tests.cpp)
set_target_properties(rypath_metal_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_metal_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_metal_tests)

if(RYPATH_ENABLE_METAL)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/invalid.metallib"
        CONTENT "This is not a Metal library.\n")
    add_test(NAME metal.api_smoke COMMAND rypath_metal_tests smoke)
    add_test(NAME metal.missing_library COMMAND rypath_metal_tests missing-library
        "${CMAKE_CURRENT_BINARY_DIR}/missing.metallib")
    add_test(NAME metal.invalid_library COMMAND rypath_metal_tests invalid-library
        "${CMAKE_CURRENT_BINARY_DIR}/invalid.metallib")
    set_tests_properties(metal.api_smoke metal.missing_library metal.invalid_library
        PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        set_tests_properties(metal.api_smoke metal.missing_library metal.invalid_library
            PROPERTIES SKIP_RETURN_CODE 77)
    endif()
else()
    add_test(NAME metal.api_stub COMMAND rypath_metal_tests stub)
    set_tests_properties(metal.api_stub PROPERTIES TIMEOUT 30)
endif()

add_executable(rypath_render_tests render_tests.cpp render_reference.cpp)
set_target_properties(rypath_render_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_include_directories(rypath_render_tests PRIVATE "${rypath_generated_directory}")
target_link_libraries(rypath_render_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_render_tests)
add_test(NAME render.cpu_reference COMMAND rypath_render_tests cpu)
set_tests_properties(render.cpu_reference PROPERTIES TIMEOUT 30)

add_executable(rypath_sampling_tests sampling_tests.cpp)
target_link_libraries(rypath_sampling_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_sampling_tests)
add_test(NAME render.sobol_sampling COMMAND rypath_sampling_tests)

add_executable(rypath_packet_roulette_tests packet_roulette_tests.cpp)
target_include_directories(rypath_packet_roulette_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_packet_roulette_tests)
add_test(NAME render.packet_roulette COMMAND rypath_packet_roulette_tests)

add_executable(rypath_qmc_sampling_tests qmc_sampling_tests.cpp)
target_link_libraries(rypath_qmc_sampling_tests PRIVATE rypath_render)
target_compile_options(rypath_qmc_sampling_tests PRIVATE
    "$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-ffp-contract=off>")
rypath_configure_host_target(rypath_qmc_sampling_tests)
add_test(NAME render.qmc_sampling COMMAND rypath_qmc_sampling_tests)

add_executable(rypath_gaussian_filter_tests gaussian_filter_tests.cpp)
target_link_libraries(rypath_gaussian_filter_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_gaussian_filter_tests)
add_test(NAME render.gaussian_filter COMMAND rypath_gaussian_filter_tests)

add_executable(rypath_pbrt_sampling_defaults_tests pbrt_sampling_defaults_tests.cpp)
target_link_libraries(rypath_pbrt_sampling_defaults_tests PRIVATE rypath_pbrt_render rypath_checkpoint)
rypath_configure_host_target(rypath_pbrt_sampling_defaults_tests)
add_test(NAME pbrt.sampling_defaults COMMAND rypath_pbrt_sampling_defaults_tests
    "${CMAKE_CURRENT_BINARY_DIR}/sampling-defaults")
set_tests_properties(render.sobol_sampling PROPERTIES TIMEOUT 30)

set(ray_tracing_test_options)
if(RYPATH_ALLOW_NO_METAL_DEVICE)
    list(APPEND ray_tracing_test_options --allow-no-device)
endif()
if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
    list(APPEND ray_tracing_test_options --allow-no-ray-tracing)
endif()

add_executable(rypath_triangle_candidate_tests triangle_candidate_tests.cpp geometry_reference.cpp)
target_link_libraries(rypath_triangle_candidate_tests PRIVATE rypath_metal_backend)
set_target_properties(rypath_triangle_candidate_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_triangle_candidate_tests)
add_test(NAME geometry.triangle_candidates COMMAND rypath_triangle_candidate_tests cpu)
set_tests_properties(geometry.triangle_candidates PROPERTIES TIMEOUT 30)
foreach(suite IN ITEMS triangle_extended triangle_traversal_bounds)
    add_executable(rypath_${suite}_tests ${suite}_tests.cpp)
    target_include_directories(rypath_${suite}_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    rypath_configure_host_target(rypath_${suite}_tests)
    add_test(NAME geometry.${suite} COMMAND rypath_${suite}_tests)
    set_tests_properties(geometry.${suite} PROPERTIES TIMEOUT 30)
endforeach()
add_executable(rypath_triangle_box_coverage_tests triangle_box_coverage_tests.cpp)
target_link_libraries(rypath_triangle_box_coverage_tests PRIVATE rypath_metal_backend)
set_target_properties(rypath_triangle_box_coverage_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_triangle_box_coverage_tests)
add_test(NAME geometry.triangle_box_coverage COMMAND rypath_triangle_box_coverage_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.triangle_box_coverage COMMAND rypath_triangle_box_coverage_tests --native ${ray_tracing_test_options})
    set_tests_properties(metal.triangle_box_coverage PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.triangle_box_coverage PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(rypath_triangle_product_guard_probe triangle_product_guard_probe.mm)
    target_link_libraries(rypath_triangle_product_guard_probe PRIVATE rypath_metal_backend)
    rypath_configure_host_target(rypath_triangle_product_guard_probe)
    set_target_properties(rypath_triangle_product_guard_probe PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_triangle_product_guard_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.triangle_product_guard COMMAND rypath_triangle_product_guard_probe ${ray_tracing_test_options})
    set_tests_properties(metal.triangle_product_guard PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        set_tests_properties(metal.triangle_product_guard PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME metal.triangle_candidates COMMAND rypath_triangle_candidate_tests metal ${ray_tracing_test_options})
    set_tests_properties(metal.triangle_candidates PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.triangle_candidates PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.masked_intersection COMMAND rypath_masked_intersection_probe ${ray_tracing_test_options})
    set_tests_properties(metal.masked_intersection PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.masked_intersection PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_image_render_tests image_render_tests.cpp)
target_link_libraries(rypath_image_render_tests PRIVATE rypath_metal_backend rypath_render)
set_target_properties(rypath_image_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_image_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.image_render COMMAND rypath_image_render_tests ${ray_tracing_test_options})
    set_tests_properties(metal.image_render PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.image_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_bilinear_tests bilinear_tests.cpp)
target_link_libraries(rypath_bilinear_tests PRIVATE rypath_metal_backend)
set_target_properties(rypath_bilinear_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_bilinear_tests)
add_test(NAME geometry.bilinear COMMAND rypath_bilinear_tests cpu)
set_tests_properties(geometry.bilinear PROPERTIES TIMEOUT 30)
add_executable(rypath_patch_render_tests patch_render_tests.cpp)
target_link_libraries(rypath_patch_render_tests PRIVATE rypath_metal_backend)
set_target_properties(rypath_patch_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_patch_render_tests)
add_test(NAME render.patch_reference COMMAND rypath_patch_render_tests --reference)
set_tests_properties(render.patch_reference PROPERTIES TIMEOUT 90)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.bilinear COMMAND rypath_bilinear_tests metal ${ray_tracing_test_options})
    add_test(NAME metal.mesh_shapes COMMAND rypath_mesh_render_tests --metal ${ray_tracing_test_options})
    set_tests_properties(metal.mesh_shapes PROPERTIES TIMEOUT 120)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.mesh_shapes PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    set_tests_properties(metal.bilinear PROPERTIES TIMEOUT 30)
    add_test(NAME metal.patch_render COMMAND rypath_patch_render_tests --metal ${ray_tracing_test_options})
    set_tests_properties(metal.patch_render PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.bilinear PROPERTIES SKIP_RETURN_CODE 77)
        set_tests_properties(metal.patch_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

set(pbrt_render_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt-render")
add_test(NAME cli.image_inputs COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath>" "-Dfixture_directory=${pbrt_render_fixtures}"
    "-Dtest_directory=${pbrt_test_directory}/$<CONFIG>/image-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_image_inputs.cmake")
set_tests_properties(cli.image_inputs PROPERTIES TIMEOUT 30)
add_test(NAME cli.spectrum_inputs COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath>"
    "-Dtest_directory=${pbrt_test_directory}/$<CONFIG>/spectrum-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_spectrum_inputs.cmake")
set_tests_properties(cli.spectrum_inputs PROPERTIES TIMEOUT 30)
add_executable(rypath_pbrt_ply_fixture_tests pbrt_ply_fixture_tests.cpp)
target_link_libraries(rypath_pbrt_ply_fixture_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_pbrt_ply_fixture_tests)
add_test(NAME pbrt.ply_render_translation COMMAND rypath_pbrt_ply_fixture_tests "${pbrt_render_fixtures}")
set_tests_properties(pbrt.ply_render_translation PROPERTIES TIMEOUT 30)
add_executable(rypath_rgb_spectrum_tests rgb_spectrum_tests.cpp)
target_link_libraries(rypath_rgb_spectrum_tests PRIVATE rypath_rgb_spectrum)
rypath_configure_host_target(rypath_rgb_spectrum_tests)
add_test(NAME render.rgb_spectrum COMMAND rypath_rgb_spectrum_tests)
set_tests_properties(render.rgb_spectrum PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_spectra_tests pbrt_spectra_tests.cpp)
target_link_libraries(rypath_pbrt_spectra_tests PRIVATE rypath_rgb_spectrum rypath_pbrt_spectra)
rypath_configure_host_target(rypath_pbrt_spectra_tests)
add_test(NAME pbrt.named_spectra COMMAND rypath_pbrt_spectra_tests)
set_tests_properties(pbrt.named_spectra PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_render_tests pbrt_render_tests.cpp)
target_link_libraries(rypath_pbrt_render_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_pbrt_render_tests)
add_test(NAME pbrt.render_translation COMMAND rypath_pbrt_render_tests
    "${CMAKE_CURRENT_BINARY_DIR}/pbrt-render/$<CONFIG>" "${pbrt_render_fixtures}")
set_tests_properties(pbrt.render_translation PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_reference pbrt_reference.cpp)
set_target_properties(rypath_pbrt_reference PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_pbrt_reference PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_reference)

add_executable(rypath_pbrt_file_spectrum_probe pbrt_file_spectrum_probe.cpp)
target_link_libraries(rypath_pbrt_file_spectrum_probe PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_pbrt_file_spectrum_probe)

add_executable(rypath_reflectance_curve_tests reflectance_curve_tests.cpp)
target_link_libraries(rypath_reflectance_curve_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_reflectance_curve_tests)
add_test(NAME render.reflectance_curve COMMAND rypath_reflectance_curve_tests)
set_tests_properties(render.reflectance_curve PROPERTIES TIMEOUT 30)

add_executable(rypath_reflectance_curve_probe reflectance_curve_probe.cpp)
target_link_libraries(rypath_reflectance_curve_probe PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_reflectance_curve_probe)

add_executable(rypath_pbrt_render_gpu_tests pbrt_render_gpu_tests.cpp)
set_target_properties(rypath_pbrt_render_gpu_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_include_directories(rypath_pbrt_render_gpu_tests PRIVATE "${rypath_generated_directory}")
target_link_libraries(rypath_pbrt_render_gpu_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_render_gpu_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.pbrt_render COMMAND rypath_pbrt_render_gpu_tests
        "${pbrt_render_fixtures}" ${ray_tracing_test_options})
    set_tests_properties(metal.pbrt_render PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.pbrt_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_pbrt_area_tests pbrt_area_tests.cpp area_reference.cpp)
set_target_properties(rypath_pbrt_area_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_include_directories(rypath_pbrt_area_tests PRIVATE "${rypath_generated_directory}")
target_link_libraries(rypath_pbrt_area_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_area_tests)
add_test(NAME render.area_reference COMMAND rypath_pbrt_area_tests
    --reference "${pbrt_render_fixtures}")
set_tests_properties(render.area_reference PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.pbrt_area COMMAND rypath_pbrt_area_tests --metal ${ray_tracing_test_options})
    set_tests_properties(metal.pbrt_area PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.pbrt_area PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_pbrt_filter_tests pbrt_filter_tests.cpp)
set_target_properties(rypath_pbrt_filter_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_pbrt_filter_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_filter_tests)
add_test(NAME render.pbrt_filter_reference COMMAND rypath_pbrt_filter_tests
    --reference "${pbrt_render_fixtures}")
set_tests_properties(render.pbrt_filter_reference PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.pbrt_filter COMMAND rypath_pbrt_filter_tests --metal ${ray_tracing_test_options})
    set_tests_properties(metal.pbrt_filter PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.pbrt_filter PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_triangle_attributes_tests triangle_attributes_tests.cpp)
target_link_libraries(rypath_triangle_attributes_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_triangle_attributes_tests)
add_test(NAME render.triangle_attributes COMMAND rypath_triangle_attributes_tests)
set_tests_properties(render.triangle_attributes PROPERTIES TIMEOUT 30)

add_executable(rypath_pbrt_attribute_tests pbrt_attribute_tests.cpp attribute_reference.cpp)
set_target_properties(rypath_pbrt_attribute_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_pbrt_attribute_tests PRIVATE rypath_pbrt_render rypath_metal_backend)
rypath_configure_host_target(rypath_pbrt_attribute_tests)
add_test(NAME render.pbrt_attribute_reference COMMAND rypath_pbrt_attribute_tests
    --reference "${pbrt_render_fixtures}")
set_tests_properties(render.pbrt_attribute_reference PROPERTIES TIMEOUT 30)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.pbrt_attributes COMMAND rypath_pbrt_attribute_tests
        --metal ${ray_tracing_test_options})
    set_tests_properties(metal.pbrt_attributes PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.pbrt_attributes PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_test(NAME cli.render_pbrt COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath>"
    "-Dfixture_directory=${pbrt_render_fixtures}"
    "-Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/pbrt-render/$<CONFIG>/cli"
    "-Dmetal_enabled=${RYPATH_ENABLE_METAL}"
    "-Dallow_no_device=${RYPATH_ALLOW_NO_METAL_DEVICE}"
    "-Dallow_no_ray_tracing=${RYPATH_ALLOW_NO_METAL_RAY_TRACING}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_pbrt_render_cli.cmake")
set_tests_properties(cli.render_pbrt PROPERTIES TIMEOUT 60
    SKIP_REGULAR_EXPRESSION "${render_skip_regex}")

add_test(NAME cli.raw_output COMMAND "${CMAKE_COMMAND}"
    "-Dexecutable=$<TARGET_FILE:rypath>"
    "-Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/pbrt-render/$<CONFIG>/raw-cli"
    "-Dmetal_enabled=${RYPATH_ENABLE_METAL}"
    "-Dallow_no_device=${RYPATH_ALLOW_NO_METAL_DEVICE}"
    "-Dallow_no_ray_tracing=${RYPATH_ALLOW_NO_METAL_RAY_TRACING}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_raw_output_cli.cmake")
set_tests_properties(cli.raw_output PROPERTIES TIMEOUT 60
    SKIP_REGULAR_EXPRESSION "${render_skip_regex}")

if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.render COMMAND rypath_render_tests metal ${ray_tracing_test_options})
    set_tests_properties(metal.render PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.render PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    add_executable(rypath_sobol_render_tests sobol_render_tests.cpp sobol_probe.mm)
    add_executable(rypath_qmc_render_tests qmc_render_tests.cpp qmc_probe.mm)
    set_target_properties(rypath_qmc_render_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_qmc_render_tests PRIVATE -ffp-contract=off
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_compile_definitions(rypath_qmc_render_tests PRIVATE
        "$<$<BOOL:${RYPATH_ALLOW_NO_METAL_DEVICE}>:RYPATH_ALLOW_NO_METAL_DEVICE=1>"
        "$<$<BOOL:${RYPATH_ALLOW_NO_METAL_RAY_TRACING}>:RYPATH_ALLOW_NO_METAL_RAY_TRACING=1>")
    target_link_libraries(rypath_qmc_render_tests PRIVATE rypath_metal_backend)
    rypath_configure_host_target(rypath_qmc_render_tests)
    add_test(NAME metal.qmc_render COMMAND rypath_qmc_render_tests)
    set_tests_properties(metal.qmc_render PROPERTIES TIMEOUT 120)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.qmc_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(rypath_gaussian_render_tests gaussian_render_tests.cpp)
    set_target_properties(rypath_gaussian_render_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_link_libraries(rypath_gaussian_render_tests PRIVATE rypath_render rypath_metal_backend)
    rypath_configure_host_target(rypath_gaussian_render_tests)
    add_test(NAME metal.gaussian_filter COMMAND rypath_gaussian_render_tests --metal ${ray_tracing_test_options})
    set_tests_properties(metal.gaussian_filter PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.gaussian_filter PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(rypath_qmc_checkpoint_tests qmc_checkpoint_tests.cpp)
    set_target_properties(rypath_qmc_checkpoint_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_link_libraries(rypath_qmc_checkpoint_tests PRIVATE rypath_metal_backend rypath_checkpoint)
    rypath_configure_host_target(rypath_qmc_checkpoint_tests)
    add_test(NAME metal.qmc_checkpoint COMMAND rypath_qmc_checkpoint_tests --metal ${ray_tracing_test_options})
    set_tests_properties(metal.qmc_checkpoint PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.qmc_checkpoint PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    set_target_properties(rypath_sobol_render_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_sobol_render_tests PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_compile_definitions(rypath_sobol_render_tests PRIVATE
        "$<$<BOOL:${RYPATH_ALLOW_NO_METAL_DEVICE}>:RYPATH_ALLOW_NO_METAL_DEVICE=1>"
        "$<$<BOOL:${RYPATH_ALLOW_NO_METAL_RAY_TRACING}>:RYPATH_ALLOW_NO_METAL_RAY_TRACING=1>")
    target_link_libraries(rypath_sobol_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
    rypath_configure_host_target(rypath_sobol_render_tests)
    add_test(NAME metal.sobol_render COMMAND rypath_sobol_render_tests)
    set_tests_properties(metal.sobol_render PROPERTIES TIMEOUT 90)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.sobol_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    add_executable(rypath_image_output_tests image_output_tests.mm)
    set_target_properties(rypath_image_output_tests PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_image_output_tests PRIVATE -fobjc-arc)
    target_link_libraries(rypath_image_output_tests PRIVATE rypath_metal_backend)
    rypath_configure_host_target(rypath_image_output_tests)
    add_test(NAME render.png COMMAND rypath_image_output_tests)
    set_tests_properties(render.png PROPERTIES TIMEOUT 30)
else()
    add_test(NAME render.stub COMMAND rypath_render_tests stub)
    set_tests_properties(render.stub PROPERTIES TIMEOUT 30)
endif()

add_executable(rypath_geometry_tests geometry_tests.cpp geometry_reference.cpp)
set_target_properties(rypath_geometry_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_link_libraries(rypath_geometry_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_geometry_tests)

add_test(NAME geometry.cpu_reference COMMAND rypath_geometry_tests cpu)
set_tests_properties(geometry.cpu_reference PROPERTIES TIMEOUT 30)

if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.geometry COMMAND rypath_geometry_tests metal ${ray_tracing_test_options})
    set_tests_properties(metal.geometry PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.geometry PROPERTIES SKIP_RETURN_CODE 77)
    endif()
else()
    add_test(NAME geometry.stub COMMAND rypath_geometry_tests stub)
    set_tests_properties(geometry.stub PROPERTIES TIMEOUT 30)
endif()
add_executable(rypath_mesh_displacement_tests mesh_displacement_tests.cpp)
target_link_libraries(rypath_mesh_displacement_tests PRIVATE rypath_geometry)
rypath_configure_host_target(rypath_mesh_displacement_tests)
add_test(NAME geometry.displacement COMMAND rypath_mesh_displacement_tests)
set_tests_properties(geometry.displacement PROPERTIES TIMEOUT 30)

add_executable(rypath_texture_graph_tests texture_graph_tests.cpp)
target_link_libraries(rypath_texture_graph_tests PRIVATE rypath_render)
rypath_configure_host_target(rypath_texture_graph_tests)
add_test(NAME render.texture_graph COMMAND rypath_texture_graph_tests)
set_tests_properties(render.texture_graph PROPERTIES TIMEOUT 30)

add_executable(rypath_core_texture_scene_tests core_texture_scene_tests.cpp)
target_link_libraries(rypath_core_texture_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_core_texture_scene_tests)
add_test(NAME pbrt.core_texture_scene COMMAND rypath_core_texture_scene_tests
    "${CMAKE_CURRENT_BINARY_DIR}/core-texture-scene")
set_tests_properties(pbrt.core_texture_scene PROPERTIES TIMEOUT 30)

add_executable(rypath_texture_graph_reference "${PROJECT_SOURCE_DIR}/scripts/rypath_texture_graph_probe.cpp")
target_include_directories(rypath_texture_graph_reference PRIVATE "${PROJECT_SOURCE_DIR}/shaders")
target_link_libraries(rypath_texture_graph_reference PRIVATE rypath_render rypath_pbrt)
rypath_configure_host_target(rypath_texture_graph_reference)

add_executable(rypath_core_texture_render_tests core_texture_render_tests.cpp)
target_link_libraries(rypath_core_texture_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_core_texture_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_core_texture_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.core_texture_render COMMAND rypath_core_texture_render_tests
        "${CMAKE_CURRENT_BINARY_DIR}/core-texture-render"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt-render/core-textures"
        ${ray_tracing_test_options})
    set_tests_properties(metal.core_texture_render PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.core_texture_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_hdr_image_tests hdr_image_tests.cpp)
target_link_libraries(rypath_hdr_image_tests PRIVATE rypath_image)
rypath_configure_host_target(rypath_hdr_image_tests)
add_test(NAME image.hdr COMMAND rypath_hdr_image_tests)
set_tests_properties(image.hdr PROPERTIES TIMEOUT 30)

add_executable(rypath_exr_output_tests exr_output_tests.cpp)
target_link_libraries(rypath_exr_output_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_exr_output_tests)
add_test(NAME render.exr COMMAND rypath_exr_output_tests)
set_tests_properties(render.exr PROPERTIES TIMEOUT 30)

add_executable(rypath_exr_codec_tests exr_codec_tests.cpp)
target_link_libraries(rypath_exr_codec_tests PRIVATE rypath_image)
rypath_configure_host_target(rypath_exr_codec_tests)
add_test(NAME image.exr COMMAND rypath_exr_codec_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exr")
set_tests_properties(image.exr PROPERTIES TIMEOUT 60)

add_executable(rypath_hdr_scene_tests hdr_scene_tests.cpp)
target_link_libraries(rypath_hdr_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_hdr_scene_tests)
add_test(NAME pbrt.hdr_scene COMMAND rypath_hdr_scene_tests
    "${CMAKE_CURRENT_BINARY_DIR}/hdr-scene/$<CONFIG>")
set_tests_properties(pbrt.hdr_scene PROPERTIES TIMEOUT 30)

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_hdr_texture_probe hdr_texture_probe.mm)
    set_target_properties(rypath_hdr_texture_probe PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_hdr_texture_probe PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(rypath_hdr_texture_probe PRIVATE rypath_metal_backend)
    rypath_configure_host_target(rypath_hdr_texture_probe)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_hdr_texture_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    add_test(NAME metal.hdr_texture COMMAND rypath_hdr_texture_probe)
    set_tests_properties(metal.hdr_texture PROPERTIES TIMEOUT 30)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        set_tests_properties(metal.hdr_texture PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_conductor_scene_tests conductor_scene_tests.cpp)
target_link_libraries(rypath_conductor_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_conductor_scene_tests)
add_test(NAME pbrt.conductor_scene COMMAND rypath_conductor_scene_tests)
set_tests_properties(pbrt.conductor_scene PROPERTIES TIMEOUT 30)

add_executable(rypath_conductor_render_tests conductor_render_tests.cpp)
target_link_libraries(rypath_conductor_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_conductor_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_conductor_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.conductor_render COMMAND rypath_conductor_render_tests
        "${CMAKE_CURRENT_BINARY_DIR}/conductor-render"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt-render/conductor" ${ray_tracing_test_options})
    set_tests_properties(metal.conductor_render PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.conductor_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(rypath_conductor_probe conductor_probe.mm)
    target_link_libraries(rypath_conductor_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_conductor_probe)
    set_target_properties(rypath_conductor_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_conductor_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.conductor_bsdf COMMAND rypath_conductor_probe "${rypath_metal_library}")
    set_tests_properties(metal.conductor_bsdf PROPERTIES TIMEOUT 60)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_conductor_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.conductor_bsdf PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_test(NAME cli.conductor_rejection COMMAND "${CMAKE_COMMAND}"
    -Dexecutable=$<TARGET_FILE:rypath>
    -Dfixture=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt-render/conductor/smooth-cu-environment.pbrt
    -Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/conductor-cli
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_conductor_cli.cmake")
set_tests_properties(cli.conductor_rejection PROPERTIES TIMEOUT 30)

add_test(NAME cli.dielectric_rejection COMMAND "${CMAKE_COMMAND}"
    -Dexecutable=$<TARGET_FILE:rypath>
    -Dtest_directory=${CMAKE_CURRENT_BINARY_DIR}/dielectric-cli
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_dielectric_cli.cmake")
set_tests_properties(cli.dielectric_rejection PROPERTIES TIMEOUT 30)

add_executable(rypath_dielectric_scene_tests dielectric_scene_tests.cpp)
target_link_libraries(rypath_dielectric_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_dielectric_scene_tests)
add_test(NAME pbrt.dielectric_scene COMMAND rypath_dielectric_scene_tests)
set_tests_properties(pbrt.dielectric_scene PROPERTIES TIMEOUT 30)

add_executable(rypath_dielectric_render_tests dielectric_render_tests.cpp)
target_link_libraries(rypath_dielectric_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_dielectric_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_dielectric_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.dielectric_render COMMAND rypath_dielectric_render_tests
        "${CMAKE_CURRENT_BINARY_DIR}/dielectric-render"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt-render/dielectric" ${ray_tracing_test_options})
    set_tests_properties(metal.dielectric_render PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.dielectric_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(rypath_dielectric_probe dielectric_probe.mm)
    target_link_libraries(rypath_dielectric_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_dielectric_probe)
    set_target_properties(rypath_dielectric_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_dielectric_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.dielectric_bsdf COMMAND rypath_dielectric_probe "${rypath_metal_library}")
    set_tests_properties(metal.dielectric_bsdf PROPERTIES TIMEOUT 60)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_dielectric_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.dielectric_bsdf PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_surface_material_scene_tests surface_material_scene_tests.cpp)
target_link_libraries(rypath_surface_material_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_surface_material_scene_tests)
add_test(NAME pbrt.surface_materials COMMAND rypath_surface_material_scene_tests)
set_tests_properties(pbrt.surface_materials PROPERTIES TIMEOUT 30)

add_executable(rypath_bsdf_table_tests bsdf_table_tests.cpp)
target_link_libraries(rypath_bsdf_table_tests PRIVATE rypath_bsdf_tables)
rypath_configure_host_target(rypath_bsdf_table_tests)
add_test(NAME render.bsdf_tables COMMAND rypath_bsdf_table_tests)
set_tests_properties(render.bsdf_tables PROPERTIES TIMEOUT 30)

if(RYPATH_ENABLE_METAL)
    foreach(probe IN ITEMS layered table_bsdf medium)
        add_executable(rypath_${probe}_probe ${probe}_probe.mm)
        target_link_libraries(rypath_${probe}_probe PRIVATE rypath_metal_backend rypath_render rypath_bsdf_tables)
        rypath_configure_host_target(rypath_${probe}_probe)
        set_target_properties(rypath_${probe}_probe PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
            OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
        target_compile_options(rypath_${probe}_probe PRIVATE -fobjc-arc)
        if(probe STREQUAL "layered")
            set(probe_test metal.layered_bsdf)
        elseif(probe STREQUAL "medium")
            set(probe_test metal.homogeneous_math)
        else()
            set(probe_test metal.table_bsdf)
        endif()
        add_test(NAME ${probe_test} COMMAND rypath_${probe}_probe "${rypath_metal_library}")
        set_tests_properties(${probe_test} PROPERTIES TIMEOUT 90)
        if(RYPATH_ALLOW_NO_METAL_DEVICE)
            target_compile_definitions(rypath_${probe}_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
            set_tests_properties(${probe_test} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()

add_executable(rypath_surface_material_render_tests surface_material_render_tests.cpp)
target_link_libraries(rypath_surface_material_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render rypath_bsdf_tables)
set_target_properties(rypath_surface_material_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_surface_material_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.surface_materials COMMAND rypath_surface_material_render_tests
        "${CMAKE_CURRENT_BINARY_DIR}/surface-material-render"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pbrt-render/surface-materials" ${ray_tracing_test_options})
    set_tests_properties(metal.surface_materials PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.surface_materials PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_point_scene_tests point_scene_tests.cpp)
target_link_libraries(rypath_point_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_point_scene_tests)
add_test(NAME pbrt.point_lights COMMAND rypath_point_scene_tests)

add_executable(rypath_point_render_tests point_render_tests.cpp)
target_link_libraries(rypath_point_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_point_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_point_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.point_lights COMMAND rypath_point_render_tests ${ray_tracing_test_options})
    set_tests_properties(metal.point_lights PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.point_lights PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_spot_scene_tests spot_scene_tests.cpp)
target_link_libraries(rypath_spot_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_spot_scene_tests)
add_test(NAME pbrt.spot_lights COMMAND rypath_spot_scene_tests)

add_executable(rypath_spot_render_tests spot_render_tests.cpp)
target_link_libraries(rypath_spot_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_spot_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_spot_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.spot_lights COMMAND rypath_spot_render_tests ${ray_tracing_test_options})
    set_tests_properties(metal.spot_lights PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.spot_lights PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_distant_scene_tests distant_scene_tests.cpp)
target_link_libraries(rypath_distant_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_distant_scene_tests)
add_test(NAME pbrt.distant_lights COMMAND rypath_distant_scene_tests)

add_executable(rypath_distant_render_tests distant_render_tests.cpp)
target_link_libraries(rypath_distant_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_distant_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_distant_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.distant_lights COMMAND rypath_distant_render_tests ${ray_tracing_test_options})
    set_tests_properties(metal.distant_lights PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.distant_lights PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_medium_render_tests medium_render_tests.cpp)
target_link_libraries(rypath_medium_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
target_compile_definitions(rypath_medium_render_tests PRIVATE RYPATH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(rypath_medium_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_medium_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.homogeneous_transport COMMAND rypath_medium_render_tests ${ray_tracing_test_options})
    set_tests_properties(metal.homogeneous_transport PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.homogeneous_transport PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_gltf_render_tests gltf_render_tests.cpp)
    target_link_libraries(rypath_gltf_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render rypath_checkpoint)
    rypath_configure_host_target(rypath_gltf_render_tests)
    set_target_properties(rypath_gltf_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    add_test(NAME metal.gltf_render COMMAND rypath_gltf_render_tests)
    set_tests_properties(metal.gltf_render PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_gltf_render_tests PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        target_compile_definitions(rypath_gltf_render_tests PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.gltf_render PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_gltf_probe gltf_probe.mm)
    target_link_libraries(rypath_gltf_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_gltf_probe)
    set_target_properties(rypath_gltf_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_gltf_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.gltf_bsdf COMMAND rypath_gltf_probe "${rypath_metal_library}")
    set_tests_properties(metal.gltf_bsdf PROPERTIES TIMEOUT 120)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_gltf_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.gltf_bsdf PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

foreach(suite IN ITEMS gltf gltf_bundle)
    add_executable(rypath_${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(rypath_${suite}_tests PRIVATE rypath_gltf)
    target_compile_definitions(rypath_${suite}_tests PRIVATE RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    rypath_configure_host_target(rypath_${suite}_tests)
    add_test(NAME import.${suite} COMMAND rypath_${suite}_tests)
    set_tests_properties(import.${suite} PROPERTIES TIMEOUT 60)
endforeach()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_gltf_texture_probe gltf_texture_probe.mm)
    target_link_libraries(rypath_gltf_texture_probe PRIVATE rypath_metal_backend rypath_render)
    rypath_configure_host_target(rypath_gltf_texture_probe)
    set_target_properties(rypath_gltf_texture_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_gltf_texture_probe PRIVATE -fobjc-arc)
    add_test(NAME metal.gltf_textures COMMAND rypath_gltf_texture_probe "${rypath_metal_library}")
    set_tests_properties(metal.gltf_textures PROPERTIES TIMEOUT 60)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_gltf_texture_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.gltf_textures PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_goniometric_image_tests goniometric_image_tests.cpp)
target_link_libraries(rypath_goniometric_image_tests PRIVATE rypath_pbrt)
rypath_configure_host_target(rypath_goniometric_image_tests)
add_test(NAME pbrt.goniometric_images COMMAND rypath_goniometric_image_tests
    "${CMAKE_CURRENT_BINARY_DIR}/goniometric-images")

add_executable(rypath_goniometric_scene_tests goniometric_scene_tests.cpp)
target_link_libraries(rypath_goniometric_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_goniometric_scene_tests)
add_test(NAME pbrt.goniometric_lights COMMAND rypath_goniometric_scene_tests)

add_executable(rypath_projection_scene_tests projection_scene_tests.cpp)
target_link_libraries(rypath_projection_scene_tests PRIVATE rypath_pbrt_render)
rypath_configure_host_target(rypath_projection_scene_tests)
add_test(NAME pbrt.projection_lights COMMAND rypath_projection_scene_tests)

add_executable(rypath_projection_render_tests projection_render_tests.cpp)
target_link_libraries(rypath_projection_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_projection_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_projection_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.projection_lights COMMAND rypath_projection_render_tests ${ray_tracing_test_options})
    set_tests_properties(metal.projection_lights PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.projection_lights PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_goniometric_render_tests goniometric_render_tests.cpp)
target_link_libraries(rypath_goniometric_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_goniometric_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_goniometric_render_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.goniometric_lights COMMAND rypath_goniometric_render_tests ${ray_tracing_test_options})
    set_tests_properties(metal.goniometric_lights PROPERTIES TIMEOUT 180)
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.goniometric_lights PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_lens_scene_tests lens_scene_tests.cpp)
target_link_libraries(rypath_lens_scene_tests PRIVATE rypath_pbrt_render rypath_checkpoint)
rypath_configure_host_target(rypath_lens_scene_tests)
add_test(NAME pbrt.perspective_lens COMMAND rypath_lens_scene_tests)
set_tests_properties(pbrt.perspective_lens PROPERTIES TIMEOUT 30)

add_executable(rypath_lens_render_tests lens_render_tests.cpp)
target_link_libraries(rypath_lens_render_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
set_target_properties(rypath_lens_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_lens_render_tests)
if(RYPATH_ENABLE_METAL)
    target_sources(rypath_lens_render_tests PRIVATE lens_probe.mm)
    target_compile_definitions(rypath_lens_render_tests PRIVATE RYPATH_ENABLE_LENS_PROBE=1)
    set_target_properties(rypath_lens_render_tests PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_lens_render_tests PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    add_test(NAME metal.perspective_lens COMMAND rypath_lens_render_tests ${ray_tracing_test_options})
    set_tests_properties(metal.perspective_lens PROPERTIES TIMEOUT 180
        ENVIRONMENT "MTL_DEBUG_LAYER=1")
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.perspective_lens PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_area_power_tests area_power_tests.cpp)
target_link_libraries(rypath_area_power_tests PRIVATE rypath_pbrt_render rypath_checkpoint)
rypath_configure_host_target(rypath_area_power_tests)
add_test(NAME render.area_power COMMAND rypath_area_power_tests)
set_tests_properties(render.area_power PROPERTIES TIMEOUT 30)

add_executable(rypath_native_hint_budget_tests native_hint_budget_tests.cpp)
target_include_directories(rypath_native_hint_budget_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
if(NOT RYPATH_ENABLE_METAL)
    target_link_libraries(rypath_native_hint_budget_tests PRIVATE rypath_metal_backend)
    target_compile_definitions(rypath_native_hint_budget_tests PRIVATE RYPATH_TEST_METAL_STUB=1)
endif()
rypath_configure_host_target(rypath_native_hint_budget_tests)
add_test(NAME geometry.native_hint_budget COMMAND rypath_native_hint_budget_tests)

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_native_triangle_hint_probe native_triangle_hint/probe.mm geometry_reference.cpp)
    target_link_libraries(rypath_native_triangle_hint_probe PRIVATE rypath_metal_backend "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    target_include_directories(rypath_native_triangle_hint_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(rypath_native_triangle_hint_probe PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_native_triangle_hint_probe PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    rypath_configure_host_target(rypath_native_triangle_hint_probe)
    add_dependencies(rypath_native_triangle_hint_probe rypath_native_hint_shaders)
    add_test(NAME metal.native_triangle_hint COMMAND rypath_native_triangle_hint_probe "${rypath_native_hint_control_library}" "${rypath_native_hint_test_library}" "${rypath_metal_library}")
    set_tests_properties(metal.native_triangle_hint PROPERTIES TIMEOUT 300)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_native_triangle_hint_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        target_compile_definitions(rypath_native_triangle_hint_probe PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.native_triangle_hint PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_acceleration_build_policy_gpu_tests acceleration_build_policy_gpu_tests.mm geometry_reference.cpp)
    target_link_libraries(rypath_acceleration_build_policy_gpu_tests PRIVATE rypath_metal_backend "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_acceleration_build_policy_gpu_tests PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_acceleration_build_policy_gpu_tests PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    rypath_configure_host_target(rypath_acceleration_build_policy_gpu_tests)
    add_test(NAME metal.acceleration_build_policy COMMAND rypath_acceleration_build_policy_gpu_tests "${rypath_metal_library}")
    set_tests_properties(metal.acceleration_build_policy PROPERTIES TIMEOUT 120 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_acceleration_build_policy_gpu_tests PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        target_compile_definitions(rypath_acceleration_build_policy_gpu_tests PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.acceleration_build_policy PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(rypath_native_intersection_profile_tests native_intersection_profile_tests.mm geometry_reference.cpp)
    target_link_libraries(rypath_native_intersection_profile_tests PRIVATE rypath_metal_backend "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_native_intersection_profile_tests PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_native_intersection_profile_tests PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    rypath_configure_host_target(rypath_native_intersection_profile_tests)
    add_test(NAME metal.native_intersection_profile COMMAND rypath_native_intersection_profile_tests "${rypath_metal_library}")
    set_tests_properties(metal.native_intersection_profile PROPERTIES TIMEOUT 120)
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_native_intersection_profile_tests PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        target_compile_definitions(rypath_native_intersection_profile_tests PRIVATE RYPATH_ALLOW_NO_METAL_RAY_TRACING=1)
    endif()
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.native_intersection_profile PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_wavefront_frame_tests wavefront_frame_tests.cpp)
target_link_libraries(rypath_wavefront_frame_tests PRIVATE rypath_metal_backend rypath_pbrt_render)
target_compile_definitions(rypath_wavefront_frame_tests PRIVATE
    RYPATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    RYPATH_TEST_WAVEFRONT_METAL=$<BOOL:${RYPATH_ENABLE_METAL}>)
set_target_properties(rypath_wavefront_frame_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_wavefront_frame_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.wavefront_frame COMMAND rypath_wavefront_frame_tests)
    # Seven policy specializations can exceed five minutes on a cold Metal compiler cache.
    set_tests_properties(metal.wavefront_frame PROPERTIES TIMEOUT 900 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    foreach(capability DEVICE RAY_TRACING)
        if(RYPATH_ALLOW_NO_METAL_${capability})
            target_compile_definitions(rypath_wavefront_frame_tests PRIVATE RYPATH_ALLOW_NO_METAL_${capability}=1)
            set_tests_properties(metal.wavefront_frame PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
else()
    add_test(NAME frame.wavefront_unavailable COMMAND rypath_wavefront_frame_tests)
endif()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_wavefront_queue_tests wavefront_queue_tests.mm)
    target_link_libraries(rypath_wavefront_queue_tests PRIVATE rypath_metal_backend "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_wavefront_queue_tests PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_wavefront_queue_tests PRIVATE -fobjc-arc)
    rypath_configure_host_target(rypath_wavefront_queue_tests)
    add_test(NAME metal.wavefront_queue COMMAND rypath_wavefront_queue_tests "${rypath_metal_library}")
    set_tests_properties(metal.wavefront_queue PROPERTIES TIMEOUT 60 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_wavefront_queue_tests PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.wavefront_queue PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_reservoir_tests reservoir_tests.cpp)
target_include_directories(rypath_reservoir_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_reservoir_tests)
target_compile_options(rypath_reservoir_tests PRIVATE
    "$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-ffp-contract=off;-fno-fast-math>")
add_test(NAME render.reservoir_finite_domain COMMAND rypath_reservoir_tests)
set_tests_properties(render.reservoir_finite_domain PROPERTIES TIMEOUT 30)

add_executable(rypath_replay_tests replay_tests.cpp)
target_include_directories(rypath_replay_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_replay_tests)
add_test(NAME render.replay_identity COMMAND rypath_replay_tests)
set_tests_properties(render.replay_identity PROPERTIES TIMEOUT 30)

add_executable(rypath_area_frame_tests area_frame_tests.cpp)
target_link_libraries(rypath_area_frame_tests PRIVATE rypath_metal_backend)
target_compile_definitions(rypath_area_frame_tests PRIVATE
    RYPATH_TEST_AREA_METAL=$<BOOL:${RYPATH_ENABLE_METAL}>)
set_target_properties(rypath_area_frame_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_area_frame_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.area_frame COMMAND rypath_area_frame_tests)
    set_tests_properties(metal.area_frame PROPERTIES TIMEOUT 300 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    foreach(capability DEVICE RAY_TRACING)
        if(RYPATH_ALLOW_NO_METAL_${capability})
            target_compile_definitions(rypath_area_frame_tests PRIVATE RYPATH_ALLOW_NO_METAL_${capability}=1)
            set_tests_properties(metal.area_frame PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
else()
    add_test(NAME frame.area_unavailable COMMAND rypath_area_frame_tests)
endif()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_area_math_probe area_math_probe.mm)
    target_link_libraries(rypath_area_math_probe PRIVATE "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_area_math_probe PROPERTIES OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_area_math_probe PRIVATE -fobjc-arc)
    target_compile_options(rypath_area_math_probe PRIVATE
        "$<$<COMPILE_LANG_AND_ID:OBJCXX,AppleClang,Clang,GNU>:-ffp-contract=off;-fno-fast-math>")
    rypath_configure_host_target(rypath_area_math_probe)
    add_dependencies(rypath_area_math_probe rypath_area_math_shader)
    add_test(NAME metal.area_math COMMAND rypath_area_math_probe "${rypath_area_math_library}")
    set_tests_properties(metal.area_math PROPERTIES TIMEOUT 60 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_area_math_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.area_math PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_bounded_target_tests bounded_target_tests.cpp)
target_include_directories(rypath_bounded_target_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_bounded_target_tests)
target_compile_options(rypath_bounded_target_tests PRIVATE
    "$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-ffp-contract=off;-fno-fast-math>")
add_test(NAME render.bounded_target COMMAND rypath_bounded_target_tests)
set_tests_properties(render.bounded_target PROPERTIES TIMEOUT 30)

add_executable(rypath_mutation_tests mutation_tests.cpp)
target_include_directories(rypath_mutation_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_mutation_tests)
target_compile_options(rypath_mutation_tests PRIVATE
    "$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-ffp-contract=off;-fno-fast-math>")
add_test(NAME render.mutation_weights COMMAND rypath_mutation_tests)
set_tests_properties(render.mutation_weights PROPERTIES TIMEOUT 30)

add_executable(rypath_mutation_frame_tests mutation_frame_tests.cpp)
target_link_libraries(rypath_mutation_frame_tests PRIVATE rypath_metal_backend)
target_compile_definitions(rypath_mutation_frame_tests PRIVATE
    RYPATH_TEST_MUTATION_METAL=$<BOOL:${RYPATH_ENABLE_METAL}>)
set_target_properties(rypath_mutation_frame_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
rypath_configure_host_target(rypath_mutation_frame_tests)
if(RYPATH_ENABLE_METAL)
    add_test(NAME metal.mutation_frame COMMAND rypath_mutation_frame_tests)
    set_tests_properties(metal.mutation_frame PROPERTIES TIMEOUT 300 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    foreach(capability DEVICE RAY_TRACING)
        if(RYPATH_ALLOW_NO_METAL_${capability})
            target_compile_definitions(rypath_mutation_frame_tests PRIVATE RYPATH_ALLOW_NO_METAL_${capability}=1)
            set_tests_properties(metal.mutation_frame PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
else()
    add_test(NAME frame.mutation_unavailable COMMAND rypath_mutation_frame_tests)
endif()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_mutation_math_probe mutation_math_probe.mm)
    target_link_libraries(rypath_mutation_math_probe PRIVATE "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_mutation_math_probe PROPERTIES OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_mutation_math_probe PRIVATE -fobjc-arc)
    target_compile_options(rypath_mutation_math_probe PRIVATE
        "$<$<COMPILE_LANG_AND_ID:OBJCXX,AppleClang,Clang,GNU>:-ffp-contract=off;-fno-fast-math>")
    rypath_configure_host_target(rypath_mutation_math_probe)
    add_dependencies(rypath_mutation_math_probe rypath_mutation_math_shader)
    add_test(NAME metal.mutation_math COMMAND rypath_mutation_math_probe "${rypath_mutation_math_library}")
    set_tests_properties(metal.mutation_math PROPERTIES TIMEOUT 60 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    if(RYPATH_ALLOW_NO_METAL_DEVICE)
        target_compile_definitions(rypath_mutation_math_probe PRIVATE RYPATH_ALLOW_NO_METAL_DEVICE=1)
        set_tests_properties(metal.mutation_math PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(rypath_acceleration_build_policy_tests acceleration_build_policy_tests.cpp)
target_link_libraries(rypath_acceleration_build_policy_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_acceleration_build_policy_tests)
add_test(NAME acceleration.build_policy COMMAND rypath_acceleration_build_policy_tests)

add_executable(rypath_reconstruction_camera_tests reconstruction_camera_tests.cpp)
target_link_libraries(rypath_reconstruction_camera_tests PRIVATE rypath_reconstruction)
rypath_configure_host_target(rypath_reconstruction_camera_tests)
add_test(NAME reconstruction.camera COMMAND rypath_reconstruction_camera_tests)

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_metalfx_conditioning_tests metalfx_conditioning_tests.mm
        ../src/metalfx_conditioning.mm)
    target_link_libraries(rypath_metalfx_conditioning_tests PRIVATE rypath_reconstruction
        "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_metalfx_conditioning_tests PROPERTIES OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_metalfx_conditioning_tests PRIVATE -fobjc-arc)
    rypath_configure_host_target(rypath_metalfx_conditioning_tests)
    if(RYPATH_ALLOW_NO_METALFX_DENOISER)
        add_test(NAME metal.reconstruction_conditioning COMMAND rypath_metalfx_conditioning_tests)
    else()
        add_test(NAME metal.reconstruction_conditioning COMMAND rypath_metalfx_conditioning_tests --require-supported)
    endif()
    set_tests_properties(metal.reconstruction_conditioning PROPERTIES TIMEOUT 120
        ENVIRONMENT "MTL_DEBUG_LAYER=1" SKIP_RETURN_CODE 77)
    add_executable(rypath_metalfx_denoiser_tests metalfx_denoiser_tests.mm)
    target_link_libraries(rypath_metalfx_denoiser_tests PRIVATE rypath_reconstruction
        "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_metalfx_denoiser_tests PROPERTIES OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_metalfx_denoiser_tests PRIVATE -fobjc-arc)
    rypath_configure_host_target(rypath_metalfx_denoiser_tests)
    if(RYPATH_ALLOW_NO_METALFX_DENOISER)
        add_test(NAME metal.reconstruction_adapter COMMAND rypath_metalfx_denoiser_tests)
    else()
        add_test(NAME metal.reconstruction_adapter COMMAND rypath_metalfx_denoiser_tests --require-supported)
    endif()
    set_tests_properties(metal.reconstruction_adapter PROPERTIES TIMEOUT 120
        ENVIRONMENT "MTL_DEBUG_LAYER=1" SKIP_RETURN_CODE 77)
endif()

if(RYPATH_ENABLE_METAL)
    foreach(kind IN ITEMS stream guide)
        add_executable(rypath_reconstruction_${kind}_tests reconstruction_${kind}_tests.mm)
        target_link_libraries(rypath_reconstruction_${kind}_tests PRIVATE rypath_reconstruction
            "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
        set_target_properties(rypath_reconstruction_${kind}_tests PROPERTIES OBJCXX_STANDARD 20
            OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO
            RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
        target_compile_options(rypath_reconstruction_${kind}_tests PRIVATE -fobjc-arc)
        rypath_configure_host_target(rypath_reconstruction_${kind}_tests)
        foreach(capability DEVICE RAY_TRACING)
            if(RYPATH_ALLOW_NO_METAL_${capability})
                target_compile_definitions(rypath_reconstruction_${kind}_tests PRIVATE
                    RYPATH_ALLOW_NO_METAL_${capability}=1)
            endif()
        endforeach()
    endforeach()
    if(RYPATH_ALLOW_NO_METALFX_DENOISER)
        target_compile_definitions(rypath_reconstruction_stream_tests PRIVATE
            RYPATH_ALLOW_NO_METALFX_DENOISER=1)
    endif()
    add_test(NAME metal.reconstruction_raw COMMAND rypath_reconstruction_stream_tests --raw)
    add_test(NAME metal.reconstruction_temporal COMMAND rypath_reconstruction_stream_tests --metalfx)
    add_test(NAME metal.reconstruction_signed COMMAND rypath_reconstruction_stream_tests --temporal)
    add_test(NAME metal.reconstruction_guides COMMAND rypath_reconstruction_guide_tests "${rypath_metal_library}")
    set_tests_properties(metal.reconstruction_raw metal.reconstruction_temporal metal.reconstruction_signed
        metal.reconstruction_guides PROPERTIES TIMEOUT 240 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    if(RYPATH_ALLOW_NO_METAL_DEVICE OR RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        set_tests_properties(metal.reconstruction_raw metal.reconstruction_temporal metal.reconstruction_signed
            metal.reconstruction_guides PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    if(RYPATH_ALLOW_NO_METALFX_DENOISER)
        set_tests_properties(metal.reconstruction_temporal PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_temporal_denoiser_tests temporal_denoiser_tests.mm)
    target_link_libraries(rypath_temporal_denoiser_tests PRIVATE rypath_reconstruction
        "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_temporal_denoiser_tests PROPERTIES OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_temporal_denoiser_tests PRIVATE -fobjc-arc)
    rypath_configure_host_target(rypath_temporal_denoiser_tests)
    add_test(NAME metal.temporal_denoiser COMMAND rypath_temporal_denoiser_tests)
    set_tests_properties(metal.temporal_denoiser PROPERTIES TIMEOUT 240 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    foreach(capability DEVICE RAY_TRACING)
        if(RYPATH_ALLOW_NO_METAL_${capability})
            target_compile_definitions(rypath_temporal_denoiser_tests PRIVATE RYPATH_ALLOW_NO_METAL_${capability}=1)
            set_tests_properties(metal.temporal_denoiser PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_camera_medium_tests camera_medium_tests.cpp)
    target_link_libraries(rypath_camera_medium_tests PRIVATE rypath_camera_motion)
    set_target_properties(rypath_camera_medium_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    rypath_configure_host_target(rypath_camera_medium_tests)
    add_test(NAME metal.camera_medium COMMAND rypath_camera_medium_tests "${rypath_metal_library}")
    set_tests_properties(metal.camera_medium PROPERTIES TIMEOUT 120 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    foreach(capability DEVICE RAY_TRACING)
        if(RYPATH_ALLOW_NO_METAL_${capability})
            target_compile_definitions(rypath_camera_medium_tests PRIVATE RYPATH_ALLOW_NO_METAL_${capability}=1)
            set_tests_properties(metal.camera_medium PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()

add_executable(rypath_viewer_options_tests viewer_options_tests.cpp ../src/viewer_options.cpp)
target_include_directories(rypath_viewer_options_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_viewer_options_tests)
add_test(NAME viewer.options COMMAND rypath_viewer_options_tests)

add_executable(rypath_gpu_diagnostics_tests gpu_diagnostics_tests.cpp)
target_include_directories(rypath_gpu_diagnostics_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_gpu_diagnostics_tests)
add_test(NAME frame.gpu_diagnostics COMMAND rypath_gpu_diagnostics_tests)
add_executable(rypath_shadow_traversal_tests shadow_traversal_tests.cpp)
target_link_libraries(rypath_shadow_traversal_tests PRIVATE rypath_metal_backend)
rypath_configure_host_target(rypath_shadow_traversal_tests)
add_test(NAME render.shadow_traversal COMMAND rypath_shadow_traversal_tests)

if(RYPATH_ENABLE_METAL)
    add_executable(rypath_opaque_shadow_tests opaque_shadow_tests.mm geometry_reference.cpp)
    target_link_libraries(rypath_opaque_shadow_tests PRIVATE rypath_metal_backend "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    set_target_properties(rypath_opaque_shadow_tests PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
    target_compile_options(rypath_opaque_shadow_tests PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    rypath_configure_host_target(rypath_opaque_shadow_tests)
    add_dependencies(rypath_opaque_shadow_tests rypath_opaque_capability_shader)
    add_test(NAME metal.opaque_shadows COMMAND rypath_opaque_shadow_tests "${rypath_metal_library}" "${rypath_opaque_capability_library}")
    set_tests_properties(metal.opaque_shadows PROPERTIES TIMEOUT 120 ENVIRONMENT "MTL_DEBUG_LAYER=1")
    foreach(capability DEVICE RAY_TRACING)
        if(RYPATH_ALLOW_NO_METAL_${capability})
            target_compile_definitions(rypath_opaque_shadow_tests PRIVATE RYPATH_ALLOW_NO_METAL_${capability}=1)
            set_tests_properties(metal.opaque_shadows PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()
