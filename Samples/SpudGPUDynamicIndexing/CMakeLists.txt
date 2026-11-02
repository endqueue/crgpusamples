add_executable(SpudGPUDynamicIndexing main.c)
target_link_libraries(SpudGPUDynamicIndexing PRIVATE spudlib SDL3::SDL3)

# Shaders are Slang, built at build time into the adapter's own format
# (SPIR-V, DXIL on Windows, a .metallib on Apple platforms) and embedded
# in the executable -- SpudGPU compiles nothing, see
# ../../cmake/spudgpusamples_shader.cmake.
# This sample only ever builds against Vulkan/D3D12 (see
# spudgpusamples/CMakeLists.txt's GRAPHICS_BACKEND guard -- bindless
# descriptor indexing isn't implemented on Metal yet).
spudgpusamples_add_shader(SpudGPUDynamicIndexing SOURCE shaders/scene_vert.slang DXIL_PROFILE vs_6_0)
spudgpusamples_add_shader(SpudGPUDynamicIndexing SOURCE shaders/scene_frag.slang DXIL_PROFILE ps_6_0
    # Its unsized texture array is SpudGPU's bindless table, which the Metal
    # backend doesn't have: spudgpu_msl refuses a runtime-sized array outside
    # a device argument buffer, and the sample never builds on Metal.
    NO_METAL "bindless texture table")

# occcity.bin (the mesh + diffuse texture data, copied from d3d12samples) is
# loaded via a plain relative path at runtime.
add_custom_command(TARGET SpudGPUDynamicIndexing POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/occcity.bin" "$<TARGET_FILE_DIR:SpudGPUDynamicIndexing>")

if(WIN32)
    add_custom_command(TARGET SpudGPUDynamicIndexing POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:SpudGPUDynamicIndexing>")
endif()
