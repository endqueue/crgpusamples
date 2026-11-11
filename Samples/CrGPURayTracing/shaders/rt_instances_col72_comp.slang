// The sample's GPU writer of one acceleration structure instance, for a
// device whose layout is CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72
// (Metal's indirect instance descriptor): the portable instance pushed
// (RtWriterInstance in ../main.c) written as the device's 72 bytes, the
// same bytes crgpu_device_write_acceleration_structure_instances() writes on
// the CPU: the transform as four columns of three floats, then the options
// (the flags), the mask, the table offset, the user index and the bottom
// level's 64-bit reference. One writer per layout kind (card S14);
// rt_instances_row64_comp.slang is the other.

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
void rt_instances_col72_comp(uint3 id : SV_DispatchThreadID) {
    const uint dst = instance.out_offset;
    // Column c, row r.
    for (uint c = 0; c < 4u; ++c)
        for (uint r = 0; r < 3u; ++r)
            instances.Store(dst + (c * 3u + r) * 4u, asuint(instance.rows[r][c]));
    instances.Store(dst + 48u, instance.flags);
    instances.Store(dst + 52u, instance.mask);
    instances.Store(dst + 56u, instance.table_offset);
    instances.Store(dst + 60u, instance.user_index);
    instances.Store(dst + 64u, instance.reference_low);
    instances.Store(dst + 68u, instance.reference_high);
}
