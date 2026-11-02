//
// A sample's shader, as ../cmake/spudgpusamples_shader.cmake builds it from
// the sample's shaders/<stem>.slang: SPIR-V on every platform, DXIL on
// Windows, a .metallib on Apple platforms, each embedded as a C array. A
// sample includes <stem>.shader.h, which declares <stem>_shader, and hands
// spudgpu_create_shader_module the code in the one format its adapter
// reports (SPUDGPU_ADAPTER_PROPERTIES::shader_formats).
//

#ifndef SPUDGPUSAMPLES_SHADER_H
#define SPUDGPUSAMPLES_SHADER_H

#include <spudgpu.h>
#include <stdint.h>
#include <stdio.h>

// One shader: its entry point (the source's stem) and its code in each
// format, NULL and 0 for a format the build didn't make.
typedef struct spudgpusamples_shader {
	const char *entry_point;
	const uint32_t *spirv;
	uint64_t spirv_size;
	const uint32_t *dxil;
	uint64_t dxil_size;
	const uint32_t *metallib;
	uint64_t metallib_size;
} spudgpusamples_shader;

// The one format an adapter's shader_formats names, or
// SPUDGPU_SHADER_FORMAT_NONE.
static inline SPUDGPU_SHADER_FORMAT spudgpusamples_shader_format(SPUDGPU_SHADER_FORMATS shader_formats) {
	for (SPUDGPU_SHADER_FORMAT format = SPUDGPU_SHADER_FORMAT_SPIRV; format <= SPUDGPU_SHADER_FORMAT_METALLIB; ++format)
		if (shader_formats & (1u << format))
			return format;
	return SPUDGPU_SHADER_FORMAT_NONE;
}

// [shader]'s module for [stage], from its code in [format]. The workgroup
// size is the shader's own ([numthreads]) for a compute or mesh stage, and
// 0, 0, 0 for every other. NULL, printed, on failure.
static inline spudgpu_shader_module spudgpusamples_create_shader_module(
    spudgpu_device device,
    SPUDGPU_SHADER_FORMAT format,
    const spudgpusamples_shader *shader,
    SPUDGPU_SHADER_STAGE stage,
    uint32_t workgroup_x,
    uint32_t workgroup_y,
    uint32_t workgroup_z) {
	spudgpu_shader_module_desc desc = {
	    .base = {SPUD_STRUCT_TYPE_GPU_SHADER_MODULE_DESC, NULL},
	    .stage          = stage,
	    .format         = format,
	    .entry_point    = shader->entry_point,
	    .workgroup_size = {workgroup_x, workgroup_y, workgroup_z},
	    .debug_name     = shader->entry_point,
	};
	switch (format) {
	case SPUDGPU_SHADER_FORMAT_SPIRV:
		desc.code      = shader->spirv;
		desc.code_size = shader->spirv_size;
		break;
	case SPUDGPU_SHADER_FORMAT_DXIL:
		desc.code      = shader->dxil;
		desc.code_size = shader->dxil_size;
		break;
	case SPUDGPU_SHADER_FORMAT_METALLIB:
		desc.code      = shader->metallib;
		desc.code_size = shader->metallib_size;
		break;
	default:
		break;
	}
	if (!desc.code) {
		fprintf(stderr, "%s: this build made no code in the adapter's shader format (%u)\n", shader->entry_point, format);
		return NULL;
	}
	spudgpu_shader_module module = NULL;
	SPUDRESULT result            = spudgpu_create_shader_module(device, &desc, &module);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_create_shader_module failed for %s: %d\n", shader->entry_point, result);
		return NULL;
	}
	return module;
}

#endif
