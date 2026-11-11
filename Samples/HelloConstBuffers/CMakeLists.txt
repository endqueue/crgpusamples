add_executable(HelloConstBuffers main.c)
target_link_libraries(HelloConstBuffers PRIVATE carrottus SDL3::SDL3)

# Shaders are Slang, built at build time into the adapter's own format
# (SPIR-V, DXIL on Windows, a .metallib on Apple platforms) and embedded
# in the executable -- CrGPU compiles nothing, see
# ../../cmake/crgpusamples_shader.cmake.
crgpusamples_add_shader(HelloConstBuffers SOURCE shaders/scene_vert.slang DXIL_PROFILE vs_6_0)
crgpusamples_add_shader(HelloConstBuffers SOURCE shaders/scene_frag.slang DXIL_PROFILE ps_6_0)

if(WIN32)
    add_custom_command(TARGET HelloConstBuffers POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:HelloConstBuffers>")
endif()
