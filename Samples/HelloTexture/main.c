//
// Carrottus GPU port of D3D12HelloWorld/HelloTexture from d3d12samples. Same
// unindexed triangle as ../HelloTriangle, but the pixel shader now samples a
// procedurally-generated black/white checkerboard texture instead of
// interpolating per-vertex color -- see ../../README.md.
//
// Three deliberate deviations from the original:
// - No static/immutable root-signature sampler. Carrottus GPU has no equivalent to
//   D3D12's D3D12_STATIC_SAMPLER_DESC / Vulkan's immutable samplers yet (see
//   carrottus/CLAUDE.md's "Known gaps") -- this port uses a real, dynamic
//   crgpu_sampler instead, which is functionally identical for a sampler
//   that never changes for the life of the app, just not the zero-descriptor-
//   cost mechanism D3D12's static sampler is.
// - Separate SAMPLED_IMAGE + SAMPLER descriptors instead of one
//   COMBINED_IMAGE_SAMPLER. The combined type exists in crgpu.h and works
//   on Vulkan/D3D12, but its Metal cross-compile fails below Metal 3:
//   crgpumetaldescriptors.m already documents that the sampler half of a
//   COMBINED_IMAGE_SAMPLER is "intentionally not written" there, and
//   SPIRV-Cross confirms why -- under Carrottus GPU's per-set-argument-buffer
//   scheme, a GLSL sampler2D's synthesized sampler member overlaps the
//   texture's own binding slot, which SPIRV-Cross only allows via "full
//   mutable aliasing" on Metal 3+. This port instead uses the same split
//   texture2D/sampler idiom CrGPUDynamicIndexing's bindless design already
//   established, which cross-compiles cleanly on all three backends.
// - CLAMP_TO_EDGE addressing instead of the original's CLAMP_TO_BORDER with a
//   transparent-black border color. crgpu_sampler_desc has no border-color
//   field. This triangle's UVs ((0.5,0), (1,1), (0,1)) never leave [0,1], so
//   the two addressing modes are visually identical here regardless.
//

#include <SDL3/SDL.h>
#include <crgpu.h>
#include <crgpu_sdl3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The shaders, Slang built into each format and embedded at build time
// (../../cmake/crgpusamples_shader.cmake).
#include "texture_vert.shader.h"
#include "texture_frag.shader.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

#define TEXTURE_WIDTH 256
#define TEXTURE_HEIGHT 256

typedef struct Vertex {
	float position[3];
	float uv[2];
} Vertex;

// Generates a simple black/white checkerboard, matching the original
// sample's D3D12HelloTexture::GenerateTextureData exactly (8x8 cells).
// NULL, printed, if there's no memory for it.
static uint8_t *generate_checkerboard_texture(uint32_t width, uint32_t height) {
	const uint32_t pixel_size  = 4;
	const uint32_t row_pitch   = width * pixel_size;
	const uint32_t cell_pitch  = row_pitch >> 3;
	const uint32_t cell_height = width >> 3;
	const uint32_t texture_size = row_pitch * height;

	uint8_t *data = (uint8_t *) malloc(texture_size);
	if (!data) {
		fprintf(stderr, "No memory for checkerboard texture\n");
		return NULL;
	}
	for (uint32_t n = 0; n < texture_size; n += pixel_size) {
		uint32_t x = n % row_pitch;
		uint32_t y = n / row_pitch;
		uint32_t i = x / cell_pitch;
		uint32_t j = y / cell_height;

		uint8_t value  = (i % 2 == j % 2) ? 0x00 : 0xff;
		data[n]     = value;
		data[n + 1] = value;
		data[n + 2] = value;
		data[n + 3] = 0xff;
	}
	return data;
}

// Uploads `pixels` (tightly packed, width*height*4 bytes) into a freshly
// created CRGPU_FORMAT_R8G8B8A8_UNORM image, via a staging buffer sized
// and strided exactly as crgpu_image_get_buffer_copy_size reports --
// portable across Vulkan (no alignment requirement) and D3D12 (256-byte row
// pitch alignment), rather than assuming a tightly-packed row pitch.
// NULL, printed, on failure.
static crgpu_image upload_rgba8_texture(
    crgpu_device device,
    crgpu_command_list cmd,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    crgpu_buffer *out_staging_buffer) {
	crgpu_image_desc image_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_IMAGE_DESC, NULL},
	    .usage        = CRGPU_IMAGE_USAGE_SAMPLED | CRGPU_IMAGE_USAGE_TRANSFER_DST,
	    .type         = CRGPU_IMAGE_TYPE_2D,
	    .format       = CRGPU_FORMAT_R8G8B8A8_UNORM,
	    .width        = width,
	    .height       = height,
	    .depth        = 1,
	    .array_layers = 1,
	    .mip_levels   = 1,
	};
	crgpu_image image = NULL;
	if (CR_FAILED(crgpu_image_create(device, &image_desc, &image))) {
		fprintf(stderr, "crgpu_image_create failed\n");
		return NULL;
	}

	uint64_t row_pitch = 0, total_size = 0;
	crgpu_image_get_buffer_copy_size(image, 0, &row_pitch, &total_size);
	if (row_pitch == 0) row_pitch = (uint64_t) width * 4;
	if (total_size == 0) total_size = row_pitch * height;

	crgpu_buffer_desc staging_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_TRANSFER_SRC,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = total_size,
	};
	crgpu_buffer staging = NULL;
	if (CR_FAILED(crgpu_buffer_create(device, &staging_desc, &staging))) {
		fprintf(stderr, "crgpu_buffer_create (staging) failed\n");
		crgpu_image_destroy(image);
		return NULL;
	}

	uint8_t *mapped = NULL;
	crgpu_buffer_map(staging, 0, 0, (void **) &mapped);
	for (uint32_t y = 0; y < height; y++)
		memcpy(mapped + y * row_pitch, pixels + (size_t) y * width * 4, (size_t) width * 4);
	crgpu_buffer_flush(staging, 0, 0);
	crgpu_buffer_unmap(staging);

	crgpu_command_list_image_barrier(cmd, image, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_TRANSFER_DST);

	crgpu_image_buffer_copy_desc copy_desc = {
	    .buffer_row_length   = (uint32_t) (row_pitch / 4),
	    .buffer_image_height = height,
	    .array_layer_count   = 1,
	    .width               = width,
	    .height              = height,
	    .depth               = 1,
	};
	crgpu_command_list_copy_buffer_to_image(cmd, staging, image, &copy_desc);

	crgpu_command_list_image_barrier(cmd, image, CRGPU_IMAGE_LAYOUT_TRANSFER_DST, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);

	// Must stay alive until the GPU has finished the copy above -- the
	// caller destroys *out_staging_buffer only after waiting for this
	// upload command list to complete.
	*out_staging_buffer = staging;
	return image;
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
	int exit_code                               = 1;
	SDL_Window *window                          = NULL;
	crgpu_instance instance                   = NULL;
	crgpu_device device                       = NULL;
	crgpu_command_queue graphics_queue        = NULL;
	crgpu_surface surface                     = NULL;
	crgpu_swap_chain swap_chain               = NULL;
	crgpu_command_allocator command_allocator = NULL;
	crgpu_command_list cmd                    = NULL;
	crgpu_buffer vertex_buffer                = NULL;
	crgpu_buffer_view vertex_buffer_view      = NULL;
	crgpu_image texture                       = NULL;
	crgpu_image_view texture_view             = NULL;
	crgpu_sampler sampler                     = NULL;
	crgpu_descriptor_set_layout set_layout    = NULL;
	crgpu_descriptor_pool descriptor_pool     = NULL;
	crgpu_shader_module vertex_module         = NULL;
	crgpu_shader_module fragment_module       = NULL;
	crgpu_graphics_pipeline pipeline            = NULL;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	window = SDL_CreateWindow("Carrottus GPU HelloTexture", WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		goto cleanup;
	}

	const CR_ALLOCATOR gpu_allocator           = cr_allocator_system();
	const crgpu_instance_desc gpu_instance_desc = {
	    .base                = {CR_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "CrGPUHelloTexture",
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

	// A window to present to and one direct queue, and exactly the features
	// the sample uses: a device has only what it is created with.
	const crgpu_device_desc device_desc = {
	    .base               = {CR_STRUCT_TYPE_GPU_DEVICE_DESC, NULL},
	    .required_features  = CRGPU_FEATURE_PRESENT,
	    .direct_queue_count = 1,
	    // What the sample's descriptor pools hold together, with room to
	    // spare: the caller's to choose, and refused past the adapter's
	    // limit.
	    .max_descriptors    = 1024,
	    .max_samplers       = 64,
	    .debug_name         = "HelloTexture",
	};
	if (CR_FAILED(crgpu_device_create(adapter, &device_desc, &device))) {
		fprintf(stderr, "crgpu_device_create failed\n");
		goto cleanup;
	}
	crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &graphics_queue);

	surface = crgpu_create_surface_from_sdl3(instance, window);
	if (!surface) {
		fprintf(stderr, "crgpu_create_surface_from_sdl3 failed\n");
		goto cleanup;
	}

	crgpu_swap_chain_desc swap_chain_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC, NULL},
	    .surface         = surface,
	    .queue           = graphics_queue,
	    .width           = WINDOW_WIDTH,
	    .height          = WINDOW_HEIGHT,
	    // Metal's CAMetalLayer hands out one drawable at a time -- no stable
	    // N-image array to double-buffer against like Vulkan/D3D12, so
	    // crgpu_swap_chain_create rejects anything but 1 on that backend.
#if CRGPU_COMPILE_METAL_API
	    .buffer_count    = 1,
#else
	    .buffer_count    = 2,
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

	crgpu_command_allocator_desc allocator_desc = {.base = {CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC, NULL}, .type = CRGPU_COMMAND_LIST_TYPE_DIRECT};
	crgpu_command_allocator_create(device, &allocator_desc, &command_allocator);

	crgpu_command_list_create(command_allocator, &cmd);

	// Vertex buffer: one hardcoded triangle with UVs, UPLOAD memory
	// written once -- same aspect-ratio-corrected shape as the original.
	float aspect_ratio = (float) WINDOW_WIDTH / (float) WINDOW_HEIGHT;
	Vertex vertices[3] = {
	    // Carrottus GPU's clip space is +Y up on every backend, as the original's
	    // is: its vertices as they are, the apex at the top.
	    {{0.0f, 0.25f * aspect_ratio, 0.0f}, {0.5f, 0.0f}},
	    {{0.25f, -0.25f * aspect_ratio, 0.0f}, {1.0f, 1.0f}},
	    {{-0.25f, -0.25f * aspect_ratio, 0.0f}, {0.0f, 1.0f}},
	};

	crgpu_buffer_desc vertex_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_VERTEX,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = sizeof(vertices),
	};
	if (CR_FAILED(crgpu_buffer_create(device, &vertex_buffer_desc, &vertex_buffer))) {
		fprintf(stderr, "crgpu_buffer_create failed\n");
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

	// ------------------------------------------------------------------
	// Texture + sampler. Upload happens on `cmd`, submitted once and waited
	// on below before the render loop starts.
	// ------------------------------------------------------------------

	crgpu_command_list_begin(cmd);

	uint8_t *checkerboard = generate_checkerboard_texture(TEXTURE_WIDTH, TEXTURE_HEIGHT);
	if (!checkerboard)
		goto cleanup;
	crgpu_buffer staging_buffer = NULL;
	texture = upload_rgba8_texture(device, cmd, checkerboard, TEXTURE_WIDTH, TEXTURE_HEIGHT, &staging_buffer);
	free(checkerboard);
	if (!texture)
		goto cleanup;

	crgpu_command_list_end(cmd);
	crgpu_command_queue_submit_lists(graphics_queue, &cmd, 1);
	crgpu_command_queue_wait_idle(graphics_queue);
	crgpu_buffer_destroy(staging_buffer);

	crgpu_image_view_desc texture_view_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC, NULL},
	    .parent_image = texture,
	    .type         = CRGPU_IMAGE_VIEW_TYPE_2D,
	    .subresource_range = {.aspect_mask = 1 /* COLOR */, .base_mip_level = 0, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
	};
	crgpu_image_view_create(texture, &texture_view_desc, &texture_view);

	crgpu_sampler_desc sampler_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_SAMPLER_DESC, NULL},
	    .mag_filter     = CRGPU_FILTER_NEAREST,
	    .min_filter     = CRGPU_FILTER_NEAREST,
	    .mipmap_filter  = CRGPU_FILTER_NEAREST,
	    .address_mode_u = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .address_mode_v = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .address_mode_w = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .max_lod        = 0.0f,
	    .max_anisotropy = 1.0f,
	};
	if (CR_FAILED(crgpu_sampler_create(device, &sampler_desc, &sampler))) {
		fprintf(stderr, "crgpu_sampler_create failed\n");
		goto cleanup;
	}

	// Descriptor set layout: the texture at binding 0 (SAMPLED_IMAGE) and its
	// sampler at binding 1 (SAMPLER), both visible to the fragment stage only
	// -- see the file header comment for why this isn't one
	// COMBINED_IMAGE_SAMPLER binding.
	crgpu_descriptor_set_layout_desc set_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings = {
	        {.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_FRAGMENT},
	        {.binding = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_FRAGMENT},
	    },
	    .binding_count = 2,
	};
	if (CR_FAILED(crgpu_descriptor_set_layout_create(device, &set_layout_desc, &set_layout))) {
		fprintf(stderr, "crgpu_descriptor_set_layout_create failed\n");
		goto cleanup;
	}

	crgpu_descriptor_pool_desc pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets   = 1,
	    .pool_sizes = {
	        {.descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1},
	        {.descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER, .count = 1},
	    },
	    .pool_size_count = 2,
	};
	if (CR_FAILED(crgpu_descriptor_pool_create(device, &pool_desc, &descriptor_pool))) {
		fprintf(stderr, "crgpu_descriptor_pool_create failed\n");
		goto cleanup;
	}

	crgpu_descriptor_set_desc set_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL},
	    .pool        = descriptor_pool,
	    .set_layouts = {set_layout},
	    .set_count   = 1,
	};
	crgpu_descriptor_set texture_set = NULL;
	if (CR_FAILED(crgpu_descriptor_set_create(device, &set_desc, &texture_set))) {
		fprintf(stderr, "crgpu_descriptor_set_create failed\n");
		goto cleanup;
	}

	crgpu_descriptor_image_info texture_image_info = {
	    .image_view   = texture_view,
	    .image_layout = CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY,
	};
	crgpu_write_descriptor_set texture_writes[2] = {
	    {
	        .base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL},
	        .dst_set          = texture_set,
	        .dst_binding      = 0,
	        .descriptor_count = 1,
	        .descriptor_type  = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	        .image_info       = &texture_image_info,
	    },
	    {
	        .base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL},
	        .dst_set          = texture_set,
	        .dst_binding      = 1,
	        .descriptor_count = 1,
	        .descriptor_type  = CRGPU_DESCRIPTOR_TYPE_SAMPLER,
	        .sampler          = sampler,
	    },
	};
	crgpu_device_update_descriptor_sets(device, texture_writes, 2);

	vertex_module   = crgpusamples_create_shader_module(device, shader_format, &texture_vert_shader, CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
	fragment_module = crgpusamples_create_shader_module(device, shader_format, &texture_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	if (!vertex_module || !fragment_module)
		goto cleanup;

	crgpu_graphics_pipeline_desc pipeline_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC, NULL},
	    .vertex_module   = vertex_module,
	    .fragment_module = fragment_module,
	    .vertex_attributes = {
	        {.location = 0, .binding = 0, .format = CRGPU_FORMAT_R32G32B32_FLOAT, .offset = offsetof(Vertex, position)},
	        {.location = 1, .binding = 0, .format = CRGPU_FORMAT_R32G32_FLOAT, .offset = offsetof(Vertex, uv)},
	    },
	    .vertex_attribute_count = 2,
	    .vertex_bindings        = {
	        {.binding = 0, .stride = sizeof(Vertex), .per_instance = false},
	    },
	    .vertex_binding_count    = 1,
	    .primitive_topology      = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	    .cull_mode               = CRGPU_CULL_MODE_NONE,
	    .front_face_ccw          = true,
	    .color_attachment_formats = {CRGPU_FORMAT_B8G8R8A8_UNORM},
	    .color_attachment_count  = 1,
	    .depth_format            = CRGPU_FORMAT_UNKNOWN,
	    .descriptor_set_layouts      = {set_layout},
	    .descriptor_set_layout_count = 1,
	};
	if (CR_FAILED(crgpu_graphics_pipeline_create(device, &pipeline_desc, &pipeline))) {
		fprintf(stderr, "crgpu_graphics_pipeline_create failed\n");
		goto cleanup;
	}

	bool running = true;
	while (running) {
		if (frame_limit != 0 && frames_run == frame_limit)
			break;
		++frames_run;
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT)
				running = false;
		}

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

		crgpu_command_list_image_barrier_view(cmd, backbuffer_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		crgpu_rendering_begin_desc rendering_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC, NULL},
		    .color_attachments = {
		        {
		            .image_view  = backbuffer_view,
		            .load_op     = CRGPU_LOAD_OP_CLEAR,
		            .store_op    = CRGPU_STORE_OP_STORE,
		            .clear_color = {0.0f, 0.2f, 0.4f, 1.0f},
		        },
		    },
		    .color_attachment_count = 1,
		    .width                  = WINDOW_WIDTH,
		    .height                 = WINDOW_HEIGHT,
		};
		crgpu_command_list_begin_rendering(cmd, &rendering_desc);

		crgpu_command_list_bind_graphics_pipeline(cmd, pipeline);
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline, 0, &texture_set, 1);

		CRGPU_VIEWPORT viewport = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT, .minDepth = 0.0f, .maxDepth = 1.0f};
		crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);

		CRGPU_SCISSOR_RECT scissor = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
		crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);

		crgpu_command_list_set_vertex_buffers(cmd, 0, 1, &vertex_buffer_view);
		crgpu_command_list_draw(cmd, 3, 0);

		crgpu_command_list_end_rendering(cmd);
		crgpu_command_list_image_barrier_view(cmd, backbuffer_view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_PRESENT_SRC);

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

	crgpu_graphics_pipeline_destroy(pipeline);
	crgpu_shader_module_destroy(fragment_module);
	crgpu_shader_module_destroy(vertex_module);
	crgpu_descriptor_pool_destroy(descriptor_pool);
	crgpu_descriptor_set_layout_destroy(set_layout);
	crgpu_sampler_destroy(sampler);
	crgpu_image_view_destroy(texture_view);
	crgpu_image_destroy(texture);
	crgpu_buffer_view_destroy(vertex_buffer_view);
	crgpu_buffer_destroy(vertex_buffer);
	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(command_allocator);
	crgpu_swap_chain_destroy(swap_chain);
	crgpu_surface_destroy(surface);
	crgpu_device_destroy(device);
	crgpu_instance_destroy(instance);

	if (window)
		SDL_DestroyWindow(window);
	SDL_Quit();
	return exit_code;
}
