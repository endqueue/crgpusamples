add_executable(SpudGPUHeaps main.c)
target_link_libraries(SpudGPUHeaps PRIVATE spudlib SDL3::SDL3)

# Shaders are Slang, built at build time into the adapter's own format
# (SPIR-V, DXIL on Windows, a .metallib on Apple platforms) and embedded
# in the executable -- SpudGPU compiles nothing, see
# ../../cmake/spudgpusamples_slang.cmake.
spudgpusamples_add_slang_shader(SpudGPUHeaps SOURCE shaders/heaps_vert.slang DXIL_PROFILE vs_6_0)
spudgpusamples_add_slang_shader(SpudGPUHeaps SOURCE shaders/heaps_frag.slang DXIL_PROFILE ps_6_0)

if(WIN32)
    add_custom_command(TARGET SpudGPUHeaps POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:SpudGPUHeaps>")
endif()
