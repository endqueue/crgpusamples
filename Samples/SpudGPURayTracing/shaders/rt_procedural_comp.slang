// The procedural mode: D3D12RaytracingProceduralGeometry's Raytracing.hlsl
// as one compute shader with inline ray queries.
//
// What became what:
// - The ray generation shader is the body: a camera ray per pixel.
// - TraceRay's recursion (a radiance ray, a shadow ray and a reflected
//   radiance ray from each hit, MAX_RAY_RECURSION_DEPTH 3) is a loop over
//   the bounces. The original's colour at a hit is linear in its reflected
//   ray's, so each bounce adds its own term, weighted by what the bounces
//   before it pass on, and the result is the original's.
// - The intersection shaders (analytic, volumetric, signed distance) are
//   the code each procedural candidate runs before it is committed
//   (rt_primitives.slang); which one, and with which material, is the
//   candidate's geometry index, the hit group's local root arguments in
//   the original.
// - The closest-hit shaders (triangle, AABB) are what a committed hit is
//   shaded with; the miss shaders are the background and a shadow ray's
//   "not in shadow".

import rt_common;
import rt_primitives;

static const uint MAX_RAY_RECURSION_DEPTH = 3;
static const float4 BACKGROUND_COLOR = float4(0.8, 0.9, 1.0, 1.0);
static const float IN_SHADOW_RADIANCE = 0.35;
// RT_PROCEDURAL_PRIMITIVE_COUNT in ../main.c: the AABB bottom level's
// geometries, one AABB each.
static const uint PRIMITIVE_COUNT = 10;
// The instances' user indices (CommittedInstanceID).
static const uint INSTANCE_PLANE = 0;

// PrimitiveConstantBuffer.
struct Material {
    float4 albedo;
    // x: reflectance, y: diffuse, z: specular, w: specular power.
    float4 coefficients;
    // x: the signed distance step scale.
    float4 step_scale;
};

// A primitive's transform from its local space (the AABB <-1, 1>) to the
// bottom level's: scale, then turn about y, then move to its AABB's centre.
struct Primitive {
    float4 center;  // xyz: its AABB's centre in the bottom level
    float4 scale;   // xyz: its scale; w: 1 if it turns with the animation
    uint4 kind;     // x: the intersection kind, y: the primitive type
};

// RtProceduralScene in ../main.c: written once.
struct Scene {
    float4 light_ambient;
    float4 light_diffuse;
    // x: the plane's vertices' byte offset in the geometry buffer, y: its
    // indices'.
    uint4 offsets;
    Material plane;
    Material materials[PRIMITIVE_COUNT];
    Primitive primitives[PRIMITIVE_COUNT];
};

// RtProceduralFrame in ../main.c: pushed every frame.
struct Frame {
    float4 projection_to_world[4]; // rows
    float4 camera_position;
    float4 light_position;
    float4 time; // x: the geometry's animation time, in seconds
};

[[vk::binding(0, 0)]] RaytracingAccelerationStructure rt_scene : register(t0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> output : register(u1, space0);
[[vk::binding(2, 0)]] ConstantBuffer<Scene> scene : register(b2, space0);
// The plane's vertices (position, normal: 24 bytes each) and 16-bit
// indices, and the AABBs: a byte-address buffer, which is what a storage
// buffer's raw UAV on D3D12 is. The same bytes the bottom levels were built
// from.
[[vk::binding(3, 0)]] RWByteAddressBuffer geometry : register(u3, space0);
[[vk::push_constant]] ConstantBuffer<Frame> frame : register(b0, space4);

// v * RotationY(angle), as DirectXMath turns a row vector.
float3 rotate_y(float3 v, float angle) {
    const float c = cos(angle);
    const float s = sin(angle);
    return float3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

float primitive_angle(Primitive p) {
    return p.scale.w != 0.0 ? -2.0 * frame.time.x : 0.0;
}

// GetRayInAABBPrimitiveLocalSpace: the bottom level's ray in the
// primitive's local space. The same t along both.
Ray to_local(Ray ray, Primitive p) {
    const float angle = primitive_angle(p);
    Ray local;
    local.origin = rotate_y(ray.origin - p.center.xyz, -angle) / p.scale.xyz;
    local.direction = rotate_y(ray.direction, -angle) / p.scale.xyz;
    return local;
}

// The original's mul(normal, (float3x3) localSpaceToBottomLevelAS).
float3 normal_to_bottom_level(float3 n, Primitive p) {
    return rotate_y(n * p.scale.xyz, primitive_angle(p));
}

// A procedural candidate: the intersection shader of its geometry, run on
// the query's object-space ray. True with the hit's t and world normal.
bool candidate_hit(float3 object_origin, float3 object_direction, uint geometry_index, float3x4 object_to_world, HitRange range,
                   out float thit, out float3 world_normal) {
    thit = 0.0;
    world_normal = float3(0.0, 0.0, 0.0);
    if (geometry_index >= PRIMITIVE_COUNT)
        return false;
    const Primitive p = scene.primitives[geometry_index];
    Ray object_ray;
    object_ray.origin = object_origin;
    object_ray.direction = object_direction;
    float3 local_normal;
    if (!intersect_primitive(to_local(object_ray, p), p.kind.x, p.kind.y, range, frame.time.x,
                             scene.materials[geometry_index].step_scale.x, thit, local_normal))
        return false;
    world_normal = normalize(mul(object_to_world, float4(normal_to_bottom_level(local_normal, p), 0.0)));
    return true;
}

struct RadianceHit {
    bool hit;
    float t;
    uint instance_id;
    uint geometry_index;
    uint primitive_index;
    float3 normal; // a procedural hit's world normal
};

// TraceRadianceRay's query: the closest hit, back faces culled, t from 0.
RadianceHit trace_radiance(Ray ray) {
    RayDesc desc;
    desc.Origin = ray.origin;
    desc.Direction = ray.direction;
    // The original's t min of 0, against artifacts where surfaces touch;
    // back-face culling keeps a ray off the surface it leaves.
    desc.TMin = 0.0;
    desc.TMax = 10000.0;
    const uint flags = RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(rt_scene, flags, 0xFF, desc);
    float3 normal = float3(0.0, 0.0, 0.0);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            HitRange range;
            range.tmin = query.RayTMin();
            range.tcurrent = query.CommittedRayT();
            range.flags = flags;
            float thit;
            float3 n;
            if (candidate_hit(query.CandidateObjectRayOrigin(), query.CandidateObjectRayDirection(), query.CandidateGeometryIndex(),
                              query.CandidateObjectToWorld3x4(), range, thit, n)) {
                query.CommitProceduralPrimitiveHit(thit);
                normal = n;
            }
        }
    }
    RadianceHit result;
    result.hit = query.CommittedStatus() != COMMITTED_NOTHING;
    result.t = query.CommittedRayT();
    result.instance_id = query.CommittedInstanceID();
    result.geometry_index = query.CommittedGeometryIndex();
    result.primitive_index = query.CommittedPrimitiveIndex();
    result.normal = normal;
    return result;
}

// TraceShadowRayAndReportIfHit: anything between [origin] and the light,
// for a ray of recursion depth [depth].
bool trace_shadow(float3 origin, float3 direction, uint depth) {
    if (depth >= MAX_RAY_RECURSION_DEPTH)
        return false;
    RayDesc desc;
    desc.Origin = origin;
    desc.Direction = direction;
    desc.TMin = 0.0;
    desc.TMax = 10000.0;
    // The original's flags but SKIP_CLOSEST_HIT_SHADER, which a query has no
    // shader to skip: the first hit ends it.
    const uint flags = RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(rt_scene, flags, 0xFF, desc);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            HitRange range;
            range.tmin = query.RayTMin();
            range.tcurrent = query.CommittedRayT();
            range.flags = flags;
            float thit;
            float3 n;
            if (candidate_hit(query.CandidateObjectRayOrigin(), query.CandidateObjectRayDirection(), query.CandidateGeometryIndex(),
                              query.CandidateObjectToWorld3x4(), range, thit, n))
                query.CommitProceduralPrimitiveHit(thit);
        }
    }
    return query.CommittedStatus() != COMMITTED_NOTHING;
}

// CalculatePhongLighting: ambient (darker facing down), diffuse and
// specular, for a hit at [position] by a ray along [ray_direction].
float4 phong(Material m, float3 normal, bool in_shadow, float3 position, float3 ray_direction) {
    const float shadow_factor = in_shadow ? IN_SHADOW_RADIANCE : 1.0;
    const float3 incident = normalize(position - frame.light_position.xyz);

    const float kd = saturate(dot(-incident, normal));
    const float4 diffuse = shadow_factor * m.coefficients.y * kd * scene.light_diffuse * m.albedo;

    float4 specular = float4(0.0, 0.0, 0.0, 0.0);
    if (!in_shadow) {
        const float3 reflected = normalize(reflect(incident, normal));
        const float ks = pow(saturate(dot(reflected, normalize(-ray_direction))), m.coefficients.w);
        specular = m.coefficients.z * ks * float4(1.0, 1.0, 1.0, 1.0);
    }

    // Fake ambient occlusion: faces turned down a little darker.
    const float a = 1.0 - saturate(dot(normal, float3(0.0, -1.0, 0.0)));
    const float4 ambient = m.albedo * lerp(scene.light_ambient - 0.1, scene.light_ambient, a);
    return ambient + diffuse + specular;
}

// AnalyticalCheckersTexture: the plane's checkers, box filtered over the
// footprint of the camera rays through the neighbouring pixels on the
// plane's tangent plane at the hit.
float checkers(float3 position, float3 normal, uint2 pixel, uint2 size) {
    const float4 r0 = frame.projection_to_world[0];
    const float4 r1 = frame.projection_to_world[1];
    const float4 r2 = frame.projection_to_world[2];
    const float4 r3 = frame.projection_to_world[3];
    const float3 camera = frame.camera_position.xyz;
    const Ray ddx = camera_ray(pixel + uint2(1, 0), size, camera, r0, r1, r2, r3);
    const Ray ddy = camera_ray(pixel + uint2(0, 1), size, camera, r0, r1, r2, r3);
    const float3 ddx_position = ddx.origin - ddx.direction * dot(ddx.origin - position, normal) / dot(ddx.direction, normal);
    const float3 ddy_position = ddy.origin - ddy.direction * dot(ddy.origin - position, normal) / dot(ddy.direction, normal);
    const float2 uv = position.xz;
    return checkers_box_filter(uv, ddx_position.xz - uv, ddy_position.xz - uv, 50.0);
}

// FresnelReflectanceSchlick.
float3 fresnel_schlick(float3 i, float3 n, float3 f0) {
    const float cosi = saturate(dot(-i, n));
    return f0 + (1.0 - f0) * pow(1.0 - cosi, 5.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rt_procedural_comp(uint3 id : SV_DispatchThreadID) {
    uint width;
    uint height;
    output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    const uint2 size = uint2(width, height);

    Ray ray = camera_ray(id.xy, size, frame.camera_position.xyz, frame.projection_to_world[0], frame.projection_to_world[1],
                         frame.projection_to_world[2], frame.projection_to_world[3]);

    // color = sum over the bounces of weight * (the bounce's own colour),
    // the weight being what the bounces before pass on of a reflection.
    float4 color = float4(0.0, 0.0, 0.0, 0.0);
    float4 weight = float4(1.0, 1.0, 1.0, 1.0);
    for (uint bounce = 0; bounce < MAX_RAY_RECURSION_DEPTH; ++bounce) {
        const RadianceHit hit = trace_radiance(ray);
        if (!hit.hit) {
            // MyMissShader.
            color += weight * BACKGROUND_COLOR;
            break;
        }
        // The closest-hit shaders, at the payload's recursion depth.
        const uint depth = bounce + 1;
        const float3 position = ray.origin + hit.t * ray.direction;
        Material m;
        float3 normal;
        float texture = 1.0;
        if (hit.instance_id == INSTANCE_PLANE) {
            // MyClosestHitShader_Triangle: the normal of the triangle's
            // first vertex, and the checkers.
            const uint3 indices = load_3x16_indices(geometry, scene.offsets.y + hit.primitive_index * 6u);
            normal = asfloat(geometry.Load3(scene.offsets.x + indices.x * 24u + 12u));
            m = scene.plane;
            texture = checkers(position, normal, id.xy, size);
        } else {
            // MyClosestHitShader_AABB.
            normal = hit.normal;
            m = scene.materials[min(hit.geometry_index, PRIMITIVE_COUNT - 1u)];
        }

        const bool in_shadow = trace_shadow(position, normalize(frame.light_position.xyz - position), depth);
        const float4 lit = phong(m, normal, in_shadow, position, ray.direction);
        // The visibility falloff into the background.
        const float falloff = 1.0 - exp(-0.000002 * hit.t * hit.t * hit.t);
        color += weight * ((1.0 - falloff) * texture * lit + falloff * BACKGROUND_COLOR);

        // The reflected ray, where the material reflects and the depth
        // allows one (TraceRadianceRay returns black past it).
        if (m.coefficients.x <= 0.001 || depth >= MAX_RAY_RECURSION_DEPTH)
            break;
        const float3 fresnel = fresnel_schlick(ray.direction, normal, m.albedo.xyz);
        weight *= (1.0 - falloff) * texture * m.coefficients.x * float4(fresnel, 1.0);
        ray.direction = reflect(ray.direction, normal);
        ray.origin = position;
    }
    output[id.xy] = color;
}
