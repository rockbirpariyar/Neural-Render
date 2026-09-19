cmake_minimum_required(VERSION 3.20)
project(ENR VERSION 0.0.4 LANGUAGES CXX)

if(NOT WIN32 OR NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ENR v0.004 requires the 32-bit Windows MINGW32 toolchain.")
endif()

set(RESHADE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../reshade/include"
    CACHE PATH "Local ReShade SDK include directory")
if(NOT EXISTS "${RESHADE_INCLUDE_DIR}/reshade.hpp")
    message(FATAL_ERROR "ReShade SDK missing: ${RESHADE_INCLUDE_DIR}/reshade.hpp")
endif()

add_library(enr SHARED src/enr.cpp src/grayscale_d3d9.cpp src/grayscale_d3d9.hpp src/depth_binding.cpp src/depth_binding.hpp)
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
target_compile_definitions(enr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
target_compile_options(enr PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(enr PRIVATE d3dcompiler_47)
# Deploy one add-on file without requiring the MSYS2 compiler runtime DLLs.
target_link_options(enr PRIVATE -static -static-libgcc -static-libstdc++)
