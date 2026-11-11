add_executable(CrGPUDepthBoundsTest main.c)
target_link_libraries(CrGPUDepthBoundsTest PRIVATE carrottus SDL3::SDL3)

# Shaders are Slang, built at build time into the adapter's own format
# (SPIR-V, DXIL on Windows, a .metallib on Apple platforms) and embedded
# in the executable -- CrGPU compiles nothing, see
# ../../cmake/crgpusamples_shader.cmake.
# This sample builds on all three backends (see
# spudgpusamples/CMakeLists.txt) -- whether crgpu_command_list_set_depth_bounds
# actually does anything on a given Metal device depends on
# crgpu_depth_bounds_capabilities::supported at runtime (gated to
# MTLGPUFamilyApple10, macOS/iOS 26+), not on GRAPHICS_BACKEND.
crgpusamples_add_shader(CrGPUDepthBoundsTest SOURCE shaders/triangle_vert.slang DXIL_PROFILE vs_6_0)
crgpusamples_add_shader(CrGPUDepthBoundsTest SOURCE shaders/triangle_frag.slang DXIL_PROFILE ps_6_0)
crgpusamples_add_shader(CrGPUDepthBoundsTest SOURCE shaders/highlight_frag.slang DXIL_PROFILE ps_6_0)

if(WIN32)
    add_custom_command(TARGET CrGPUDepthBoundsTest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:CrGPUDepthBoundsTest>")
endif()
