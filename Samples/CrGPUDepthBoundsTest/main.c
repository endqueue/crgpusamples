//
// Carrottus GPU port of D3D12DepthBoundsTest from d3d12samples. Same scene (one
// hardcoded, unindexed triangle whose three vertices sit at different
// depths) and the same animated depth-bounds window, rebuilt against
// crgpu_* calls instead of ID3D12*/DXGI -- see ../../README.md for the one
// deliberate deviation this port takes (a highlight-colored second pass
// instead of the original's colorless depth-only priming pass).
//

#include <SDL3/SDL.h>
#include <math.h>
#include <crgpu.h>
#include <crgpu_sdl3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The shaders, Slang built into each format and embedded at build time
// (../../cmake/crgpusamples_shader.cmake).
#include "triangle_vert.shader.h"
#include "triangle_frag.shader.h"
#include "highlight_frag.shader.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

// aspect_mask is a plain uint64_t on crgpu_image_subresource_range (see
// crgpu.h) -- Carrottus GPU has no enum/constant of its own for it yet, so this
// mirrors the raw VK_IMAGE_ASPECT_DEPTH_BIT value every backend agrees on,
// the same local #define CrGPUDynamicIndexing already uses.
#define CRGPU_ASPECT_DEPTH_BIT 0x00000002u

typedef struct Vertex {
	float position[3];
	float color[4];
} Vertex;

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
	crgpu_buffer vertex_buffer                = NULL;
	crgpu_buffer_view vertex_buffer_view      = NULL;
	crgpu_shader_module vertex_module         = NULL;
	crgpu_shader_module fragment_module       = NULL;
	crgpu_shader_module highlight_module      = NULL;
	crgpu_graphics_pipeline prime_pipeline      = NULL;
	crgpu_graphics_pipeline bounds_pipeline     = NULL;
	crgpu_command_allocator command_allocator = NULL;
	crgpu_command_list cmd                    = NULL;
	crgpu_image depth_image                   = NULL;
	crgpu_image_view depth_view               = NULL;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	window = SDL_CreateWindow("Carrottus GPU Depth Bounds Test", WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		goto cleanup;
	}

	const CR_ALLOCATOR gpu_allocator           = cr_allocator_system();
	const crgpu_instance_desc gpu_instance_desc = {
	    .base                = {CR_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "CrGPUDepthBoundsTest",
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

	// The depth bounds test where the adapter has it; the sample runs
	// without it, and the window then has no visible effect.
	const bool depth_bounds = (adapter_properties.supported_features & CRGPU_FEATURE_DEPTH_BOUNDS_TEST) != 0;
	printf("Depth bounds test supported: %s\n", depth_bounds ? "yes" : "no (bounds window will have no visible effect)");

	// A window to present to and one direct queue, and exactly the features
	// the sample uses: a device has only what it is created with.
	const crgpu_device_desc device_desc = {
	    .base               = {CR_STRUCT_TYPE_GPU_DEVICE_DESC, NULL},
	    .required_features  = CRGPU_FEATURE_PRESENT | (adapter_properties.supported_features & CRGPU_FEATURE_DEPTH_BOUNDS_TEST),
	    .direct_queue_count = 1,
	    // What the sample's descriptor pools hold together, with room to
	    // spare: the caller's to choose, and refused past the adapter's
	    // limit.
	    .max_descriptors    = 1024,
	    .max_samplers       = 64,
	    .debug_name         = "CrGPUDepthBoundsTest",
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

	// One hardcoded, unindexed triangle whose three vertices sit at
	// different depths (0.1/0.9/0.5) -- same scene as the D3D12 original,
	// same aspect-ratio correction on the Y axis.
	const float aspect_ratio = (float) WINDOW_WIDTH / (float) WINDOW_HEIGHT;
	const Vertex vertices[3] = {
	    // Carrottus GPU's clip space is +Y up on every backend, as the original's
	    // is: its vertices as they are, the top at the top.
	    {{0.00f, 0.25f * aspect_ratio, 0.1f}, {1.0f, 0.0f, 0.0f, 1.0f}}, // Top, red
	    {{0.25f, -0.25f * aspect_ratio, 0.9f}, {0.0f, 1.0f, 0.0f, 1.0f}}, // Right, green
	    {{-0.25f, -0.25f * aspect_ratio, 0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}}, // Left, blue
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

	void *mapped = NULL;
	crgpu_buffer_map(vertex_buffer, 0, 0, &mapped);
	memcpy(mapped, vertices, sizeof(vertices));
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

	vertex_module    = crgpusamples_create_shader_module(device, shader_format, &triangle_vert_shader, CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
	fragment_module  = crgpusamples_create_shader_module(device, shader_format, &triangle_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	highlight_module = crgpusamples_create_shader_module(device, shader_format, &highlight_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	if (!vertex_module || !fragment_module || !highlight_module)
		goto cleanup;

	// Priming pass: renders the triangle's true per-vertex colors and
	// writes its true per-vertex depth into the depth attachment. Ordinary
	// depth test, no depth bounds test.
	crgpu_graphics_pipeline_desc prime_pipeline_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC, NULL},
	    .vertex_module   = vertex_module,
	    .fragment_module = fragment_module,
	    .vertex_attributes = {
	        {.location = 0, .binding = 0, .format = CRGPU_FORMAT_R32G32B32_FLOAT, .offset = offsetof(Vertex, position)},
	        {.location = 1, .binding = 0, .format = CRGPU_FORMAT_R32G32B32A32_FLOAT, .offset = offsetof(Vertex, color)},
	    },
	    .vertex_attribute_count = 2,
	    .vertex_bindings        = {
	        {.binding = 0, .stride = sizeof(Vertex), .per_instance = false},
	    },
	    .vertex_binding_count   = 1,
	    .primitive_topology      = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	    .cull_mode               = CRGPU_CULL_MODE_NONE,
	    .front_face_ccw          = true,
	    .depth_test_enable       = true,
	    .depth_write_enable      = true,
	    .depth_compare_op        = CRGPU_COMPARE_OP_LESS,
	    .color_attachment_formats = {CRGPU_FORMAT_B8G8R8A8_UNORM},
	    .color_attachment_count  = 1,
	    .depth_format            = CRGPU_FORMAT_D32_FLOAT,
	};
	if (CR_FAILED(crgpu_graphics_pipeline_create(device, &prime_pipeline_desc, &prime_pipeline))) {
		fprintf(stderr, "crgpu_graphics_pipeline_create (prime) failed\n");
		goto cleanup;
	}

	// Depth-bounds pass: redraws the same triangle in a fixed highlight
	// color, with the ordinary depth test off and the depth bounds test on
	// instead -- a fragment is only visible here if the depth value the
	// priming pass just wrote at that pixel falls inside
	// crgpu_command_list_set_depth_bounds' animated [min, max] window.
	crgpu_graphics_pipeline_desc bounds_pipeline_desc = prime_pipeline_desc;
	bounds_pipeline_desc.fragment_module         = highlight_module;
	bounds_pipeline_desc.depth_test_enable       = false;
	bounds_pipeline_desc.depth_write_enable      = false;
	bounds_pipeline_desc.depth_bounds_test_enable = true;
	if (CR_FAILED(crgpu_graphics_pipeline_create(device, &bounds_pipeline_desc, &bounds_pipeline))) {
		fprintf(stderr, "crgpu_graphics_pipeline_create (bounds) failed\n");
		goto cleanup;
	}

	crgpu_command_allocator_desc allocator_desc = {.base = {CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC, NULL}, .type = CRGPU_COMMAND_LIST_TYPE_DIRECT};
	crgpu_command_allocator_create(device, &allocator_desc, &command_allocator);

	crgpu_command_list_create(command_allocator, &cmd);

	// Depth buffer, created and transitioned once up front -- it never
	// leaves CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL for the
	// life of this sample.
	crgpu_image_desc depth_image_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_IMAGE_DESC, NULL},
	    .usage        = CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT,
	    .type         = CRGPU_IMAGE_TYPE_2D,
	    .format       = CRGPU_FORMAT_D32_FLOAT,
	    .width        = WINDOW_WIDTH,
	    .height       = WINDOW_HEIGHT,
	    .depth        = 1,
	    .array_layers = 1,
	    .mip_levels   = 1,
	    .clear_value  = {.format = CRGPU_FORMAT_D32_FLOAT, .depth_stencil = {.depth = 1.0f, .stencil = 0}},
	};
	if (CR_FAILED(crgpu_image_create(device, &depth_image_desc, &depth_image))) {
		fprintf(stderr, "crgpu_image_create (depth) failed\n");
		goto cleanup;
	}
	crgpu_image_view_desc depth_view_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC, NULL},
	    .parent_image = depth_image,
	    .type         = CRGPU_IMAGE_VIEW_TYPE_2D,
	    .subresource_range = {.aspect_mask = CRGPU_ASPECT_DEPTH_BIT, .base_mip_level = 0, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
	};
	crgpu_image_view_create(depth_image, &depth_view_desc, &depth_view);

	crgpu_command_list_begin(cmd);
	crgpu_command_list_image_barrier(cmd, depth_image, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	crgpu_command_list_end(cmd);
	crgpu_command_queue_submit_lists(graphics_queue, &cmd, 1);
	crgpu_command_queue_wait_idle(graphics_queue);

	bool running          = true;
	uint64_t frame_number = 0;
	while (running) {
		if (frame_limit != 0 && frames_run == frame_limit)
			break;
		++frames_run;
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT)
				running = false;
		}

		frame_number++;

		uint32_t image_index              = crgpu_swap_chain_acquire_next_image(swap_chain);
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
		            .clear_color = {0.392f, 0.584f, 0.929f, 1.0f},
		        },
		    },
		    .color_attachment_count = 1,
		    .depth_attachment       = {.image_view = depth_view, .depth_load_op = CRGPU_LOAD_OP_CLEAR, .depth_store_op = CRGPU_STORE_OP_STORE, .clear_depth = 1.0f},
		    .width                  = WINDOW_WIDTH,
		    .height                 = WINDOW_HEIGHT,
		};
		crgpu_command_list_begin_rendering(cmd, &rendering_desc);

		CRGPU_VIEWPORT viewport = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT, .minDepth = 0.0f, .maxDepth = 1.0f};
		crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);

		CRGPU_SCISSOR_RECT scissor = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
		crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);

		crgpu_command_list_set_vertex_buffers(cmd, 0, 1, &vertex_buffer_view);

		// Prime the depth attachment with the triangle's real per-vertex
		// depth values.
		crgpu_command_list_bind_graphics_pipeline(cmd, prime_pipeline);
		crgpu_command_list_draw(cmd, 3, 0);

		// Slide the bounds window back and forth across [0, 0.25] each way
		// from the middle -- matches the D3D12 original's sinf-driven
		// animation exactly.
		float f = 0.125f + sinf((float) (frame_number & 0x7F) / 127.0f) * 0.125f;
		if (depth_bounds)
			crgpu_command_list_set_depth_bounds(cmd, 0.0f + f, 1.0f - f);

		crgpu_command_list_bind_graphics_pipeline(cmd, bounds_pipeline);
		crgpu_command_list_draw(cmd, 3, 0);

		if (depth_bounds)
			crgpu_command_list_set_depth_bounds(cmd, 0.0f, 1.0f);

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

	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(command_allocator);
	crgpu_image_view_destroy(depth_view);
	crgpu_image_destroy(depth_image);
	crgpu_graphics_pipeline_destroy(bounds_pipeline);
	crgpu_graphics_pipeline_destroy(prime_pipeline);
	crgpu_shader_module_destroy(highlight_module);
	crgpu_shader_module_destroy(fragment_module);
	crgpu_shader_module_destroy(vertex_module);
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
