//
// Carrottus GPU port of D3D12ExecuteIndirect from d3d12samples. A compute pass
// decides, per triangle, whether it's visible; the graphics pass then draws
// every triangle with one crgpu_command_list_draw_indirect call, reading its
// per-triangle draw arguments straight out of the compute pass's output
// buffer. SPACE toggles the compute culling pass on/off -- see ../../README.md
// for the deliberate simplification versus the original D3D12 sample (flat
// indirect draw instead of GPU-side compaction) and why it's the correct
// portable choice rather than a workaround.
//
// Per-triangle offset/color is delivered as a per-instance vertex attribute
// (scene_buffer bound at vertex binding slot 1, per_instance = true), NOT via
// gl_InstanceIndex/SV_InstanceID indexing into a storage buffer. D3D12's
// SV_InstanceID does not include StartInstanceLocation the way Vulkan's
// gl_InstanceIndex does -- unlike the fixed-function input assembler's
// per-instance vertex fetch, which *does* correctly offset by
// StartInstanceLocation (first_instance) on every backend. That's the
// portable equivalent of the original sample's per-draw root CBV update
// (see ../../README.md) without needing an indirect-argument shape D3D12
// alone can do.
//

#include <SDL3/SDL.h>
#include <crgpu.h>
#include <crgpu_sdl3.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CARROTTUS_PLATFORM_WINDOWS
#define _USE_MATH_DEFINES // MSVC <math.h> checks this, needed to use M_PI macro
#include <corecrt_math_defines.h>
#endif

// The shaders, Slang built into each format and embedded at build time
// (../../cmake/crgpusamples_shader.cmake).
#include "cull_comp.shader.h"
#include "scene_vert.shader.h"
#include "scene_frag.shader.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

// Must match the constants of the same name in shaders/cull_comp.slang.
#define TRIANGLE_COUNT 256
#define COMPUTE_THREAD_BLOCK_SIZE 128
#define TRIANGLE_HALF_WIDTH 0.05f
#define TRIANGLE_DEPTH 1.0f
#define CULL_OFFSET 0.5f

typedef struct Vertex {
	float position[3];
} Vertex;

// Matches shaders/cull_comp.slang's SceneConstantBuffer exactly (three
// float4s need no manual padding). Unlike the original D3D12
// sample, projection isn't duplicated per-triangle here (it's identical for
// every triangle there too) -- see ProjectionBuffer below -- which keeps this
// well under Vulkan's guaranteed-minimum uniform buffer size at
// TRIANGLE_COUNT triangles.
typedef struct SceneConstantBuffer {
	float velocity[4];
	float offset[4];
	float color[4];
} SceneConstantBuffer;

// Matches crgpu_draw_indirect_args exactly (see crgpu.h) -- the compute
// shader reads/writes this shape directly, no repacking.
typedef struct IndirectCommand {
	uint32_t vertex_count;
	uint32_t instance_count;
	uint32_t first_vertex;
	uint32_t first_instance;
} IndirectCommand;

static float rand_float(float min, float max) {
	float scale = (float) rand() / (float) RAND_MAX;
	return min + scale * (max - min);
}

// Column-major left-handed perspective projection (z in [0,1]), matching the
// original sample's XMMatrixPerspectiveFovLH -- the shaders are compiled
// column-major (-matrix-layout-column-major), so this array uploads
// directly as a float4x4 constant with no transpose.
static void build_perspective_lh(float fov_y_radians, float aspect, float z_near, float z_far, float out[16]) {
	float f = 1.0f / tanf(fov_y_radians * 0.5f);
	memset(out, 0, sizeof(float) * 16);
	out[0]  = f / aspect;
	// Not negated: Carrottus GPU's clip space is +Y up on every backend.
	out[5]  = f;
	out[10] = z_far / (z_far - z_near);
	out[11] = 1.0f;
	out[14] = -(z_far * z_near) / (z_far - z_near);
}

int main(int argc, char **argv) {
	// --frames N: render N frames, then leave the loop as closing the window
	// does, through the same cleanup. Without it the sample runs until the
	// window is closed.
	// --device N: use the Nth adapter crgpu_adapter_enumerate lists. 0 if
	// not given.
	uint64_t frame_limit  = 0;
	uint32_t device_index = 0;
	for (int i = 1; i + 1 < argc; ++i) {
		if (strcmp(argv[i], "--frames") == 0)
			frame_limit = strtoull(argv[i + 1], NULL, 10);
		else if (strcmp(argv[i], "--device") == 0)
			device_index = (uint32_t)strtoul(argv[i + 1], NULL, 10);
	}
	uint64_t frames_run = 0;

	// Every object main makes, NULL until it is made. A failure jumps to
	// cleanup, which destroys whatever exists: every destroy call takes NULL.
	int exit_code                                     = 1;
	SDL_Window *window                                = NULL;
	crgpu_instance instance                         = NULL;
	crgpu_device device                             = NULL;
	crgpu_command_queue graphics_queue              = NULL;
	crgpu_surface surface                           = NULL;
	crgpu_swap_chain swap_chain                     = NULL;
	crgpu_buffer vertex_buffer                      = NULL;
	crgpu_buffer_view vertex_buffer_view            = NULL;
	crgpu_buffer scene_buffer                       = NULL;
	crgpu_buffer_view scene_buffer_view             = NULL;
	crgpu_buffer projection_buffer                  = NULL;
	crgpu_buffer input_commands_buffer              = NULL;
	crgpu_buffer output_commands_buffer             = NULL;
	crgpu_descriptor_set_layout compute_set_layout  = NULL;
	crgpu_descriptor_pool compute_pool              = NULL;
	crgpu_shader_module compute_module              = NULL;
	crgpu_compute_pipeline compute_pipeline         = NULL;
	crgpu_descriptor_set_layout graphics_set_layout = NULL;
	crgpu_descriptor_pool graphics_pool             = NULL;
	crgpu_shader_module vertex_module               = NULL;
	crgpu_shader_module fragment_module             = NULL;
	crgpu_graphics_pipeline graphics_pipeline         = NULL;
	crgpu_command_allocator command_allocator       = NULL;
	crgpu_command_list cmd                          = NULL;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	window = SDL_CreateWindow("Carrottus GPU ExecuteIndirect", WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		goto cleanup;
	}

	const CR_ALLOCATOR gpu_allocator           = cr_allocator_system();
	const crgpu_instance_desc gpu_instance_desc = {
	    .base                = {CR_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "CrGPUExecuteIndirect",
	    .application_version = 1,
	    .engine_name         = "CrGPUSamples",
	    .engine_version      = 1,
	};
	if (CR_FAILED(crgpu_instance_create(&gpu_instance_desc, &instance))) {
		fprintf(stderr, "crgpu_instance_create failed\n");
		goto cleanup;
	}

	const crgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	if (CR_FAILED(crgpu_adapter_enumerate(instance, &adapters, &adapter_count)) || adapter_count == 0) {
		fprintf(stderr, "crgpu_adapter_enumerate failed or listed no adapters\n");
		goto cleanup;
	}
	if (device_index >= adapter_count) {
		fprintf(stderr, "--device %u: there are %u adapters\n", device_index, adapter_count);
		goto cleanup;
	}
	const crgpu_adapter adapter = adapters[device_index];

	CRGPU_ADAPTER_PROPERTIES adapter_properties = {.base = {CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES, NULL}};
	crgpu_adapter_get_properties(adapter, &adapter_properties);
	// The one shader format the adapter takes: each module below is made
	// from the embedded code in it.
	const CRGPU_SHADER_FORMAT shader_format = crgpusamples_shader_format(adapter_properties.shader_formats);
	printf("Using device: %s\n", adapter_properties.description);

	// Every draw command's first_instance picks its triangle (see the input
	// commands below), which a device allows only with this feature.
	if (!(adapter_properties.supported_features & CRGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE)) {
		fprintf(stderr, "This device/driver doesn't support an indirect draw's first_instance\n");
		goto cleanup;
	}

	// A window to present to and one direct queue, and exactly the features
	// the sample uses: a device has only what it is created with.
	const crgpu_device_desc device_desc = {
	    .base               = {CR_STRUCT_TYPE_GPU_DEVICE_DESC, NULL},
	    .required_features  = CRGPU_FEATURE_PRESENT | CRGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE |
	                         (adapter_properties.supported_features & CRGPU_FEATURE_MULTI_DRAW_INDIRECT),
	    .direct_queue_count = 1,
	    // What the sample's descriptor pools hold together, with room to
	    // spare: the caller's to choose, and refused past the adapter's
	    // limit.
	    .max_descriptors    = 1024,
	    .max_samplers       = 64,
	    .debug_name         = "CrGPUExecuteIndirect",
	};
	if (CR_FAILED(crgpu_device_create(adapter, &device_desc, &device))) {
		fprintf(stderr, "crgpu_device_create failed\n");
		goto cleanup;
	}
	crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &graphics_queue);
	// One indirect call takes the adapter's most entries with
	// multi-draw-indirect, else one.
	const uint32_t max_draw_indirect_count =
	    (crgpu_device_get_features(device) & CRGPU_FEATURE_MULTI_DRAW_INDIRECT) ? adapter_properties.max_draw_indirect_count : 1;

	surface = crgpu_create_surface_from_sdl3(instance, window);
	if (!surface) {
		fprintf(stderr, "crgpu_create_surface_from_sdl3 failed\n");
		goto cleanup;
	}

	crgpu_swap_chain_desc swap_chain_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC, NULL},
	    .surface = surface,
	    .queue   = graphics_queue,
	    .width   = WINDOW_WIDTH,
	    .height  = WINDOW_HEIGHT,
#if CRGPU_COMPILE_METAL_API
	    .buffer_count = 1,
#else
	    .buffer_count = 2,
#endif
	    .format          = CRGPU_FORMAT_B8G8R8A8_UNORM,
	    .present_mode    = CRGPU_PRESENT_MODE_FIFO,
	    // The window can't be resized and the back buffers are its size, so
	    // nothing is scaled; ONE_TO_ONE is the scaling every surface takes.
	    .present_scaling = CRGPU_PRESENT_SCALING_ONE_TO_ONE,
	    .fullscreen_mode = CRGPU_FULLSCREEN_MODE_WINDOWED,
	};
	if (CR_FAILED(crgpu_swap_chain_create(device, &swap_chain_desc, &swap_chain))) {
		fprintf(stderr, "crgpu_swap_chain_create failed\n");
		goto cleanup;
	}

	// Vertex buffer: one hardcoded triangle, instanced/drawn TRIANGLE_COUNT
	// times via indirect draw args -- each instance's animated offset/color
	// comes from scene_buffer, bound as a second, per-instance vertex buffer
	// (see graphics_pipeline_desc.vertex_bindings below).
	static const Vertex vertices[3] = {
	    {{0.0f, TRIANGLE_HALF_WIDTH, TRIANGLE_DEPTH}},
	    {{TRIANGLE_HALF_WIDTH, -TRIANGLE_HALF_WIDTH, TRIANGLE_DEPTH}},
	    {{-TRIANGLE_HALF_WIDTH, -TRIANGLE_HALF_WIDTH, TRIANGLE_DEPTH}},
	};

	crgpu_buffer_desc vertex_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_VERTEX,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = sizeof(vertices),
	};
	if (CR_FAILED(crgpu_buffer_create(device, &vertex_buffer_desc, &vertex_buffer))) {
		fprintf(stderr, "crgpu_buffer_create (vertex) failed\n");
		goto cleanup;
	}
	void *vertex_mapped = NULL;
	crgpu_buffer_map(vertex_buffer, 0, 0, &vertex_mapped);
	memcpy(vertex_mapped, vertices, sizeof(vertices));
	crgpu_buffer_flush(vertex_buffer, 0, 0);
	crgpu_buffer_unmap(vertex_buffer);

	crgpu_buffer_view_desc vertex_buffer_view_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC, NULL},
	    .parent_buffer             = vertex_buffer,
	    .offset_from_parent_buffer = 0,
	    .stride                    = sizeof(Vertex),
	    .size                      = sizeof(vertices),
	};
	crgpu_buffer_view_create(vertex_buffer, &vertex_buffer_view_desc, &vertex_buffer_view);

	// Per-triangle animated state -- UNIFORM (culling test, read by the
	// compute pass through a descriptor set) + VERTEX (offset/color, read by
	// the graphics pass as a per-instance vertex buffer -- see
	// graphics_pipeline_desc below). Kept mapped for the app's lifetime and
	// rewritten every frame, same pattern as HelloConstBuffers.
	SceneConstantBuffer scene_cb_data[TRIANGLE_COUNT];
	for (uint32_t i = 0; i < TRIANGLE_COUNT; i++) {
		scene_cb_data[i].velocity[0] = rand_float(0.01f, 0.02f);
		scene_cb_data[i].velocity[1] = scene_cb_data[i].velocity[2] = scene_cb_data[i].velocity[3] = 0.0f;
		scene_cb_data[i].offset[0]   = rand_float(-5.0f, -1.5f);
		scene_cb_data[i].offset[1]   = rand_float(-1.0f, 1.0f);
		scene_cb_data[i].offset[2]   = rand_float(0.0f, 2.0f);
		scene_cb_data[i].offset[3]   = 0.0f;
		scene_cb_data[i].color[0]    = rand_float(0.5f, 1.0f);
		scene_cb_data[i].color[1]    = rand_float(0.5f, 1.0f);
		scene_cb_data[i].color[2]    = rand_float(0.5f, 1.0f);
		scene_cb_data[i].color[3]    = 1.0f;
	}

	crgpu_buffer_desc scene_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_UNIFORM | CRGPU_BUFFER_USAGE_VERTEX,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = sizeof(scene_cb_data),
	};
	if (CR_FAILED(crgpu_buffer_create(device, &scene_buffer_desc, &scene_buffer))) {
		fprintf(stderr, "crgpu_buffer_create (scene) failed\n");
		goto cleanup;
	}
	void *scene_mapped = NULL;
	crgpu_buffer_map(scene_buffer, 0, 0, &scene_mapped);
	memcpy(scene_mapped, scene_cb_data, sizeof(scene_cb_data));
	crgpu_buffer_flush(scene_buffer, 0, sizeof(scene_cb_data));

	crgpu_buffer_view_desc scene_buffer_view_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC, NULL},
	    .parent_buffer             = scene_buffer,
	    .offset_from_parent_buffer = 0,
	    .stride                    = sizeof(SceneConstantBuffer),
	    .size                      = sizeof(scene_cb_data),
	};
	crgpu_buffer_view_create(scene_buffer, &scene_buffer_view_desc, &scene_buffer_view);

	// Shared projection matrix -- identical for every triangle (fixed aspect
	// ratio/FOV), so it's one small uniform rather than duplicated
	// TRIANGLE_COUNT times the way the D3D12 original packs it.
	float projection[16];
	build_perspective_lh(
	    (float) M_PI / 4.0f, (float) WINDOW_WIDTH / (float) WINDOW_HEIGHT, 0.01f, 20.0f, projection);

	crgpu_buffer_desc projection_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_UNIFORM,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = sizeof(projection),
	};
	if (CR_FAILED(crgpu_buffer_create(device, &projection_buffer_desc, &projection_buffer))) {
		fprintf(stderr, "crgpu_buffer_create (projection) failed\n");
		goto cleanup;
	}
	void *projection_mapped = NULL;
	crgpu_buffer_map(projection_buffer, 0, 0, &projection_mapped);
	memcpy(projection_mapped, projection, sizeof(projection));
	crgpu_buffer_flush(projection_buffer, 0, 0);
	crgpu_buffer_unmap(projection_buffer);

	// Input draw-argument buffer: TRIANGLE_COUNT plain draw commands, built
	// once and never touched again -- first_instance bakes in the triangle's
	// index into scene_buffer (see shaders/scene_vert.slang). UNIFORM (not STORAGE)
	// so it can be UPLOAD memory and written directly (UPLOAD refuses
	// STORAGE on every backend: the GPU only reads it), plus INDIRECT so it
	// can be
	// read directly by crgpu_command_list_draw_indirect when culling is off.
	IndirectCommand input_commands[TRIANGLE_COUNT];
	for (uint32_t i = 0; i < TRIANGLE_COUNT; i++) {
		input_commands[i].vertex_count   = 3;
		input_commands[i].instance_count = 1;
		input_commands[i].first_vertex   = 0;
		input_commands[i].first_instance = i;
	}

	crgpu_buffer_desc input_commands_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_UNIFORM | CRGPU_BUFFER_USAGE_INDIRECT,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = sizeof(input_commands),
	};
	if (CR_FAILED(crgpu_buffer_create(device, &input_commands_desc, &input_commands_buffer))) {
		fprintf(stderr, "crgpu_buffer_create (input commands) failed\n");
		goto cleanup;
	}
	void *input_commands_mapped = NULL;
	crgpu_buffer_map(input_commands_buffer, 0, 0, &input_commands_mapped);
	memcpy(input_commands_mapped, input_commands, sizeof(input_commands));
	crgpu_buffer_flush(input_commands_buffer, 0, 0);
	crgpu_buffer_unmap(input_commands_buffer);

	// Output draw-argument buffer: written by the compute pass every frame
	// (real args for a visible triangle, zeroed vertex_count for a culled
	// one), then read directly as the indirect-draw source. STORAGE (the
	// compute shader writes it) + INDIRECT, in DEVICE memory -- it's never
	// touched from the CPU, so nothing needs it mapped.
	crgpu_buffer_desc output_commands_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_INDIRECT,
	    .memory_kind  = CRGPU_MEMORY_KIND_DEVICE,
	    .size         = sizeof(input_commands),
	};
	if (CR_FAILED(crgpu_buffer_create(device, &output_commands_desc, &output_commands_buffer))) {
		fprintf(stderr, "crgpu_buffer_create (output commands) failed\n");
		goto cleanup;
	}

	// Compute descriptor set: scene (0), input commands (1), output commands
	// (2, read/write), projection (3).
	crgpu_descriptor_set_layout_desc compute_set_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings = {
	        {.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_COMPUTE},
	        {.binding = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_COMPUTE},
	        {.binding = 2, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_COMPUTE},
	        {.binding = 3, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_COMPUTE},
	    },
	    .binding_count = 4,
	};
	if (CR_FAILED(crgpu_descriptor_set_layout_create(device, &compute_set_layout_desc, &compute_set_layout))) {
		fprintf(stderr, "crgpu_descriptor_set_layout_create (compute) failed\n");
		goto cleanup;
	}

	crgpu_descriptor_pool_desc compute_pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets   = 1,
	    .pool_sizes = {
	        {.descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 3},
	        {.descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1},
	    },
	    .pool_size_count = 2,
	};
	if (CR_FAILED(crgpu_descriptor_pool_create(device, &compute_pool_desc, &compute_pool))) {
		fprintf(stderr, "crgpu_descriptor_pool_create (compute) failed\n");
		goto cleanup;
	}

	crgpu_descriptor_set_desc compute_set_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL},
	    .pool        = compute_pool,
	    .set_layouts = {compute_set_layout},
	    .set_count   = 1,
	};
	crgpu_descriptor_set compute_set = NULL;
	if (CR_FAILED(crgpu_descriptor_set_create(device, &compute_set_desc, &compute_set))) {
		fprintf(stderr, "crgpu_descriptor_set_create (compute) failed\n");
		goto cleanup;
	}

	crgpu_descriptor_buffer_info compute_scene_info      = {.buffer = scene_buffer, .offset = 0, .range = sizeof(scene_cb_data)};
	crgpu_descriptor_buffer_info compute_input_info      = {.buffer = input_commands_buffer, .offset = 0, .range = sizeof(input_commands)};
	crgpu_descriptor_buffer_info compute_output_info     = {.buffer = output_commands_buffer, .offset = 0, .range = sizeof(input_commands)};
	crgpu_descriptor_buffer_info compute_projection_info = {.buffer = projection_buffer, .offset = 0, .range = sizeof(projection)};
	crgpu_write_descriptor_set compute_writes[4] = {
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = compute_set, .dst_binding = 0, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .buffer_info = &compute_scene_info},
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = compute_set, .dst_binding = 1, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .buffer_info = &compute_input_info},
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = compute_set, .dst_binding = 2, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .buffer_info = &compute_output_info},
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = compute_set, .dst_binding = 3, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .buffer_info = &compute_projection_info},
	};
	crgpu_device_update_descriptor_sets(device, compute_writes, 4);

	compute_module = crgpusamples_create_shader_module(device, shader_format, &cull_comp_shader, CRGPU_SHADER_STAGE_COMPUTE, COMPUTE_THREAD_BLOCK_SIZE, 1, 1);
	if (!compute_module)
		goto cleanup;

	crgpu_compute_pipeline_desc compute_pipeline_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC, NULL},
	    .compute_module              = compute_module,
	    .descriptor_set_layouts      = {compute_set_layout},
	    .descriptor_set_layout_count = 1,
	};
	if (CR_FAILED(crgpu_compute_pipeline_create(device, &compute_pipeline_desc, &compute_pipeline))) {
		fprintf(stderr, "crgpu_compute_pipeline_create failed\n");
		goto cleanup;
	}

	// Graphics descriptor set: projection (0). Per-triangle offset/color no
	// longer goes through a descriptor -- it's a per-instance vertex buffer
	// (see graphics_pipeline_desc below).
	crgpu_descriptor_set_layout_desc graphics_set_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings = {
	        {.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_VERTEX},
	    },
	    .binding_count = 1,
	};
	if (CR_FAILED(crgpu_descriptor_set_layout_create(device, &graphics_set_layout_desc, &graphics_set_layout))) {
		fprintf(stderr, "crgpu_descriptor_set_layout_create (graphics) failed\n");
		goto cleanup;
	}

	crgpu_descriptor_pool_desc graphics_pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets        = 1,
	    .pool_sizes      = {{.descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1}},
	    .pool_size_count = 1,
	};
	if (CR_FAILED(crgpu_descriptor_pool_create(device, &graphics_pool_desc, &graphics_pool))) {
		fprintf(stderr, "crgpu_descriptor_pool_create (graphics) failed\n");
		goto cleanup;
	}

	crgpu_descriptor_set_desc graphics_set_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL},
	    .pool        = graphics_pool,
	    .set_layouts = {graphics_set_layout},
	    .set_count   = 1,
	};
	crgpu_descriptor_set graphics_set = NULL;
	if (CR_FAILED(crgpu_descriptor_set_create(device, &graphics_set_desc, &graphics_set))) {
		fprintf(stderr, "crgpu_descriptor_set_create (graphics) failed\n");
		goto cleanup;
	}

	crgpu_descriptor_buffer_info graphics_projection_info = {.buffer = projection_buffer, .offset = 0, .range = sizeof(projection)};
	crgpu_write_descriptor_set graphics_writes[1] = {
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = graphics_set, .dst_binding = 0, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .buffer_info = &graphics_projection_info},
	};
	crgpu_device_update_descriptor_sets(device, graphics_writes, 1);

	vertex_module   = crgpusamples_create_shader_module(device, shader_format, &scene_vert_shader, CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
	fragment_module = crgpusamples_create_shader_module(device, shader_format, &scene_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	if (!vertex_module || !fragment_module)
		goto cleanup;

	crgpu_graphics_pipeline_desc graphics_pipeline_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC, NULL},
	    .vertex_module      = vertex_module,
	    .fragment_module    = fragment_module,
	    .vertex_attributes  = {
	        {.location = 0, .binding = 0, .format = CRGPU_FORMAT_R32G32B32_FLOAT, .offset = offsetof(Vertex, position)},
	        {.location = 1, .binding = 1, .format = CRGPU_FORMAT_R32G32B32A32_FLOAT, .offset = offsetof(SceneConstantBuffer, offset)},
	        {.location = 2, .binding = 1, .format = CRGPU_FORMAT_R32G32B32A32_FLOAT, .offset = offsetof(SceneConstantBuffer, color)},
	    },
	    .vertex_attribute_count = 3,
	    .vertex_bindings        = {
	        {.binding = 0, .stride = sizeof(Vertex), .per_instance = false},
	        {.binding = 1, .stride = sizeof(SceneConstantBuffer), .per_instance = true},
	    },
	    .vertex_binding_count        = 2,
	    .primitive_topology          = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	    .cull_mode                   = CRGPU_CULL_MODE_NONE,
	    .front_face_ccw              = true,
	    .color_attachment_formats    = {CRGPU_FORMAT_B8G8R8A8_UNORM},
	    .color_attachment_count      = 1,
	    .depth_format                = CRGPU_FORMAT_UNKNOWN,
	    .descriptor_set_layouts      = {graphics_set_layout},
	    .descriptor_set_layout_count = 1,
	};
	if (CR_FAILED(crgpu_graphics_pipeline_create(device, &graphics_pipeline_desc, &graphics_pipeline))) {
		fprintf(stderr, "crgpu_graphics_pipeline_create failed\n");
		goto cleanup;
	}

	crgpu_command_allocator_desc allocator_desc = {.base = {CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC, NULL}, .type = CRGPU_COMMAND_LIST_TYPE_DIRECT};
	crgpu_command_allocator_create(device, &allocator_desc, &command_allocator);

	crgpu_command_list_create(command_allocator, &cmd);

	const uint32_t dispatch_group_count = (TRIANGLE_COUNT + COMPUTE_THREAD_BLOCK_SIZE - 1) / COMPUTE_THREAD_BLOCK_SIZE;
	const float offset_bounds           = 2.5f;

	CRGPU_SCISSOR_RECT full_scissor = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
	CRGPU_SCISSOR_RECT culling_scissor = {
	    .x      = (WINDOW_WIDTH / 2.0f) - (WINDOW_WIDTH / 2.0f) * CULL_OFFSET,
	    .y      = 0,
	    .width  = WINDOW_WIDTH * CULL_OFFSET,
	    .height = WINDOW_HEIGHT,
	};

	bool running        = true;
	bool enable_culling = true;
	while (running) {
		if (frame_limit != 0 && frames_run == frame_limit)
			break;
		++frames_run;
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT)
				running = false;
			if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE && !event.key.repeat)
				enable_culling = !enable_culling;
		}

		for (uint32_t i = 0; i < TRIANGLE_COUNT; i++) {
			scene_cb_data[i].offset[0] += scene_cb_data[i].velocity[0];
			if (scene_cb_data[i].offset[0] > offset_bounds) {
				scene_cb_data[i].velocity[0] = rand_float(0.01f, 0.02f);
				scene_cb_data[i].offset[0]   = -offset_bounds;
			}
		}
		memcpy(scene_mapped, scene_cb_data, sizeof(scene_cb_data));
		crgpu_buffer_flush(scene_buffer, 0, sizeof(scene_cb_data));

		uint32_t image_index               = crgpu_swap_chain_acquire_next_image(swap_chain);
		// No back buffer: the device was lost, or the surface is gone. The
		// window can't be resized, so none will come back; nothing is recorded
		// into a view that isn't there.
		if (image_index == CR_UINT32_MAX) {
			fprintf(stderr, "crgpu_swap_chain_acquire_next_image failed: no back buffer to render into\n");
			goto cleanup;
		}
		crgpu_image_view backbuffer_view = crgpu_swap_chain_get_image_view(swap_chain, image_index);

		crgpu_command_allocator_reset(command_allocator);
		crgpu_command_list_begin(cmd);

		if (enable_culling) {
			crgpu_command_list_bind_compute_pipeline(cmd, compute_pipeline);
			crgpu_command_list_bind_compute_descriptor_sets(cmd, compute_pipeline, 0, &compute_set, 1);
			crgpu_command_list_dispatch(cmd, dispatch_group_count, 1, 1);

			crgpu_buffer_barrier post_dispatch_barrier = {
			    .base = {CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL},
			    .buffer       = output_commands_buffer,
			    .state_before = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS,
			    .state_after  = CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT,
			};
			crgpu_command_list_pipeline_barrier(cmd, &post_dispatch_barrier, 1, NULL, 0);
		}

		crgpu_command_list_image_barrier_view(cmd, backbuffer_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		crgpu_rendering_begin_desc rendering_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC, NULL},
		    .color_attachments = {
		        {
		            .image_view  = backbuffer_view,
		            .load_op     = CRGPU_LOAD_OP_CLEAR,
		            .store_op    = CRGPU_STORE_OP_STORE,
		            .clear_color = {0.0f, 0.02f, 0.04f, 1.0f},
		        },
		    },
		    .color_attachment_count = 1,
		    .width                  = WINDOW_WIDTH,
		    .height                 = WINDOW_HEIGHT,
		};
		crgpu_command_list_begin_rendering(cmd, &rendering_desc);

		crgpu_command_list_bind_graphics_pipeline(cmd, graphics_pipeline);
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, graphics_pipeline, 0, &graphics_set, 1);

		CRGPU_VIEWPORT viewport = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT, .minDepth = 0.0f, .maxDepth = 1.0f};
		crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);
		crgpu_command_list_set_scissor_rects(cmd, 0, 1, enable_culling ? &culling_scissor : &full_scissor);

		crgpu_buffer_view graphics_vertex_buffer_views[2] = {vertex_buffer_view, scene_buffer_view};
		crgpu_command_list_set_vertex_buffers(cmd, 0, 2, graphics_vertex_buffer_views);
		// One call where the device takes TRIANGLE_COUNT draws in one (a
		// device with CRGPU_FEATURE_MULTI_DRAW_INDIRECT), else as many calls
		// as its max_draw_indirect_count needs, at advancing offsets.
		for (uint32_t first = 0; first < TRIANGLE_COUNT;) {
			uint32_t left  = TRIANGLE_COUNT - first;
			uint32_t count = left < max_draw_indirect_count ? left : max_draw_indirect_count;
			crgpu_command_list_draw_indirect(
			    cmd,
			    enable_culling ? output_commands_buffer : input_commands_buffer,
			    (uint64_t)first * sizeof(IndirectCommand), count, sizeof(IndirectCommand));
			first += count;
		}

		crgpu_command_list_end_rendering(cmd);
		crgpu_command_list_image_barrier_view(cmd, backbuffer_view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_PRESENT_SRC);

		if (enable_culling) {
			// Ready output_commands_buffer for next frame's compute write.
			crgpu_buffer_barrier pre_dispatch_barrier = {
			    .base = {CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL},
			    .buffer       = output_commands_buffer,
			    .state_before = CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT,
			    .state_after  = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS,
			};
			crgpu_command_list_pipeline_barrier(cmd, &pre_dispatch_barrier, 1, NULL, 0);
		}

		crgpu_command_list_end(cmd);

		const CR_RESULT submitted = crgpu_command_queue_submit_lists_synced(graphics_queue, &cmd, 1, swap_chain);
		if (CR_FAILED(submitted)) {
			fprintf(stderr, "crgpu_command_queue_submit_lists_synced failed: %s\n", cr_result_str(submitted));
			goto cleanup;
		}
		crgpu_swap_chain_present(swap_chain);
	}

	exit_code = 0;

cleanup:
	if (graphics_queue)
		crgpu_command_queue_wait_idle(graphics_queue);

	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(command_allocator);
	crgpu_graphics_pipeline_destroy(graphics_pipeline);
	crgpu_shader_module_destroy(fragment_module);
	crgpu_shader_module_destroy(vertex_module);
	crgpu_descriptor_pool_destroy(graphics_pool);
	crgpu_descriptor_set_layout_destroy(graphics_set_layout);
	crgpu_compute_pipeline_destroy(compute_pipeline);
	crgpu_shader_module_destroy(compute_module);
	crgpu_descriptor_pool_destroy(compute_pool);
	crgpu_descriptor_set_layout_destroy(compute_set_layout);
	if (scene_buffer)
		crgpu_buffer_unmap(scene_buffer);
	crgpu_buffer_view_destroy(scene_buffer_view);
	crgpu_buffer_destroy(scene_buffer);
	crgpu_buffer_destroy(projection_buffer);
	crgpu_buffer_destroy(input_commands_buffer);
	crgpu_buffer_destroy(output_commands_buffer);
	crgpu_buffer_view_destroy(vertex_buffer_view);
	crgpu_buffer_destroy(vertex_buffer);
	crgpu_swap_chain_destroy(swap_chain);
	crgpu_surface_destroy(surface);
	crgpu_device_destroy(device);
	crgpu_instance_destroy(instance);

	if (window)
		SDL_DestroyWindow(window);
	SDL_Quit();
	return exit_code;
}
