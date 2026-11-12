cmake_minimum_required(VERSION 3.25)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND
   (NOT DEFINED RYPATH_ENABLE_METAL OR RYPATH_ENABLE_METAL) AND
   NOT CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(rypath VERSION 0.1.0 LANGUAGES CXX)

set(rypath_macos OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(rypath_macos ON)
endif()

option(RYPATH_ENABLE_METAL "Build the macOS Metal backend" ${rypath_macos})
option(RYPATH_ALLOW_NO_METAL_DEVICE "Allow Metal tests to skip when no device is visible" OFF)
option(RYPATH_ALLOW_NO_METAL_RAY_TRACING
    "Allow ray tracing tests to skip when the Metal device lacks compute ray tracing" OFF)
option(RYPATH_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(RYPATH_ENABLE_METAL AND NOT rypath_macos)
    message(FATAL_ERROR "RYPATH_ENABLE_METAL=ON requires a macOS target.")
endif()

if(RYPATH_ENABLE_SANITIZERS AND
   (MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$"))
    message(FATAL_ERROR "The sanitizer build requires a compiler with GCC-style sanitizer flags.")
endif()

function(rypath_configure_host_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>"
            "$<$<COMPILE_LANG_AND_ID:OBJCXX,AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>"
        )
    endif()
    if(RYPATH_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize-recover=undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

set(rypath_runtime_directory "${CMAKE_CURRENT_BINARY_DIR}")
get_property(rypath_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(rypath_multi_config)
    string(APPEND rypath_runtime_directory "/$<CONFIG>")
endif()

add_library(rypath_geometry STATIC src/geometry.cpp)
target_include_directories(rypath_geometry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_geometry)

add_library(rypath_pbrt STATIC src/pbrt.cpp src/pbrt_parser.cpp src/pbrt_scene.cpp)
target_include_directories(rypath_pbrt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rypath_pbrt PRIVATE rypath_geometry)
rypath_configure_host_target(rypath_pbrt)

include(cmake/CieData.cmake)
set(rypath_generated_directory "${CMAKE_CURRENT_BINARY_DIR}/generated")
rypath_generate_cie_data("${rypath_generated_directory}/cie_xyz_data.hpp")
include(cmake/RgbSpectrumData.cmake)
set(rypath_rgb_data_source "${rypath_generated_directory}/rgb_spectrum_data.cpp")
rypath_generate_rgb_spectrum_data("${rypath_rgb_data_source}")

add_library(rypath_rgb_spectrum STATIC src/rgb_spectrum.cpp "${rypath_rgb_data_source}")
target_include_directories(rypath_rgb_spectrum PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
rypath_configure_host_target(rypath_rgb_spectrum)

add_library(rypath_render STATIC src/render.cpp)
target_include_directories(rypath_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${rypath_generated_directory}")
target_link_libraries(rypath_render PRIVATE rypath_geometry PUBLIC rypath_rgb_spectrum)
rypath_configure_host_target(rypath_render)

add_library(rypath_pbrt_render STATIC src/pbrt_render.cpp)
target_include_directories(rypath_pbrt_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rypath_pbrt_render PUBLIC rypath_pbrt rypath_render)
rypath_configure_host_target(rypath_pbrt_render)

add_library(rypath_metal_backend STATIC src/render_driver.cpp)
target_include_directories(rypath_metal_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rypath_metal_backend PRIVATE rypath_geometry rypath_render)
rypath_configure_host_target(rypath_metal_backend)

if(RYPATH_ENABLE_METAL)
    if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "14.0")
        message(FATAL_ERROR "The Metal backend requires CMAKE_OSX_DEPLOYMENT_TARGET >= 14.0.")
    endif()
    enable_language(OBJCXX)
    find_library(RYPATH_METAL_FRAMEWORK Metal REQUIRED)
    find_library(RYPATH_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(RYPATH_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(RYPATH_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(RYPATH_IMAGEIO_FRAMEWORK ImageIO REQUIRED)

    target_sources(rypath_metal_backend PRIVATE
        src/metal_backend.mm src/metal_geometry.mm src/metal_render.mm src/image_output.mm)
    set_target_properties(rypath_metal_backend PROPERTIES
        OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED YES
        OBJCXX_EXTENSIONS NO
    )
    target_compile_options(rypath_metal_backend PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(rypath_metal_backend PRIVATE
        "${RYPATH_METAL_FRAMEWORK}"
        "${RYPATH_FOUNDATION_FRAMEWORK}"
        "${RYPATH_COREGRAPHICS_FRAMEWORK}"
        "${RYPATH_COREFOUNDATION_FRAMEWORK}"
        "${RYPATH_IMAGEIO_FRAMEWORK}"
    )

    include(cmake/MetalShaders.cmake)
else()
    target_sources(rypath_metal_backend PRIVATE
        src/metal_backend_stub.cpp src/metal_geometry_stub.cpp
        src/metal_render_stub.cpp src/image_output_stub.cpp)
endif()

add_executable(rypath src/main.cpp)
set_target_properties(rypath PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${rypath_runtime_directory}")
target_compile_definitions(rypath PRIVATE RYPATH_VERSION="${PROJECT_VERSION}")
target_link_libraries(rypath PRIVATE rypath_metal_backend rypath_pbrt_render)
rypath_configure_host_target(rypath)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
