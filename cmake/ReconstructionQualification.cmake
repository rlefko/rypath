# Explicit diagnostic tools; ordinary builds acquire no new dependency.
if(RYPATH_ENABLE_METAL)
    add_executable(rypath_reconstruction_diagnostics src/reconstruction_diagnostics.mm)
    set_target_properties(rypath_reconstruction_diagnostics PROPERTIES
        OUTPUT_NAME rypath-reconstruction-diagnostics
        RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(rypath_reconstruction_diagnostics PRIVATE -fobjc-arc)
    target_compile_definitions(rypath_reconstruction_diagnostics PRIVATE
        RYPATH_BUILD_DESCRIPTION="${PROJECT_VERSION} ${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION} $<CONFIG> source=${rypath_source_digest} flags=${rypath_flags_digest}")
    target_link_libraries(rypath_reconstruction_diagnostics PRIVATE
        rypath_reconstruction rypath_pbrt_render rypath_checkpoint
        "${RYPATH_METAL_FRAMEWORK}" "${RYPATH_FOUNDATION_FRAMEWORK}")
    rypath_configure_host_target(rypath_reconstruction_diagnostics)
endif()
