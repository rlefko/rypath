cmake_minimum_required(VERSION 3.25)
project(rypath VERSION 0.1.0 LANGUAGES CXX)

option(RYPATH_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_executable(rypath src/main.cpp)
set_target_properties(rypath PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_compile_definitions(rypath PRIVATE RYPATH_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(rypath PRIVATE /W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
    target_compile_options(rypath PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(RYPATH_ENABLE_SANITIZERS)
    if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        message(FATAL_ERROR "The sanitizer build requires a compiler with GCC-style sanitizer flags.")
    endif()
    target_compile_options(rypath PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer
    )
    target_link_options(rypath PRIVATE -fsanitize=address,undefined)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
