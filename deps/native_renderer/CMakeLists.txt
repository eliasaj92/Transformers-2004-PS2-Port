cmake_minimum_required(VERSION 3.24)
project(tf_native_renderer LANGUAGES CXX)
if(NOT WIN32)
  message(FATAL_ERROR "The first native renderer uses D3D12 on Windows")
endif()

# Deliberately independent of hwgs, PCSX2, the EE runtime and generated code.
add_library(tf_native_renderer STATIC renderer.cpp)
target_compile_features(tf_native_renderer PUBLIC cxx_std_20)
target_include_directories(tf_native_renderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(tf_native_renderer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(tf_native_renderer PRIVATE /W4 /permissive-)
target_link_libraries(tf_native_renderer PUBLIC d3d12 dxgi d3dcompiler)

add_executable(tf_native_mesh_probe probe.cpp)
target_compile_definitions(tf_native_mesh_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(tf_native_mesh_probe PRIVATE /W4 /permissive-)
target_link_libraries(tf_native_mesh_probe PRIVATE tf_native_renderer)
configure_file(mesh.hlsl mesh.hlsl COPYONLY)
