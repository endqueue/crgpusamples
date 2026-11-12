//
// Carrottus GPU port of D3D12MeshShaders/MeshletRender from d3d12samples -- the
// simplest of that sample family (no amplification/task shader, no meshlet
// culling, no textures). Renders a pre-built meshlet binary (Dragon_LOD0.bin,
// copied verbatim from the original sample, no converter tool needed) via
// Carrottus GPU's mesh-shader pipeline, first exercised here -- see ../../README.md
// for the one deliberate simplification versus the original.
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
#include "mesh_mesh.shader.h"
#include "scene_frag.shader.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

// ---------------------------------------------------------------------------
// Dragon_LOD0.bin file format -- mirrors d3d12samples' Model.h/Model.cpp
// exactly (FileHeader | MeshHeader[] | Accessor[] | BufferView[] | raw blob,
// with MeshHeader fields being accessor indices, each accessor pointing at a
// buffer-view index). Verified against the actual file bytes (magic
// 0x4D53484C == 'MSHL', header sizes/offsets add up to the real file size)
// before writing this.
// ---------------------------------------------------------------------------

#define MESHLET_FILE_PROLOG 0x4D53484Cu // 'MSHL'
#define MESHLET_ATTRIBUTE_POSITION 0u
#define MESHLET_ATTRIBUTE_NORMAL 1u
#define MESHLET_INVALID_ACCESSOR 0xFFFFFFFFu

typedef struct FileHeader {
	uint32_t prolog;
	uint32_t version;
	uint32_t mesh_count;
	uint32_t accessor_count;
	uint32_t buffer_view_count;
	uint32_t buffer_size;
} FileHeader;

typedef struct FileMeshHeader {
	uint32_t indices;
	uint32_t index_subsets;
	uint32_t attributes[5]; // Position, Normal, TexCoord, Tangent, Bitangent
	uint32_t meshlets;
	uint32_t meshlet_subsets;
	uint32_t unique_vertex_indices;
	uint32_t primitive_indices;
	uint32_t cull_data;
} FileMeshHeader;

typedef struct FileAccessor {
	uint32_t buffer_view;
	uint32_t offset;
	uint32_t size;
	uint32_t stride;
	uint32_t count;
} FileAccessor;

typedef struct FileBufferView {
	uint32_t offset;
	uint32_t size;
} FileBufferView;

// GPU-side structs -- must match the byte offsets shaders/mesh_mesh.slang
// reads its buffers at and its Meshlet/PushConstants/Globals exactly
// (all-uint32/float members, naturally 4-byte-aligned, no manual padding
// needed but Globals' below).
typedef struct Vertex {
	float position[3];
	float normal[3];
} Vertex; // stride 24, matches the file's interleaved Position+Normal buffer view.

typedef struct Meshlet {
	uint32_t vert_count;
	uint32_t vert_offset;
	uint32_t prim_count;
	uint32_t prim_offset;
} Meshlet;

typedef struct Subset {
	uint32_t offset;
	uint32_t count;
} Subset;

typedef struct PushConstants {
	uint32_t index_bytes;
	uint32_t meshlet_offset;
} PushConstants;

typedef struct Globals {
	float world[16];
	float world_view[16];
	float world_view_proj[16];
	uint32_t draw_meshlets;
	float _pad[3]; // round up to a multiple of 16 bytes for std140/CBV alignment.
} Globals;

// Resolves a MeshHeader field (an accessor index, or MESHLET_INVALID_ACCESSOR
// if unused) to the raw byte range in the blob that its buffer view covers.
// Every resource this sample loads is either its own dedicated buffer view
// (Meshlets/MeshletSubsets/UniqueVertexIndices/PrimitiveIndices) or, for
// Position/Normal, the one interleaved buffer view both attributes share --
// either way the whole buffer-view range is what we want to upload verbatim,
// not any single attribute's own accessor sub-window within it.
static bool resolve_buffer_view(
    const FileAccessor *accessors,
    const FileBufferView *buffer_views,
    uint32_t accessor_index,
    uint32_t *out_offset,
    uint32_t *out_size) {
	if (accessor_index == MESHLET_INVALID_ACCESSOR)
		return false;
	uint32_t bv_index = accessors[accessor_index].buffer_view;
	*out_offset        = buffer_views[bv_index].offset;
	*out_size          = buffer_views[bv_index].size;
	return true;
}

// ---------------------------------------------------------------------------
// Math (column-major, right-handed) -- identical to CrGPUDynamicIndexing's,
// which already ported this exact model/view/projection convention.
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

static void mat4_identity(float out[16]) {
	memset(out, 0, sizeof(float) * 16);
	out[0] = out[5] = out[10] = out[15] = 1.0f;
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
	// Not negated: Carrottus GPU's clip space is +Y up on every backend, as the
	// original's Direct3D one is, so its clockwise front faces hold too.
	out[5] = f;
	out[10] = z_far / (z_near - z_far);
	out[11] = -1.0f;
	out[14] = (z_near * z_far) / (z_near - z_far);
}

// ---------------------------------------------------------------------------
// Camera -- identical keyboard-only WASD+arrows camera already ported twice
// (CrGPUExecuteIndirect, CrGPUDynamicIndexing); this sample's own move
// speed/FOV/initial position match the original MeshletRender's SimpleCamera
// overrides.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------

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

// Creates a storage buffer in DEVICE memory and uploads [data, data+size)
// into it once, via a temporary UPLOAD staging buffer + crgpu_command_list_copy_buffer.
// CRGPU_MEMORY_KIND_UPLOAD refuses CRGPU_BUFFER_USAGE_STORAGE on every
// backend, and the one mappable kind that takes it
// (CRGPU_MEMORY_KIND_DEVICE_MAPPABLE) isn't on every device -- so this
// can't just map the real buffer directly the way every other one-time
// upload in this suite does. Synchronous (one begin/end/submit/wait per call): this only
// runs a handful of times at startup, so the simplicity is worth more than
// batching all four uploads into one submission. NULL, printed, on failure.
static crgpu_buffer create_storage_buffer_with_data(
    crgpu_device device, crgpu_command_list cmd, crgpu_command_queue graphics_queue,
    const void *data, uint64_t size) {
	crgpu_buffer_desc staging_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_TRANSFER_SRC,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = size,
	};
	crgpu_buffer staging = NULL;
	if (CR_FAILED(crgpu_buffer_create(device, &staging_desc, &staging))) {
		fprintf(stderr, "crgpu_buffer_create (staging) failed\n");
		return NULL;
	}
	void *mapped = NULL;
	crgpu_buffer_map(staging, 0, 0, &mapped);
	memcpy(mapped, data, size);
	crgpu_buffer_flush(staging, 0, 0);
	crgpu_buffer_unmap(staging);

	crgpu_buffer_desc real_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST,
	    .memory_kind  = CRGPU_MEMORY_KIND_DEVICE,
	    .size         = size,
	};
	crgpu_buffer buffer = NULL;
	if (CR_FAILED(crgpu_buffer_create(device, &real_desc, &buffer))) {
		fprintf(stderr, "crgpu_buffer_create (storage) failed\n");
		crgpu_buffer_destroy(staging);
		return NULL;
	}

	// No COMMON -> UNORDERED_ACCESS barrier here: D3D12 implicitly promotes
	// this buffer to COPY_DEST for the copy itself, and that promoted state
	// doesn't decay back to COMMON until this command list actually finishes
	// executing (at the ExecuteCommandLists boundary) -- a barrier recorded
	// right here, in the same not-yet-submitted command list, would have to
	// declare "before = COPY_DEST" to match, not COMMON. Simplest correct
	// fix: transition to UNORDERED_ACCESS later, in a separate command list
	// recorded after this one has been submitted and the queue drained
	// (see the buffer_barriers passed alongside the depth image's barrier
	// below) -- by then the buffer has legitimately decayed back to COMMON.
	crgpu_command_list_begin(cmd);
	crgpu_command_list_copy_buffer(cmd, staging, buffer, 0, 0, size);
	crgpu_command_list_end(cmd);
	crgpu_command_queue_submit_lists(graphics_queue, &cmd, 1);
	crgpu_command_queue_wait_idle(graphics_queue);

	crgpu_buffer_destroy(staging);
	return buffer;
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
	int exit_code                                  = 1;
	SDL_Window *window                             = NULL;
	crgpu_instance instance                      = NULL;
	crgpu_device device                          = NULL;
	crgpu_command_queue graphics_queue           = NULL;
	crgpu_surface surface                        = NULL;
	crgpu_swap_chain swap_chain                  = NULL;
	crgpu_command_allocator command_allocator    = NULL;
	crgpu_command_list cmd                       = NULL;
	uint8_t *file_data                             = NULL;
	Subset *meshlet_subsets                        = NULL;
	crgpu_buffer vertices_buffer                 = NULL;
	crgpu_buffer meshlets_buffer                 = NULL;
	crgpu_buffer unique_vtx_idx_buffer           = NULL;
	crgpu_buffer primitive_idx_buffer            = NULL;
	crgpu_descriptor_set_layout mesh_data_layout = NULL;
	crgpu_descriptor_pool mesh_data_pool         = NULL;
	crgpu_descriptor_set_layout globals_layout   = NULL;
	crgpu_descriptor_pool globals_pool           = NULL;
	crgpu_buffer globals_buffer                  = NULL;
	crgpu_shader_module mesh_module              = NULL;
	crgpu_shader_module fragment_module          = NULL;
	crgpu_graphics_pipeline pipeline               = NULL;
	crgpu_image depth_image                      = NULL;
	crgpu_image_view depth_view                  = NULL;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	window = SDL_CreateWindow("Carrottus GPU MeshShaders", WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		goto cleanup;
	}

	const CR_ALLOCATOR gpu_allocator           = cr_allocator_system();
	const crgpu_instance_desc gpu_instance_desc = {
	    .base                = {CR_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "CrGPUMeshShaders",
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

	if (!(adapter_properties.supported_features & CRGPU_FEATURE_MESH_SHADING)) {
		fprintf(stderr, "This device/driver doesn't support mesh shading\n");
		goto cleanup;
	}

	// A window to present to and one direct queue, and exactly the features
	// the sample uses: a device has only what it is created with.
	const crgpu_device_desc device_desc = {
	    .base               = {CR_STRUCT_TYPE_GPU_DEVICE_DESC, NULL},
	    .required_features  = CRGPU_FEATURE_PRESENT | CRGPU_FEATURE_MESH_SHADING,
	    .direct_queue_count = 1,
	    // What the sample's descriptor pools hold together, with room to
	    // spare: the caller's to choose, and refused past the adapter's
	    // limit.
	    .max_descriptors    = 1024,
	    .max_samplers       = 64,
	    .debug_name         = "CrGPUMeshShaders",
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

	// Created here (rather than just before the main loop, as in most other
	// samples) because create_storage_buffer_with_data below already needs a
	// command list to record its staging-buffer upload into.
	crgpu_command_allocator_desc allocator_desc = {.base = {CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC, NULL}, .type = CRGPU_COMMAND_LIST_TYPE_DIRECT};
	crgpu_command_allocator_create(device, &allocator_desc, &command_allocator);

	crgpu_command_list_create(command_allocator, &cmd);

	// ------------------------------------------------------------------
	// Load and parse Dragon_LOD0.bin (single mesh).
	// ------------------------------------------------------------------

	size_t file_size;
	file_data = (uint8_t *) read_entire_file("Dragon_LOD0.bin", &file_size);
	if (!file_data)
		goto cleanup;

	const FileHeader *header = (const FileHeader *) file_data;
	if (header->prolog != MESHLET_FILE_PROLOG || header->mesh_count == 0) {
		fprintf(stderr, "Dragon_LOD0.bin: bad file header\n");
		goto cleanup;
	}
	const FileMeshHeader *meshes = (const FileMeshHeader *) (file_data + sizeof(FileHeader));
	const FileAccessor *accessors = (const FileAccessor *) ((const uint8_t *) meshes + (size_t) header->mesh_count * sizeof(FileMeshHeader));
	const FileBufferView *buffer_views = (const FileBufferView *) ((const uint8_t *) accessors + (size_t) header->accessor_count * sizeof(FileAccessor));
	const uint8_t *blob = (const uint8_t *) buffer_views + (size_t) header->buffer_view_count * sizeof(FileBufferView);

	const FileMeshHeader *mesh = &meshes[0];

	uint32_t vtx_offset, vtx_size;
	uint32_t meshlet_offset, meshlet_size;
	uint32_t meshlet_subset_offset, meshlet_subset_size;
	uint32_t uvi_offset, uvi_size;
	uint32_t pi_offset, pi_size;
	if (!resolve_buffer_view(accessors, buffer_views, mesh->attributes[MESHLET_ATTRIBUTE_POSITION], &vtx_offset, &vtx_size) ||
	    !resolve_buffer_view(accessors, buffer_views, mesh->meshlets, &meshlet_offset, &meshlet_size) ||
	    !resolve_buffer_view(accessors, buffer_views, mesh->meshlet_subsets, &meshlet_subset_offset, &meshlet_subset_size) ||
	    !resolve_buffer_view(accessors, buffer_views, mesh->unique_vertex_indices, &uvi_offset, &uvi_size) ||
	    !resolve_buffer_view(accessors, buffer_views, mesh->primitive_indices, &pi_offset, &pi_size)) {
		fprintf(stderr, "Dragon_LOD0.bin: missing a required buffer view\n");
		goto cleanup;
	}
	// Only the Indices accessor's Size is needed (2 or 4 bytes per index --
	// see the original's Model.cpp: "mesh.IndexSize = accessor.Size") -- the
	// actual index buffer it describes is unused by this sample's mesh
	// shader, same as the original. Size, not Stride: for a tightly packed
	// scalar index buffer the two happen to differ in this asset (Stride
	// here is 0, since nothing else shares this accessor's buffer view),
	// so reading Stride was silently feeding get_vertex_index() the wrong
	// index_bytes and corrupting every 16-bit-packed vertex-index lookup.
	uint32_t index_bytes = accessors[mesh->indices].size;

	// Copied out of the file: the render loop reads them every frame, and
	// file_data is freed below. They once pointed into it, and the loop read
	// freed memory.
	uint32_t meshlet_subset_count = meshlet_subset_size / sizeof(Subset);
	meshlet_subsets               = (Subset *) malloc(meshlet_subset_size);
	if (!meshlet_subsets) {
		fprintf(stderr, "Out of memory copying the meshlet subsets\n");
		goto cleanup;
	}
	memcpy(meshlet_subsets, blob + meshlet_subset_offset, meshlet_subset_size);

	vertices_buffer       = create_storage_buffer_with_data(device, cmd, graphics_queue, blob + vtx_offset, vtx_size);
	meshlets_buffer       = create_storage_buffer_with_data(device, cmd, graphics_queue, blob + meshlet_offset, meshlet_size);
	unique_vtx_idx_buffer = create_storage_buffer_with_data(device, cmd, graphics_queue, blob + uvi_offset, uvi_size);
	primitive_idx_buffer  = create_storage_buffer_with_data(device, cmd, graphics_queue, blob + pi_offset, pi_size);
	if (!vertices_buffer || !meshlets_buffer || !unique_vtx_idx_buffer || !primitive_idx_buffer)
		goto cleanup;

	free(file_data);
	file_data = NULL;

	// ------------------------------------------------------------------
	// Descriptor set 0: the 4 mesh-data storage buffers (mesh-visible).
	// ------------------------------------------------------------------

	crgpu_descriptor_set_layout_desc mesh_data_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings = {
	        {.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_MESH},
	        {.binding = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_MESH},
	        {.binding = 2, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_MESH},
	        {.binding = 3, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_MESH},
	    },
	    .binding_count = 4,
	};
	crgpu_descriptor_set_layout_create(device, &mesh_data_layout_desc, &mesh_data_layout);

	crgpu_descriptor_pool_desc mesh_data_pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets = 1, .pool_sizes = {{.descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 4}}, .pool_size_count = 1};
	crgpu_descriptor_pool_create(device, &mesh_data_pool_desc, &mesh_data_pool);

	crgpu_descriptor_set_desc mesh_data_set_desc = {.base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL}, .pool = mesh_data_pool, .set_layouts = {mesh_data_layout}, .set_count = 1};
	crgpu_descriptor_set mesh_data_set = NULL;
	crgpu_descriptor_set_create(device, &mesh_data_set_desc, &mesh_data_set);

	crgpu_descriptor_buffer_info vertices_info = {.buffer = vertices_buffer, .range = vtx_size};
	crgpu_descriptor_buffer_info meshlets_info = {.buffer = meshlets_buffer, .range = meshlet_size};
	crgpu_descriptor_buffer_info uvi_info      = {.buffer = unique_vtx_idx_buffer, .range = uvi_size};
	crgpu_descriptor_buffer_info pi_info       = {.buffer = primitive_idx_buffer, .range = pi_size};
	crgpu_write_descriptor_set mesh_data_writes[4] = {
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = mesh_data_set, .dst_binding = 0, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .buffer_info = &vertices_info},
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = mesh_data_set, .dst_binding = 1, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .buffer_info = &meshlets_info},
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = mesh_data_set, .dst_binding = 2, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .buffer_info = &uvi_info},
	    {.base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}, .dst_set = mesh_data_set, .dst_binding = 3, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, .buffer_info = &pi_info},
	};
	crgpu_device_update_descriptor_sets(device, mesh_data_writes, 4);

	// ------------------------------------------------------------------
	// Descriptor set 1: Globals CBV (mesh + fragment visible).
	// ------------------------------------------------------------------

	crgpu_descriptor_set_layout_desc globals_layout_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL},
	    .bindings = {{.binding = 0, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .stage_flags = CRGPU_SHADER_STAGE_MESH | CRGPU_SHADER_STAGE_FRAGMENT}},
	    .binding_count = 1,
	};
	crgpu_descriptor_set_layout_create(device, &globals_layout_desc, &globals_layout);

	crgpu_descriptor_pool_desc globals_pool_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL},
	    .max_sets = 1, .pool_sizes = {{.descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1}}, .pool_size_count = 1};
	crgpu_descriptor_pool_create(device, &globals_pool_desc, &globals_pool);

	crgpu_descriptor_set_desc globals_set_desc = {.base = {CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL}, .pool = globals_pool, .set_layouts = {globals_layout}, .set_count = 1};
	crgpu_descriptor_set globals_set = NULL;
	crgpu_descriptor_set_create(device, &globals_set_desc, &globals_set);

	crgpu_buffer_desc globals_buffer_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage        = CRGPU_BUFFER_USAGE_UNIFORM,
	    .memory_kind  = CRGPU_MEMORY_KIND_UPLOAD,
	    .size         = sizeof(Globals),
	};
	crgpu_buffer_create(device, &globals_buffer_desc, &globals_buffer);
	void *globals_mapped = NULL;
	crgpu_buffer_map(globals_buffer, 0, 0, &globals_mapped);

	crgpu_descriptor_buffer_info globals_info = {.buffer = globals_buffer, .range = sizeof(Globals)};
	crgpu_write_descriptor_set globals_write = {
	    .base = {CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL},
	    .dst_set = globals_set, .dst_binding = 0, .descriptor_count = 1, .descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .buffer_info = &globals_info};
	crgpu_device_update_descriptor_sets(device, &globals_write, 1);

	// ------------------------------------------------------------------
	// Mesh-shader pipeline + depth buffer.
	// ------------------------------------------------------------------

	mesh_module     = crgpusamples_create_shader_module(device, shader_format, &mesh_mesh_shader, CRGPU_SHADER_STAGE_MESH, 128, 1, 1);
	fragment_module = crgpusamples_create_shader_module(device, shader_format, &scene_frag_shader, CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	if (!mesh_module || !fragment_module)
		goto cleanup;

	crgpu_graphics_pipeline_desc pipeline_desc = {
	    .base = {CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC, NULL},
	    .mesh_module     = mesh_module,
	    .fragment_module = fragment_module,
	    .primitive_topology = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	    .cull_mode          = CRGPU_CULL_MODE_BACK,
	    .front_face_ccw     = false, // matches D3D12_DEFAULT's clockwise-is-front-facing.
	    .depth_test_enable  = true,
	    .depth_write_enable = true,
	    .depth_compare_op   = CRGPU_COMPARE_OP_LESS,
	    .color_attachment_formats = {CRGPU_FORMAT_B8G8R8A8_UNORM},
	    .color_attachment_count  = 1,
	    .depth_format            = CRGPU_FORMAT_D32_FLOAT,
	    .descriptor_set_layouts      = {mesh_data_layout, globals_layout},
	    .descriptor_set_layout_count = 2,
	    .push_constant_ranges        = {{.offset = 0, .size = sizeof(PushConstants), .stage_flags = CRGPU_SHADER_STAGE_MESH}},
	    .push_constant_range_count   = 1,
	};
	if (CR_FAILED(crgpu_graphics_pipeline_create(device, &pipeline_desc, &pipeline))) {
		fprintf(stderr, "crgpu_graphics_pipeline_create failed\n");
		goto cleanup;
	}

	// depth image created/transitioned via `cmd` before the main loop starts.
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
	    .subresource_range = {.aspect_mask = 0x2 /* DEPTH, Vulkan-aligned - see crgpu.h */, .base_mip_level = 0, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
	};
	crgpu_image_view_create(depth_image, &depth_view_desc, &depth_view);

	crgpu_command_list_begin(cmd);
	crgpu_command_list_image_barrier(cmd, depth_image, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	// Mesh shaders read these as UAVs (CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER
	// maps to D3D12_DESCRIPTOR_RANGE_TYPE_UAV) -- unlike the read-only states
	// D3D12 promotes to implicitly, UNORDERED_ACCESS needs an explicit
	// transition. COMMON is valid as "before" here specifically because each
	// buffer's own upload command list (create_storage_buffer_with_data) has
	// already been submitted and the queue drained by this point, so the
	// COPY_DEST state that upload promoted to has already decayed back to
	// COMMON -- see that function's comment for why the same transition
	// can't happen inside its own command list.
	crgpu_buffer_barrier buffer_barriers[4] = {
	    {.base = {CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL}, .buffer = vertices_buffer, .state_before = CRGPU_RESOURCE_STATE_COMMON, .state_after = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS},
	    {.base = {CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL}, .buffer = meshlets_buffer, .state_before = CRGPU_RESOURCE_STATE_COMMON, .state_after = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS},
	    {.base = {CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL}, .buffer = unique_vtx_idx_buffer, .state_before = CRGPU_RESOURCE_STATE_COMMON, .state_after = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS},
	    {.base = {CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL}, .buffer = primitive_idx_buffer, .state_before = CRGPU_RESOURCE_STATE_COMMON, .state_after = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS},
	};
	crgpu_command_list_pipeline_barrier(cmd, buffer_barriers, 4, NULL, 0);
	crgpu_command_list_end(cmd);
	crgpu_command_queue_submit_lists(graphics_queue, &cmd, 1);
	crgpu_command_queue_wait_idle(graphics_queue);

	// ------------------------------------------------------------------
	// Main loop.
	// ------------------------------------------------------------------

	Camera camera;
	camera_init(&camera, 0.0f, 75.0f, 150.0f, 150.0f);

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

		Globals globals_data;
		mat4_identity(globals_data.world);
		float view[16], proj[16];
		mat4_look_to_rh(camera.position, camera.look_dir, up, view);
		mat4_perspective_rh((float) M_PI / 3.0f, (float) WINDOW_WIDTH / (float) WINDOW_HEIGHT, 1.0f, 1000.0f, proj);
		memcpy(globals_data.world_view, view, sizeof(view)); // world is identity.
		mat4_multiply(proj, view, globals_data.world_view_proj);
		globals_data.draw_meshlets = 1;
		memcpy(globals_mapped, &globals_data, sizeof(globals_data));
		crgpu_buffer_flush(globals_buffer, 0, sizeof(globals_data));

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
		    .color_attachments = {{.image_view = backbuffer_view, .load_op = CRGPU_LOAD_OP_CLEAR, .store_op = CRGPU_STORE_OP_STORE, .clear_color = {0.0f, 0.2f, 0.4f, 1.0f}}},
		    .color_attachment_count = 1,
		    .depth_attachment       = {.image_view = depth_view, .depth_load_op = CRGPU_LOAD_OP_CLEAR, .depth_store_op = CRGPU_STORE_OP_STORE, .clear_depth = 1.0f},
		    .width                  = WINDOW_WIDTH,
		    .height                 = WINDOW_HEIGHT,
		};
		crgpu_command_list_begin_rendering(cmd, &rendering_desc);

		crgpu_command_list_bind_graphics_pipeline(cmd, pipeline);
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline, 0, &mesh_data_set, 1);
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline, 1, &globals_set, 1);

		CRGPU_VIEWPORT viewport = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT, .minDepth = 0.0f, .maxDepth = 1.0f};
		crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);
		CRGPU_SCISSOR_RECT scissor = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
		crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);

		for (uint32_t s = 0; s < meshlet_subset_count; s++) {
			PushConstants pc = {.index_bytes = index_bytes, .meshlet_offset = meshlet_subsets[s].offset};
			crgpu_command_list_push_graphics_constants(cmd, pipeline, 0, sizeof(pc), &pc);
			crgpu_command_list_dispatch_mesh(cmd, meshlet_subsets[s].count, 1, 1);
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

	crgpu_image_view_destroy(depth_view);
	crgpu_image_destroy(depth_image);
	crgpu_graphics_pipeline_destroy(pipeline);
	crgpu_shader_module_destroy(fragment_module);
	crgpu_shader_module_destroy(mesh_module);
	free(meshlet_subsets);
	free(file_data);
	if (globals_buffer)
		crgpu_buffer_unmap(globals_buffer);
	crgpu_buffer_destroy(globals_buffer);
	crgpu_descriptor_pool_destroy(globals_pool);
	crgpu_descriptor_set_layout_destroy(globals_layout);
	crgpu_descriptor_pool_destroy(mesh_data_pool);
	crgpu_descriptor_set_layout_destroy(mesh_data_layout);
	crgpu_buffer_destroy(primitive_idx_buffer);
	crgpu_buffer_destroy(unique_vtx_idx_buffer);
	crgpu_buffer_destroy(meshlets_buffer);
	crgpu_buffer_destroy(vertices_buffer);
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
