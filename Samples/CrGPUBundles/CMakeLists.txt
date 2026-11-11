add_executable(CrGPUBundles main.c)
target_link_libraries(CrGPUBundles PRIVATE carrottus SDL3::SDL3)

# Shaders are Slang, built at build time into the adapter's own format
# (SPIR-V, DXIL on Windows, a .metallib on Apple platforms) and embedded
# in the executable -- CrGPU compiles nothing, see
# ../../cmake/crgpusamples_shader.cmake.
# This sample only ever builds against Vulkan/D3D12 (see
# crgpusamples/CMakeLists.txt's GRAPHICS_BACKEND guard -- Metal has no
# CPU-side reusable secondary-command/bundle mechanism, see carrottus/
# CLAUDE.md's CRGPU_EXT_BUNDLES section).
crgpusamples_add_shader(CrGPUBundles SOURCE shaders/scene_vert.slang DXIL_PROFILE vs_6_0)
crgpusamples_add_shader(CrGPUBundles SOURCE shaders/scene_frag.slang DXIL_PROFILE ps_6_0)

# occcity.bin (mesh data only here -- see main.c for why this sample skips
# the diffuse texture), copied verbatim from CrGPUDynamicIndexing, which
# in turn copied it from d3d12samples. Loaded via a plain relative path at
# runtime.
add_custom_command(TARGET CrGPUBundles POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/occcity.bin" "$<TARGET_FILE_DIR:CrGPUBundles>")

if(WIN32)
    add_custom_command(TARGET CrGPUBundles POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:CrGPUBundles>")
endif()
