# The samples' shaders: Slang in, each graphics API's own format out, at
# build time. SpudGPU compiles nothing: a shader module takes code in the
# one format its adapter reports (SPUDGPU_ADAPTER_PROPERTIES::shader_formats),
# so this is the samples' build step, as any caller's would be. It follows
# spudlib's own step for its test shaders (../spudlib/cmake/spud_slang.cmake).
#
# For each shader, in the sample's build directory under shaders/:
# - <stem>.spv, SPIR-V 1.5, checked by spirv-val; on every platform.
# - <stem>.dxil, DXIL: Slang writes HLSL and DXC compiles it, its validator
#   signing what it passes; on Windows, where the D3D12 backend builds.
# - <stem>.slang.metal and <stem>.cross.metal, Metal source by both routes
#   (Slang's own, and Slang to SPIR-V to spudlib's spudgpu_msl, in the Metal
#   backend's binding layout), kept as build outputs on every platform.
#   Metal 3.1 (Apple7 and later, macOS 14, iOS 17).
# - <stem>.metallib on Apple platforms, from the route the shader names
#   (spudgpu_msl's by default, card S21).
# - <stem>.<format>.inc, each binary as a C array (uint32_t words, zero
#   padded to a whole word, and <stem>_<format>_size, the exact byte count),
#   by cmake/spudgpusamples_embed.cmake.
# - <stem>.shader.h, written at configure time: includes the .inc files this
#   platform builds and declares <stem>_shader, a spudgpusamples_shader
#   (../Samples/spudgpusamples_shader.h) holding the entry point and the
#   code in each format. A sample includes it and hands a module the format
#   its adapter reports.
#
# The tools come from the Vulkan SDK (VULKAN_SDK); their versions are
# printed at configure time.

include_guard(GLOBAL)

set(SPUDGPUSAMPLES_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(_spudgpusamples_sdk_bin "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(SPUDGPUSAMPLES_SLANGC slangc HINTS ${_spudgpusamples_sdk_bin} REQUIRED)
find_program(SPUDGPUSAMPLES_SPIRV_VAL spirv-val HINTS ${_spudgpusamples_sdk_bin} REQUIRED)
if(WIN32)
    find_program(SPUDGPUSAMPLES_DXC dxc HINTS ${_spudgpusamples_sdk_bin} REQUIRED)
endif()
execute_process(COMMAND ${SPUDGPUSAMPLES_SLANGC} -version
    OUTPUT_VARIABLE _spudgpusamples_v ERROR_VARIABLE _spudgpusamples_v
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
message(STATUS "spudgpusamples shaders: slangc ${_spudgpusamples_v} (${SPUDGPUSAMPLES_SLANGC})")
if(WIN32)
    execute_process(COMMAND ${SPUDGPUSAMPLES_DXC} --version
        OUTPUT_VARIABLE _spudgpusamples_v OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "spudgpusamples shaders: ${_spudgpusamples_v} (${SPUDGPUSAMPLES_DXC})")
endif()
# spudgpu_msl, the Metal fallback route's cross-compiler, is a tool spudlib
# defines for the build machine (not in a cross-compiling build).
if(NOT TARGET spudgpu_msl)
    message(FATAL_ERROR "spudgpusamples shaders: spudlib defines no spudgpu_msl target")
endif()

# spudgpusamples_add_slang_shader(<target> SOURCE <file.slang>
#     DXIL_PROFILE <profile> [SPIRV_CAPABILITIES <cap>...] [DEPENDS <module>...]
#     [METAL_ROUTE SLANG <reason> | NO_METAL <reason>])
#
# The source names its stage ([shader("vertex")]...) and its entry point is
# the source's stem (triangle_vert.slang: triangle_vert) in every format,
# never main, which Metal reserves. <profile> is the shader model DXC
# compiles for (vs_6_0...). SPIR-V is 1.5, which Vulkan 1.2 and later
# take, plus the capabilities named. DEPENDS names the Slang modules the
# source imports (found beside it), so that a change to one builds it
# again. METAL_ROUTE SLANG makes the .metallib
# from Slang's own source instead of spudgpu_msl's (card S21), with the
# reason written beside the call. NO_METAL makes no Metal source or .metallib at all, for
# a shader of a sample that never builds on Metal and that neither Metal
# route takes (bindless), with the reason written beside the call. Builds
# the shader's outputs with <target> and adds the
# directory of <stem>.shader.h to its include path.
function(spudgpusamples_add_slang_shader target)
    cmake_parse_arguments(ARG "" "SOURCE;DXIL_PROFILE" "SPIRV_CAPABILITIES;DEPENDS;METAL_ROUTE;NO_METAL" ${ARGN})
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE})
    set(modules "")
    foreach(module IN LISTS ARG_DEPENDS)
        list(APPEND modules ${CMAKE_CURRENT_SOURCE_DIR}/${module})
    endforeach()
    get_filename_component(entry ${ARG_SOURCE} NAME_WE)
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    set(out ${out_dir}/${entry})
    set(embed ${SPUDGPUSAMPLES_DIR}/cmake/spudgpusamples_embed.cmake)
    set(caps "")
    foreach(cap IN LISTS ARG_SPIRV_CAPABILITIES)
        list(APPEND caps -capability ${cap})
    endforeach()

    add_custom_command(
        OUTPUT ${out}.spv ${out}.spv.inc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${SPUDGPUSAMPLES_SLANGC} ${src} -entry ${entry} -target spirv -profile spirv_1_5 ${caps}
                -fvk-use-entrypoint-name -matrix-layout-column-major
                -warnings-as-errors all -o ${out}.spv
        COMMAND ${SPUDGPUSAMPLES_SPIRV_VAL} --target-env vulkan1.2 ${out}.spv
        COMMAND ${CMAKE_COMMAND} -DIN=${out}.spv -DOUT=${out}.spv.inc -DNAME=${entry}_spirv -P ${embed}
        DEPENDS ${src} ${modules} ${embed}
        COMMENT "Slang: ${ARG_SOURCE} to SPIR-V (spirv-val)")
    set(outputs ${out}.spv.inc)
    set(includes "#include \"${entry}.spv.inc\"\n")
    set(dxil "NULL, 0")
    set(metallib "NULL, 0")

    if(WIN32)
        add_custom_command(
            OUTPUT ${out}.hlsl ${out}.dxil ${out}.dxil.inc
            COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
            COMMAND ${SPUDGPUSAMPLES_SLANGC} ${src} -entry ${entry} -target hlsl -profile ${ARG_DXIL_PROFILE}
                    -matrix-layout-column-major
                    -warnings-as-errors all -o ${out}.hlsl
            COMMAND ${SPUDGPUSAMPLES_DXC} -nologo -T ${ARG_DXIL_PROFILE} -E ${entry} -WX ${out}.hlsl -Fo ${out}.dxil
            COMMAND ${CMAKE_COMMAND} -DIN=${out}.dxil -DOUT=${out}.dxil.inc -DNAME=${entry}_dxil -P ${embed}
            DEPENDS ${src} ${modules} ${embed}
            COMMENT "Slang: ${ARG_SOURCE} to DXIL (DXC, validated)")
        list(APPEND outputs ${out}.dxil.inc)
        string(APPEND includes "#include \"${entry}.dxil.inc\"\n")
        set(dxil "${entry}_dxil, ${entry}_dxil_size")
    endif()

    # Both Metal routes' source, kept, and on Apple platforms the .metallib.
    if(ARG_NO_METAL)
        if(APPLE)
            message(FATAL_ERROR "${ARG_SOURCE}: NO_METAL, but this build is for an Apple platform")
        endif()
    else()
        add_custom_command(
            OUTPUT ${out}.slang.metal ${out}.cross.metal
            COMMAND ${SPUDGPUSAMPLES_SLANGC} ${src} -entry ${entry} -target metal -matrix-layout-column-major
                    -o ${out}.slang.metal
            COMMAND spudgpu_msl --platform $<IF:$<PLATFORM_ID:iOS>,ios,macos> --msl-version 30100
                    ${out}.spv ${out}.cross.metal
            DEPENDS ${src} ${modules} ${out}.spv spudgpu_msl
            COMMENT "Slang: ${ARG_SOURCE} to Metal source, both routes")
        list(APPEND outputs ${out}.slang.metal ${out}.cross.metal)
    endif()

    if(APPLE)
        set(metal_src ${out}.cross.metal)
        list(GET ARG_METAL_ROUTE 0 route)
        if(route STREQUAL "SLANG")
            set(metal_src ${out}.slang.metal)
        endif()
        add_custom_command(
            OUTPUT ${out}.metallib ${out}.metallib.inc
            COMMAND xcrun -sdk macosx metal -std=metal3.1 -Werror -c ${metal_src} -o ${out}.air
            COMMAND xcrun -sdk macosx metallib ${out}.air -o ${out}.metallib
            COMMAND ${CMAKE_COMMAND} -DIN=${out}.metallib -DOUT=${out}.metallib.inc -DNAME=${entry}_metallib -P ${embed}
            DEPENDS ${metal_src} ${embed}
            COMMENT "Metal: ${entry} to a metallib")
        list(APPEND outputs ${out}.metallib.inc)
        string(APPEND includes "#include \"${entry}.metallib.inc\"\n")
        set(metallib "${entry}_metallib, ${entry}_metallib_size")
    endif()

    # <stem>.shader.h, rewritten only when its text changes (configure_file
    # copies only a different file), so a reconfigure rebuilds nothing.
    string(TOUPPER "${entry}" guard)
    file(WRITE ${out}.shader.h.tmp
        "// Written at configure time by cmake/spudgpusamples_slang.cmake from\n"
        "// ${ARG_SOURCE}: not by hand.\n"
        "#ifndef ${guard}_SHADER_H\n"
        "#define ${guard}_SHADER_H\n\n"
        "#include \"spudgpusamples_shader.h\"\n"
        "${includes}\n"
        "static const spudgpusamples_shader ${entry}_shader = {\n"
        "    \"${entry}\", ${entry}_spirv, ${entry}_spirv_size, ${dxil}, ${metallib}};\n\n"
        "#endif\n")
    configure_file(${out}.shader.h.tmp ${out}.shader.h COPYONLY)

    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE ${out_dir} ${SPUDGPUSAMPLES_DIR}/Samples)
endfunction()
