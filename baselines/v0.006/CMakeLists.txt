cmake_minimum_required(VERSION 3.20)
project(ENR VERSION 0.0.6 LANGUAGES CXX)

if(NOT WIN32 OR NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ENR v0.006 requires the 32-bit Windows MINGW32 toolchain.")
endif()

set(RESHADE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../reshade/include"
    CACHE PATH "Local ReShade SDK include directory")
if(NOT EXISTS "${RESHADE_INCLUDE_DIR}/reshade.hpp")
    message(FATAL_ERROR "ReShade SDK missing: ${RESHADE_INCLUDE_DIR}/reshade.hpp")
endif()

add_executable(enr_shader_compiler EXCLUDE_FROM_ALL
    tools/compile_motion_shaders.cpp src/motion_shaders.hpp)
set_target_properties(enr_shader_compiler PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools")
target_include_directories(enr_shader_compiler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(enr_shader_compiler PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(enr_shader_compiler PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(enr_shader_compiler PRIVATE d3dcompiler_47)
target_link_options(enr_shader_compiler PRIVATE -static -static-libgcc -static-libstdc++)

# Compile complex motion shaders during the build, avoiding a multi-second
# D3DCompile hitch on the first gameplay frame and after device reset.
set(ENR_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ENR_MOTION_BYTECODE "${ENR_GENERATED_DIR}/motion_shaders_bytecode.hpp")
add_custom_command(OUTPUT "${ENR_MOTION_BYTECODE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ENR_GENERATED_DIR}"
    COMMAND $<TARGET_FILE:enr_shader_compiler> "${ENR_MOTION_BYTECODE}"
    DEPENDS enr_shader_compiler "${CMAKE_CURRENT_SOURCE_DIR}/src/motion_shaders.hpp"
    COMMENT "Compiling ENR motion shaders for Shader Model 3"
    VERBATIM)

add_library(enr SHARED src/enr.cpp src/grayscale_d3d9.cpp src/grayscale_d3d9.hpp
    src/depth_binding.cpp src/depth_binding.hpp src/motion_d3d9.cpp src/motion_d3d9.hpp
    "${ENR_MOTION_BYTECODE}")
configure_file(shaders/ENR_Depth.addonfx ENR_Depth.addonfx COPYONLY)
set_target_properties(enr PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    PREFIX ""
    OUTPUT_NAME "enr"
    SUFFIX ".addon32"
)
target_include_directories(enr SYSTEM PRIVATE "${RESHADE_INCLUDE_DIR}")
target_include_directories(enr PRIVATE "${ENR_GENERATED_DIR}")
target_compile_definitions(enr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
target_compile_options(enr PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(enr PRIVATE d3dcompiler_47)
# Deploy one add-on file without requiring the MSYS2 compiler runtime DLLs.
target_link_options(enr PRIVATE -static -static-libgcc -static-libstdc++)
