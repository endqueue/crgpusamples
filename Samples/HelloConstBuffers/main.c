//
// Carrottus GPU port of D3D12HelloWorld/HelloConstBuffers from d3d12samples.
// Same triangle as ../HelloTriangle, but the vertex shader now offsets the
// triangle by a value read from a uniform buffer bound through a real
// crgpu_descriptor_set -- see ../../README.md.
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
#include "scene_vert.shader.h"
#include "scene_frag.shader.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

typedef struct Vertex {
	float position[2];
	float color[3];
} Vertex;

// Matches shaders/scene_vert.slang's SceneConstantBuffer exactly (a single
// float4 needs no manual padding, unlike the 256-byte-aligned D3D12
// original -- that padding is a CBV root-descriptor alignment rule specific
// to D3D12, not a Vulkan/Metal requirement).
typedef struct SceneConstantBuffer {
	float offset[4];
} SceneConstantBuffer;

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
	crgpu_buffer scene_cb                     = NULL;
	crgpu_descriptor_set_layout set_layout    = NULL;
	crgpu_descriptor_pool descriptor_pool     = NULL;
	crgpu_shader_module vertex_module         = NULL;
	crgpu_shader_module fragment_module       = NULL;
	crgpu_graphics_pipeline pipeline            = NULL;
	crgpu_command_allocator command_allocator = NULL;
	crgpu_command_list cmd                    = NULL;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	window = SDL_CreateWindow("Carrottus GPU HelloConstBuffers", WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		goto cleanup;
	}

	const CR_ALLOCATOR gpu_allocator           = cr_allocator_system();
	const crgpu_instance_desc gpu_instance_desc = {
	    .base                = {CR_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "CrGPUHelloConstBuffers",
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
	    .debug_name         = "HelloConstBuffers",
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

	// Vertex buffer: one hardcoded triangle, UPLOAD memory written once --
	// same "just enough to draw a triangle" scope as HelloTriangle.
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

	// Scene constant buffer: one vec4 offset, updated every frame and kept
	// mapped for the app's lifetime -- same pattern as the D3D12 original
	// (map once, memcpy new contents in each frame, never unmap until exit).
	crgpu_buffer_desc scene_cb_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_UNIFORM,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = sizeof(SceneConstantBuffer),
	};
	if (CR_FAILED(crgpu_buffer_create(device, &scene_cb_desc, &scene_cb))) {
		fprintf(stderr, "crgpu_buffer_create (scene constant buffer) failed\n");
		goto cleanup;
	}
	SceneConstantBuffer scene_cb_data = {.offset = {0.0f, 0.0f, 0.0f, 0.0f}};
	void *scene_cb_mapped             = NULL;
	crgpu_buffer_map(scene_cb, 0, 0, &scene_cb_mapped);
	memcpy(scene_cb_mapped, &scene_cb_data, sizeof(scene_cb_data));
	crgpu_buffer_flush(scene_cb, 0, sizeof(scene_cb_data));

	// Descriptor set layout: one uniform buffer at binding 0, visible to the
	// vertex stage only (the fragment shader never reads it).
	crgpu_descriptor_set_layout_desc set_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings = {
	        {.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_VERTEX},
	    },
	    .binding_count = 1,
	};
	if (CR_FAILED(crgpu_descriptor_set_layout_create(device, &set_layout_desc, &set_layout))) {
		fprintf(stderr, "crgpu_descriptor_set_layout_create failed\n");
		goto cleanup;
	}

	crgpu_descriptor_pool_desc pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets       = 1,
	    .pool_sizes     = {{.descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1}},
	    .pool_size_count = 1,
	};
	if (CR_FAILED(crgpu_descriptor_pool_create(device, &pool_desc, &descriptor_pool))) {
		fprintf(stderr, "crgpu_descriptor_pool_create failed\n");
		goto cleanup;
	}

	crgpu_descriptor_set_desc set_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL},
	    .pool       = descriptor_pool,
	    .set_layouts = {set_layout},
	    .set_count  = 1,
	};
	crgpu_descriptor_set scene_set = NULL;
	if (CR_FAILED(crgpu_descriptor_set_create(device, &set_desc, &scene_set))) {
		fprintf(stderr, "crgpu_descriptor_set_create failed\n");
		goto cleanup;
	}

	crgpu_descriptor_buffer_info scene_cb_info = {
	    .buffer = scene_cb,
	    .offset = 0,
	    .range  = sizeof(SceneConstantBuffer),
	};
	crgpu_write_descriptor_set scene_write = {
	    .base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL},
	    .dst_set           = scene_set,
	    .dst_binding       = 0,
	    .dst_array_element = 0,
	    .descriptor_count  = 1,
	    .descriptor_type   = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	    .buffer_info       = &scene_cb_info,
	};
	crgpu_device_update_descriptor_sets(device, &scene_write, 1);

	vertex_module   = crgpusamples_create_shader_module(device, shader_format, &scene_vert_shader, CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
	fragment_module = crgpusamples_create_shader_module(device, shader_format, &scene_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
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
	    .descriptor_set_layouts     = {set_layout},
	    .descriptor_set_layout_count = 1,
	};
	if (CR_FAILED(crgpu_graphics_pipeline_create(device, &pipeline_desc, &pipeline))) {
		fprintf(stderr, "crgpu_graphics_pipeline_create failed\n");
		goto cleanup;
	}

	crgpu_command_allocator_desc allocator_desc = {.base = {CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC, NULL}, .type = CRGPU_COMMAND_LIST_TYPE_DIRECT};
	crgpu_command_allocator_create(device, &allocator_desc, &command_allocator);

	crgpu_command_list_create(command_allocator, &cmd);

	const float translation_speed = 0.005f;
	const float offset_bounds     = 1.25f;

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

		scene_cb_data.offset[0] += translation_speed;
		if (scene_cb_data.offset[0] > offset_bounds)
			scene_cb_data.offset[0] = -offset_bounds;
		memcpy(scene_cb_mapped, &scene_cb_data, sizeof(scene_cb_data));
		crgpu_buffer_flush(scene_cb, 0, sizeof(scene_cb_data));

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
		            .image_view = backbuffer_view,
		            .load_op    = CRGPU_LOAD_OP_CLEAR,
		            .store_op   = CRGPU_STORE_OP_STORE,
		            .clear_color = {0.02f, 0.02f, 0.05f, 1.0f},
		        },
            },
		    .color_attachment_count = 1,
		    .width                  = WINDOW_WIDTH,
		    .height                 = WINDOW_HEIGHT,
		};
		crgpu_command_list_begin_rendering(cmd, &rendering_desc);

		crgpu_command_list_bind_graphics_pipeline(cmd, pipeline);
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline, 0, &scene_set, 1);

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

	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(command_allocator);
	crgpu_graphics_pipeline_destroy(pipeline);
	crgpu_shader_module_destroy(fragment_module);
	crgpu_shader_module_destroy(vertex_module);
	crgpu_descriptor_pool_destroy(descriptor_pool);
	crgpu_descriptor_set_layout_destroy(set_layout);
	if (scene_cb)
		crgpu_buffer_unmap(scene_cb);
	crgpu_buffer_destroy(scene_cb);
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
