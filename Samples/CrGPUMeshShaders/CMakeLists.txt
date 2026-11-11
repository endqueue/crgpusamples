add_executable(CrGPUMeshShaders main.c)
target_link_libraries(CrGPUMeshShaders PRIVATE carrottus SDL3::SDL3)

# Shaders are Slang, built at build time into the adapter's own format
# (SPIR-V, DXIL on Windows, a .metallib on Apple platforms) and embedded
# in the executable -- CrGPU compiles nothing, see
# ../../cmake/crgpusamples_shader.cmake.
# The mesh shader needs SPIR-V's mesh shading capability (SPV_EXT_mesh_shader)
# and shader model 6.5 on D3D12.
crgpusamples_add_shader(CrGPUMeshShaders SOURCE shaders/mesh_mesh.slang DXIL_PROFILE ms_6_5
    SPIRV_CAPABILITIES spvMeshShadingEXT)
crgpusamples_add_shader(CrGPUMeshShaders SOURCE shaders/scene_frag.slang DXIL_PROFILE ps_6_0)

# Dragon_LOD0.bin (the meshlet binary, copied from d3d12samples) is loaded
# via a plain relative path at runtime.
add_custom_command(TARGET CrGPUMeshShaders POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Dragon_LOD0.bin" "$<TARGET_FILE_DIR:CrGPUMeshShaders>")

if(WIN32)
    add_custom_command(TARGET CrGPUMeshShaders POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:CrGPUMeshShaders>")
endif()
