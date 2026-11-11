//
// Carrottus GPU port of D3D12HelloWorld/HelloTriangle from d3d12samples.
// Same scene (one hardcoded, unindexed, unlit triangle), rebuilt entirely
// against crgpu_* calls instead of ID3D12*/DXGI -- see ../../README.md.
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
#include "triangle_vert.shader.h"
#include "triangle_frag.shader.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

typedef struct Vertex {
	float position[2];
	float color[3];
} Vertex;

int main(int argc, char **argv) {
	// --frames N: render N frames, then leave the loop as closing the window
	// does, through the same cleanup. Without it the sample runs until the
	// window is closed.
	// --device N: use the Nth adapter crgpu_adapter_enumerate lists. 0 if
	// not given.
	// --scaling one-to-one|stretch|aspect-fit: the swap chain's present
	// scaling. one-to-one if not given.
	// --window WxH: make the window W by H, while the back buffers stay
	// WINDOW_WIDTH by WINDOW_HEIGHT, so that the compositor shows them at
	// another size and the scaling can be seen. The window's size if not
	// given.
	uint64_t frame_limit                    = 0;
	uint32_t device_index                   = 0;
	CRGPU_PRESENT_SCALING present_scaling = CRGPU_PRESENT_SCALING_ONE_TO_ONE;
	int window_width                        = WINDOW_WIDTH;
	int window_height                       = WINDOW_HEIGHT;
	for (int i = 1; i + 1 < argc; ++i) {
		if (strcmp(argv[i], "--frames") == 0)
			frame_limit = strtoull(argv[i + 1], NULL, 10);
		else if (strcmp(argv[i], "--device") == 0)
			device_index = (uint32_t)strtoul(argv[i + 1], NULL, 10);
		else if (strcmp(argv[i], "--scaling") == 0) {
			if (strcmp(argv[i + 1], "one-to-one") == 0)
				present_scaling = CRGPU_PRESENT_SCALING_ONE_TO_ONE;
			else if (strcmp(argv[i + 1], "stretch") == 0)
				present_scaling = CRGPU_PRESENT_SCALING_STRETCH;
			else if (strcmp(argv[i + 1], "aspect-fit") == 0)
				present_scaling = CRGPU_PRESENT_SCALING_ASPECT_FIT;
			else {
				fprintf(stderr, "--scaling %s: one-to-one, stretch or aspect-fit\n", argv[i + 1]);
				return 1;
			}
		} else if (strcmp(argv[i], "--window") == 0) {
			if (sscanf(argv[i + 1], "%dx%d", &window_width, &window_height) != 2 || window_width <= 0 || window_height <= 0) {
				fprintf(stderr, "--window %s: WxH, both above 0\n", argv[i + 1]);
				return 1;
			}
		}
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
	crgpu_graphics_pipeline pipeline            = NULL;
	crgpu_command_allocator command_allocator = NULL;
	crgpu_command_list cmd                    = NULL;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	window = SDL_CreateWindow("Carrottus GPU HelloTriangle", window_width, window_height, 0);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		goto cleanup;
	}

	const CR_ALLOCATOR gpu_allocator           = cr_allocator_system();
	const crgpu_instance_desc gpu_instance_desc = {
	    .base                = {CR_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "CrGPUHelloTriangle",
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
	    .debug_name         = "HelloTriangle",
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

	// Which scalings the surface takes, and the one asked for among them.
	uint32_t surface_scalings = 0;
	if (CR_FAILED(crgpu_surface_get_present_scalings(device, surface, &surface_scalings))) {
		fprintf(stderr, "crgpu_surface_get_present_scalings failed\n");
		goto cleanup;
	}
	printf("Present scalings the surface takes:%s%s%s\n",
	       (surface_scalings & CRGPU_PRESENT_SCALING_ONE_TO_ONE) ? " one-to-one" : "",
	       (surface_scalings & CRGPU_PRESENT_SCALING_STRETCH) ? " stretch" : "",
	       (surface_scalings & CRGPU_PRESENT_SCALING_ASPECT_FIT) ? " aspect-fit" : "");
	if (!(surface_scalings & present_scaling)) {
		fprintf(stderr, "The surface doesn't take the --scaling asked for\n");
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
	    // The window can't be resized, and without --window the back
	    // buffers are its size, so nothing is scaled; ONE_TO_ONE, the
	    // default here, is the scaling every surface takes.
	    .present_scaling = present_scaling,
	    .fullscreen_mode = CRGPU_FULLSCREEN_MODE_WINDOWED,
	};
	if (CR_FAILED(crgpu_swap_chain_create(device, &swap_chain_desc, &swap_chain))) {
		fprintf(stderr, "crgpu_swap_chain_create failed\n");
		goto cleanup;
	}
	// The back buffers' real size, which is what is drawn: on Vulkan a
	// surface without a scaling for the swap chain gives its own size.
	crgpu_swap_chain_desc created_desc = {.base = {CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC, NULL}};
	if (CR_FAILED(crgpu_swap_chain_get_desc(swap_chain, &created_desc))) {
		fprintf(stderr, "crgpu_swap_chain_get_desc failed\n");
		goto cleanup;
	}
	const uint32_t back_buffer_width  = created_desc.width;
	const uint32_t back_buffer_height = created_desc.height;
	printf("Back buffers: %ux%u, window %dx%d\n", back_buffer_width, back_buffer_height, window_width, window_height);

	// Vertex buffer: one hardcoded triangle, UPLOAD memory written once --
	// same "just enough to draw a triangle" scope as the D3D12 sample.
	// Carrottus GPU's clip space is +Y up on every backend: the apex is at the top.
	static const Vertex vertices[3] = {
	    {{0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}},
	    {{0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
	    {{-0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
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

	vertex_module   = crgpusamples_create_shader_module(device, shader_format, &triangle_vert_shader, CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
	fragment_module = crgpusamples_create_shader_module(device, shader_format, &triangle_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	if (!vertex_module || !fragment_module)
		goto cleanup;

	crgpu_graphics_pipeline_desc pipeline_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC, NULL},
	    .vertex_module   = vertex_module,
	    .fragment_module = fragment_module,
	    .vertex_attributes = {
	        {.location = 0, .binding = 0, .format = CRGPU_FORMAT_R32G32_FLOAT, .offset = offsetof(Vertex, position)},
	        {.location = 1, .binding = 0, .format = CRGPU_FORMAT_R32G32B32_FLOAT, .offset = offsetof(Vertex, color)},
	    },
	    .vertex_attribute_count = 2,
	    .vertex_bindings        = {
	        {.binding = 0, .stride = sizeof(Vertex), .per_instance = false},
        },
	    .vertex_binding_count   = 1,
	    .primitive_topology     = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	    .cull_mode              = CRGPU_CULL_MODE_NONE,
	    .front_face_ccw         = true,
	    .color_attachment_formats = {CRGPU_FORMAT_B8G8R8A8_UNORM},
	    .color_attachment_count  = 1,
	    .depth_format            = CRGPU_FORMAT_UNKNOWN,
	};
	if (CR_FAILED(crgpu_graphics_pipeline_create(device, &pipeline_desc, &pipeline))) {
		fprintf(stderr, "crgpu_graphics_pipeline_create failed\n");
		goto cleanup;
	}

	crgpu_command_allocator_desc allocator_desc = {.base = {CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC, NULL}, .type = CRGPU_COMMAND_LIST_TYPE_DIRECT};
	crgpu_command_allocator_create(device, &allocator_desc, &command_allocator);

	crgpu_command_list_create(command_allocator, &cmd);

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
		crgpu_image_view backbuffer_view  = crgpu_swap_chain_get_image_view(swap_chain, image_index);

		crgpu_command_allocator_reset(command_allocator);
		crgpu_command_list_begin(cmd);

		crgpu_command_list_image_barrier_view(cmd, backbuffer_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		crgpu_rendering_begin_desc rendering_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC, NULL},
		    .color_attachments = {
		        {
		            .image_view = backbuffer_view,
		            .load_op    = CRGPU_LOAD_OP_CLEAR,
		            .store_op   = CRGPU_STORE_OP_STORE,
		            .clear_color = {0.02f, 0.02f, 0.05f, 1.0f},
		        },
            },
		    .color_attachment_count = 1,
		    .width                  = back_buffer_width,
		    .height                 = back_buffer_height,
		};
		crgpu_command_list_begin_rendering(cmd, &rendering_desc);

		crgpu_command_list_bind_graphics_pipeline(cmd, pipeline);

		CRGPU_VIEWPORT viewport = {.x = 0, .y = 0, .width = (float)back_buffer_width, .height = (float)back_buffer_height, .minDepth = 0.0f, .maxDepth = 1.0f};
		crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);

		CRGPU_SCISSOR_RECT scissor = {.x = 0, .y = 0, .width = (float)back_buffer_width, .height = (float)back_buffer_height};
		crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);

		crgpu_command_list_set_vertex_buffers(cmd, 0, 1, &vertex_buffer_view);
		crgpu_command_list_draw(cmd, 3, 0);

		crgpu_command_list_end_rendering(cmd);
		crgpu_command_list_image_barrier_view(cmd, backbuffer_view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_PRESENT_SRC);

		crgpu_command_list_end(cmd);

		crgpu_command_queue_submit_lists_synced(graphics_queue, &cmd, 1, swap_chain);
		crgpu_swap_chain_present(swap_chain);
	}

	exit_code = 0;

cleanup:
	if (graphics_queue)
		crgpu_command_queue_wait_idle(graphics_queue);

	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(command_allocator);
	crgpu_graphics_pipeline_destroy(pipeline);
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
