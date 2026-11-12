//
// Carrottus GPU port of D3D12SmallResources from d3d12samples, named for the
// Carrottus GPU feature it shows: heaps and the resources placed in them
// (crgpu_heap_create, crgpu_get_*_placement_requirements,
// crgpu_buffer_create_placed/image) - see ../../README.md.
//
// An 11 x 7 grid of small (32 x 32) checkerboard textures, each drawn on its
// own quad. Space switches between the two ways of making the textures:
// placed, one after another in a single heap, or committed, one device
// allocation each - the switch D3D12SmallResources makes with
// m_usePlacedResources. The window title says which, and how much heap the
// placed textures take.
//
// Deviations from the original:
// - The vertex buffer is placed too, in a heap of device memory, and the
//   staging buffer that fills it and the textures is placed in a heap of
//   upload memory where the device has one (CRGPU_FEATURE_MAPPABLE_HEAPS;
//   not a Mac without Apple silicon, where it falls back to
//   crgpu_buffer_create). The original has no buffer heaps. This way the
//   sample places every kind of resource Carrottus GPU can place.
// - The title shows the heap's size, not the adapter's memory use: Carrottus GPU
//   has no memory budget query (QueryVideoMemoryInfo,
//   VK_EXT_memory_budget, MTLDevice.currentAllocatedSize) yet. So the
//   committed mode can't say what it costs.
// - Textures are placed at the alignment the heap reports. On D3D12 that is
//   64 KiB per texture today, as committed: Carrottus GPU doesn't ask for
//   D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT (4 KiB) yet, which is the
//   original's whole point (see carrottus/CLAUDE.md). On Vulkan and Metal the
//   placed textures pack as tightly as the driver allows.
// - One upload on the graphics queue instead of a copy queue, as in
//   ../HelloTexture, and one staging buffer for all textures, filled once,
//   instead of one per texture per switch.
// - A dynamic sampler and separate SAMPLED_IMAGE + SAMPLER descriptors, for
//   the same reasons as ../HelloTexture (see its header). One descriptor set
//   per texture stands in for the original's SRV table slot per texture.
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
#include "heaps_vert.shader.h"
#include "heaps_frag.shader.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

#define GRID_WIDTH 11
#define GRID_HEIGHT 7
#define TEXTURE_COUNT (GRID_WIDTH * GRID_HEIGHT)
#define TEXTURE_WIDTH 32
#define TEXTURE_HEIGHT 32
#define TEXTURE_PIXEL_SIZE 4

// Where texture data starts in a buffer: D3D12's
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, the strictest of the three APIs.
#define TEXTURE_DATA_ALIGNMENT 512u

typedef struct Vertex {
	float position[3];
	float uv[2];
} Vertex;

// false, printed, on failure.
static bool check(CR_RESULT result, const char *what) {
	if (CR_FAILED(result)) {
		fprintf(stderr, "%s failed: %d\n", what, result);
		return false;
	}
	return true;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
	return (value + alignment - 1) / alignment * alignment;
}

// D3D12SmallResources::GenerateTexture: an 8 x 8 checkerboard of black and
// one random color. The caller seeds rand() so the colors come out the same
// every time.
static void generate_texture(uint8_t *pixels) {
	const uint32_t row_pitch    = TEXTURE_WIDTH * TEXTURE_PIXEL_SIZE;
	const uint32_t cell_pitch   = row_pitch >> 3;
	const uint32_t cell_height  = TEXTURE_WIDTH >> 3;
	const uint32_t texture_size = row_pitch * TEXTURE_HEIGHT;

	const uint8_t r = (uint8_t) (rand() & 0xff);
	const uint8_t g = (uint8_t) (rand() & 0xff);
	const uint8_t b = (uint8_t) (rand() & 0xff);
	for (uint32_t n = 0; n < texture_size; n += TEXTURE_PIXEL_SIZE) {
		const uint32_t i = (n % row_pitch) / cell_pitch;
		const uint32_t j = (n / row_pitch) / cell_height;
		const bool black = i % 2 == j % 2;
		pixels[n]        = black ? 0x00 : r;
		pixels[n + 1]    = black ? 0x00 : g;
		pixels[n + 2]    = black ? 0x00 : b;
		pixels[n + 3]    = 0xff;
	}
}

// Rounds a heap size up to what crgpu_heap_create takes.
static uint64_t heap_size_for(uint64_t bytes) {
	return align_up(bytes, CRGPU_HEAP_SIZE_GRANULARITY);
}

static const crgpu_image_desc texture_desc = {
    .base = {CR_STRUCT_TYPE_GPU_IMAGE_DESC, NULL},
    .usage        = CRGPU_IMAGE_USAGE_SAMPLED | CRGPU_IMAGE_USAGE_TRANSFER_DST,
    .type         = CRGPU_IMAGE_TYPE_2D,
    .format       = CRGPU_FORMAT_R8G8B8A8_UNORM,
    .width        = TEXTURE_WIDTH,
    .height       = TEXTURE_HEIGHT,
    .depth        = 1,
    .array_layers = 1,
    .mip_levels   = 1,
};

// The textures of one mode, and the heap they are in if they are placed.
typedef struct Textures {
	bool placed;
	crgpu_heap heap; // NULL when committed
	crgpu_placement_requirements requirements;
	uint64_t stride; // Bytes from one placed texture to the next
	crgpu_image images[TEXTURE_COUNT];
	crgpu_image_view views[TEXTURE_COUNT];
} Textures;

// D3D12SmallResources::CreateTextures, without the upload: the images, in
// one heap when [placed] and one allocation each when not. False, printed,
// on failure; what was made is left in [out] for destroy_textures.
static bool create_textures(crgpu_device device, bool placed, Textures *out) {
	memset(out, 0, sizeof(*out));
	out->placed = placed;

	if (placed) {
		// A heap is needed to ask what a texture needs from one, and its
		// size depends on the answer: ask a small heap of the same kind and
		// class first. The answer is the same for every heap of that kind
		// and class on a device.
		crgpu_heap_desc probe_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_HEAP_DESC, NULL},
		    .memory_kind    = CRGPU_MEMORY_KIND_DEVICE,
		    .resource_class = CRGPU_HEAP_RESOURCE_CLASS_IMAGES,
		    .size           = CRGPU_HEAP_SIZE_GRANULARITY,
		};
		crgpu_heap probe = NULL;
		if (!check(crgpu_heap_create(device, &probe_desc, &probe), "crgpu_heap_create (probe)"))
			return false;
		out->requirements.base = (CR_STRUCT_BASE_OUT) {CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS, NULL};
		bool probed = check(crgpu_heap_get_image_placement_requirements(probe, &texture_desc, &out->requirements),
		                    "crgpu_heap_get_image_placement_requirements");
		crgpu_heap_destroy(probe);
		if (!probed)
			return false;

		// The textures one after another, each at the next offset its
		// alignment allows.
		out->stride = align_up(out->requirements.size, out->requirements.alignment);
		crgpu_heap_desc heap_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_HEAP_DESC, NULL},
		    .memory_kind    = CRGPU_MEMORY_KIND_DEVICE,
		    .resource_class = CRGPU_HEAP_RESOURCE_CLASS_IMAGES,
		    .size           = heap_size_for(out->stride * TEXTURE_COUNT),
		};
		if (!check(crgpu_heap_create(device, &heap_desc, &out->heap), "crgpu_heap_create (textures)"))
			return false;

		for (uint32_t n = 0; n < TEXTURE_COUNT; n++)
			if (!check(crgpu_image_create_placed(out->heap, n * out->stride, &texture_desc, &out->images[n]),
			           "crgpu_image_create_placed"))
				return false;
	} else {
		for (uint32_t n = 0; n < TEXTURE_COUNT; n++)
			if (!check(crgpu_image_create(device, &texture_desc, &out->images[n]), "crgpu_image_create"))
				return false;
	}

	for (uint32_t n = 0; n < TEXTURE_COUNT; n++) {
		crgpu_image_view_desc view_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC, NULL},
		    .parent_image      = out->images[n],
		    .type              = CRGPU_IMAGE_VIEW_TYPE_2D,
		    .subresource_range = {.aspect_mask = CRGPU_IMAGE_ASPECT_COLOR, .base_mip_level = 0, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
		};
		if (!check(crgpu_image_view_create(out->images[n], &view_desc, &out->views[n]), "crgpu_image_view_create"))
			return false;
	}
	return true;
}

// Every placed image is destroyed before its heap.
static void destroy_textures(Textures *textures) {
	for (uint32_t n = 0; n < TEXTURE_COUNT; n++) {
		crgpu_image_view_destroy(textures->views[n]);
		crgpu_image_destroy(textures->images[n]);
	}
	crgpu_heap_destroy(textures->heap);
	memset(textures, 0, sizeof(*textures));
}

// Records the copy of every texture's pixels from [staging], where texture
// n starts at texture_offsets[n] with rows [row_pitch] apart.
static void record_texture_upload(
    crgpu_command_list cmd,
    const Textures *textures,
    crgpu_buffer staging,
    const uint64_t *texture_offsets,
    uint64_t row_pitch) {
	for (uint32_t n = 0; n < TEXTURE_COUNT; n++) {
		// A placed image's contents are undefined, as a new image's are:
		// UNDEFINED is the honest old layout either way.
		crgpu_command_list_image_barrier(cmd, textures->images[n], CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_TRANSFER_DST);
		crgpu_image_buffer_copy_desc copy_desc = {
		    .buffer_offset       = texture_offsets[n],
		    .buffer_row_length   = (uint32_t) (row_pitch / TEXTURE_PIXEL_SIZE),
		    .buffer_image_height = TEXTURE_HEIGHT,
		    .array_layer_count   = 1,
		    .width               = TEXTURE_WIDTH,
		    .height              = TEXTURE_HEIGHT,
		    .depth               = 1,
		};
		crgpu_command_list_copy_buffer_to_image(cmd, staging, textures->images[n], &copy_desc);
		crgpu_command_list_image_barrier(cmd, textures->images[n], CRGPU_IMAGE_LAYOUT_TRANSFER_DST, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	}
}

// Points each texture's descriptor set at its view.
static void write_texture_sets(crgpu_device device, const Textures *textures, crgpu_descriptor_set *sets, crgpu_sampler sampler) {
	for (uint32_t n = 0; n < TEXTURE_COUNT; n++) {
		crgpu_descriptor_image_info image_info = {
		    .image_view   = textures->views[n],
		    .image_layout = CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY,
		};
		crgpu_write_descriptor_set writes[2] = {
		    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = sets[n], .dst_binding = 0, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .image_info = &image_info},
		    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = sets[n], .dst_binding = 1, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER, .sampler = sampler},
		};
		crgpu_device_update_descriptor_sets(device, writes, 2);
	}
}

static void set_title(SDL_Window *window, const Textures *textures) {
	char title[256];
	if (textures->placed) {
		snprintf(title, sizeof(title),
		         "Carrottus GPU Heaps [Placed] - %u textures in one %llu KB heap (%llu B each, %llu B alignment) - Space: committed",
		         TEXTURE_COUNT,
		         (unsigned long long) (heap_size_for(textures->stride * TEXTURE_COUNT) / 1024),
		         (unsigned long long) textures->requirements.size,
		         (unsigned long long) textures->requirements.alignment);
	} else {
		snprintf(title, sizeof(title),
		         "Carrottus GPU Heaps [Committed] - %u textures, one device allocation each - Space: placed",
		         TEXTURE_COUNT);
	}
	SDL_SetWindowTitle(window, title);
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
	Textures textures                           = {0};
	crgpu_heap staging_heap                   = NULL;
	crgpu_buffer staging                      = NULL;
	crgpu_heap vertex_heap                    = NULL;
	crgpu_buffer vertex_buffer                = NULL;
	crgpu_buffer_view vertex_buffer_view      = NULL;
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

	window = SDL_CreateWindow("Carrottus GPU Heaps", WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		goto cleanup;
	}

	const CR_ALLOCATOR gpu_allocator           = cr_allocator_system();
	const crgpu_instance_desc gpu_instance_desc = {
	    .base                = {CR_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "CrGPUHeaps",
	    .application_version = 1,
	    .engine_name         = "CrGPUSamples",
	    .engine_version      = 1,
	};
	if (!check(crgpu_instance_create(&gpu_instance_desc, &instance), "crgpu_instance_create"))
		goto cleanup;

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

	// Upload heaps where the adapter has them; buffers otherwise.
	const bool mappable_heaps = (adapter_properties.supported_features & CRGPU_FEATURE_MAPPABLE_HEAPS) != 0;
	printf("Using device: %s (mappable heaps: %s)\n", adapter_properties.description, mappable_heaps ? "yes" : "no");

	// A window to present to and one direct queue, and exactly the features
	// the sample uses: a device has only what it is created with.
	const crgpu_device_desc device_desc = {
	    .base               = {CR_STRUCT_TYPE_GPU_DEVICE_DESC, NULL},
	    .required_features  = CRGPU_FEATURE_PRESENT | (adapter_properties.supported_features & CRGPU_FEATURE_MAPPABLE_HEAPS),
	    .direct_queue_count = 1,
	    // The one descriptor pool: a sampled image and a sampler per
	    // texture. The caller's to choose, and refused past the adapter's
	    // limit.
	    .max_descriptors    = TEXTURE_COUNT,
	    .max_samplers       = TEXTURE_COUNT,
	    .debug_name         = "CrGPUHeaps",
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
	    .surface = surface,
	    .queue   = graphics_queue,
	    .width   = WINDOW_WIDTH,
	    .height  = WINDOW_HEIGHT,
	    // Metal hands out one drawable at a time - see ../HelloTexture.
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
	if (!check(crgpu_swap_chain_create(device, &swap_chain_desc, &swap_chain), "crgpu_swap_chain_create"))
		goto cleanup;

	crgpu_command_allocator_desc allocator_desc = {.base = {CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC, NULL}, .type = CRGPU_COMMAND_LIST_TYPE_DIRECT};
	if (!check(crgpu_command_allocator_create(device, &allocator_desc, &command_allocator), "crgpu_command_allocator_create"))
		goto cleanup;

	if (!check(crgpu_command_list_create(command_allocator, &cmd), "crgpu_command_list_create"))
		goto cleanup;

	// ------------------------------------------------------------------
	// The quads: four vertices each, drawn as a triangle strip, laid out
	// as in the original.
	// ------------------------------------------------------------------

	const float aspect_ratio = (float) WINDOW_WIDTH / (float) WINDOW_HEIGHT;
	static Vertex quad_vertices[TEXTURE_COUNT * 4];
	{
		uint32_t index       = 0;
		const float offset_x = 0.15f;
		const float margin_x = offset_x / 10.0f;
		const float start_x  = (GRID_WIDTH / 2.0f) * -(offset_x + margin_x) + margin_x / 2.0f;
		const float offset_y = offset_x * aspect_ratio;
		const float margin_y = offset_y / 10.0f;
		float y              = (GRID_HEIGHT / 2.0f) * (offset_y + margin_y) - margin_y / 2.0f;
		for (uint32_t row = 0; row < GRID_HEIGHT; row++) {
			float x = start_x;
			for (uint32_t column = 0; column < GRID_WIDTH; column++) {
				// Carrottus GPU's clip space is +Y up on every backend, as the
				// original's is: the original's positions as they are.
				quad_vertices[index++] = (Vertex) {{x, y - offset_y, 0.0f}, {0.0f, 0.0f}};
				quad_vertices[index++] = (Vertex) {{x, y, 0.0f}, {0.0f, 1.0f}};
				quad_vertices[index++] = (Vertex) {{x + offset_x, y - offset_y, 0.0f}, {1.0f, 0.0f}};
				quad_vertices[index++] = (Vertex) {{x + offset_x, y, 0.0f}, {1.0f, 1.0f}};
				x += offset_x + margin_x;
			}
			y -= offset_y + margin_y;
		}
	}

	// ------------------------------------------------------------------
	// The textures, placed to begin with as in the original. They are made
	// before the staging buffer because its layout depends on the row pitch
	// a copy into one of them needs.
	// ------------------------------------------------------------------

	if (!create_textures(device, true, &textures))
		goto cleanup;

	uint64_t row_pitch = 0, texture_copy_size = 0;
	crgpu_image_get_buffer_copy_size(textures.images[0], 0, &row_pitch, &texture_copy_size);
	if (row_pitch == 0) row_pitch = (uint64_t) TEXTURE_WIDTH * TEXTURE_PIXEL_SIZE;
	if (texture_copy_size == 0) texture_copy_size = row_pitch * TEXTURE_HEIGHT;

	// Staging: the vertices first, then each texture at the next offset
	// texture data may start at.
	uint64_t texture_offsets[TEXTURE_COUNT];
	uint64_t staging_size = sizeof(quad_vertices);
	for (uint32_t n = 0; n < TEXTURE_COUNT; n++) {
		texture_offsets[n] = align_up(staging_size, TEXTURE_DATA_ALIGNMENT);
		staging_size       = texture_offsets[n] + texture_copy_size;
	}

	// ------------------------------------------------------------------
	// The staging buffer, placed in a heap of upload memory where the
	// device has one, and the vertex buffer, placed in a heap of device
	// memory. Each heap is asked what its buffer needs, and sized to fit.
	// ------------------------------------------------------------------

	const crgpu_buffer_desc staging_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage       = CRGPU_BUFFER_USAGE_TRANSFER_SRC,
	    .memory_kind = CRGPU_MEMORY_KIND_UPLOAD,
	    .size        = staging_size,
	};
	if (mappable_heaps) {
		crgpu_heap_desc probe_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_HEAP_DESC, NULL},
		    .memory_kind    = CRGPU_MEMORY_KIND_UPLOAD,
		    .resource_class = CRGPU_HEAP_RESOURCE_CLASS_BUFFERS,
		    .size           = CRGPU_HEAP_SIZE_GRANULARITY,
		};
		crgpu_heap probe = NULL;
		if (!check(crgpu_heap_create(device, &probe_desc, &probe), "crgpu_heap_create (upload probe)"))
			goto cleanup;
		crgpu_placement_requirements staging_requirements = {.base = {CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS, NULL}};
		bool probed = check(crgpu_heap_get_buffer_placement_requirements(probe, &staging_desc, &staging_requirements),
		                    "crgpu_heap_get_buffer_placement_requirements (staging)");
		crgpu_heap_destroy(probe);
		if (!probed)
			goto cleanup;

		crgpu_heap_desc heap_desc = probe_desc;
		heap_desc.size              = heap_size_for(staging_requirements.size);
		if (!check(crgpu_heap_create(device, &heap_desc, &staging_heap), "crgpu_heap_create (upload)"))
			goto cleanup;
		if (!check(crgpu_buffer_create_placed(staging_heap, 0, &staging_desc, &staging), "crgpu_buffer_create_placed (staging)"))
			goto cleanup;
	} else {
		if (!check(crgpu_buffer_create(device, &staging_desc, &staging), "crgpu_buffer_create (staging)"))
			goto cleanup;
	}

	{
		uint8_t *mapped = NULL;
		if (!check(crgpu_buffer_map(staging, 0, 0, (void **) &mapped), "crgpu_buffer_map"))
			goto cleanup;
		memcpy(mapped, quad_vertices, sizeof(quad_vertices));

		// Colors are random; the seed is the original's, so they are the
		// same every run.
		srand(100);
		uint8_t pixels[TEXTURE_WIDTH * TEXTURE_HEIGHT * TEXTURE_PIXEL_SIZE];
		for (uint32_t n = 0; n < TEXTURE_COUNT; n++) {
			generate_texture(pixels);
			for (uint32_t y = 0; y < TEXTURE_HEIGHT; y++)
				memcpy(mapped + texture_offsets[n] + y * row_pitch,
				       pixels + (size_t) y * TEXTURE_WIDTH * TEXTURE_PIXEL_SIZE,
				       (size_t) TEXTURE_WIDTH * TEXTURE_PIXEL_SIZE);
		}
		if (!check(crgpu_buffer_flush(staging, 0, 0), "crgpu_buffer_flush")) {
			crgpu_buffer_unmap(staging);
			goto cleanup;
		}
		crgpu_buffer_unmap(staging);
	}

	const crgpu_buffer_desc vertex_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage       = CRGPU_BUFFER_USAGE_VERTEX | CRGPU_BUFFER_USAGE_TRANSFER_DST,
	    .memory_kind = CRGPU_MEMORY_KIND_DEVICE,
	    .size        = sizeof(quad_vertices),
	};
	{
		crgpu_heap_desc heap_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_HEAP_DESC, NULL},
		    .memory_kind    = CRGPU_MEMORY_KIND_DEVICE,
		    .resource_class = CRGPU_HEAP_RESOURCE_CLASS_BUFFERS,
		    .size           = CRGPU_HEAP_SIZE_GRANULARITY,
		};
		if (!check(crgpu_heap_create(device, &heap_desc, &vertex_heap), "crgpu_heap_create (vertices)"))
			goto cleanup;
		crgpu_placement_requirements vertex_requirements = {.base = {CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS, NULL}};
		if (!check(crgpu_heap_get_buffer_placement_requirements(vertex_heap, &vertex_buffer_desc, &vertex_requirements),
		           "crgpu_heap_get_buffer_placement_requirements (vertices)"))
			goto cleanup;
		// 6 KB of vertices fits in the smallest heap there is; a caller
		// that didn't know that would size the heap from the answer.
		if (vertex_requirements.size > heap_desc.size) {
			fprintf(stderr, "vertex buffer needs %llu bytes, more than its heap\n",
			        (unsigned long long) vertex_requirements.size);
			goto cleanup;
		}
		if (!check(crgpu_buffer_create_placed(vertex_heap, 0, &vertex_buffer_desc, &vertex_buffer), "crgpu_buffer_create_placed (vertices)"))
			goto cleanup;
	}

	crgpu_buffer_view_desc vertex_buffer_view_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC, NULL},
	    .parent_buffer             = vertex_buffer,
	    .offset_from_parent_buffer = 0,
	    .stride                    = sizeof(Vertex),
	    .size                      = sizeof(quad_vertices),
	};
	if (!check(crgpu_buffer_view_create(vertex_buffer, &vertex_buffer_view_desc, &vertex_buffer_view), "crgpu_buffer_view_create"))
		goto cleanup;

	// ------------------------------------------------------------------
	// The first upload: vertices and textures.
	// ------------------------------------------------------------------

	crgpu_command_list_begin(cmd);
	{
		crgpu_buffer_barrier to_copy = {.base = {CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL}, .buffer = vertex_buffer, .state_before = CRGPU_RESOURCE_STATE_COMMON, .state_after = CRGPU_RESOURCE_STATE_COPY_DEST};
		crgpu_command_list_pipeline_barrier(cmd, &to_copy, 1, NULL, 0);
		crgpu_command_list_copy_buffer(cmd, staging, vertex_buffer, 0, 0, sizeof(quad_vertices));
		crgpu_buffer_barrier to_vertex = {.base = {CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL}, .buffer = vertex_buffer, .state_before = CRGPU_RESOURCE_STATE_COPY_DEST, .state_after = CRGPU_RESOURCE_STATE_VERTEX_BUFFER};
		crgpu_command_list_pipeline_barrier(cmd, &to_vertex, 1, NULL, 0);
	}
	record_texture_upload(cmd, &textures, staging, texture_offsets, row_pitch);
	crgpu_command_list_end(cmd);
	crgpu_command_queue_submit_lists(graphics_queue, &cmd, 1);
	crgpu_command_queue_wait_idle(graphics_queue);

	// ------------------------------------------------------------------
	// Sampler, one descriptor set per texture, pipeline.
	// ------------------------------------------------------------------

	crgpu_sampler_desc sampler_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_SAMPLER_DESC, NULL},
	    .mag_filter     = CRGPU_FILTER_LINEAR,
	    .min_filter     = CRGPU_FILTER_LINEAR,
	    .mipmap_filter  = CRGPU_FILTER_LINEAR,
	    .address_mode_u = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .address_mode_v = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .address_mode_w = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .max_lod        = 0.0f,
	    .max_anisotropy = 1.0f,
	};
	if (!check(crgpu_sampler_create(device, &sampler_desc, &sampler), "crgpu_sampler_create"))
		goto cleanup;

	crgpu_descriptor_set_layout_desc set_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings = {
	        {.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_FRAGMENT},
	        {.binding = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_FRAGMENT},
	    },
	    .binding_count = 2,
	};
	if (!check(crgpu_descriptor_set_layout_create(device, &set_layout_desc, &set_layout), "crgpu_descriptor_set_layout_create"))
		goto cleanup;

	crgpu_descriptor_pool_desc pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets   = TEXTURE_COUNT,
	    .pool_sizes = {
	        {.descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = TEXTURE_COUNT},
	        {.descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER, .count = TEXTURE_COUNT},
	    },
	    .pool_size_count = 2,
	};
	if (!check(crgpu_descriptor_pool_create(device, &pool_desc, &descriptor_pool), "crgpu_descriptor_pool_create"))
		goto cleanup;

	crgpu_descriptor_set texture_sets[TEXTURE_COUNT];
	for (uint32_t n = 0; n < TEXTURE_COUNT; n++) {
		crgpu_descriptor_set_desc set_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL},
		    .pool        = descriptor_pool,
		    .set_layouts = {set_layout},
		    .set_count   = 1,
		};
		if (!check(crgpu_descriptor_set_create(device, &set_desc, &texture_sets[n]), "crgpu_descriptor_set_create"))
			goto cleanup;
	}
	write_texture_sets(device, &textures, texture_sets, sampler);
	set_title(window, &textures);

	vertex_module   = crgpusamples_create_shader_module(device, shader_format, &heaps_vert_shader, CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
	fragment_module = crgpusamples_create_shader_module(device, shader_format, &heaps_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	if (!vertex_module || !fragment_module)
		goto cleanup;

	crgpu_graphics_pipeline_desc pipeline_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC, NULL},
	    .vertex_module     = vertex_module,
	    .fragment_module   = fragment_module,
	    .vertex_attributes = {
	        {.location = 0, .binding = 0, .format = CRGPU_FORMAT_R32G32B32_FLOAT, .offset = offsetof(Vertex, position)},
	        {.location = 1, .binding = 0, .format = CRGPU_FORMAT_R32G32_FLOAT, .offset = offsetof(Vertex, uv)},
	    },
	    .vertex_attribute_count = 2,
	    .vertex_bindings        = {
            {.binding = 0, .stride = sizeof(Vertex), .per_instance = false},
        },
	    .vertex_binding_count        = 1,
	    .primitive_topology          = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	    .cull_mode                   = CRGPU_CULL_MODE_NONE,
	    .front_face_ccw              = true,
	    .color_attachment_formats    = {CRGPU_FORMAT_B8G8R8A8_UNORM},
	    .color_attachment_count      = 1,
	    .depth_format                = CRGPU_FORMAT_UNKNOWN,
	    .descriptor_set_layouts      = {set_layout},
	    .descriptor_set_layout_count = 1,
	};
	if (!check(crgpu_graphics_pipeline_create(device, &pipeline_desc, &pipeline), "crgpu_graphics_pipeline_create"))
		goto cleanup;

	bool running = true;
	while (running) {
		if (frame_limit != 0 && frames_run == frame_limit)
			break;
		++frames_run;
		bool switch_textures = false;
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT)
				running = false;
			if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE && !event.key.repeat)
				switch_textures = true;
		}

		// D3D12SmallResources::OnKeyDown: wait for the GPU, then make the
		// textures again the other way. The old ones (and their heap) go
		// first: nothing references them once the queue is idle.
		if (switch_textures) {
			crgpu_command_queue_wait_idle(graphics_queue);
			const bool placed = !textures.placed;
			destroy_textures(&textures);
			if (!create_textures(device, placed, &textures))
				goto cleanup;

			crgpu_command_allocator_reset(command_allocator);
			crgpu_command_list_begin(cmd);
			record_texture_upload(cmd, &textures, staging, texture_offsets, row_pitch);
			crgpu_command_list_end(cmd);
			crgpu_command_queue_submit_lists(graphics_queue, &cmd, 1);
			crgpu_command_queue_wait_idle(graphics_queue);

			write_texture_sets(device, &textures, texture_sets, sampler);
			set_title(window, &textures);
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

		CRGPU_VIEWPORT viewport = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT, .minDepth = 0.0f, .maxDepth = 1.0f};
		crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);
		CRGPU_SCISSOR_RECT scissor = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
		crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);

		crgpu_command_list_set_vertex_buffers(cmd, 0, 1, &vertex_buffer_view);
		for (uint32_t n = 0; n < TEXTURE_COUNT; n++) {
			crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline, 0, &texture_sets[n], 1);
			crgpu_command_list_draw(cmd, 4, n * 4);
		}

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

	// Placed resources before the heaps they are in.
	crgpu_graphics_pipeline_destroy(pipeline);
	crgpu_shader_module_destroy(fragment_module);
	crgpu_shader_module_destroy(vertex_module);
	crgpu_descriptor_pool_destroy(descriptor_pool);
	crgpu_descriptor_set_layout_destroy(set_layout);
	crgpu_sampler_destroy(sampler);
	destroy_textures(&textures);
	crgpu_buffer_view_destroy(vertex_buffer_view);
	crgpu_buffer_destroy(vertex_buffer);
	crgpu_heap_destroy(vertex_heap);
	crgpu_buffer_destroy(staging);
	crgpu_heap_destroy(staging_heap); // NULL when the staging buffer is its own allocation
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
