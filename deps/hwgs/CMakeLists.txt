cmake_minimum_required(VERSION 3.24)
project(transformers_hwgs LANGUAGES C CXX)
option(TF_HWGS_GPU_GEOMETRY "Build the experimental D3D12 native geometry interface" OFF)
option(TF_HWGS_NATIVE_MESH_PROBE "Build the opt-in native mesh host correctness probe" OFF)
if(TF_HWGS_GPU_GEOMETRY)
    set(KZGS_PCSX2_DIR "${CMAKE_CURRENT_LIST_DIR}/../pcsx2" CACHE PATH "PCSX2 source tree")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_LIST_DIR}/apply_gpu_vertex_patch.py" "${KZGS_PCSX2_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
set(KZGS_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(KZGS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../kzgs" "${CMAKE_CURRENT_BINARY_DIR}/kzgs")
add_library(tf_hwgs SHARED tf_hwgs.cpp tf_direct.cpp tf_scene_capture.cpp)
target_compile_definitions(tf_hwgs PRIVATE TF_HWGS_BUILD)
target_compile_features(tf_hwgs PRIVATE cxx_std_20)
target_link_libraries(tf_hwgs PRIVATE kzgs kzgs_flags)
if(TF_HWGS_NATIVE_MESH_PROBE)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../native_renderer" "${CMAKE_CURRENT_BINARY_DIR}/native_renderer")
    target_sources(tf_hwgs PRIVATE tf_native_probe.cpp)
    target_compile_definitions(tf_hwgs PRIVATE TF_HWGS_NATIVE_MESH_PROBE=1)
    target_link_libraries(tf_hwgs PRIVATE tf_native_mesh_probe_support)
endif()
target_include_directories(tf_hwgs PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(tf_hwgs PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../PS2Recomp/ps2xRuntime/include")
if(TF_HWGS_GPU_GEOMETRY)
    target_compile_definitions(tf_hwgs PRIVATE TF_HWGS_GPU_GEOMETRY=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        shaders/dual_transform.hlsl shaders/grass_emit.hlsl shaders/reflection_lighting.hlsl shaders/vu_math.hlsl)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/shaders/dual_transform.hlsl" TF_GPU_DUAL_TRANSFORM_SHADER)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/shaders/grass_emit.hlsl" TF_GPU_GRASS_EMIT_SHADER)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/shaders/reflection_lighting.hlsl" TF_GPU_REFLECTION_SHADER)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/shaders/vu_math.hlsl" TF_GPU_VU_MATH_SHADER)
    foreach(SHADER TF_GPU_DUAL_TRANSFORM_SHADER TF_GPU_GRASS_EMIT_SHADER TF_GPU_REFLECTION_SHADER)
        string(REPLACE "#include \"vu_math.hlsl\"" "${TF_GPU_VU_MATH_SHADER}" ${SHADER} "${${SHADER}}")
    endforeach()
    configure_file(tf_gpu_shaders_template.h tf_gpu_shaders.h @ONLY)
    target_include_directories(tf_hwgs PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_sources(tf_hwgs PRIVATE tf_gpu_geometry_probe.cpp tf_gpu_dual_probe.cpp
        tf_gpu_dual_geometry.cpp tf_gpu_grass_probe.cpp tf_gpu_grass_draw_probe.cpp tf_gpu_dual_draw_probe.cpp tf_gpu_draw_trace.cpp
        tf_gpu_dual_frontend.cpp tf_gpu_frontend_probe.cpp tf_gpu_grass_frontend_probe.cpp tf_gpu_live_draw.cpp
        tf_gpu_live_grass.cpp tf_gpu_live_dual.cpp tf_gpu_merge_guards.cpp tf_gpu_reflection_probe.cpp
        tf_gpu_reflection_draw_probe.cpp tf_gpu_reflection_source_probe.cpp tf_gpu_live_reflection.cpp
        tf_gpu_reflection_frontend_probe.cpp tf_scene_capture_probe.cpp)
    # Bounds must honor/restore the caller's floating-point environment. Keep
    # this object separate from kzgs_flags, whose /fp:contract conflicts with
    # /fp:strict under MSVC.
    add_library(tf_gpu_dual_bounds OBJECT tf_gpu_dual_bounds.cpp)
    target_sources(tf_gpu_dual_bounds PRIVATE
        ../../PS2Recomp/ps2xRuntime/src/lib/gs/gs_native_dual_math.cpp
        ../../PS2Recomp/ps2xRuntime/src/lib/gs/gs_native_grass_math.cpp
        ../../PS2Recomp/ps2xRuntime/src/lib/gs/gs_native_reflection_math.cpp tf_gpu_reflection_bounds.cpp)
    target_include_directories(tf_gpu_dual_bounds PRIVATE ../../PS2Recomp/ps2xRuntime/include)
    target_compile_features(tf_gpu_dual_bounds PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(tf_gpu_dual_bounds PRIVATE /fp:strict)
    endif()
    target_link_libraries(tf_hwgs PRIVATE d3dcompiler tf_gpu_dual_bounds)
endif()
kzgs_stage_resources(tf_hwgs)
