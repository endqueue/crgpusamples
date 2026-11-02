//
// SpudGPU port of D3D12Raytracing's three tutorials from d3d12samples, as
// one sample with three modes (--mode):
// - hello:      D3D12RaytracingHelloWorld, a triangle ray traced in screen
//               space inside a stencil window.
// - lighting:   D3D12RaytracingSimpleLighting, a cube under a point light,
//               the camera and the light going round it.
// - procedural: D3D12RaytracingProceduralGeometry, analytic, volumetric and
//               signed distance primitives as procedural candidates, over a
//               reflective ground plane, with shadow and reflection rays.
//
// The originals use DXR 1.0 ray tracing pipelines. SpudGPU has no pipeline
// EXT (card S12), so each mode traces with inline ray queries
// (SPUDGPU_EXT_RAY_QUERY) in one compute shader, the original's ray
// generation, miss, closest-hit and intersection shaders becoming functions
// of it. The trace writes an image the window's size, which a small pass
// then draws onto the back buffer. See ../../README.md.
//
// The acceleration structures are built once, at start: bottom levels over
// the originals' vertex and index data (and AABBs), a top level over their
// instances. The instances are written in the device's own layout
// (spudgpu_get_acceleration_structure_instance_layout()): by
// spudgpu_write_acceleration_structure_instances() on the CPU, and in the
// procedural mode one of the two by a compute shader too, the sample's own
// writer for the layout's kind, so that both writers run. Each frame then
// pushes its camera and light, traces and presents, and creates nothing.
//

#include <SDL3/SDL.h>
#include <spudgpu.h>
#include <spudgpu_sdl3.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The shaders, Slang built into each format and embedded at build time
// (../../cmake/spudgpusamples_shader.cmake).
#include "rt_hello_comp.shader.h"
#include "rt_lighting_comp.shader.h"
#include "rt_procedural_comp.shader.h"
#include "rt_instances_row64_comp.shader.h"
#include "rt_instances_col72_comp.shader.h"
#include "rt_present_vert.shader.h"
#include "rt_present_frag.shader.h"

#if !SPUDGPU_EXT_RAY_QUERY
#error "SpudGPURayTracing needs SPUDGPU_EXT_RAY_QUERY, which every backend has"
#endif

// The originals' window: 1280 x 720.
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
// The trace shaders' [numthreads(8, 8, 1)].
#define TRACE_GROUP_SIZE 8u
// What the sample's descriptor sets hold together, with room to spare.
#define MAX_DESCRIPTORS 32u

#define RT_PI 3.14159265358979323846f
// No --device given: rt_create_device takes the first adapter able to run
// the sample.
#define RT_DEVICE_FIRST_CAPABLE UINT32_MAX

typedef enum RtMode {
	RT_MODE_HELLO,
	RT_MODE_LIGHTING,
	RT_MODE_PROCEDURAL
} RtMode;

// ---------------------------------------------------------------------------
// What the shaders read, laid out as they declare it: every member a 16-byte
// row, so the layout is the same in a constant buffer on every target.
// ---------------------------------------------------------------------------

// rt_hello_comp.slang's Frame: pushed.
typedef struct RtHelloFrame {
	float viewport[4]; // left, top, right, bottom
	float stencil[4];
} RtHelloFrame;

// rt_lighting_comp.slang's Scene: written once.
typedef struct RtLightingScene {
	float light_ambient[4];
	float light_diffuse[4];
	float albedo[4];
	uint32_t offsets[4]; // the vertices' and indices' byte offsets in the geometry buffer
} RtLightingScene;

// rt_lighting_comp.slang's Frame: pushed every frame.
typedef struct RtLightingFrame {
	float projection_to_world[16]; // rows
	float camera_position[4];
	float light_position[4];
} RtLightingFrame;

// rt_procedural_comp.slang's Material (the original's PrimitiveConstantBuffer).
typedef struct RtMaterial {
	float albedo[4];
	float coefficients[4]; // reflectance, diffuse, specular, specular power
	float step_scale[4];   // x: the signed distance step scale
} RtMaterial;

// rt_procedural_comp.slang's Primitive: the transform from the primitive's
// local space to the bottom level's, and its intersection test.
typedef struct RtPrimitive {
	float center[4]; // its AABB's centre
	float scale[4];  // w: 1 if it turns with the animation
	uint32_t kind[4]; // the intersection kind, the primitive type
} RtPrimitive;

// The AABB bottom level's geometries, one AABB each: PRIMITIVE_COUNT in
// rt_procedural_comp.slang.
#define RT_PROCEDURAL_PRIMITIVE_COUNT 10u

// rt_procedural_comp.slang's Scene: written once.
typedef struct RtProceduralScene {
	float light_ambient[4];
	float light_diffuse[4];
	uint32_t offsets[4]; // the plane's vertices' and indices' byte offsets in the geometry buffer
	RtMaterial plane;
	RtMaterial materials[RT_PROCEDURAL_PRIMITIVE_COUNT];
	RtPrimitive primitives[RT_PROCEDURAL_PRIMITIVE_COUNT];
} RtProceduralScene;

// rt_procedural_comp.slang's Frame: pushed every frame.
typedef struct RtProceduralFrame {
	float projection_to_world[16]; // rows
	float camera_position[4];
	float light_position[4];
	float time[4]; // x: the geometry's animation time
} RtProceduralFrame;

// rt_instances_*_comp.slang's Instance: one portable instance, pushed to
// the GPU writer.
typedef struct RtWriterInstance {
	float rows[12];
	uint32_t user_index;
	uint32_t mask;
	uint32_t table_offset;
	uint32_t flags;
	uint32_t reference_low;
	uint32_t reference_high;
	uint32_t out_offset;
	uint32_t padding;
} RtWriterInstance;

// What the shaders declare, checked at compile time: an array of -1
// elements doesn't compile. (MSVC's C has no _Static_assert before C11.)
#define RT_CHECK(name, condition) typedef char rt_check_##name[(condition) ? 1 : -1]
RT_CHECK(hello_frame, sizeof(RtHelloFrame) == 32);
RT_CHECK(lighting_scene, sizeof(RtLightingScene) == 64);
RT_CHECK(lighting_frame, sizeof(RtLightingFrame) == 96);
RT_CHECK(material, sizeof(RtMaterial) == 48);
RT_CHECK(primitive, sizeof(RtPrimitive) == 48);
RT_CHECK(procedural_scene, sizeof(RtProceduralScene) == 48 + 48 + 96 * RT_PROCEDURAL_PRIMITIVE_COUNT);
RT_CHECK(procedural_frame, sizeof(RtProceduralFrame) == 112);
RT_CHECK(writer_instance, sizeof(RtWriterInstance) == 80);

// The originals' Vertex: a position and, for the lighting and procedural
// modes, a normal. Positions are what the bottom levels are built from.
typedef struct RtVertex {
	float position[3];
	float normal[3];
} RtVertex;

// ---------------------------------------------------------------------------
// Math: DirectXMath's conventions, as the originals use them. A matrix is
// 16 floats, row-major, and multiplies a row vector on its left (v * M);
// the views and projections are left-handed.
// ---------------------------------------------------------------------------

typedef struct Vec3 {
	float x, y, z;
} Vec3;

static Vec3 vec3(float x, float y, float z) {
	Vec3 v = {x, y, z};
	return v;
}

static Vec3 vec3_sub(Vec3 a, Vec3 b) {
	return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static float vec3_dot(Vec3 a, Vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 vec3_cross(Vec3 a, Vec3 b) {
	return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static Vec3 vec3_normalize(Vec3 v) {
	const float length = sqrtf(vec3_dot(v, v));
	return length > 0.0f ? vec3(v.x / length, v.y / length, v.z / length) : v;
}

// v * XMMatrixRotationY(angle).
static Vec3 vec3_rotate_y(Vec3 v, float angle) {
	const float c = cosf(angle);
	const float s = sinf(angle);
	return vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

static void mat4_multiply(const float a[16], const float b[16], float out[16]) {
	float result[16];
	for (int row = 0; row < 4; ++row)
		for (int col = 0; col < 4; ++col) {
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k)
				sum += a[row * 4 + k] * b[k * 4 + col];
			result[row * 4 + col] = sum;
		}
	memcpy(out, result, sizeof(result));
}

// XMMatrixLookAtLH.
static void mat4_look_at_lh(Vec3 eye, Vec3 at, Vec3 up, float out[16]) {
	const Vec3 z = vec3_normalize(vec3_sub(at, eye));
	const Vec3 x = vec3_normalize(vec3_cross(up, z));
	const Vec3 y = vec3_cross(z, x);
	const float m[16] = {
	    x.x, y.x, z.x, 0.0f,
	    x.y, y.y, z.y, 0.0f,
	    x.z, y.z, z.z, 0.0f,
	    -vec3_dot(x, eye), -vec3_dot(y, eye), -vec3_dot(z, eye), 1.0f,
	};
	memcpy(out, m, sizeof(m));
}

// XMMatrixPerspectiveFovLH.
static void mat4_perspective_fov_lh(float fov_y, float aspect, float z_near, float z_far, float out[16]) {
	const float h     = 1.0f / tanf(0.5f * fov_y);
	const float w     = h / aspect;
	const float range = z_far / (z_far - z_near);
	const float m[16] = {
	    w, 0.0f, 0.0f, 0.0f,
	    0.0f, h, 0.0f, 0.0f,
	    0.0f, 0.0f, range, 1.0f,
	    0.0f, 0.0f, -range * z_near, 0.0f,
	};
	memcpy(out, m, sizeof(m));
}

// The inverse of [m], by cofactors; false, [out] untouched, if it has none.
static bool mat4_inverse(const float m[16], float out[16]) {
	float inv[16];
	inv[0]  = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8]  = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5]  = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9]  = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2]  = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6]  = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3]  = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7]  = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0.0f)
		return false;
	for (int i = 0; i < 16; ++i)
		out[i] = inv[i] / det;
	return true;
}

// The originals' camera: an eye, the point it looks at and its up.
typedef struct Camera {
	Vec3 eye;
	Vec3 at;
	Vec3 up;
} Camera;

// The originals' InitializeScene: the eye at [eye] looking at the origin,
// up square to the view and the x axis, then all of it turned 45 degrees
// about y.
static Camera camera_init(Vec3 eye) {
	Camera camera;
	camera.eye           = eye;
	camera.at            = vec3(0.0f, 0.0f, 0.0f);
	const Vec3 direction = vec3_normalize(vec3_sub(camera.at, camera.eye));
	camera.up            = vec3_normalize(vec3_cross(direction, vec3(1.0f, 0.0f, 0.0f)));
	camera.eye           = vec3_rotate_y(camera.eye, 45.0f * RT_PI / 180.0f);
	camera.up            = vec3_rotate_y(camera.up, 45.0f * RT_PI / 180.0f);
	return camera;
}

static void camera_rotate_y(Camera *camera, float angle) {
	camera->eye = vec3_rotate_y(camera->eye, angle);
	camera->up  = vec3_rotate_y(camera->up, angle);
	camera->at  = vec3_rotate_y(camera->at, angle);
}

// The originals' UpdateCameraMatrices: the inverse of the view-projection,
// its rows, for the shader's unprojection. A 45 degree field of view.
static void camera_projection_to_world(const Camera *camera, float z_near, float z_far, float out_rows[16]) {
	float view[16];
	float projection[16];
	float view_projection[16];
	mat4_look_at_lh(camera->eye, camera->at, camera->up, view);
	mat4_perspective_fov_lh(45.0f * RT_PI / 180.0f, (float)WINDOW_WIDTH / (float)WINDOW_HEIGHT, z_near, z_far, projection);
	mat4_multiply(view, projection, view_projection);
	if (!mat4_inverse(view_projection, out_rows))
		memset(out_rows, 0, sizeof(float) * 16);
}

static void vec4_set(float out[4], float x, float y, float z, float w) {
	out[0] = x;
	out[1] = y;
	out[2] = z;
	out[3] = w;
}

// ---------------------------------------------------------------------------
// Everything the sample makes, NULL until made. rt_destroy() destroys
// whatever exists: every destroy call takes NULL.
// ---------------------------------------------------------------------------

typedef struct RtSample {
	RtMode mode;
	SDL_Window *window;
	spudgpu_instance instance;
	spudgpu_device device;
	spudgpu_command_queue queue;
	spudgpu_surface surface;
	spudgpu_swap_chain swap_chain;
	spudgpu_command_allocator allocator;
	spudgpu_command_list cmd;
	SPUDGPU_SHADER_FORMAT shader_format;
	SPUDGPU_RAY_QUERY_LIMITS limits;
	spudgpu_acceleration_structure_instance_layout instance_layout;

	// The trace's output, and the pass that draws it onto the back buffer.
	spudgpu_image output;
	spudgpu_image_view output_view;
	spudgpu_shader_module present_vert;
	spudgpu_shader_module present_frag;
	spudgpu_descriptor_set_layout present_layout;
	spudgpu_descriptor_pool present_pool;
	spudgpu_descriptor_set present_set;
	spudgpu_shader_pipeline present_pipeline;

	// The trace: its compute pipeline and set, the scene's constants, the
	// geometry the bottom levels were built from, and the levels.
	spudgpu_shader_module trace_module;
	spudgpu_descriptor_set_layout trace_layout;
	spudgpu_descriptor_pool trace_pool;
	spudgpu_descriptor_set trace_set;
	spudgpu_compute_pipeline trace_pipeline;
	spudgpu_buffer scene;
	spudgpu_buffer geometry;
	uint64_t geometry_size;
	spudgpu_acceleration_structure bottoms[2];
	uint32_t bottom_count;
	spudgpu_acceleration_structure top;

	// Used only by the setup submission, destroyed once it completes (a
	// build's inputs and scratch live until then).
	spudgpu_buffer staging_geometry;
	spudgpu_buffer staging_instances;
	spudgpu_buffer instances;
	spudgpu_buffer scratch;
	spudgpu_shader_module writer_module;
	spudgpu_descriptor_set_layout writer_layout;
	spudgpu_descriptor_pool writer_pool;
	spudgpu_descriptor_set writer_set;
	spudgpu_compute_pipeline writer_pipeline;
} RtSample;

// What the setup submission alone used. The queue is idle.
static void rt_destroy_setup_objects(RtSample *s) {
	spudgpu_destroy_compute_pipeline(s->writer_pipeline);
	spudgpu_destroy_descriptor_pool(s->writer_pool);
	spudgpu_destroy_descriptor_set_layout(s->writer_layout);
	spudgpu_destroy_shader_module(s->writer_module);
	spudgpu_destroy_buffer(s->scratch);
	spudgpu_destroy_buffer(s->instances);
	spudgpu_destroy_buffer(s->staging_instances);
	spudgpu_destroy_buffer(s->staging_geometry);
	s->writer_pipeline   = NULL;
	s->writer_pool       = NULL;
	s->writer_set        = NULL;
	s->writer_layout     = NULL;
	s->writer_module     = NULL;
	s->scratch           = NULL;
	s->instances         = NULL;
	s->staging_instances = NULL;
	s->staging_geometry  = NULL;
}

static void rt_destroy(RtSample *s) {
	if (s->queue)
		spudgpu_queue_wait_idle(s->queue);
	rt_destroy_setup_objects(s);
	// The top level before the bottom levels it was built over.
	spudgpu_destroy_acceleration_structure(s->top);
	for (uint32_t i = 0; i < 2; ++i)
		spudgpu_destroy_acceleration_structure(s->bottoms[i]);
	spudgpu_destroy_buffer(s->geometry);
	spudgpu_destroy_buffer(s->scene);
	spudgpu_destroy_compute_pipeline(s->trace_pipeline);
	spudgpu_destroy_descriptor_pool(s->trace_pool);
	spudgpu_destroy_descriptor_set_layout(s->trace_layout);
	spudgpu_destroy_shader_module(s->trace_module);
	spudgpu_destroy_shader_pipeline(s->present_pipeline);
	spudgpu_destroy_descriptor_pool(s->present_pool);
	spudgpu_destroy_descriptor_set_layout(s->present_layout);
	spudgpu_destroy_shader_module(s->present_frag);
	spudgpu_destroy_shader_module(s->present_vert);
	spudgpu_destroy_image_view(s->output_view);
	spudgpu_destroy_image(s->output);
	spudgpu_destroy_command_list(s->cmd);
	spudgpu_destroy_command_allocator(s->allocator);
	spudgpu_destroy_swap_chain(s->swap_chain);
	spudgpu_destroy_surface(s->surface);
	spudgpu_destroy_device(s->device);
	spudgpu_destroy_instance(s->instance);
	if (s->window)
		SDL_DestroyWindow(s->window);
}

// ---------------------------------------------------------------------------
// Helpers. Each prints what failed and returns NULL or false.
// ---------------------------------------------------------------------------

static spudgpu_buffer create_buffer(spudgpu_device device, SPUDGPU_MEMORY_KIND kind, SPUDGPU_BUFFER_USAGE usage, uint64_t size, const char *name) {
	const spudgpu_buffer_desc desc = {
	    .base        = {SPUD_STRUCT_TYPE_GPU_BUFFER_DESC, NULL},
	    .usage       = usage,
	    .memory_kind = kind,
	    .size        = size,
	    .debug_name  = name,
	};
	spudgpu_buffer buffer   = NULL;
	const SPUDRESULT result = spudgpu_create_buffer(device, &desc, &buffer);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_create_buffer failed for %s: %s\n", name, spudresult_str(result));
		return NULL;
	}
	return buffer;
}

// [size] bytes of [data] copied into [buffer], which is mapped memory.
static bool fill_buffer(spudgpu_buffer buffer, const void *data, uint64_t size) {
	void *mapped = NULL;
	if (SPUDFAIL(spudgpu_map_buffer(buffer, 0, size, &mapped)) || !mapped) {
		fprintf(stderr, "spudgpu_map_buffer failed\n");
		return false;
	}
	memcpy(mapped, data, (size_t)size);
	spudgpu_flush_buffer(buffer, 0, size);
	spudgpu_unmap_buffer(buffer);
	return true;
}

static spudgpu_buffer_barrier buffer_barrier(spudgpu_buffer buffer, SPUDGPU_RESOURCE_STATE before, SPUDGPU_RESOURCE_STATE after) {
	const spudgpu_buffer_barrier barrier = {
	    .base         = {SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL},
	    .buffer       = buffer,
	    .state_before = before,
	    .state_after  = after,
	};
	return barrier;
}

static spudgpu_acceleration_structure_barrier structure_barrier(spudgpu_acceleration_structure structure, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS before,
                                                                SPUDGPU_ACCELERATION_STRUCTURE_ACCESS after) {
	const spudgpu_acceleration_structure_barrier barrier = {
	    .base          = {SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER, NULL},
	    .structure     = structure,
	    .access_before = before,
	    .access_after  = after,
	};
	return barrier;
}

// [value] rounded up to [alignment], a power of two; false if that doesn't
// fit in 64 bits.
static bool align_up(uint64_t value, uint64_t alignment, uint64_t *out) {
	if (value > UINT64_MAX - (alignment - 1))
		return false;
	*out = (value + alignment - 1) & ~(alignment - 1);
	return true;
}

// A descriptor set layout of [count] bindings 0 to count - 1, of [types],
// read by [stages], with its pool of one set and the set. False, printed,
// on failure; what was made is in the out parameters for rt_destroy().
static bool create_set(spudgpu_device device, const SPUDGPU_DESCRIPTOR_TYPE *types, uint32_t count, SPUDGPU_SHADER_STAGE stages,
                       spudgpu_descriptor_set_layout *out_layout, spudgpu_descriptor_pool *out_pool, spudgpu_descriptor_set *out_set) {
	spudgpu_descriptor_set_layout_desc layout_desc = {.base = {SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC, NULL}, .binding_count = count};
	spudgpu_descriptor_pool_desc pool_desc         = {.base = {SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC, NULL}, .max_sets = 1};
	for (uint32_t i = 0; i < count; ++i) {
		layout_desc.bindings[i].binding         = i;
		layout_desc.bindings[i].descriptor_type = types[i];
		layout_desc.bindings[i].count           = 1;
		layout_desc.bindings[i].stage_flags     = stages;
		// One pool size per type, every binding of it counted.
		uint32_t p = 0;
		while (p < pool_desc.pool_size_count && pool_desc.pool_sizes[p].descriptor_type != types[i])
			++p;
		if (p == pool_desc.pool_size_count) {
			pool_desc.pool_sizes[p].descriptor_type = types[i];
			++pool_desc.pool_size_count;
		}
		++pool_desc.pool_sizes[p].count;
	}
	SPUDRESULT result = spudgpu_create_descriptor_set_layout(device, &layout_desc, out_layout);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_create_descriptor_set_layout failed: %s\n", spudresult_str(result));
		return false;
	}
	result = spudgpu_create_descriptor_pool(device, &pool_desc, out_pool);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_create_descriptor_pool failed: %s\n", spudresult_str(result));
		return false;
	}
	const spudgpu_descriptor_set_desc set_desc = {
	    .base        = {SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC, NULL},
	    .pool        = *out_pool,
	    .set_layouts = {*out_layout},
	    .set_count   = 1,
	};
	result = spudgpu_create_descriptor_sets(device, &set_desc, out_set);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_create_descriptor_sets failed: %s\n", spudresult_str(result));
		return false;
	}
	return true;
}

static spudgpu_compute_pipeline create_compute_pipeline(spudgpu_device device, spudgpu_shader_module module, spudgpu_descriptor_set_layout layout,
                                                        uint32_t push_bytes) {
	const spudgpu_compute_pipeline_desc desc = {
	    .base                        = {SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC, NULL},
	    .compute_module              = module,
	    .descriptor_set_layouts      = {layout},
	    .descriptor_set_layout_count = 1,
	    .push_constant_ranges        = {{.stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE, .offset = 0, .size = push_bytes}},
	    .push_constant_range_count   = 1,
	};
	spudgpu_compute_pipeline pipeline = NULL;
	const SPUDRESULT result           = spudgpu_create_compute_pipeline(device, &desc, &pipeline);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_create_compute_pipeline failed: %s\n", spudresult_str(result));
		return NULL;
	}
	return pipeline;
}

// ---------------------------------------------------------------------------
// The window, the device and what every mode has: the output image and the
// pass that presents it.
// ---------------------------------------------------------------------------

static const char *mode_title(RtMode mode) {
	switch (mode) {
	case RT_MODE_HELLO:
		return "SpudGPU Raytracing - Hello World";
	case RT_MODE_LIGHTING:
		return "SpudGPU Raytracing - Simple Lighting";
	case RT_MODE_PROCEDURAL:
		return "SpudGPU Raytracing - Procedural Geometry";
	}
	return "SpudGPU Raytracing";
}

static bool rt_create_device(RtSample *s, uint32_t device_index) {
	s->window = SDL_CreateWindow(mode_title(s->mode), WINDOW_WIDTH, WINDOW_HEIGHT, 0);
	if (!s->window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		return false;
	}

	const SPUD_ALLOCATOR gpu_allocator             = spud_allocator_system();
	const spudgpu_instance_desc gpu_instance_desc = {
	    .base                = {SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC, NULL},
	    .allocator           = &gpu_allocator,
	    .application_name    = "SpudGPURayTracing",
	    .application_version = 1,
	    .engine_name         = "SpudGPUSamples",
	    .engine_version      = 1,
	};
	if (SPUDFAIL(spudgpu_create_instance(&gpu_instance_desc, &s->instance))) {
		fprintf(stderr, "spudgpu_create_instance failed\n");
		return false;
	}

	const spudgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	if (SPUDFAIL(spudgpu_enumerate_adapters(s->instance, &adapters, &adapter_count)) || adapter_count == 0) {
		fprintf(stderr, "spudgpu_enumerate_adapters failed or listed no adapters\n");
		return false;
	}
	// Without --device, the first adapter that has every feature the device
	// below is created with: chosen by what it can do, never by its name or
	// vendor, so the same choice holds on every backend.
	const SPUDGPU_FEATURES required_features = SPUDGPU_FEATURE_PRESENT | SPUDGPU_FEATURE_RAY_QUERY;
	if (device_index == RT_DEVICE_FIRST_CAPABLE) {
		for (uint32_t i = 0; i < adapter_count; ++i) {
			SPUDGPU_ADAPTER_PROPERTIES probe = {.base = {SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES, NULL}};
			if (SPUDFAIL(spudgpu_get_adapter_properties(adapters[i], &probe)))
				continue;
			if ((probe.supported_features & required_features) == required_features) {
				device_index = i;
				break;
			}
		}
		if (device_index == RT_DEVICE_FIRST_CAPABLE) {
			fprintf(stderr, "None of the %u adapters supports ray queries and presenting\n", adapter_count);
			return false;
		}
	}
	if (device_index >= adapter_count) {
		fprintf(stderr, "--device %u: there are %u adapters\n", device_index, adapter_count);
		return false;
	}
	const spudgpu_adapter adapter = adapters[device_index];

	// The adapter's properties, with its ray query limits chained on: the
	// scratch alignment the builds below keep.
	memset(&s->limits, 0, sizeof(s->limits));
	s->limits.base.type                           = SPUD_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS;
	SPUDGPU_ADAPTER_PROPERTIES adapter_properties = {.base = {SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES, &s->limits.base}};
	if (SPUDFAIL(spudgpu_get_adapter_properties(adapter, &adapter_properties))) {
		fprintf(stderr, "spudgpu_get_adapter_properties failed\n");
		return false;
	}
	// The one shader format the adapter takes: each module below is made
	// from the embedded code in it.
	s->shader_format = spudgpusamples_shader_format(adapter_properties.shader_formats);
	printf("Using device: %s\n", adapter_properties.description);

	if (!(adapter_properties.supported_features & SPUDGPU_FEATURE_RAY_QUERY)) {
		fprintf(stderr, "This device/driver doesn't support ray queries (no SPUDGPU_FEATURE_RAY_QUERY)\n");
		return false;
	}

	// A window to present to, one direct queue, and exactly the features
	// the sample uses: a device has only what it is created with.
	const spudgpu_device_desc device_desc = {
	    .base               = {SPUD_STRUCT_TYPE_GPU_DEVICE_DESC, NULL},
	    .required_features  = required_features,
	    .direct_queue_count = 1,
	    // What the sample's descriptor pools hold together, with room to
	    // spare: the caller's to choose, and refused past the adapter's
	    // limit. No sampler: the present pass loads texels.
	    .max_descriptors    = MAX_DESCRIPTORS,
	    .max_samplers       = 0,
	    .debug_name         = "SpudGPURayTracing",
	};
	SPUDRESULT result = spudgpu_create_device(adapter, &device_desc, &s->device);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_create_device failed: %s\n", spudresult_str(result));
		return false;
	}
	spudgpu_get_command_queue(s->device, SPUDGPU_COMMAND_LIST_TYPE_DIRECT, 0, &s->queue);

	result = spudgpu_get_acceleration_structure_instance_layout(s->device, &s->instance_layout);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_get_acceleration_structure_instance_layout failed: %s\n", spudresult_str(result));
		return false;
	}
	printf("Instances: %u bytes each (%s), at %u-byte alignment\n", s->instance_layout.size,
	       s->instance_layout.kind == SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 ? "column major, Metal's" : "row major",
	       s->instance_layout.alignment);

	s->surface = spudgpu_create_surface_from_sdl3(s->instance, s->window);
	if (!s->surface) {
		fprintf(stderr, "spudgpu_create_surface_from_sdl3 failed\n");
		return false;
	}
	const spudgpu_swap_chain_desc swap_chain_desc = {
	    .base    = {SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC, NULL},
	    .surface = s->surface,
	    .queue   = s->queue,
	    .width   = WINDOW_WIDTH,
	    .height  = WINDOW_HEIGHT,
#if SPUDGPU_COMPILE_METAL_API
	    .buffer_count = 1,
#else
	    .buffer_count = 2,
#endif
	    .format          = SPUDGPU_FORMAT_B8G8R8A8_UNORM,
	    .present_mode    = SPUDGPU_PRESENT_MODE_FIFO,
	    // The window can't be resized and the back buffers are its size, so
	    // nothing is scaled; ONE_TO_ONE is the scaling every surface takes.
	    .present_scaling = SPUDGPU_PRESENT_SCALING_ONE_TO_ONE,
	    .fullscreen_mode = SPUDGPU_FULLSCREEN_MODE_WINDOWED,
	};
	if (SPUDFAIL(spudgpu_create_swap_chain(s->device, &swap_chain_desc, &s->swap_chain))) {
		fprintf(stderr, "spudgpu_create_swap_chain failed\n");
		return false;
	}

	const spudgpu_command_allocator_desc allocator_desc = {.base = {SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC, NULL}, .type = SPUDGPU_COMMAND_LIST_TYPE_DIRECT};
	if (SPUDFAIL(spudgpu_create_command_allocator(s->device, &allocator_desc, &s->allocator)) ||
	    SPUDFAIL(spudgpu_create_command_list(s->allocator, &s->cmd))) {
		fprintf(stderr, "spudgpu_create_command_allocator or _list failed\n");
		return false;
	}
	return true;
}

// The image the trace writes, the window's size, as the originals' output
// texture (R8G8B8A8_UNORM, written as a storage image), and the pass that
// draws it onto the back buffer: one triangle over the whole target, each
// pixel loading its texel.
static bool rt_create_output(RtSample *s) {
	const spudgpu_image_desc image_desc = {
	    .base         = {SPUD_STRUCT_TYPE_GPU_IMAGE_DESC, NULL},
	    .usage        = SPUDGPU_IMAGE_USAGE_STORAGE | SPUDGPU_IMAGE_USAGE_SAMPLED,
	    .type         = SPUDGPU_IMAGE_TYPE_2D,
	    .format       = SPUDGPU_FORMAT_R8G8B8A8_UNORM,
	    .width        = WINDOW_WIDTH,
	    .height       = WINDOW_HEIGHT,
	    .depth        = 1,
	    .array_layers = 1,
	    .mip_levels   = 1,
	    .debug_name   = "trace output",
	};
	if (SPUDFAIL(spudgpu_create_image(s->device, &image_desc, &s->output))) {
		fprintf(stderr, "spudgpu_create_image (trace output) failed\n");
		return false;
	}
	const spudgpu_image_view_desc view_desc = {
	    .base              = {SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC, NULL},
	    .parent_image      = s->output,
	    .type              = SPUDGPU_IMAGE_VIEW_TYPE_2D,
	    .subresource_range = {.aspect_mask = 0x1 /* COLOR, Vulkan-aligned - see spudgpu.h */, .base_mip_level = 0, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
	};
	if (SPUDFAIL(spudgpu_create_image_view(s->output, &view_desc, &s->output_view))) {
		fprintf(stderr, "spudgpu_create_image_view (trace output) failed\n");
		return false;
	}

	s->present_vert = spudgpusamples_create_shader_module(s->device, s->shader_format, &rt_present_vert_shader, SPUDGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
	s->present_frag = spudgpusamples_create_shader_module(s->device, s->shader_format, &rt_present_frag_shader, SPUDGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	if (!s->present_vert || !s->present_frag)
		return false;
	const SPUDGPU_DESCRIPTOR_TYPE types[1] = {SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
	if (!create_set(s->device, types, 1, SPUDGPU_SHADER_STAGE_FRAGMENT, &s->present_layout, &s->present_pool, &s->present_set))
		return false;
	const spudgpu_descriptor_image_info image_info = {.image_view = s->output_view, .image_layout = SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY};
	const spudgpu_write_descriptor_set write       = {
	          .base             = {SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL},
	          .dst_set          = s->present_set,
	          .dst_binding      = 0,
	          .descriptor_count = 1,
	          .descriptor_type  = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	          .image_info       = &image_info,
    };
	spudgpu_update_descriptor_sets(s->device, &write, 1);

	const spudgpu_shader_pipeline_desc pipeline_desc = {
	    .base                        = {SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC, NULL},
	    .debug_name                  = "present",
	    .vertex_module               = s->present_vert,
	    .fragment_module             = s->present_frag,
	    .primitive_topology          = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	    .cull_mode                   = SPUDGPU_CULL_MODE_NONE,
	    .color_attachment_formats    = {SPUDGPU_FORMAT_B8G8R8A8_UNORM},
	    .color_attachment_count      = 1,
	    .depth_format                = SPUDGPU_FORMAT_UNKNOWN,
	    .descriptor_set_layouts      = {s->present_layout},
	    .descriptor_set_layout_count = 1,
	};
	if (SPUDFAIL(spudgpu_create_shader_pipeline(s->device, &pipeline_desc, &s->present_pipeline))) {
		fprintf(stderr, "spudgpu_create_shader_pipeline (present) failed\n");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The scene of each mode: the originals' geometry, staged into a buffer of
// the device's memory, and the inputs of its bottom levels.
// ---------------------------------------------------------------------------

// The hello mode: D3D12RaytracingHelloWorld's triangle, in its screen
// space (y down), at depth 1, wound as the original winds it.
typedef struct RtHelloGeometry {
	float vertices[9];
	uint16_t indices[3];
} RtHelloGeometry;

static const RtHelloGeometry hello_geometry = {
    {0.0f, -0.7f, 1.0f, -0.7f, 0.7f, 1.0f, 0.7f, 0.7f, 1.0f},
    {0, 1, 2},
};

// The lighting mode: D3D12RaytracingSimpleLighting's cube, 24 vertices
// (four a face, each with the face's normal) and 36 indices.
typedef struct RtCubeGeometry {
	RtVertex vertices[24];
	uint16_t indices[36];
} RtCubeGeometry;

static const RtCubeGeometry cube_geometry = {
    {
        {{-1.0f, 1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
        {{1.0f, 1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
        {{1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{-1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},

        {{-1.0f, -1.0f, -1.0f}, {0.0f, -1.0f, 0.0f}},
        {{1.0f, -1.0f, -1.0f}, {0.0f, -1.0f, 0.0f}},
        {{1.0f, -1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
        {{-1.0f, -1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},

        {{-1.0f, -1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}},
        {{-1.0f, -1.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}},
        {{-1.0f, 1.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}},
        {{-1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}},

        {{1.0f, -1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{1.0f, -1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
        {{1.0f, 1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
        {{1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},

        {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}},
        {{1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}},
        {{1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}},
        {{-1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}},

        {{-1.0f, -1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{1.0f, -1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {{-1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
    },
    {
        3, 1, 0, 2, 1, 3,
        6, 4, 5, 7, 4, 6,
        11, 9, 8, 10, 9, 11,
        14, 12, 13, 15, 12, 14,
        19, 17, 16, 18, 17, 19,
        22, 20, 21, 23, 20, 22,
    },
};

// The procedural mode: D3D12RaytracingProceduralGeometry's ground plane (a
// unit quad, scaled out by its instance) and its AABBs, one a primitive,
// each six floats (min x, y, z, max x, y, z), at a multiple of 8 bytes.
typedef struct RtProceduralGeometry {
	RtVertex plane_vertices[4];
	uint16_t plane_indices[6];
	uint16_t padding[2];
	float aabbs[RT_PROCEDURAL_PRIMITIVE_COUNT][6];
} RtProceduralGeometry;

// AABB data is at a multiple of 8 bytes.
RT_CHECK(aabb_offset, offsetof(RtProceduralGeometry, aabbs) % 8 == 0);

// The original's AABB grid: AABBs 2 wide, 2 apart, on a 4 x 1 x 4 grid
// centred on the origin.
#define RT_AABB_WIDTH 2.0f
#define RT_AABB_DISTANCE 2.0f

// One procedural primitive, as the original's BuildProceduralGeometryAABBs,
// UpdateAABBPrimitiveAttributes and InitializeScene set it up, in the
// order of the AABB bottom level's geometries.
typedef struct RtPrimitiveDesc {
	float grid[3];  // its AABB's place on the grid
	float size[3];  // its AABB's size
	float scale[3]; // from local space to the AABB
	bool rotates;   // turns with the animation
	uint32_t kind;  // rt_primitives.slang's KIND_*
	uint32_t type;  // the primitive within the kind
	float albedo[4];
	float reflectance;
	float diffuse;
	float specular;
	float specular_power;
	float step_scale;
} RtPrimitiveDesc;

#define RT_GREEN {0.1f, 1.0f, 0.5f, 1.0f}
#define RT_RED {1.0f, 0.5f, 0.5f, 1.0f}
#define RT_YELLOW {1.0f, 1.0f, 0.5f, 1.0f}
// The original's ChromiumReflectance, from Hoffman's "Physics and Math of
// Shading" (SIGGRAPH 2015).
#define RT_CHROMIUM {0.549f, 0.556f, 0.554f, 1.0f}

static const RtPrimitiveDesc procedural_primitives[RT_PROCEDURAL_PRIMITIVE_COUNT] = {
    // Analytic: a box, and three spheres.
    {{3.0f, 0.0f, 0.0f}, {2.0f, 3.0f, 2.0f}, {1.0f, 1.5f, 1.0f}, false, 0, 0, RT_RED, 0.0f, 0.9f, 0.7f, 50.0f, 1.0f},
    {{2.25f, 0.0f, 0.75f}, {3.0f, 3.0f, 3.0f}, {1.5f, 1.5f, 1.5f}, true, 0, 1, RT_CHROMIUM, 1.0f, 0.9f, 0.7f, 50.0f, 1.0f},
    // Volumetric: metaballs.
    {{0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 3.0f}, {1.5f, 1.5f, 1.5f}, true, 1, 0, RT_CHROMIUM, 1.0f, 0.9f, 0.7f, 50.0f, 1.0f},
    // Signed distance: mini spheres, an intersected round cube, a square
    // torus, a twisted torus, a cog, cylinders and a fractal pyramid.
    {{2.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, {1.0f, 1.0f, 1.0f}, false, 2, 0, RT_GREEN, 0.0f, 0.9f, 0.7f, 50.0f, 1.0f},
    {{0.0f, 0.0f, 2.0f}, {2.0f, 2.0f, 2.0f}, {1.0f, 1.0f, 1.0f}, false, 2, 1, RT_GREEN, 0.0f, 0.9f, 0.7f, 50.0f, 1.0f},
    {{0.75f, -0.1f, 2.25f}, {3.0f, 3.0f, 3.0f}, {1.5f, 1.5f, 1.5f}, false, 2, 2, RT_CHROMIUM, 1.0f, 0.9f, 0.7f, 50.0f, 1.0f},
    {{0.0f, 0.0f, 1.0f}, {2.0f, 2.0f, 2.0f}, {1.0f, 1.0f, 1.0f}, true, 2, 3, RT_YELLOW, 0.0f, 1.0f, 0.7f, 50.0f, 0.5f},
    {{1.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, {1.0f, 1.0f, 1.0f}, true, 2, 4, RT_YELLOW, 0.0f, 1.0f, 0.1f, 2.0f, 1.0f},
    {{0.0f, 0.0f, 3.0f}, {2.0f, 3.0f, 2.0f}, {1.0f, 1.5f, 1.0f}, false, 2, 5, RT_RED, 0.0f, 0.9f, 0.7f, 50.0f, 1.0f},
    {{2.0f, 0.0f, 2.0f}, {6.0f, 6.0f, 6.0f}, {3.0f, 3.0f, 3.0f}, false, 2, 6, RT_GREEN, 0.0f, 1.0f, 0.1f, 4.0f, 0.8f},
};

// What rt_create_levels() builds: one or two bottom levels and a top level.
typedef struct RtBuildPlan {
	spudgpu_acceleration_structure_geometry triangles;
	spudgpu_acceleration_structure_geometry boxes[RT_PROCEDURAL_PRIMITIVE_COUNT];
	spudgpu_acceleration_structure_inputs bottom_inputs[2];
	spudgpu_acceleration_structure_inputs top_inputs;
	uint64_t bottom_scratch_offsets[2];
	uint64_t top_scratch_offset;
} RtBuildPlan;

// A geometry of indexed triangles: positions [stride] apart from
// [vertex_offset] of [buffer], 16-bit indices from [index_offset].
static spudgpu_acceleration_structure_geometry indexed_triangles(spudgpu_buffer buffer, uint64_t vertex_offset, uint64_t stride, uint32_t vertex_count,
                                                                 uint64_t index_offset, uint32_t index_count) {
	const spudgpu_acceleration_structure_geometry geometry = {
	    .kind         = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES,
	    // The originals': opaque, no any-hit decision to make.
	    .flags        = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE,
	    .data         = buffer,
	    .data_offset  = vertex_offset,
	    .data_stride  = stride,
	    .data_count   = vertex_count,
	    .index_format = SPUDGPU_FORMAT_R16_UINT,
	    .indices      = buffer,
	    .index_offset = index_offset,
	    .index_count  = index_count,
	};
	return geometry;
}

static spudgpu_acceleration_structure_inputs bottom_inputs(const spudgpu_acceleration_structure_geometry *geometries, uint32_t count) {
	const spudgpu_acceleration_structure_inputs inputs = {
	    .base           = {SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS, NULL},
	    .level          = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM,
	    // The originals' build flag.
	    .flags          = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE,
	    .geometries     = geometries,
	    .geometry_count = count,
	};
	return inputs;
}

// [size] bytes of [data] in a new buffer of the device's memory with
// [usage], by way of an upload buffer the setup submission copies from.
static bool stage_geometry(RtSample *s, SPUDGPU_BUFFER_USAGE usage, const void *data, uint64_t size) {
	s->geometry_size    = size;
	s->staging_geometry = create_buffer(s->device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, size, "geometry staging");
	s->geometry         = create_buffer(s->device, SPUDGPU_MEMORY_KIND_DEVICE,
	                                    usage | SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT | SPUDGPU_BUFFER_USAGE_TRANSFER_DST, size, "geometry");
	return s->staging_geometry && s->geometry && fill_buffer(s->staging_geometry, data, size);
}

static bool rt_hello_geometry(RtSample *s, RtBuildPlan *plan) {
	if (!stage_geometry(s, 0, &hello_geometry, sizeof(hello_geometry)))
		return false;
	plan->triangles        = indexed_triangles(s->geometry, 0, sizeof(float) * 3, 3, offsetof(RtHelloGeometry, indices), 3);
	plan->bottom_inputs[0] = bottom_inputs(&plan->triangles, 1);
	s->bottom_count        = 1;
	return true;
}

static bool rt_lighting_geometry(RtSample *s, RtBuildPlan *plan) {
	// The shader reads the normals and indices too: a storage buffer.
	if (!stage_geometry(s, SPUDGPU_BUFFER_USAGE_STORAGE, &cube_geometry, sizeof(cube_geometry)))
		return false;
	plan->triangles        = indexed_triangles(s->geometry, 0, sizeof(RtVertex), 24, offsetof(RtCubeGeometry, indices), 36);
	plan->bottom_inputs[0] = bottom_inputs(&plan->triangles, 1);
	s->bottom_count        = 1;
	return true;
}

// The AABB of primitive [i] on the original's grid.
static void procedural_aabb(uint32_t i, float out[6]) {
	const float grid[3]      = {4.0f, 1.0f, 4.0f};
	const float stride       = RT_AABB_WIDTH + RT_AABB_DISTANCE;
	const RtPrimitiveDesc *p = &procedural_primitives[i];
	for (int axis = 0; axis < 3; ++axis) {
		const float base = -(grid[axis] * RT_AABB_WIDTH + (grid[axis] - 1.0f) * RT_AABB_DISTANCE) / 2.0f;
		out[axis]        = base + p->grid[axis] * stride;
		out[axis + 3]    = out[axis] + p->size[axis];
	}
}

static bool rt_procedural_geometry(RtSample *s, RtBuildPlan *plan) {
	RtProceduralGeometry geometry = {
	    {
	        {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
	        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
	        {{1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
	        {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
	    },
	    {3, 1, 0, 2, 1, 3},
	    {0, 0},
	    {{0.0f}},
	};
	for (uint32_t i = 0; i < RT_PROCEDURAL_PRIMITIVE_COUNT; ++i)
		procedural_aabb(i, geometry.aabbs[i]);
	if (!stage_geometry(s, SPUDGPU_BUFFER_USAGE_STORAGE, &geometry, sizeof(geometry)))
		return false;

	// The plane; and the AABBs one geometry each, as the original has
	// them, so that a candidate's geometry index says which primitive it is.
	plan->triangles = indexed_triangles(s->geometry, 0, sizeof(RtVertex), 4, offsetof(RtProceduralGeometry, plane_indices), 6);
	for (uint32_t i = 0; i < RT_PROCEDURAL_PRIMITIVE_COUNT; ++i) {
		const spudgpu_acceleration_structure_geometry box = {
		    .kind         = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_AABBS,
		    .flags        = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE,
		    .data         = s->geometry,
		    .data_offset  = offsetof(RtProceduralGeometry, aabbs) + (uint64_t)i * sizeof(geometry.aabbs[0]),
		    .data_stride  = sizeof(geometry.aabbs[0]),
		    .data_count   = 1,
		    .index_format = SPUDGPU_FORMAT_UNKNOWN,
		};
		plan->boxes[i] = box;
	}
	plan->bottom_inputs[0] = bottom_inputs(&plan->triangles, 1);
	plan->bottom_inputs[1] = bottom_inputs(plan->boxes, RT_PROCEDURAL_PRIMITIVE_COUNT);
	s->bottom_count        = 2;
	return true;
}

// ---------------------------------------------------------------------------
// The instances, once the bottom levels they name exist.
// ---------------------------------------------------------------------------

static const float identity_transform[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

// An instance of [bottom_level] with [transform] (row-major 3x4) and
// [user_index], seen by every ray (the originals' InstanceMask 1, tested
// against the rays' ~0).
static bool instance_of(spudgpu_acceleration_structure bottom_level, const float transform[12], uint32_t user_index,
                        spudgpu_acceleration_structure_instance *out_instance) {
	memset(out_instance, 0, sizeof(*out_instance));
	memcpy(out_instance->transform, transform, sizeof(out_instance->transform));
	out_instance->user_index = user_index;
	out_instance->mask       = 0x01;
	const SPUDRESULT result  = spudgpu_get_acceleration_structure_reference(bottom_level, &out_instance->bottom_level_reference);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_get_acceleration_structure_reference failed: %s\n", spudresult_str(result));
		return false;
	}
	return true;
}

// [count] instances written in the device's layout by the CPU call into
// [buffer], mapped: all of them in one call.
static bool write_instances(RtSample *s, spudgpu_buffer buffer, const spudgpu_acceleration_structure_instance *instances, uint32_t count) {
	const uint64_t size = (uint64_t)s->instance_layout.size * count;
	void *mapped        = NULL;
	if (SPUDFAIL(spudgpu_map_buffer(buffer, 0, size, &mapped)) || !mapped) {
		fprintf(stderr, "spudgpu_map_buffer (instances) failed\n");
		return false;
	}
	const SPUDRESULT result = spudgpu_write_acceleration_structure_instances(s->device, instances, count, mapped);
	spudgpu_flush_buffer(buffer, 0, size);
	spudgpu_unmap_buffer(buffer);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_write_acceleration_structure_instances failed: %s\n", spudresult_str(result));
		return false;
	}
	return true;
}

// The hello and lighting modes: one instance of the one bottom level where
// the originals place it (identity), written on the CPU straight into an
// upload buffer the top level's build reads.
static bool rt_single_instance(RtSample *s) {
	spudgpu_acceleration_structure_instance instance;
	if (!instance_of(s->bottoms[0], identity_transform, 0, &instance))
		return false;
	s->instances = create_buffer(s->device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, s->instance_layout.size,
	                             "instances");
	return s->instances && write_instances(s, s->instances, &instance, 1);
}

// The procedural mode's two instances, in one buffer of the device's
// memory: the plane's written by the CPU call into an upload buffer the
// setup submission copies from; the AABBs' written in the same submission
// by the sample's GPU writer for the device's layout kind
// (rt_record_setup()), from what [out_gpu_instance] receives.
static bool rt_procedural_instances(RtSample *s, RtWriterInstance *out_gpu_instance) {
	// The original's plane: the unit quad scaled out to 700 AABB widths and
	// their gaps, and moved so that the AABBs stand near one corner of it.
	const float n                   = 700.0f;
	const float width               = n * RT_AABB_WIDTH + (n - 1.0f) * RT_AABB_DISTANCE;
	const float plane_transform[12] = {
	    width, 0.0f, 0.0f, -0.35f * width,
	    0.0f, RT_AABB_WIDTH, 0.0f, 0.0f,
	    0.0f, 0.0f, width, -0.35f * width,
	};
	// The AABBs, moved up half a width onto the plane.
	const float aabb_transform[12] = {
	    1.0f, 0.0f, 0.0f, 0.0f,
	    0.0f, 1.0f, 0.0f, RT_AABB_WIDTH / 2.0f,
	    0.0f, 0.0f, 1.0f, 0.0f,
	};
	spudgpu_acceleration_structure_instance plane;
	spudgpu_acceleration_structure_instance aabbs;
	if (!instance_of(s->bottoms[0], plane_transform, 0, &plane) || !instance_of(s->bottoms[1], aabb_transform, 1, &aabbs))
		return false;

	s->staging_instances = create_buffer(s->device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, s->instance_layout.size,
	                                     "instance staging");
	s->instances = create_buffer(s->device, SPUDGPU_MEMORY_KIND_DEVICE,
	                             SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT | SPUDGPU_BUFFER_USAGE_TRANSFER_DST,
	                             (uint64_t)s->instance_layout.size * 2, "instances");
	if (!s->staging_instances || !s->instances || !write_instances(s, s->staging_instances, &plane, 1))
		return false;

	// The GPU writer for the device's layout kind, its set naming the
	// instance buffer.
	const spudgpusamples_shader *writer = s->instance_layout.kind == SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72
	                                          ? &rt_instances_col72_comp_shader
	                                          : &rt_instances_row64_comp_shader;
	s->writer_module = spudgpusamples_create_shader_module(s->device, s->shader_format, writer, SPUDGPU_SHADER_STAGE_COMPUTE, 1, 1, 1);
	if (!s->writer_module)
		return false;
	const SPUDGPU_DESCRIPTOR_TYPE types[1] = {SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	if (!create_set(s->device, types, 1, SPUDGPU_SHADER_STAGE_COMPUTE, &s->writer_layout, &s->writer_pool, &s->writer_set))
		return false;
	s->writer_pipeline = create_compute_pipeline(s->device, s->writer_module, s->writer_layout, sizeof(RtWriterInstance));
	if (!s->writer_pipeline)
		return false;
	const spudgpu_descriptor_buffer_info buffer_info = {.buffer = s->instances, .offset = 0, .range = (uint64_t)s->instance_layout.size * 2};
	const spudgpu_write_descriptor_set write         = {
	    .base             = {SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL},
	    .dst_set          = s->writer_set,
	    .dst_binding      = 0,
	    .descriptor_count = 1,
	    .descriptor_type  = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	    .buffer_info      = &buffer_info,
	};
	spudgpu_update_descriptor_sets(s->device, &write, 1);

	// What the writer is pushed: the AABBs' instance, the second in the
	// buffer.
	memset(out_gpu_instance, 0, sizeof(*out_gpu_instance));
	memcpy(out_gpu_instance->rows, aabbs.transform, sizeof(out_gpu_instance->rows));
	out_gpu_instance->user_index     = aabbs.user_index;
	out_gpu_instance->mask           = aabbs.mask;
	out_gpu_instance->table_offset   = aabbs.table_offset;
	out_gpu_instance->flags          = aabbs.flags;
	out_gpu_instance->reference_low  = (uint32_t)aabbs.bottom_level_reference;
	out_gpu_instance->reference_high = (uint32_t)(aabbs.bottom_level_reference >> 32);
	out_gpu_instance->out_offset     = s->instance_layout.size;
	return true;
}

// ---------------------------------------------------------------------------
// The levels: sized and created, and one scratch buffer with a range for
// each build at the device's scratch alignment.
// ---------------------------------------------------------------------------

static bool create_structure(RtSample *s, SPUDGPU_ACCELERATION_STRUCTURE_LEVEL level, uint64_t size, uint32_t max_bottom_levels, const char *name,
                             spudgpu_acceleration_structure *out_structure) {
	const spudgpu_acceleration_structure_desc desc = {
	    .base              = {SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC, NULL},
	    .level             = level,
	    .size              = size,
	    .max_bottom_levels = max_bottom_levels,
	    .allow_compaction  = false,
	    .debug_name        = name,
	};
	const SPUDRESULT result = spudgpu_create_acceleration_structure(s->device, &desc, out_structure);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_create_acceleration_structure failed for %s: %s\n", name, spudresult_str(result));
		return false;
	}
	return true;
}

static bool structure_sizes(RtSample *s, const spudgpu_acceleration_structure_inputs *inputs, spudgpu_acceleration_structure_sizes *out_sizes) {
	memset(out_sizes, 0, sizeof(*out_sizes));
	out_sizes->base.type    = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES;
	const SPUDRESULT result = spudgpu_get_acceleration_structure_build_sizes(s->device, inputs, out_sizes);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_get_acceleration_structure_build_sizes failed: %s\n", spudresult_str(result));
		return false;
	}
	return true;
}

// Each bottom level of [plan], sized and created.
static bool rt_create_bottom_levels(RtSample *s, RtBuildPlan *plan, uint64_t *out_scratch_end) {
	static const char *const names[2] = {"bottom level 0", "bottom level 1"};
	uint64_t scratch_end               = 0;
	for (uint32_t i = 0; i < s->bottom_count; ++i) {
		spudgpu_acceleration_structure_sizes sizes;
		if (!structure_sizes(s, &plan->bottom_inputs[i], &sizes) ||
		    !create_structure(s, SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, sizes.storage_size, 0, names[i], &s->bottoms[i]))
			return false;
		printf("%s: %llu bytes, %llu of scratch\n", names[i], (unsigned long long)sizes.storage_size, (unsigned long long)sizes.build_scratch_size);
		plan->bottom_scratch_offsets[i] = scratch_end;
		if (sizes.build_scratch_size > UINT64_MAX - scratch_end ||
		    !align_up(scratch_end + sizes.build_scratch_size, s->limits.scratch_offset_alignment, &scratch_end)) {
			fprintf(stderr, "the builds' scratch doesn't fit in 64 bits\n");
			return false;
		}
	}
	*out_scratch_end = scratch_end;
	return true;
}

// The top level over [instance_count] instances in s->instances, sized and
// created, and the scratch buffer of every build, the top level's from
// [scratch_end].
static bool rt_create_top_level(RtSample *s, RtBuildPlan *plan, uint32_t instance_count, uint64_t scratch_end) {
	const spudgpu_acceleration_structure_inputs top_inputs = {
	    .base               = {SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS, NULL},
	    .level              = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP,
	    .flags              = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE,
	    .instances          = s->instances,
	    .instance_offset    = 0,
	    .instance_count     = instance_count,
	    .bottom_levels      = s->bottoms,
	    .bottom_level_count = s->bottom_count,
	};
	plan->top_inputs = top_inputs;
	spudgpu_acceleration_structure_sizes sizes;
	if (!structure_sizes(s, &plan->top_inputs, &sizes) ||
	    !create_structure(s, SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP, sizes.storage_size, s->bottom_count, "top level", &s->top))
		return false;
	printf("top level: %llu bytes, %llu of scratch\n", (unsigned long long)sizes.storage_size, (unsigned long long)sizes.build_scratch_size);
	plan->top_scratch_offset = scratch_end;
	if (sizes.build_scratch_size > UINT64_MAX - scratch_end) {
		fprintf(stderr, "the builds' scratch doesn't fit in 64 bits\n");
		return false;
	}
	scratch_end += sizes.build_scratch_size;
	s->scratch = create_buffer(s->device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE, scratch_end > 0 ? scratch_end : 1, "scratch");
	return s->scratch != NULL;
}

// ---------------------------------------------------------------------------
// The trace of each mode: its compute pipeline, its set, and the scene's
// constants.
// ---------------------------------------------------------------------------

// The trace's set: the top level and the output image; in the lighting and
// procedural modes also the scene's constants, written once here (what
// changes is pushed every frame), and the geometry buffer.
static bool rt_create_trace(RtSample *s, const spudgpusamples_shader *shader, uint32_t push_bytes, const void *scene, uint64_t scene_size) {
	s->trace_module = spudgpusamples_create_shader_module(s->device, s->shader_format, shader, SPUDGPU_SHADER_STAGE_COMPUTE, TRACE_GROUP_SIZE,
	                                                      TRACE_GROUP_SIZE, 1);
	if (!s->trace_module)
		return false;
	const SPUDGPU_DESCRIPTOR_TYPE types[4] = {SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	                                          SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	const uint32_t binding_count           = scene ? 4u : 2u;
	if (!create_set(s->device, types, binding_count, SPUDGPU_SHADER_STAGE_COMPUTE, &s->trace_layout, &s->trace_pool, &s->trace_set))
		return false;
	s->trace_pipeline = create_compute_pipeline(s->device, s->trace_module, s->trace_layout, push_bytes);
	if (!s->trace_pipeline)
		return false;
	if (scene) {
		s->scene = create_buffer(s->device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_UNIFORM, scene_size, "scene constants");
		if (!s->scene || !fill_buffer(s->scene, scene, scene_size))
			return false;
	}

	const spudgpu_descriptor_image_info image_info   = {.image_view = s->output_view, .image_layout = SPUDGPU_IMAGE_LAYOUT_GENERAL};
	const spudgpu_descriptor_buffer_info scene_info  = {.buffer = s->scene, .offset = 0, .range = scene_size};
	const spudgpu_descriptor_buffer_info geometry_info = {.buffer = s->geometry, .offset = 0, .range = s->geometry_size};
	spudgpu_write_descriptor_set writes[4];
	memset(writes, 0, sizeof(writes));
	for (uint32_t i = 0; i < binding_count; ++i) {
		writes[i].base.type        = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		writes[i].dst_set          = s->trace_set;
		writes[i].dst_binding      = i;
		writes[i].descriptor_count = 1;
		writes[i].descriptor_type  = types[i];
	}
	writes[0].acceleration_structures = &s->top;
	writes[1].image_info              = &image_info;
	writes[2].buffer_info             = &scene_info;
	writes[3].buffer_info             = &geometry_info;
	spudgpu_update_descriptor_sets(s->device, writes, binding_count);
	return true;
}

static RtMaterial material(const float albedo[4], float reflectance, float diffuse, float specular, float specular_power, float step_scale) {
	RtMaterial m;
	memset(&m, 0, sizeof(m));
	memcpy(m.albedo, albedo, sizeof(m.albedo));
	vec4_set(m.coefficients, reflectance, diffuse, specular, specular_power);
	m.step_scale[0] = step_scale;
	return m;
}

// The procedural mode's scene constants: the original's light colours, the
// plane's material and each primitive's material and transform.
static void procedural_scene(RtProceduralScene *scene) {
	memset(scene, 0, sizeof(*scene));
	vec4_set(scene->light_ambient, 0.25f, 0.25f, 0.25f, 1.0f);
	vec4_set(scene->light_diffuse, 0.6f, 0.6f, 0.6f, 1.0f);
	scene->offsets[0]           = (uint32_t)offsetof(RtProceduralGeometry, plane_vertices);
	scene->offsets[1]           = (uint32_t)offsetof(RtProceduralGeometry, plane_indices);
	const float plane_albedo[4] = {0.9f, 0.9f, 0.9f, 1.0f};
	scene->plane                = material(plane_albedo, 0.25f, 1.0f, 0.4f, 50.0f, 1.0f);
	for (uint32_t i = 0; i < RT_PROCEDURAL_PRIMITIVE_COUNT; ++i) {
		const RtPrimitiveDesc *p = &procedural_primitives[i];
		scene->materials[i]      = material(p->albedo, p->reflectance, p->diffuse, p->specular, p->specular_power, p->step_scale);
		float aabb[6];
		procedural_aabb(i, aabb);
		RtPrimitive *primitive = &scene->primitives[i];
		vec4_set(primitive->center, 0.5f * (aabb[0] + aabb[3]), 0.5f * (aabb[1] + aabb[4]), 0.5f * (aabb[2] + aabb[5]), 1.0f);
		vec4_set(primitive->scale, p->scale[0], p->scale[1], p->scale[2], p->rotates ? 1.0f : 0.0f);
		primitive->kind[0] = p->kind;
		primitive->kind[1] = p->type;
	}
}

// ---------------------------------------------------------------------------
// The setup submission: the geometry and instances copied in place, the
// GPU writer's instance, every build, and the barriers between, as
// spudgpu.h states them.
// ---------------------------------------------------------------------------

static bool rt_record_setup(RtSample *s, const RtBuildPlan *plan, const RtWriterInstance *gpu_instance) {
	spudgpu_command_list cmd = s->cmd;
	spudgpu_reset_command_allocator(s->allocator);
	spudgpu_begin_command_list(cmd);

	// The geometry (and the procedural mode's instances) copied from their
	// upload buffers.
	spudgpu_buffer_barrier barriers[3];
	uint32_t barrier_count     = 0;
	barriers[barrier_count++]  = buffer_barrier(s->geometry, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST);
	if (s->staging_instances)
		barriers[barrier_count++] = buffer_barrier(s->instances, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST);
	spudgpu_cmd_pipeline_barrier(cmd, barriers, barrier_count, NULL, 0);
	spudgpu_cmd_copy_buffer(cmd, s->staging_geometry, s->geometry, 0, 0, s->geometry_size);
	barrier_count             = 0;
	barriers[barrier_count++] = buffer_barrier(s->geometry, SPUDGPU_RESOURCE_STATE_COPY_DEST, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT);
	if (s->staging_instances) {
		// The plane's instance, as the CPU call wrote it, first in the
		// buffer; then the AABBs', as the GPU writer writes it, second.
		spudgpu_cmd_copy_buffer(cmd, s->staging_instances, s->instances, 0, 0, s->instance_layout.size);
		barriers[barrier_count++] = buffer_barrier(s->instances, SPUDGPU_RESOURCE_STATE_COPY_DEST, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	barriers[barrier_count++] = buffer_barrier(s->scratch, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	spudgpu_cmd_pipeline_barrier(cmd, barriers, barrier_count, NULL, 0);
	if (s->writer_pipeline) {
		spudgpu_cmd_bind_compute_pipeline(cmd, s->writer_pipeline);
		spudgpu_cmd_bind_descriptor_sets_compute(cmd, s->writer_pipeline, 0, &s->writer_set, 1);
		spudgpu_cmd_push_constants_compute(cmd, s->writer_pipeline, 0, sizeof(*gpu_instance), gpu_instance);
		spudgpu_cmd_dispatch(cmd, 1, 1, 1);
		const spudgpu_buffer_barrier written =
		    buffer_barrier(s->instances, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT);
		spudgpu_cmd_pipeline_barrier(cmd, &written, 1, NULL, 0);
	}

	// The bottom levels, in one call, each with its own scratch range.
	spudgpu_acceleration_structure_build builds[2];
	for (uint32_t i = 0; i < s->bottom_count; ++i) {
		const spudgpu_acceleration_structure_build build = {
		    .base           = {SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD, NULL},
		    .mode           = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD,
		    .inputs         = &plan->bottom_inputs[i],
		    .source         = NULL,
		    .destination    = s->bottoms[i],
		    .scratch        = s->scratch,
		    .scratch_offset = plan->bottom_scratch_offsets[i],
		};
		builds[i] = build;
	}
	SPUDRESULT result = spudgpu_cmd_build_acceleration_structures(cmd, builds, s->bottom_count);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_cmd_build_acceleration_structures (bottom levels) failed: %s\n", spudresult_str(result));
		spudgpu_end_command_list(cmd);
		return false;
	}

	// The top level reads what the bottom levels' builds wrote.
	spudgpu_acceleration_structure_barrier structure_barriers[3];
	for (uint32_t i = 0; i < s->bottom_count; ++i)
		structure_barriers[i] =
		    structure_barrier(s->bottoms[i], SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	structure_barriers[s->bottom_count] =
	    structure_barrier(s->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	spudgpu_cmd_acceleration_structure_barrier(cmd, structure_barriers, s->bottom_count + 1);
	const spudgpu_acceleration_structure_build top_build = {
	    .base           = {SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD, NULL},
	    .mode           = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD,
	    .inputs         = &plan->top_inputs,
	    .source         = NULL,
	    .destination    = s->top,
	    .scratch        = s->scratch,
	    .scratch_offset = plan->top_scratch_offset,
	};
	result = spudgpu_cmd_build_acceleration_structures(cmd, &top_build, 1);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_cmd_build_acceleration_structures (top level) failed: %s\n", spudresult_str(result));
		spudgpu_end_command_list(cmd);
		return false;
	}

	// Every frame's trace reads the top level, and (lighting, procedural)
	// the geometry through a storage slot; the present pass reads the
	// output as a sampled image until a frame's trace writes it.
	const spudgpu_acceleration_structure_barrier traced =
	    structure_barrier(s->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	spudgpu_cmd_acceleration_structure_barrier(cmd, &traced, 1);
	if (s->mode != RT_MODE_HELLO) {
		const spudgpu_buffer_barrier read =
		    buffer_barrier(s->geometry, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS);
		spudgpu_cmd_pipeline_barrier(cmd, &read, 1, NULL, 0);
	}
	spudgpu_cmd_image_barrier(cmd, s->output, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);

	spudgpu_end_command_list(cmd);
	result = spudgpu_submit_command_lists(s->queue, &cmd, 1);
	if (SPUDFAIL(result)) {
		fprintf(stderr, "spudgpu_submit_command_lists (setup) failed: %s\n", spudresult_str(result));
		return false;
	}
	spudgpu_queue_wait_idle(s->queue);
	return true;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
	// --mode hello|lighting|procedural: which of the originals. hello if
	// not given.
	// --frames N: render N frames, then leave the loop as closing the window
	// does, through the same cleanup. Without it the sample runs until the
	// window is closed.
	// --device N: use the Nth adapter spudgpu_enumerate_adapters lists. If
	// not given, the first that supports ray queries and presenting.
	RtMode mode           = RT_MODE_HELLO;
	uint64_t frame_limit  = 0;
	uint32_t device_index = RT_DEVICE_FIRST_CAPABLE;
	for (int i = 1; i + 1 < argc; ++i) {
		if (strcmp(argv[i], "--frames") == 0)
			frame_limit = strtoull(argv[i + 1], NULL, 10);
		else if (strcmp(argv[i], "--device") == 0)
			device_index = (uint32_t)strtoul(argv[i + 1], NULL, 10);
		else if (strcmp(argv[i], "--mode") == 0) {
			if (strcmp(argv[i + 1], "hello") == 0)
				mode = RT_MODE_HELLO;
			else if (strcmp(argv[i + 1], "lighting") == 0)
				mode = RT_MODE_LIGHTING;
			else if (strcmp(argv[i + 1], "procedural") == 0)
				mode = RT_MODE_PROCEDURAL;
			else {
				fprintf(stderr, "--mode %s: hello, lighting or procedural\n", argv[i + 1]);
				return 1;
			}
		}
	}
	uint64_t frames_run = 0;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	int exit_code = 1;
	RtSample s;
	memset(&s, 0, sizeof(s));
	s.mode = mode;
	if (!rt_create_device(&s, device_index) || !rt_create_output(&s))
		goto cleanup;

	// The scene, its levels and its trace.
	RtBuildPlan plan;
	memset(&plan, 0, sizeof(plan));
	RtWriterInstance gpu_instance;
	memset(&gpu_instance, 0, sizeof(gpu_instance));
	uint64_t scratch_end = 0;
	bool made            = false;
	switch (mode) {
	case RT_MODE_HELLO:
		made = rt_hello_geometry(&s, &plan) && rt_create_bottom_levels(&s, &plan, &scratch_end) && rt_single_instance(&s) &&
		       rt_create_top_level(&s, &plan, 1, scratch_end) && rt_create_trace(&s, &rt_hello_comp_shader, sizeof(RtHelloFrame), NULL, 0);
		break;
	case RT_MODE_LIGHTING: {
		RtLightingScene scene;
		memset(&scene, 0, sizeof(scene));
		vec4_set(scene.light_ambient, 0.5f, 0.5f, 0.5f, 1.0f);
		vec4_set(scene.light_diffuse, 0.5f, 0.0f, 0.0f, 1.0f);
		vec4_set(scene.albedo, 1.0f, 1.0f, 1.0f, 1.0f);
		scene.offsets[0] = (uint32_t)offsetof(RtCubeGeometry, vertices);
		scene.offsets[1] = (uint32_t)offsetof(RtCubeGeometry, indices);
		made = rt_lighting_geometry(&s, &plan) && rt_create_bottom_levels(&s, &plan, &scratch_end) && rt_single_instance(&s) &&
		       rt_create_top_level(&s, &plan, 1, scratch_end) &&
		       rt_create_trace(&s, &rt_lighting_comp_shader, sizeof(RtLightingFrame), &scene, sizeof(scene));
		break;
	}
	case RT_MODE_PROCEDURAL: {
		RtProceduralScene scene;
		procedural_scene(&scene);
		made = rt_procedural_geometry(&s, &plan) && rt_create_bottom_levels(&s, &plan, &scratch_end) && rt_procedural_instances(&s, &gpu_instance) &&
		       rt_create_top_level(&s, &plan, 2, scratch_end) &&
		       rt_create_trace(&s, &rt_procedural_comp_shader, sizeof(RtProceduralFrame), &scene, sizeof(scene));
		break;
	}
	}
	if (!made || !rt_record_setup(&s, &plan, &gpu_instance))
		goto cleanup;
	// The setup submission has completed: its inputs, scratch, staging and
	// the GPU writer are done with.
	rt_destroy_setup_objects(&s);

	// What each frame pushes. The originals' scenes: the hello mode's
	// viewport and stencil window; the lighting and procedural modes'
	// cameras and lights.
	RtHelloFrame hello_frame;
	memset(&hello_frame, 0, sizeof(hello_frame));
	{
		const float aspect = (float)WINDOW_WIDTH / (float)WINDOW_HEIGHT;
		const float border = 0.1f;
		vec4_set(hello_frame.viewport, -1.0f, -1.0f, 1.0f, 1.0f);
		if (WINDOW_WIDTH <= WINDOW_HEIGHT)
			vec4_set(hello_frame.stencil, -1.0f + border, -1.0f + border * aspect, 1.0f - border, 1.0f - border * aspect);
		else
			vec4_set(hello_frame.stencil, -1.0f + border / aspect, -1.0f + border, 1.0f - border / aspect, 1.0f - border);
	}
	Camera camera = camera_init(mode == RT_MODE_PROCEDURAL ? vec3(0.0f, 5.3f, -17.0f) : vec3(0.0f, 2.0f, -5.0f));
	Vec3 light    = mode == RT_MODE_PROCEDURAL ? vec3(0.0f, 18.0f, -20.0f) : vec3(0.0f, 1.8f, -3.0f);
	// The procedural original's animation switches (C, G, L) and their
	// defaults; the lighting original turns its camera and light always.
	bool animate_camera   = mode == RT_MODE_LIGHTING;
	bool animate_geometry = true;
	bool animate_light    = mode == RT_MODE_LIGHTING;
	float geometry_time   = 0.0f;
	if (mode == RT_MODE_PROCEDURAL)
		printf("C: camera animation, G: geometry animation, L: light animation\n");

	uint64_t last_ticks = SDL_GetTicksNS();
	bool running        = true;
	while (running) {
		if (frame_limit != 0 && frames_run == frame_limit)
			break;
		++frames_run;
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT)
				running = false;
			if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
				switch (event.key.scancode) {
				case SDL_SCANCODE_ESCAPE:
					running = false;
					break;
				case SDL_SCANCODE_C:
					animate_camera = mode == RT_MODE_PROCEDURAL ? !animate_camera : animate_camera;
					break;
				case SDL_SCANCODE_G:
					animate_geometry = !animate_geometry;
					break;
				case SDL_SCANCODE_L:
					animate_light = mode == RT_MODE_PROCEDURAL ? !animate_light : animate_light;
					break;
				default:
					break;
				}
			}
		}

		const uint64_t now = SDL_GetTicksNS();
		const float dt     = (float)(now - last_ticks) / 1e9f;
		last_ticks         = now;
		// The lighting original's camera goes round in 24 seconds, the
		// procedural one's in 48; both lights in 8, the other way.
		if (animate_camera)
			camera_rotate_y(&camera, 2.0f * RT_PI * dt / (mode == RT_MODE_PROCEDURAL ? 48.0f : 24.0f));
		if (animate_light)
			light = vec3_rotate_y(light, -2.0f * RT_PI * dt / 8.0f);
		if (animate_geometry)
			geometry_time += dt;

		const uint32_t image_index               = spudgpu_swap_chain_acquire_next_image(s.swap_chain);
		const spudgpu_image_view backbuffer_view = spudgpu_get_swap_chain_image_view(s.swap_chain, image_index);

		spudgpu_reset_command_allocator(s.allocator);
		spudgpu_begin_command_list(s.cmd);

		// The trace: the output image written as a storage image.
		spudgpu_cmd_image_barrier(s.cmd, s.output, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, SPUDGPU_IMAGE_LAYOUT_GENERAL);
		spudgpu_cmd_bind_compute_pipeline(s.cmd, s.trace_pipeline);
		spudgpu_cmd_bind_descriptor_sets_compute(s.cmd, s.trace_pipeline, 0, &s.trace_set, 1);
		switch (mode) {
		case RT_MODE_HELLO:
			spudgpu_cmd_push_constants_compute(s.cmd, s.trace_pipeline, 0, sizeof(hello_frame), &hello_frame);
			break;
		case RT_MODE_LIGHTING: {
			RtLightingFrame frame;
			camera_projection_to_world(&camera, 1.0f, 125.0f, frame.projection_to_world);
			vec4_set(frame.camera_position, camera.eye.x, camera.eye.y, camera.eye.z, 1.0f);
			vec4_set(frame.light_position, light.x, light.y, light.z, 1.0f);
			spudgpu_cmd_push_constants_compute(s.cmd, s.trace_pipeline, 0, sizeof(frame), &frame);
			break;
		}
		case RT_MODE_PROCEDURAL: {
			RtProceduralFrame frame;
			camera_projection_to_world(&camera, 0.01f, 125.0f, frame.projection_to_world);
			vec4_set(frame.camera_position, camera.eye.x, camera.eye.y, camera.eye.z, 1.0f);
			vec4_set(frame.light_position, light.x, light.y, light.z, 1.0f);
			vec4_set(frame.time, geometry_time, 0.0f, 0.0f, 0.0f);
			spudgpu_cmd_push_constants_compute(s.cmd, s.trace_pipeline, 0, sizeof(frame), &frame);
			break;
		}
		}
		spudgpu_cmd_dispatch(s.cmd, (WINDOW_WIDTH + TRACE_GROUP_SIZE - 1) / TRACE_GROUP_SIZE, (WINDOW_HEIGHT + TRACE_GROUP_SIZE - 1) / TRACE_GROUP_SIZE,
		                     1);
		spudgpu_cmd_image_barrier(s.cmd, s.output, SPUDGPU_IMAGE_LAYOUT_GENERAL, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);

		// The present pass: the output drawn onto the back buffer.
		spudgpu_cmd_image_barrier_view(s.cmd, backbuffer_view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		const spudgpu_rendering_begin_desc rendering_desc = {
		    .base                   = {SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC, NULL},
		    .color_attachments      = {{.image_view = backbuffer_view, .load_op = SPUDGPU_LOAD_OP_CLEAR, .store_op = SPUDGPU_STORE_OP_STORE, .clear_color = {0.0f, 0.0f, 0.0f, 1.0f}}},
		    .color_attachment_count = 1,
		    .width                  = WINDOW_WIDTH,
		    .height                 = WINDOW_HEIGHT,
		};
		spudgpu_cmd_begin_rendering(s.cmd, &rendering_desc);
		spudgpu_cmd_bind_pipeline(s.cmd, s.present_pipeline);
		spudgpu_cmd_bind_descriptor_sets(s.cmd, s.present_pipeline, 0, &s.present_set, 1);
		const SPUDGPU_VIEWPORT viewport = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT, .minDepth = 0.0f, .maxDepth = 1.0f};
		spudgpu_cmd_set_viewports(s.cmd, 0, 1, &viewport);
		const SPUDGPU_SCISSOR_RECT scissor = {.x = 0, .y = 0, .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
		spudgpu_cmd_set_scissor_rects(s.cmd, 0, 1, &scissor);
		spudgpu_cmd_draw(s.cmd, 3, 0);
		spudgpu_cmd_end_rendering(s.cmd);
		spudgpu_cmd_image_barrier_view(s.cmd, backbuffer_view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, SPUDGPU_IMAGE_LAYOUT_PRESENT_SRC);

		spudgpu_end_command_list(s.cmd);
		spudgpu_submit_command_lists_synced(s.queue, &s.cmd, 1, s.swap_chain);
		spudgpu_swap_chain_present(s.swap_chain);
	}

	exit_code = 0;

cleanup:
	rt_destroy(&s);
	SDL_Quit();
	return exit_code;
}
