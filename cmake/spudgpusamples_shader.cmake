# The samples' shaders: Slang in, each graphics API's own format out, at
# build time, by spudlib's shader step (spud_add_shader(), the file
# spudlib's build names in SPUD_SHADERS_CMAKE), written once for every
# caller: SPIR-V on every platform, DXIL on Windows, Metal source and a
# .metallib on Apple platforms. SpudGPU compiles nothing: a shader module
# takes code in the one format its adapter reports
# (SPUDGPU_ADAPTER_PROPERTIES::shader_formats). This file adds only what the
# samples read:
#
# - <stem>.shader.h, written at configure time: includes the .inc files this
#   platform builds and declares <stem>_shader, a spudgpusamples_shader
#   (../Samples/spudgpusamples_shader.h) holding the entry point and the
#   code in each format. A sample includes it and hands a module the format
#   its adapter reports.

include_guard(GLOBAL)

set(SPUDGPUSAMPLES_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
if(NOT SPUD_SHADERS_CMAKE)
    message(FATAL_ERROR "spudgpusamples shaders: spudlib's build names no shader step (SPUD_SHADERS_CMAKE)")
endif()
include(${SPUD_SHADERS_CMAKE})

# spudgpusamples_add_shader(<target> SOURCE <file.slang> DXIL_PROFILE <profile>
#     [SPIRV_CAPABILITIES <cap>...] [DEPENDS <module>...]
#     [METAL_ROUTE SLANG <reason> | NO_METAL <reason>])
#
# spud_add_shader()'s arguments, the shader's outputs in the sample's build
# directory under shaders/, its imports found beside it. Builds the outputs
# with <target> and adds the directory of <stem>.shader.h to its include
# path.
function(spudgpusamples_add_shader target)
    cmake_parse_arguments(ARG "" "SOURCE;DXIL_PROFILE" "SPIRV_CAPABILITIES;DEPENDS;METAL_ROUTE;NO_METAL" ${ARGN})
    get_filename_component(entry ${ARG_SOURCE} NAME_WE)
    get_filename_component(source_dir ${ARG_SOURCE} DIRECTORY)
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    set(modules "")
    foreach(module IN LISTS ARG_DEPENDS)
        list(APPEND modules ${CMAKE_CURRENT_SOURCE_DIR}/${module})
    endforeach()
    set(metal "")
    if(ARG_METAL_ROUTE)
        set(metal METAL_ROUTE ${ARG_METAL_ROUTE})
    elseif(ARG_NO_METAL)
        set(metal NO_METAL ${ARG_NO_METAL})
    endif()
    set(outputs "")
    spud_add_shader(${entry} SOURCE ${ARG_SOURCE} OUT_DIR ${out_dir} DXIL_PROFILE ${ARG_DXIL_PROFILE}
        INCLUDE_DIRS ${source_dir} DEPENDS ${modules} SPIRV_CAPABILITIES ${ARG_SPIRV_CAPABILITIES} ${metal}
        OUTPUTS outputs)

    set(includes "#include \"${entry}.spv.inc\"\n")
    set(dxil "NULL, 0")
    set(metallib "NULL, 0")
    if("dxil" IN_LIST ${entry}_FORMATS)
        string(APPEND includes "#include \"${entry}.dxil.inc\"\n")
        set(dxil "${entry}_dxil, ${entry}_dxil_size")
    endif()
    if("metallib" IN_LIST ${entry}_FORMATS)
        string(APPEND includes "#include \"${entry}.metallib.inc\"\n")
        set(metallib "${entry}_metallib, ${entry}_metallib_size")
    endif()

    # <stem>.shader.h, rewritten only when its text changes (configure_file
    # copies only a different file), so a reconfigure rebuilds nothing.
    string(TOUPPER "${entry}" guard)
    file(WRITE ${out_dir}/${entry}.shader.h.tmp
        "// Written at configure time by cmake/spudgpusamples_shader.cmake from\n"
        "// ${ARG_SOURCE}: not by hand.\n"
        "#ifndef ${guard}_SHADER_H\n"
        "#define ${guard}_SHADER_H\n\n"
        "#include \"spudgpusamples_shader.h\"\n"
        "${includes}\n"
        "static const spudgpusamples_shader ${entry}_shader = {\n"
        "    \"${entry}\", ${entry}_spirv, ${entry}_spirv_size, ${dxil}, ${metallib}};\n\n"
        "#endif\n")
    configure_file(${out_dir}/${entry}.shader.h.tmp ${out_dir}/${entry}.shader.h COPYONLY)

    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE ${out_dir} ${SPUDGPUSAMPLES_DIR}/Samples)
endfunction()
