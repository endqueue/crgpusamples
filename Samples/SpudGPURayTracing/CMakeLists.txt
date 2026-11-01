add_executable(SpudGPURayTracing main.c)
target_link_libraries(SpudGPURayTracing PRIVATE spudlib SDL3::SDL3)

# Ray queries (SPUDGPU_EXT_RAY_QUERY) are 1 on every backend, Vulkan, D3D12
# and Metal, so this sample builds everywhere. Whether an adapter can trace
# is SPUDGPU_FEATURE_RAY_QUERY, which main.c reads at run time: on an
# adapter without it the sample says so and exits 1.
#
# Shaders are Slang, built at build time into the adapter's own format
# (SPIR-V, DXIL on Windows, a .metallib on Apple platforms) and embedded
# in the executable -- SpudGPU compiles nothing, see
# ../../cmake/spudgpusamples_slang.cmake.
# The three trace shaders query with RayQuery: SPIR-V's ray query
# capability (SPV_KHR_ray_query) and shader model 6.5 on D3D12. They import
# rt_common.slang, and the procedural one rt_primitives.slang too.
spudgpusamples_add_slang_shader(SpudGPURayTracing SOURCE shaders/rt_hello_comp.slang DXIL_PROFILE cs_6_5
    SPIRV_CAPABILITIES spvRayQueryKHR)
spudgpusamples_add_slang_shader(SpudGPURayTracing SOURCE shaders/rt_lighting_comp.slang DXIL_PROFILE cs_6_5
    SPIRV_CAPABILITIES spvRayQueryKHR DEPENDS shaders/rt_common.slang)
spudgpusamples_add_slang_shader(SpudGPURayTracing SOURCE shaders/rt_procedural_comp.slang DXIL_PROFILE cs_6_5
    SPIRV_CAPABILITIES spvRayQueryKHR DEPENDS shaders/rt_common.slang shaders/rt_primitives.slang)
# The GPU instance writers, one per layout kind (card S14); main.c runs the
# one the device's layout names.
spudgpusamples_add_slang_shader(SpudGPURayTracing SOURCE shaders/rt_instances_row64_comp.slang DXIL_PROFILE cs_6_0)
spudgpusamples_add_slang_shader(SpudGPURayTracing SOURCE shaders/rt_instances_col72_comp.slang DXIL_PROFILE cs_6_0)
# The present pass: the trace's output drawn onto the back buffer.
spudgpusamples_add_slang_shader(SpudGPURayTracing SOURCE shaders/rt_present_vert.slang DXIL_PROFILE vs_6_0)
spudgpusamples_add_slang_shader(SpudGPURayTracing SOURCE shaders/rt_present_frag.slang DXIL_PROFILE ps_6_0)

if(WIN32)
    add_custom_command(TARGET SpudGPURayTracing POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:SpudGPURayTracing>")
endif()
