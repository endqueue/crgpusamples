// 128/64/126 match the meshlet generator's limits used across the whole
// D3D12MeshShaders sample family (see ../../../README.md) - not a SpudGPU
// constraint.
static const uint MAX_VERTICES = 64;
static const uint MAX_PRIMITIVES = 126;

// 16 bytes per element in the meshlet buffer.
struct Meshlet {
	uint vert_count;
	uint vert_offset;
	uint prim_count;
	uint prim_offset;
};

// The storage buffers are byte-address buffers, which is what a storage
// buffer's raw UAV on D3D12 is, read at byte offsets computed by hand. The
// vertex file's data (and the host upload in main.c) is tightly packed at
// 24 bytes/vertex (2x float3, no padding), so get_vertex_position/normal
// below read it at the real 24-byte-per-vertex offsets; a float3 struct
// would be padded to 32 bytes in std430 and read every vertex past index 0
// from the wrong byte offset.
[[vk::binding(0, 0)]] RWByteAddressBuffer vertices : register(u0, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer meshlets : register(u1, space0);
// Packed 16- or 32-bit vertex indices, always read as whole aligned uint32
// words (see get_vertex_index) - matches the original's ByteAddressBuffer
// usage exactly (every byte offset it computes is a multiple of 4).
[[vk::binding(2, 0)]] RWByteAddressBuffer unique_vertex_indices : register(u2, space0);
// One packed 10:10:10 triangle per element.
[[vk::binding(3, 0)]] RWByteAddressBuffer primitive_indices : register(u3, space0);

struct Globals {
	float4x4 world;
	float4x4 world_view;
	float4x4 world_view_proj;
	uint draw_meshlets;
};

[[vk::binding(0, 1)]] ConstantBuffer<Globals> globals : register(b0, space1);

struct PushConstants {
	uint index_bytes;
	uint meshlet_offset;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0, space4);

struct VertexOutput {
	[[vk::location(0)]] float3 position_vs : TEXCOORD0;
	[[vk::location(1)]] float3 normal : TEXCOORD1;
	[[vk::location(2)]] nointerpolation uint meshlet_index : TEXCOORD2;
	float4 position : SV_Position;
};

Meshlet get_meshlet(uint index) {
	uint4 words = meshlets.Load4(index * 16u);
	Meshlet m;
	m.vert_count  = words.x;
	m.vert_offset = words.y;
	m.prim_count  = words.z;
	m.prim_offset = words.w;
	return m;
}

uint3 unpack_primitive(uint primitive) {
	return uint3(primitive & 0x3FFu, (primitive >> 10) & 0x3FFu, (primitive >> 20) & 0x3FFu);
}

uint3 get_primitive(Meshlet m, uint index) {
	return unpack_primitive(primitive_indices.Load((m.prim_offset + index) * 4u));
}

float3 get_vertex_position(uint vertex_index) {
	uint base = vertex_index * 24u;
	return asfloat(vertices.Load3(base));
}

float3 get_vertex_normal(uint vertex_index) {
	uint base = vertex_index * 24u;
	return asfloat(vertices.Load3(base + 12u));
}

uint get_vertex_index(Meshlet m, uint local_index) {
	local_index = m.vert_offset + local_index;

	if (pc.index_bytes == 4u) {
		return unique_vertex_indices.Load(local_index * 4u);
	} else {
		uint word_offset = local_index & 1u;
		uint dword_index = local_index / 2u;
		uint index_pair = unique_vertex_indices.Load(dword_index * 4u);
		return (index_pair >> (word_offset * 16u)) & 0xFFFFu;
	}
}

[shader("mesh")]
[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void mesh_mesh(
	uint3 group_id : SV_GroupID,
	uint gtid : SV_GroupIndex,
	OutputVertices<VertexOutput, MAX_VERTICES> out_vertices,
	OutputIndices<uint3, MAX_PRIMITIVES> out_triangles) {
	uint gid = group_id.x;

	Meshlet m = get_meshlet(pc.meshlet_offset + gid);

	SetMeshOutputCounts(m.vert_count, m.prim_count);

	if (gtid < m.prim_count) {
		out_triangles[gtid] = get_primitive(m, gtid);
	}

	if (gtid < m.vert_count) {
		uint vertex_index = get_vertex_index(m, gtid);
		float3 position = get_vertex_position(vertex_index);
		float3 normal   = get_vertex_normal(vertex_index);

		float4 pos_vs = mul(globals.world_view, float4(position, 1.0));
		float4 pos_hs = mul(globals.world_view_proj, float4(position, 1.0));

		VertexOutput v;
		v.position      = pos_hs;
		v.position_vs   = pos_vs.xyz;
		v.normal        = mul(globals.world, float4(normal, 0.0)).xyz;
		v.meshlet_index = gid;
		out_vertices[gtid] = v;
	}
}
