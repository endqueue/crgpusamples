add_executable(SpudGPUDepthBoundsTest main.c)
target_link_libraries(SpudGPUDepthBoundsTest PRIVATE spudlib SDL3::SDL3)

# Shaders are Slang, built at build time into the adapter's own format
# (SPIR-V, DXIL on Windows, a .metallib on Apple platforms) and embedded
# in the executable -- SpudGPU compiles nothing, see
# ../../cmake/spudgpusamples_shader.cmake.
# This sample builds on all three backends (see
# spudgpusamples/CMakeLists.txt) -- whether spudgpu_cmd_set_depth_bounds
# actually does anything on a given Metal device depends on
# spudgpu_depth_bounds_capabilities::supported at runtime (gated to
# MTLGPUFamilyApple10, macOS/iOS 26+), not on GRAPHICS_BACKEND.
spudgpusamples_add_shader(SpudGPUDepthBoundsTest SOURCE shaders/triangle_vert.slang DXIL_PROFILE vs_6_0)
spudgpusamples_add_shader(SpudGPUDepthBoundsTest SOURCE shaders/triangle_frag.slang DXIL_PROFILE ps_6_0)
spudgpusamples_add_shader(SpudGPUDepthBoundsTest SOURCE shaders/highlight_frag.slang DXIL_PROFILE ps_6_0)

if(WIN32)
    add_custom_command(TARGET SpudGPUDepthBoundsTest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:SpudGPUDepthBoundsTest>")
endif()
