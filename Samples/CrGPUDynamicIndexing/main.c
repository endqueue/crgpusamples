//
// Carrottus GPU port of D3D12DynamicIndexing from d3d12samples. A 15x8 grid of
// identical "occcity" building meshes; each building's pixel shader
// dynamically indexes into one shared unbounded texture table (Carrottus GPU's
// bindless sampled-image feature) to pick its own procedurally-generated
// material color, blended against a shared diffuse texture -- see
// ../../README.md for the two things this port does differently from the
// original and why.
//
// occcity.bin is copied verbatim from
// d3d12samples/Samples/Desktop/D3D12DynamicIndexing/src/ -- the fixed byte
// offsets/sizes below (VERTEX_DATA_OFFSET etc.) come directly from that
// sample's occcity.h.
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
#include "scene_vert.shader.h"
#include "scene_frag.shader.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

#define CITY_ROW_COUNT 15
#define CITY_COLUMN_COUNT 8
#define CITY_MATERIAL_COUNT (CITY_ROW_COUNT * CITY_COLUMN_COUNT) // 120
#define CITY_MATERIAL_TEXTURE_WIDTH 64
#define CITY_MATERIAL_TEXTURE_HEIGHT 64
#define CITY_SPACING_INTERVAL 16.0f

// occcity.bin layout, from d3d12samples' occcity.h (SampleAssets namespace).
#define VERTEX_DATA_OFFSET 524288u
#define VERTEX_DATA_SIZE 820248u
#define INDEX_DATA_OFFSET 1344536u
#define INDEX_DATA_SIZE 74568u
#define VERTEX_STRIDE 44u
#define DIFFUSE_BC1_OFFSET 0u
#define DIFFUSE_BC1_ROW_PITCH 2048u
#define DIFFUSE_TEXTURE_WIDTH 1024u
#define DIFFUSE_TEXTURE_HEIGHT 1024u

// Vulkan-aligned aspect bits (COLOR=1, DEPTH=2, STENCIL=4) -- crgpu.h's
// crgpu_image_view_desc_subresource_range::aspect_mask has no named
// constants yet; the Vulkan backend passes this straight through to
// VkImageAspectFlags (see crgpuvulkanimage.c), and D3D12/Metal ignore it
// entirely (they infer DSV/RTV/SRV purely from the image's usage bits).
#define CRGPU_ASPECT_DEPTH_BIT 0x00000002u

typedef struct Vertex {
	float position[3];
	float normal[3]; // Unused by these shaders (no lighting), kept only so
	                  // the vertex stride/offsets match occcity.bin's layout.
	float uv[2];
	float tangent[3]; // Unused.
} Vertex;

typedef struct PushConstants {
	uint32_t diffuse_index;
	uint32_t material_index;
} PushConstants;

// ---------------------------------------------------------------------------
// Math (column-major, right-handed -- matches SimpleCamera's
// XMMatrixLookToRH/PerspectiveFovRH exactly, unlike CrGPUExecuteIndirect's
// left-handed projection).
// ---------------------------------------------------------------------------

static void vec3_cross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float vec3_dot(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void vec3_normalize(float v[3]) {
	float len = sqrtf(vec3_dot(v, v));
	if (len > 1e-6f) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

static void mat4_multiply(const float a[16], const float b[16], float out[16]) {
	float result[16];
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += a[k * 4 + row] * b[col * 4 + k];
			result[col * 4 + row] = sum;
		}
	}
	memcpy(out, result, sizeof(result));
}

static void mat4_translation(float x, float y, float z, float out[16]) {
	memset(out, 0, sizeof(float) * 16);
	out[0] = out[5] = out[10] = out[15] = 1.0f;
	out[12] = x;
	out[13] = y;
	out[14] = z;
}

static void mat4_look_to_rh(const float eye[3], const float dir[3], const float up[3], float out[16]) {
	float zaxis[3] = {-dir[0], -dir[1], -dir[2]};
	vec3_normalize(zaxis);
	float xaxis[3];
	vec3_cross(up, zaxis, xaxis);
	vec3_normalize(xaxis);
	float yaxis[3];
	vec3_cross(zaxis, xaxis, yaxis);

	out[0] = xaxis[0];
	out[1] = yaxis[0];
	out[2] = zaxis[0];
	out[3] = 0.0f;
	out[4] = xaxis[1];
	out[5] = yaxis[1];
	out[6] = zaxis[1];
	out[7] = 0.0f;
	out[8] = xaxis[2];
	out[9] = yaxis[2];
	out[10] = zaxis[2];
	out[11] = 0.0f;
	out[12] = -vec3_dot(xaxis, eye);
	out[13] = -vec3_dot(yaxis, eye);
	out[14] = -vec3_dot(zaxis, eye);
	out[15] = 1.0f;
}

static void mat4_perspective_rh(float fov_y_radians, float aspect, float z_near, float z_far, float out[16]) {
	float f = 1.0f / tanf(fov_y_radians * 0.5f);
	memset(out, 0, sizeof(float) * 16);
	out[0] = f / aspect;
	// Not negated: Carrottus GPU's clip space is +Y up on every backend.
	out[5] = f;
	out[10] = z_far / (z_near - z_far);
	out[11] = -1.0f;
	out[14] = (z_near * z_far) / (z_near - z_far);
}

// ---------------------------------------------------------------------------
// BC1 (DXT1) -> RGBA8 CPU decode. See ../../README.md for why this port
// decodes on load instead of uploading the compressed texture directly.
// ---------------------------------------------------------------------------

static void bc1_unpack_565(uint16_t c, uint8_t rgb[3]) {
	rgb[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
	rgb[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
	rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31);
}

static void bc1_decode_block(const uint8_t *block, uint32_t x0, uint32_t y0, uint32_t width, uint32_t height, uint8_t *out_rgba) {
	uint16_t c0 = (uint16_t)(block[0] | (block[1] << 8));
	uint16_t c1 = (uint16_t)(block[2] | (block[3] << 8));
	// Widened before shifting: a byte promotes to int, and 255 << 24 overflows it.
	uint32_t indices = (uint32_t)block[4] | ((uint32_t)block[5] << 8) | ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);

	uint8_t colors[4][3];
	bc1_unpack_565(c0, colors[0]);
	bc1_unpack_565(c1, colors[1]);
	if (c0 > c1) {
		for (int i = 0; i < 3; i++) {
			colors[2][i] = (uint8_t)((2 * colors[0][i] + colors[1][i]) / 3);
			colors[3][i] = (uint8_t)((colors[0][i] + 2 * colors[1][i]) / 3);
		}
	} else {
		for (int i = 0; i < 3; i++) {
			colors[2][i] = (uint8_t)((colors[0][i] + colors[1][i]) / 2);
			colors[3][i] = 0;
		}
	}

	for (uint32_t py = 0; py < 4; py++) {
		uint32_t y = y0 + py;
		if (y >= height) continue;
		for (uint32_t px = 0; px < 4; px++) {
			uint32_t x = x0 + px;
			if (x >= width) continue;
			uint32_t idx = (indices >> (2 * (py * 4 + px))) & 0x3u;
			uint8_t *dst = out_rgba + (size_t) (y * width + x) * 4;
			dst[0] = colors[idx][0];
			dst[1] = colors[idx][1];
			dst[2] = colors[idx][2];
			dst[3] = 255;
		}
	}
}

static void bc1_decode(const uint8_t *src, uint32_t width, uint32_t height, uint32_t src_row_pitch, uint8_t *out_rgba) {
	for (uint32_t by = 0; by * 4 < height; by++) {
		for (uint32_t bx = 0; bx * 4 < width; bx++) {
			const uint8_t *block = src + (size_t) by * src_row_pitch + (size_t) bx * 8;
			bc1_decode_block(block, bx * 4, by * 4, width, height, out_rgba);
		}
	}
}

// ---------------------------------------------------------------------------
// HSL -> RGB, for the procedurally-generated material textures (same
// rainbow-gradient technique as the original sample).
// ---------------------------------------------------------------------------

static float hsl_hue_to_rgb(float p, float q, float t) {
	if (t < 0.0f) t += 1.0f;
	if (t > 1.0f) t -= 1.0f;
	if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
	if (t < 1.0f / 2.0f) return q;
	if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
	return p;
}

static void hsl_to_rgb8(float h, float s, float l, uint8_t out[3]) {
	float r, g, b;
	if (s == 0.0f) {
		r = g = b = l;
	} else {
		float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
		float p = 2.0f * l - q;
		r = hsl_hue_to_rgb(p, q, h + 1.0f / 3.0f);
		g = hsl_hue_to_rgb(p, q, h);
		b = hsl_hue_to_rgb(p, q, h - 1.0f / 3.0f);
	}
	out[0] = (uint8_t)(r * 255.0f);
	out[1] = (uint8_t)(g * 255.0f);
	out[2] = (uint8_t)(b * 255.0f);
}

typedef struct Camera {
	float position[3];
	float yaw, pitch;
	float look_dir[3];
	float move_speed;
	float turn_speed;
	bool key_w, key_a, key_s, key_d;
	bool key_left, key_right, key_up, key_down;
} Camera;

static void camera_init(Camera *cam, float x, float y, float z, float move_speed) {
	memset(cam, 0, sizeof(*cam));
	cam->position[0] = x;
	cam->position[1] = y;
	cam->position[2] = z;
	cam->yaw          = (float) M_PI;
	cam->pitch        = 0.0f;
	cam->look_dir[2]  = -1.0f;
	cam->move_speed   = move_speed;
	cam->turn_speed   = (float) M_PI / 2.0f;
}

static void camera_update(Camera *cam, float dt) {
	float move_x = 0.0f, move_z = 0.0f;
	if (cam->key_a) move_x -= 1.0f;
	if (cam->key_d) move_x += 1.0f;
	if (cam->key_w) move_z -= 1.0f;
	if (cam->key_s) move_z += 1.0f;
	if (fabsf(move_x) > 0.1f && fabsf(move_z) > 0.1f) {
		float inv_len = 1.0f / sqrtf(move_x * move_x + move_z * move_z);
		move_x *= inv_len;
		move_z *= inv_len;
	}

	float move_interval   = cam->move_speed * dt;
	float rotate_interval = cam->turn_speed * dt;

	if (cam->key_left) cam->yaw += rotate_interval;
	if (cam->key_right) cam->yaw -= rotate_interval;
	if (cam->key_up) cam->pitch += rotate_interval;
	if (cam->key_down) cam->pitch -= rotate_interval;

	const float pitch_limit = (float) M_PI / 4.0f;
	if (cam->pitch > pitch_limit) cam->pitch = pitch_limit;
	if (cam->pitch < -pitch_limit) cam->pitch = -pitch_limit;

	float x = move_x * -cosf(cam->yaw) - move_z * sinf(cam->yaw);
	float z = move_x * sinf(cam->yaw) - move_z * cosf(cam->yaw);
	cam->position[0] += x * move_interval;
	cam->position[2] += z * move_interval;

	float r = cosf(cam->pitch);
	cam->look_dir[0] = r * sinf(cam->yaw);
	cam->look_dir[1] = sinf(cam->pitch);
	cam->look_dir[2] = r * cosf(cam->yaw);
}


// NULL, printed, if the file can't be read.
static void *read_entire_file(const char *path, size_t *out_size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "Failed to open file: %s\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buffer = malloc((size_t) size);
	if (!buffer) {
		fprintf(stderr, "No memory for file: %s\n", path);
		fclose(f);
		return NULL;
	}
	fread(buffer, 1, (size_t) size, f);
	fclose(f);
	*out_size = (size_t) size;
	return buffer;
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
	// upload command list to complete, same "keep alive past Close(), free
	// after WaitForGpu" pattern the original sample uses for its upload
	// heaps.
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
	int exit_code                                           = 1;
	SDL_Window *window                                      = NULL;
	crgpu_instance instance                               = NULL;
	crgpu_device device                                   = NULL;
	crgpu_command_queue graphics_queue                    = NULL;
	crgpu_surface surface                                 = NULL;
	crgpu_swap_chain swap_chain                           = NULL;
	crgpu_command_allocator command_allocator             = NULL;
	crgpu_command_list cmd                                = NULL;
	crgpu_buffer vertex_buffer                            = NULL;
	crgpu_buffer_view vertex_buffer_view                  = NULL;
	crgpu_buffer index_buffer                             = NULL;
	crgpu_buffer_view index_buffer_view                   = NULL;
	crgpu_buffer staging_buffers[1 + CITY_MATERIAL_COUNT] = {0};
	uint32_t staging_buffer_count                           = 0;
	crgpu_image diffuse_image                             = NULL;
	crgpu_image_view diffuse_view                         = NULL;
	uint32_t diffuse_bindless_index                         = CRGPU_BINDLESS_INVALID_INDEX;
	crgpu_image material_images[CITY_MATERIAL_COUNT]      = {0};
	crgpu_image_view material_views[CITY_MATERIAL_COUNT]  = {0};
	uint32_t material_bindless_index[CITY_MATERIAL_COUNT]   = {0};
	crgpu_sampler sampler                                 = NULL;
	crgpu_descriptor_set_layout sampler_layout            = NULL;
	crgpu_descriptor_pool sampler_pool                    = NULL;
	crgpu_descriptor_set_layout cbv_layout                = NULL;
	crgpu_descriptor_pool cbv_pool                        = NULL;
	crgpu_buffer cbv_buffer                               = NULL;
	crgpu_shader_module vertex_module                     = NULL;
	crgpu_shader_module fragment_module                   = NULL;
	crgpu_graphics_pipeline pipeline                        = NULL;
	crgpu_image depth_image                               = NULL;
	crgpu_image_view depth_view                           = NULL;
	// No slot until registered: 0 is a real one, and cleanup unregisters each.
	for (uint32_t i = 0; i < CITY_MATERIAL_COUNT; i++)
		material_bindless_index[i] = CRGPU_BINDLESS_INVALID_INDEX;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	window = SDL_CreateWindow("Carrottus GPU DynamicIndexing", WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		goto cleanup;
	}

	const CR_ALLOCATOR gpu_allocator           = cr_allocator_system();
	const crgpu_instance_desc gpu_instance_desc = {
	    .base                = {CR_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "CrGPUDynamicIndexing",
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

	// The bindless table's size comes chained onto the adapter's properties.
	CRGPU_BINDLESS_LIMITS bindless_limits       = {.base = {CR_STRUCT_TYPE_GPU_BINDLESS_LIMITS, NULL}};
	CRGPU_ADAPTER_PROPERTIES adapter_properties = {.base = {CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES, &bindless_limits.base}};
	crgpu_adapter_get_properties(adapter, &adapter_properties);
	// The one shader format the adapter takes: each module below is made
	// from the embedded code in it.
	const CRGPU_SHADER_FORMAT shader_format = crgpusamples_shader_format(adapter_properties.shader_formats);
	printf("Using device: %s\n", adapter_properties.description);

	if (!(adapter_properties.supported_features & CRGPU_FEATURE_BINDLESS)) {
		fprintf(stderr, "This device/driver doesn't support bindless descriptor indexing\n");
		goto cleanup;
	}
	// The bindless table comes out of the descriptor budget: its three
	// classes' slots, and the sample's own pools beside it.
	const uint32_t bindless_table =
	    bindless_limits.max_sampled_images + bindless_limits.max_storage_images + bindless_limits.max_storage_buffers;

	// A window to present to and one direct queue, and exactly the features
	// the sample uses: a device has only what it is created with.
	const crgpu_device_desc device_desc = {
	    .base               = {CR_STRUCT_TYPE_GPU_DEVICE_DESC, NULL},
	    .required_features  = CRGPU_FEATURE_PRESENT | CRGPU_FEATURE_BINDLESS,
	    .direct_queue_count = 1,
	    .max_descriptors    = bindless_table + 1024,
	    .max_samplers       = 64,
	    .debug_name         = "CrGPUDynamicIndexing",
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
	    .buffer_count    = 2,
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

	// ------------------------------------------------------------------
	// Mesh (occcity.bin).
	// ------------------------------------------------------------------

	size_t mesh_data_size;
	uint8_t *mesh_data = (uint8_t *) read_entire_file("occcity.bin", &mesh_data_size);
	if (!mesh_data)
		goto cleanup;

	crgpu_buffer_desc vertex_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_VERTEX,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = VERTEX_DATA_SIZE,
	};
	crgpu_buffer_create(device, &vertex_buffer_desc, &vertex_buffer);
	void *vertex_mapped = NULL;
	crgpu_buffer_map(vertex_buffer, 0, 0, &vertex_mapped);
	memcpy(vertex_mapped, mesh_data + VERTEX_DATA_OFFSET, VERTEX_DATA_SIZE);
	crgpu_buffer_flush(vertex_buffer, 0, 0);
	crgpu_buffer_unmap(vertex_buffer);

	crgpu_buffer_view_desc vertex_view_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC, NULL},
	    .parent_buffer = vertex_buffer, .offset_from_parent_buffer = 0, .stride = VERTEX_STRIDE, .size = VERTEX_DATA_SIZE};
	crgpu_buffer_view_create(vertex_buffer, &vertex_view_desc, &vertex_buffer_view);

	crgpu_buffer_desc index_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_INDEX,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = INDEX_DATA_SIZE,
	};
	crgpu_buffer_create(device, &index_buffer_desc, &index_buffer);
	void *index_mapped = NULL;
	crgpu_buffer_map(index_buffer, 0, 0, &index_mapped);
	memcpy(index_mapped, mesh_data + INDEX_DATA_OFFSET, INDEX_DATA_SIZE);
	crgpu_buffer_flush(index_buffer, 0, 0);
	crgpu_buffer_unmap(index_buffer);

	crgpu_buffer_view_desc index_view_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC, NULL},
	    .parent_buffer = index_buffer, .offset_from_parent_buffer = 0, .stride = 4, .size = INDEX_DATA_SIZE};
	crgpu_buffer_view_create(index_buffer, &index_view_desc, &index_buffer_view);

	uint32_t index_count = INDEX_DATA_SIZE / 4;

	// ------------------------------------------------------------------
	// Textures + sampler. Uploads happen on `cmd`, submitted once and
	// waited on below before the render loop starts.
	// ------------------------------------------------------------------

	crgpu_command_list_begin(cmd);

	// One staging buffer per uploaded texture (1 diffuse + CITY_MATERIAL_COUNT
	// materials), kept alive until the upload command list below is waited on.
	uint8_t *diffuse_rgba = (uint8_t *) malloc((size_t) DIFFUSE_TEXTURE_WIDTH * DIFFUSE_TEXTURE_HEIGHT * 4);
	bc1_decode(mesh_data + DIFFUSE_BC1_OFFSET, DIFFUSE_TEXTURE_WIDTH, DIFFUSE_TEXTURE_HEIGHT, DIFFUSE_BC1_ROW_PITCH, diffuse_rgba);
	diffuse_image = upload_rgba8_texture(
	    device, cmd, diffuse_rgba, DIFFUSE_TEXTURE_WIDTH, DIFFUSE_TEXTURE_HEIGHT, &staging_buffers[staging_buffer_count++]);
	free(diffuse_rgba);
	free(mesh_data);
	if (!diffuse_image)
		goto cleanup;

	crgpu_image_view_desc diffuse_view_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC, NULL},
	    .parent_image = diffuse_image,
	    .type         = CRGPU_IMAGE_VIEW_TYPE_2D,
	    .subresource_range = {.aspect_mask = 1 /* COLOR */, .base_mip_level = 0, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
	};
	crgpu_image_view_create(diffuse_image, &diffuse_view_desc, &diffuse_view);

	crgpu_bindless_register_sampled_image(device, diffuse_view, &diffuse_bindless_index);

	uint8_t *material_rgba = (uint8_t *) malloc((size_t) CITY_MATERIAL_TEXTURE_WIDTH * CITY_MATERIAL_TEXTURE_HEIGHT * 4);
	float grad_step = 1.0f / (float) CITY_MATERIAL_COUNT;
	for (uint32_t i = 0; i < CITY_MATERIAL_COUNT; i++) {
		float t = (float) i * grad_step;
		for (uint32_t y = 0; y < CITY_MATERIAL_TEXTURE_HEIGHT; y++) {
			float t_row = t + ((float) y / (float) CITY_MATERIAL_TEXTURE_HEIGHT) * grad_step;
			uint8_t rgb[3];
			hsl_to_rgb8(t_row, 0.5f, 0.5f, rgb);
			for (uint32_t x = 0; x < CITY_MATERIAL_TEXTURE_WIDTH; x++) {
				uint8_t *dst = material_rgba + (size_t) (y * CITY_MATERIAL_TEXTURE_WIDTH + x) * 4;
				dst[0] = rgb[0];
				dst[1] = rgb[1];
				dst[2] = rgb[2];
				dst[3] = 255;
			}
		}

		material_images[i] = upload_rgba8_texture(
		    device, cmd, material_rgba, CITY_MATERIAL_TEXTURE_WIDTH, CITY_MATERIAL_TEXTURE_HEIGHT, &staging_buffers[staging_buffer_count++]);
		if (!material_images[i]) {
			free(material_rgba);
			goto cleanup;
		}

		crgpu_image_view_desc mat_view_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC, NULL},
		    .parent_image = material_images[i],
		    .type         = CRGPU_IMAGE_VIEW_TYPE_2D,
		    .subresource_range = {.aspect_mask = 1, .base_mip_level = 0, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
		};
		crgpu_image_view_create(material_images[i], &mat_view_desc, &material_views[i]);
		crgpu_bindless_register_sampled_image(device, material_views[i], &material_bindless_index[i]);
	}
	free(material_rgba);

	crgpu_command_list_end(cmd);
	crgpu_command_queue_submit_lists(graphics_queue, &cmd, 1);
	crgpu_command_queue_wait_idle(graphics_queue);
	for (uint32_t i = 0; i < staging_buffer_count; i++)
		crgpu_buffer_destroy(staging_buffers[i]);
	staging_buffer_count = 0;

	crgpu_sampler_desc sampler_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_SAMPLER_DESC, NULL},
	    .mag_filter      = CRGPU_FILTER_LINEAR,
	    .min_filter      = CRGPU_FILTER_LINEAR,
	    .mipmap_filter   = CRGPU_FILTER_LINEAR,
	    .address_mode_u  = CRGPU_ADDRESS_MODE_REPEAT,
	    .address_mode_v  = CRGPU_ADDRESS_MODE_REPEAT,
	    .address_mode_w  = CRGPU_ADDRESS_MODE_REPEAT,
	    .max_lod         = 1000.0f,
	    .max_anisotropy  = 1.0f,
	};
	if (CR_FAILED(crgpu_sampler_create(device, &sampler_desc, &sampler))) {
		fprintf(stderr, "crgpu_sampler_create failed\n");
		goto cleanup;
	}

	// ------------------------------------------------------------------
	// Descriptor sets: 0 = bindless (textures), 1 = sampler, 2 = per-building CBV.
	// ------------------------------------------------------------------

	crgpu_descriptor_set_layout bindless_layout = crgpu_device_get_bindless_descriptor_set_layout(device);

	crgpu_descriptor_set_layout_desc sampler_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings      = {{.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_FRAGMENT}},
	    .binding_count = 1,
	};
	crgpu_descriptor_set_layout_create(device, &sampler_layout_desc, &sampler_layout);

	crgpu_descriptor_pool_desc sampler_pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets = 1, .pool_sizes = {{.descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER, .count = 1}}, .pool_size_count = 1};
	crgpu_descriptor_pool_create(device, &sampler_pool_desc, &sampler_pool);

	crgpu_descriptor_set_desc sampler_set_desc = {.base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL}, .pool = sampler_pool, .set_layouts = {sampler_layout}, .set_count = 1};
	crgpu_descriptor_set sampler_set = NULL;
	crgpu_descriptor_set_create(device, &sampler_set_desc, &sampler_set);

	crgpu_write_descriptor_set sampler_write = {
	    .base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL},
	    .dst_set = sampler_set, .dst_binding = 0, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER, .sampler = sampler};
	crgpu_device_update_descriptor_sets(device, &sampler_write, 1);

	crgpu_descriptor_set_layout_desc cbv_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings      = {{.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_VERTEX}},
	    .binding_count = 1,
	};
	crgpu_descriptor_set_layout_create(device, &cbv_layout_desc, &cbv_layout);

	crgpu_descriptor_pool_desc cbv_pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets = CITY_MATERIAL_COUNT, .pool_sizes = {{.descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = CITY_MATERIAL_COUNT}}, .pool_size_count = 1};
	crgpu_descriptor_pool_create(device, &cbv_pool_desc, &cbv_pool);

	const uint32_t cbv_stride = 256; // D3D12 CBV alignment; harmless elsewhere.
	crgpu_buffer_desc cbv_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_UNIFORM,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = (uint64_t) cbv_stride * CITY_MATERIAL_COUNT,
	};
	crgpu_buffer_create(device, &cbv_buffer_desc, &cbv_buffer);
	void *cbv_mapped = NULL;
	crgpu_buffer_map(cbv_buffer, 0, 0, &cbv_mapped);

	crgpu_descriptor_set cbv_sets[CITY_MATERIAL_COUNT];
	float model_matrices[CITY_MATERIAL_COUNT][16];
	for (uint32_t row = 0; row < CITY_ROW_COUNT; row++) {
		for (uint32_t col = 0; col < CITY_COLUMN_COUNT; col++) {
			uint32_t i = row * CITY_COLUMN_COUNT + col;

			mat4_translation(col * CITY_SPACING_INTERVAL, 0.02f * (float) i, -(float) row * CITY_SPACING_INTERVAL, model_matrices[i]);

			crgpu_descriptor_set_desc set_desc = {.base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL}, .pool = cbv_pool, .set_layouts = {cbv_layout}, .set_count = 1};
			crgpu_descriptor_set_create(device, &set_desc, &cbv_sets[i]);

			crgpu_descriptor_buffer_info buf_info = {.buffer = cbv_buffer, .offset = (uint64_t) i * cbv_stride, .range = sizeof(float) * 16};
			crgpu_write_descriptor_set write = {
			    .base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL},
			    .dst_set = cbv_sets[i], .dst_binding = 0, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .buffer_info = &buf_info};
			crgpu_device_update_descriptor_sets(device, &write, 1);
		}
	}

	// ------------------------------------------------------------------
	// Pipeline + depth buffer.
	// ------------------------------------------------------------------

	vertex_module   = crgpusamples_create_shader_module(device, shader_format, &scene_vert_shader, CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
	fragment_module = crgpusamples_create_shader_module(device, shader_format, &scene_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
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
	    .vertex_bindings        = {{.binding = 0, .stride = sizeof(Vertex), .per_instance = false}},
	    .vertex_binding_count   = 1,
	    .primitive_topology     = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	    .cull_mode              = CRGPU_CULL_MODE_NONE,
	    .front_face_ccw         = true,
	    .depth_test_enable      = true,
	    .depth_write_enable     = true,
	    .depth_compare_op       = CRGPU_COMPARE_OP_LESS,
	    .color_attachment_formats = {CRGPU_FORMAT_B8G8R8A8_UNORM},
	    .color_attachment_count  = 1,
	    .depth_format            = CRGPU_FORMAT_D32_FLOAT,
	    .descriptor_set_layouts      = {bindless_layout, sampler_layout, cbv_layout},
	    .descriptor_set_layout_count = 3,
	    .push_constant_ranges        = {{.offset = 0, .size = sizeof(PushConstants), .stage_flags = CRGPU_SHADER_STAGE_FRAGMENT}},
	    .push_constant_range_count   = 1,
	};
	if (CR_FAILED(crgpu_graphics_pipeline_create(device, &pipeline_desc, &pipeline))) {
		fprintf(stderr, "crgpu_graphics_pipeline_create failed\n");
		goto cleanup;
	}

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

	// ------------------------------------------------------------------
	// Main loop.
	// ------------------------------------------------------------------

	Camera camera;
	camera_init(&camera, (CITY_COLUMN_COUNT / 2.0f) * CITY_SPACING_INTERVAL - (CITY_SPACING_INTERVAL / 2.0f), 15.0f, 50.0f, CITY_SPACING_INTERVAL * 2.0f);

	const float up[3] = {0.0f, 1.0f, 0.0f};
	uint64_t last_ticks = SDL_GetTicks();

	bool running = true;
	while (running) {
		if (frame_limit != 0 && frames_run == frame_limit)
			break;
		++frames_run;
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT)
				running = false;
			if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
				bool down = event.type == SDL_EVENT_KEY_DOWN;
				switch (event.key.scancode) {
				case SDL_SCANCODE_W: camera.key_w = down; break;
				case SDL_SCANCODE_A: camera.key_a = down; break;
				case SDL_SCANCODE_S: camera.key_s = down; break;
				case SDL_SCANCODE_D: camera.key_d = down; break;
				case SDL_SCANCODE_LEFT: camera.key_left = down; break;
				case SDL_SCANCODE_RIGHT: camera.key_right = down; break;
				case SDL_SCANCODE_UP: camera.key_up = down; break;
				case SDL_SCANCODE_DOWN: camera.key_down = down; break;
				case SDL_SCANCODE_ESCAPE:
					if (down) running = false;
					break;
				default: break;
				}
			}
		}

		uint64_t now = SDL_GetTicks();
		float dt     = (float) (now - last_ticks) / 1000.0f;
		last_ticks   = now;
		camera_update(&camera, dt);

		float view[16], proj[16], view_proj[16];
		mat4_look_to_rh(camera.position, camera.look_dir, up, view);
		mat4_perspective_rh(0.8f, (float) WINDOW_WIDTH / (float) WINDOW_HEIGHT, 1.0f, 1000.0f, proj);
		mat4_multiply(proj, view, view_proj);

		for (uint32_t i = 0; i < CITY_MATERIAL_COUNT; i++) {
			float mvp[16];
			mat4_multiply(view_proj, model_matrices[i], mvp);
			memcpy((uint8_t *) cbv_mapped + i * cbv_stride, mvp, sizeof(mvp));
			crgpu_buffer_flush(cbv_buffer, i * cbv_stride, sizeof(mvp));
		}

		uint32_t image_index               = crgpu_swap_chain_acquire_next_image(swap_chain);
		crgpu_image_view backbuffer_view = crgpu_swap_chain_get_image_view(swap_chain, image_index);

		crgpu_command_allocator_reset(command_allocator);
		crgpu_command_list_begin(cmd);

		crgpu_command_list_image_barrier_view(cmd, backbuffer_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		// Depth is cleared every pass, so it comes from UNDEFINED each frame,
		// like the backbuffer. It was once transitioned at setup, into the
		// upload list after that list had been closed and submitted.
		crgpu_command_list_image_barrier(cmd, depth_image, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

		crgpu_rendering_begin_desc rendering_desc = {
		    .base = {CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC, NULL},
		    .color_attachments = {{.image_view = backbuffer_view, .load_op = CRGPU_LOAD_OP_CLEAR, .store_op = CRGPU_STORE_OP_STORE, .clear_color = {0.0f, 0.2f, 0.4f, 1.0f}}},
		    .color_attachment_count = 1,
		    .depth_attachment       = {.image_view = depth_view, .depth_load_op = CRGPU_LOAD_OP_CLEAR, .depth_store_op = CRGPU_STORE_OP_STORE, .clear_depth = 1.0f},
		    .width                  = WINDOW_WIDTH,
		    .height                 = WINDOW_HEIGHT,
		};
		crgpu_command_list_begin_rendering(cmd, &rendering_desc);

		crgpu_command_list_bind_graphics_pipeline(cmd, pipeline);
		crgpu_command_list_bind_graphics_bindless_resources(cmd, pipeline, 0);
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline, 1, &sampler_set, 1);

		CRGPU_VIEWPORT viewport = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT, .minDepth = 0.0f, .maxDepth = 1.0f};
		crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);
		CRGPU_SCISSOR_RECT scissor = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
		crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);

		crgpu_command_list_set_vertex_buffers(cmd, 0, 1, &vertex_buffer_view);
		crgpu_command_list_set_index_buffer(cmd, index_buffer_view);

		for (uint32_t i = 0; i < CITY_MATERIAL_COUNT; i++) {
			PushConstants pc = {.diffuse_index = diffuse_bindless_index, .material_index = material_bindless_index[i]};
			crgpu_command_list_push_graphics_constants(cmd, pipeline, 0, sizeof(pc), &pc);
			crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline, 2, &cbv_sets[i], 1);
			crgpu_command_list_draw_indexed(cmd, index_count, 0, 0);
		}

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

	crgpu_image_view_destroy(depth_view);
	crgpu_image_destroy(depth_image);
	crgpu_graphics_pipeline_destroy(pipeline);
	crgpu_shader_module_destroy(fragment_module);
	crgpu_shader_module_destroy(vertex_module);
	if (cbv_buffer)
		crgpu_buffer_unmap(cbv_buffer);
	crgpu_buffer_destroy(cbv_buffer);
	crgpu_descriptor_pool_destroy(cbv_pool);
	crgpu_descriptor_set_layout_destroy(cbv_layout);
	crgpu_descriptor_pool_destroy(sampler_pool);
	crgpu_descriptor_set_layout_destroy(sampler_layout);
	crgpu_sampler_destroy(sampler);
	for (uint32_t i = 0; i < CITY_MATERIAL_COUNT; i++) {
		crgpu_bindless_unregister_sampled_image(device, material_bindless_index[i]);
		crgpu_image_view_destroy(material_views[i]);
		crgpu_image_destroy(material_images[i]);
	}
	crgpu_bindless_unregister_sampled_image(device, diffuse_bindless_index);
	crgpu_image_view_destroy(diffuse_view);
	crgpu_image_destroy(diffuse_image);
	for (uint32_t i = 0; i < staging_buffer_count; i++)
		crgpu_buffer_destroy(staging_buffers[i]);
	crgpu_buffer_view_destroy(index_buffer_view);
	crgpu_buffer_destroy(index_buffer);
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
