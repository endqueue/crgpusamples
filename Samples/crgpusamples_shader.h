//
// A sample's shader, as ../cmake/crgpusamples_shader.cmake builds it from
// the sample's shaders/<stem>.slang: SPIR-V on every platform, DXIL on
// Windows, a .metallib on Apple platforms, each embedded as a C array. A
// sample includes <stem>.shader.h, which declares <stem>_shader, and hands
// crgpu_shader_module_create the code in the one format its adapter
// reports (CRGPU_ADAPTER_PROPERTIES::shader_formats).
//

#ifndef CRGPUSAMPLES_SHADER_H
#define CRGPUSAMPLES_SHADER_H

#include <crgpu.h>
#include <stdint.h>
#include <stdio.h>

// One shader: its entry point (the source's stem) and its code in each
// format, NULL and 0 for a format the build didn't make.
typedef struct crgpusamples_shader {
	const char *entry_point;
	const uint32_t *spirv;
	uint64_t spirv_size;
	const uint32_t *dxil;
	uint64_t dxil_size;
	const uint32_t *metallib;
	uint64_t metallib_size;
} crgpusamples_shader;

// The one format an adapter's shader_formats names, or
// CRGPU_SHADER_FORMAT_NONE.
static inline CRGPU_SHADER_FORMAT crgpusamples_shader_format(CRGPU_SHADER_FORMATS shader_formats) {
	for (CRGPU_SHADER_FORMAT format = CRGPU_SHADER_FORMAT_SPIRV; format <= CRGPU_SHADER_FORMAT_METALLIB; ++format)
		if (shader_formats & (1u << format))
			return format;
	return CRGPU_SHADER_FORMAT_NONE;
}

// [shader]'s module for [stage], from its code in [format]. The workgroup
// size is the shader's own ([numthreads]) for a compute or mesh stage, and
// 0, 0, 0 for every other. NULL, printed, on failure.
static inline crgpu_shader_module crgpusamples_create_shader_module(
    crgpu_device device,
    CRGPU_SHADER_FORMAT format,
    const crgpusamples_shader *shader,
    CRGPU_SHADER_STAGE stage,
    uint32_t workgroup_x,
    uint32_t workgroup_y,
    uint32_t workgroup_z) {
	crgpu_shader_module_desc desc = {
	    .base = {CR_STRUCT_TYPE_GPU_SHADER_MODULE_DESC, NULL},
	    .stage          = stage,
	    .format         = format,
	    .entry_point    = shader->entry_point,
	    .workgroup_size = {workgroup_x, workgroup_y, workgroup_z},
	    .debug_name     = shader->entry_point,
	};
	switch (format) {
	case CRGPU_SHADER_FORMAT_SPIRV:
		desc.code      = shader->spirv;
		desc.code_size = shader->spirv_size;
		break;
	case CRGPU_SHADER_FORMAT_DXIL:
		desc.code      = shader->dxil;
		desc.code_size = shader->dxil_size;
		break;
	case CRGPU_SHADER_FORMAT_METALLIB:
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
	crgpu_shader_module module = NULL;
	CR_RESULT result            = crgpu_shader_module_create(device, &desc, &module);
	if (CR_FAILED(result)) {
		fprintf(stderr, "crgpu_shader_module_create failed for %s: %d\n", shader->entry_point, result);
		return NULL;
	}
	return module;
}

#endif
