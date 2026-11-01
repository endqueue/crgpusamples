// The sample's GPU writer of one acceleration structure instance, for a
// device whose layout is SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64
// (Vulkan, D3D12): the portable instance pushed (spudgpu.h's
// spudgpu_acceleration_structure_instance, as RtWriterInstance in
// ../main.c packs it) written as the device's 64 bytes, the same bytes
// spudgpu_write_acceleration_structure_instances() writes on the CPU: the
// row-major 3x4 transform, the 24-bit user index with the 8-bit mask
// above it, the 24-bit table offset with the 8 bits of flags above it, and
// the bottom level's 64-bit reference. One writer per layout kind (card
// S14); rt_instances_col72_comp.slang is the other.

// RtWriterInstance in ../main.c.
struct Instance {
    float4 rows[3]; // object to world, row-major: (x, y, z, translation)
    uint user_index;
    uint mask;
    uint table_offset;
    uint flags;
    uint reference_low;
    uint reference_high;
    uint out_offset; // where in the instance buffer, in bytes
    uint padding;
};

// The instance buffer: a byte-address buffer, which is what a storage
// buffer's raw UAV on D3D12 is.
[[vk::binding(0, 0)]] RWByteAddressBuffer instances : register(u0, space0);
[[vk::push_constant]] ConstantBuffer<Instance> instance : register(b0, space4);

[shader("compute")]
[numthreads(1, 1, 1)]
void rt_instances_row64_comp(uint3 id : SV_DispatchThreadID) {
    const uint dst = instance.out_offset;
    for (uint r = 0; r < 3u; ++r)
        instances.Store4(dst + r * 16u, asuint(instance.rows[r]));
    instances.Store(dst + 48u, (instance.user_index & 0xFFFFFFu) | (instance.mask << 24));
    instances.Store(dst + 52u, (instance.table_offset & 0xFFFFFFu) | (instance.flags << 24));
    instances.Store(dst + 56u, instance.reference_low);
    instances.Store(dst + 60u, instance.reference_high);
}
