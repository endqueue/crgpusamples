// Must match TRIANGLE_COUNT/TRIANGLE_HALF_WIDTH/TRIANGLE_DEPTH/CULL_OFFSET in
// ../main.c and numthreads below matching COMPUTE_THREAD_BLOCK_SIZE there --
// all four are genuinely compile-time constants in the original D3D12 sample
// too (never updated at runtime), so they're baked in here rather than
// plumbed through push constants.
static const uint TRIANGLE_COUNT = 256;
static const float TRIANGLE_HALF_WIDTH = 0.05;
static const float TRIANGLE_DEPTH = 1.0;
static const float CULL_OFFSET = 0.5;

struct SceneConstantBuffer {
	float4 velocity;
	float4 offset;
	float4 color;
};

struct IndirectCommand {
	uint vertex_count;
	uint instance_count;
	uint first_vertex;
	uint first_instance;
};

struct SceneBuffer {
	SceneConstantBuffer triangles[TRIANGLE_COUNT];
};

struct InputCommands {
	IndirectCommand commands[TRIANGLE_COUNT];
};

struct ProjectionBuffer {
	float4x4 projection;
};

[[vk::binding(0, 0)]] ConstantBuffer<SceneBuffer> scene : register(b0, space0);
[[vk::binding(1, 0)]] ConstantBuffer<InputCommands> input_commands : register(b1, space0);
// IndirectCommand[TRIANGLE_COUNT], 16 bytes each: a byte-address buffer,
// which is what a storage buffer's raw UAV on D3D12 is.
[[vk::binding(2, 0)]] RWByteAddressBuffer output_commands : register(u2, space0);
[[vk::binding(3, 0)]] ConstantBuffer<ProjectionBuffer> proj : register(b3, space0);

[shader("compute")]
[numthreads(128, 1, 1)]
void cull_comp(uint3 dispatch_id : SV_DispatchThreadID) {
	uint index = dispatch_id.x;
	if (index >= TRIANGLE_COUNT)
		return;

	float4 offset = scene.triangles[index].offset;

	// Project the triangle's left and right extent into clip space, same
	// test as the original compute.hlsl -- only draw triangles whose
	// projected span still overlaps [-CULL_OFFSET, CULL_OFFSET].
	float4 left = mul(proj.projection, float4(-TRIANGLE_HALF_WIDTH, 0.0, TRIANGLE_DEPTH, 1.0) + offset);
	left /= left.w;

	float4 right = mul(proj.projection, float4(TRIANGLE_HALF_WIDTH, 0.0, TRIANGLE_DEPTH, 1.0) + offset);
	right /= right.w;

	bool visible = (-CULL_OFFSET < right.x) && (left.x < CULL_OFFSET);

	IndirectCommand cmd = input_commands.commands[index];
	if (!visible)
		cmd.vertex_count = 0;

	output_commands.Store4(index * 16u, uint4(cmd.vertex_count, cmd.instance_count, cmd.first_vertex, cmd.first_instance));
}
