# A build/source fingerprint is provenance, never a resume compatibility gate.
file(GLOB_RECURSE rypath_provenance_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/*")
list(APPEND rypath_provenance_sources "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(SORT rypath_provenance_sources)
set(rypath_source_manifest "")
foreach(source IN LISTS rypath_provenance_sources)
    file(SHA256 "${source}" digest)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${source}")
    string(APPEND rypath_source_manifest "${relative}:${digest}\n")
endforeach()
get_property(rypath_configure_inputs DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
list(APPEND rypath_configure_inputs ${rypath_provenance_sources})
set(rypath_normalized_inputs)
foreach(source IN LISTS rypath_configure_inputs)
    cmake_path(NORMAL_PATH source OUTPUT_VARIABLE normalized)
    list(APPEND rypath_normalized_inputs "${normalized}")
endforeach()
list(REMOVE_DUPLICATES rypath_normalized_inputs)
set_property(DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS ${rypath_normalized_inputs})
string(SHA256 rypath_source_digest "${rypath_source_manifest}")
string(TOUPPER "${CMAKE_BUILD_TYPE}" rypath_config_upper)
string(SHA256 rypath_flags_digest
    "${CMAKE_CXX_FLAGS}|${CMAKE_CXX_FLAGS_${rypath_config_upper}}|${CMAKE_OSX_DEPLOYMENT_TARGET}|${CMAKE_OSX_SYSROOT}|${RYPATH_ENABLE_SANITIZERS}")
target_compile_definitions(rypath PRIVATE
    RYPATH_BUILD_DESCRIPTION="${PROJECT_VERSION} ${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION} $<CONFIG> source=${rypath_source_digest} flags=${rypath_flags_digest}")
