module rt_primitives;

// The procedural mode's ray / primitive tests: D3D12RaytracingProceduralGeometry's
// AnalyticPrimitives.hlsli, VolumetricPrimitives.hlsli,
// SignedDistancePrimitives.hlsli, SignedDistanceFractals.hlsli and
// ProceduralPrimitivesLibrary.hlsli, written again in Slang. The signed
// distance functions are Inigo Quilez's (MIT License, Copyright (c) 2013
// Inigo Quilez; http://www.iquilezles.org/www/articles/distfunctions/distfunctions.htm),
// as the original credits them.
//
// The original's intersection shaders read RayTMin(), RayTCurrent() and
// RayFlags(). An inline candidate has no such intrinsics of its own: the
// query's t min, its committed t so far (its t max until something is
// committed) and its flags come in a HitRange instead. Every test works in
// the primitive's local space, the AABB being <-1, 1> there.

import rt_common;

// What the original's intersection shaders read from the ray.
public struct HitRange {
    public float tmin;     // RayTMin()
    public float tcurrent; // RayTCurrent(): the closest t committed, or t max
    public uint flags;     // RayFlags()
};

// The intersection shader kinds (IntersectionShaderType in the original).
public static const uint KIND_ANALYTIC = 0;
public static const uint KIND_VOLUMETRIC = 1;
public static const uint KIND_SIGNED_DISTANCE = 2;

// AnalyticPrimitive.
static const uint ANALYTIC_AABB = 0;
static const uint ANALYTIC_SPHERES = 1;
// SignedDistancePrimitive.
static const uint SD_MINI_SPHERES = 0;
static const uint SD_INTERSECTED_ROUND_CUBE = 1;
static const uint SD_SQUARE_TORUS = 2;
static const uint SD_TWISTED_TORUS = 3;
static const uint SD_COG = 4;
static const uint SD_CYLINDER = 5;
static const uint SD_FRACTAL_PYRAMID = 6;

// N_METABALLS and N_FRACTAL_ITERATIONS, as the original sets them.
static const uint METABALL_COUNT = 3;
static const int FRACTAL_ITERATIONS = 4;

float length_squared(float3 p) {
    return dot(p, p);
}

bool is_in_range(float value, float lo, float hi) {
    return value >= lo && value <= hi;
}

// IsCulled: a hit culled by the ray's flags, by its normal's facing.
bool is_culled(Ray ray, float3 normal, HitRange range) {
    const float d = dot(ray.direction, normal);
    return ((range.flags & RAY_FLAG_CULL_BACK_FACING_TRIANGLES) != 0 && d > 0.0) ||
           ((range.flags & RAY_FLAG_CULL_FRONT_FACING_TRIANGLES) != 0 && d < 0.0);
}

// IsAValidHit.
bool is_valid_hit(Ray ray, float thit, float3 normal, HitRange range) {
    return is_in_range(thit, range.tmin, range.tcurrent) && !is_culled(ray, normal, range);
}

// ---------------------------------------------------------------------------
// Analytic primitives.
// ---------------------------------------------------------------------------

bool solve_quadratic(float a, float b, float c, out float x0, out float x1) {
    x0 = 0.0;
    x1 = 0.0;
    const float discr = b * b - 4.0 * a * c;
    if (discr < 0.0)
        return false;
    if (discr == 0.0) {
        x0 = -0.5 * b / a;
        x1 = x0;
    } else {
        const float q = (b > 0.0) ? -0.5 * (b + sqrt(discr)) : -0.5 * (b - sqrt(discr));
        x0 = q / a;
        x1 = c / q;
    }
    if (x0 > x1) {
        const float t = x0;
        x0 = x1;
        x1 = t;
    }
    return true;
}

bool solve_ray_sphere(Ray ray, float3 center, float radius, out float t0, out float t1) {
    const float3 l = ray.origin - center;
    const float a = dot(ray.direction, ray.direction);
    const float b = 2.0 * dot(ray.direction, l);
    const float c = dot(l, l) - radius * radius;
    return solve_quadratic(a, b, c, t0, t1);
}

// A hollow sphere: its nearer crossing in range and not culled, else its
// farther one.
bool ray_sphere(Ray ray, float3 center, float radius, HitRange range, out float thit, out float3 normal) {
    thit = 0.0;
    normal = float3(0.0, 0.0, 0.0);
    float t0;
    float t1;
    if (!solve_ray_sphere(ray, center, radius, t0, t1))
        return false;
    if (t0 < range.tmin) {
        if (t1 < range.tmin)
            return false;
        normal = normalize(ray.origin + t1 * ray.direction - center);
        if (is_valid_hit(ray, t1, normal, range)) {
            thit = t1;
            return true;
        }
    } else {
        normal = normalize(ray.origin + t0 * ray.direction - center);
        if (is_valid_hit(ray, t0, normal, range)) {
            thit = t0;
            return true;
        }
        normal = normalize(ray.origin + t1 * ray.direction - center);
        if (is_valid_hit(ray, t1, normal, range)) {
            thit = t1;
            return true;
        }
    }
    return false;
}

// A solid sphere, clipped to the ray's range; no normal.
bool ray_solid_sphere(Ray ray, float3 center, float radius, HitRange range, out float tmin, out float tmax) {
    float t0;
    float t1;
    tmin = 0.0;
    tmax = 0.0;
    if (!solve_ray_sphere(ray, center, radius, t0, t1))
        return false;
    tmin = max(t0, range.tmin);
    tmax = min(t1, range.tcurrent);
    return true;
}

// Three spheres, the closest hit.
bool ray_spheres(Ray ray, HitRange range, out float thit, out float3 normal) {
    const float3 centers[3] = { float3(-0.3, -0.3, -0.3), float3(0.1, 0.1, 0.4), float3(0.35, 0.35, 0.0) };
    const float radii[3] = { 0.6, 0.3, 0.15 };
    bool found = false;
    thit = range.tcurrent;
    normal = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < 3u; ++i) {
        float t;
        float3 n;
        if (ray_sphere(ray, centers[i], radii[i], range, t, n) && t < thit) {
            thit = t;
            normal = n;
            found = true;
        }
    }
    return found;
}

// The slab test against the box [lo, hi]: entry and exit t.
bool ray_box_range(Ray ray, float3 lo, float3 hi, HitRange range, out float tmin, out float tmax) {
    const float infinity = asfloat(0x7f800000u);
    // A ray parallel to a pair of slabs: +-infinity, ignored by the min and
    // max inside the slabs and failing the test outside them.
    float3 inv;
    inv.x = ray.direction.x != 0.0 ? 1.0 / ray.direction.x : (ray.direction.x > 0.0 ? infinity : -infinity);
    inv.y = ray.direction.y != 0.0 ? 1.0 / ray.direction.y : (ray.direction.y > 0.0 ? infinity : -infinity);
    inv.z = ray.direction.z != 0.0 ? 1.0 / ray.direction.z : (ray.direction.z > 0.0 ? infinity : -infinity);
    const float3 near = float3(ray.direction.x > 0.0 ? lo.x : hi.x, ray.direction.y > 0.0 ? lo.y : hi.y, ray.direction.z > 0.0 ? lo.z : hi.z);
    const float3 far = float3(ray.direction.x > 0.0 ? hi.x : lo.x, ray.direction.y > 0.0 ? hi.y : lo.y, ray.direction.z > 0.0 ? hi.z : lo.z);
    const float3 tmin3 = (near - ray.origin) * inv;
    const float3 tmax3 = (far - ray.origin) * inv;
    tmin = max(max(tmin3.x, tmin3.y), tmin3.z);
    tmax = min(min(tmax3.x, tmax3.y), tmax3.z);
    return tmax > tmin && tmax >= range.tmin && tmin <= range.tcurrent;
}

// A hollow box, crossed from outside; the normal of the face hit.
bool ray_box(Ray ray, HitRange range, out float thit, out float3 normal) {
    const float3 lo = float3(-1.0, -1.0, -1.0);
    const float3 hi = float3(1.0, 1.0, 1.0);
    thit = 0.0;
    normal = float3(0.0, 0.0, 0.0);
    float tmin;
    float tmax;
    if (!ray_box_range(ray, lo, hi, range, tmin, tmax))
        return false;
    if (tmin < range.tmin || tmin > range.tcurrent)
        return false;
    thit = tmin;
    const float3 p = ray.origin + thit * ray.direction;
    const float3 to_lo = abs(lo - p);
    const float3 to_hi = abs(hi - p);
    const float eps = 0.0001;
    if (to_lo.x < eps)
        normal = float3(-1.0, 0.0, 0.0);
    else if (to_lo.y < eps)
        normal = float3(0.0, -1.0, 0.0);
    else if (to_lo.z < eps)
        normal = float3(0.0, 0.0, -1.0);
    else if (to_hi.x < eps)
        normal = float3(1.0, 0.0, 0.0);
    else if (to_hi.y < eps)
        normal = float3(0.0, 1.0, 0.0);
    else if (to_hi.z < eps)
        normal = float3(0.0, 0.0, 1.0);
    return is_valid_hit(ray, thit, normal, range);
}

// ---------------------------------------------------------------------------
// Volumetric primitives: metaballs.
// ---------------------------------------------------------------------------

struct Metaball {
    float3 center;
    float radius;
};

// A cycling 0 -> 1 -> 0 interpolant.
float animation_interpolant(float elapsed, float cycle) {
    float t = fmod(elapsed, cycle) / cycle;
    t = (t <= 0.5) ? 2.0 * t : 1.0 - 2.0 * (t - 0.5);
    return smoothstep(0.0, 1.0, t);
}

// A quintic field, 1 at the centre and 0 at the radius.
float metaball_potential(float3 position, Metaball blob) {
    const float distance = length(position - blob.center);
    if (distance > blob.radius)
        return 0.0;
    const float d = blob.radius - distance;
    const float r = blob.radius;
    return 6.0 * (d * d * d * d * d) / (r * r * r * r * r) - 15.0 * (d * d * d * d) / (r * r * r * r) + 10.0 * (d * d * d) / (r * r * r);
}

float metaballs_potential(float3 position, Metaball blobs[METABALL_COUNT]) {
    float sum = 0.0;
    for (uint j = 0; j < METABALL_COUNT; ++j)
        sum += metaball_potential(position, blobs[j]);
    return sum;
}

float3 metaballs_normal(float3 position, Metaball blobs[METABALL_COUNT]) {
    const float e = 0.5773 * 0.00001;
    return normalize(float3(metaballs_potential(position + float3(-e, 0.0, 0.0), blobs) - metaballs_potential(position + float3(e, 0.0, 0.0), blobs),
                            metaballs_potential(position + float3(0.0, -e, 0.0), blobs) - metaballs_potential(position + float3(0.0, e, 0.0), blobs),
                            metaballs_potential(position + float3(0.0, 0.0, -e), blobs) - metaballs_potential(position + float3(0.0, 0.0, e), blobs)));
}

// Ray marches the field between the first and last metaball crossed, up
// to the isosurface at the threshold.
bool ray_metaballs(Ray ray, HitRange range, float elapsed, out float thit, out float3 normal) {
    thit = 0.0;
    normal = float3(0.0, 0.0, 0.0);
    const float3 key_from[METABALL_COUNT] = { float3(-0.3, -0.3, -0.4), float3(0.0, -0.2, 0.5), float3(0.4, 0.4, 0.4) };
    const float3 key_to[METABALL_COUNT] = { float3(0.3, -0.3, -0.0), float3(0.0, 0.4, 0.5), float3(-0.4, 0.2, -0.4) };
    const float radii[METABALL_COUNT] = { 0.45, 0.55, 0.45 };
    const float t_animate = animation_interpolant(elapsed, 12.0);
    Metaball blobs[METABALL_COUNT];
    for (uint j = 0; j < METABALL_COUNT; ++j) {
        blobs[j].center = lerp(key_from[j], key_to[j], t_animate);
        blobs[j].radius = radii[j];
    }

    // Where the ray enters the first sphere of influence and leaves the
    // last.
    const float infinity = asfloat(0x7f800000u);
    float tmin = infinity;
    float tmax = -infinity;
    for (uint i = 0; i < METABALL_COUNT; ++i) {
        float t0;
        float t1;
        if (ray_solid_sphere(ray, blobs[i].center, blobs[i].radius, range, t0, t1)) {
            tmin = min(t0, tmin);
            tmax = max(t1, tmax);
        }
    }
    tmin = max(tmin, range.tmin);
    tmax = min(tmax, range.tcurrent);

    const uint max_steps = 128;
    const float step = (tmax - tmin) / float(max_steps);
    float t = tmin;
    for (uint s = 0; s < max_steps; ++s) {
        const float3 position = ray.origin + t * ray.direction;
        // The isosurface: the field's threshold, in (0, 1].
        if (metaballs_potential(position, blobs) >= 0.25) {
            const float3 n = metaballs_normal(position, blobs);
            if (is_valid_hit(ray, t, n, range)) {
                thit = t;
                normal = n;
                return true;
            }
        }
        t += step;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Signed distance primitives.
// ---------------------------------------------------------------------------

float op_s(float d1, float d2) {
    return max(d1, -d2);
}

float op_i(float d1, float d2) {
    return max(d1, d2);
}

float3 op_rep(float3 p, float3 c) {
    return fmod(p, c) - 0.5 * c;
}

float3 op_twist(float3 p) {
    const float c = cos(3.0 * p.y);
    const float s = sin(3.0 * p.y);
    const float2x2 m = float2x2(c, -s, s, c);
    return float3(mul(m, p.xz), p.y);
}

float sd_sphere(float3 p, float s) {
    return length(p) - s;
}

float sd_box(float3 p, float3 b) {
    const float3 d = abs(p) - b;
    return min(max(d.x, max(d.y, d.z)), 0.0) + length(max(d, float3(0.0, 0.0, 0.0)));
}

float ud_round_box(float3 p, float3 b, float r) {
    return length(max(abs(p) - b, float3(0.0, 0.0, 0.0))) - r;
}

float sd_torus(float3 p, float2 t) {
    const float2 q = float2(length(p.xz) - t.x, p.y);
    return length(q) - t.y;
}

float sd_cylinder(float3 p, float2 h) {
    const float2 d = abs(float2(length(p.xz), p.y)) - h;
    return min(max(d.x, d.y), 0.0) + length(max(d, float2(0.0, 0.0)));
}

// h: sin and cos of the angle between a side and the base, and the height.
float sd_octahedron(float3 p, float3 h) {
    const float d = dot(float2(max(abs(p.x), abs(p.z)), abs(p.y)), float2(h.x, h.y));
    return d - h.y * h.z;
}

float sd_pyramid(float3 p, float3 h) {
    return op_s(sd_octahedron(p, h), p.y);
}

float length_to_pow_negative_8(float2 p) {
    p = p * p;
    p = p * p;
    p = p * p;
    return pow(p.x + p.y, 1.0 / 8.0);
}

float sd_torus82(float3 p, float2 t) {
    const float2 q = float2(length(p.xz) - t.x, p.y);
    return length_to_pow_negative_8(q) - t.y;
}

// A pyramid folded into itself FRACTAL_ITERATIONS times, its base on y = 0.
float sd_fractal_pyramid(float3 position, float3 h, float scale) {
    const float a = h.z * h.y / h.x;
    const float3 v1 = float3(0.0, h.z, 0.0);
    const float3 v2 = float3(-a, 0.0, a);
    const float3 v3 = float3(a, 0.0, -a);
    const float3 v4 = float3(a, 0.0, a);
    const float3 v5 = float3(-a, 0.0, -a);
    for (int n = 0; n < FRACTAL_ITERATIONS; ++n) {
        float3 v = v1;
        float dist = length_squared(position - v1);
        float d = length_squared(position - v2);
        if (d < dist) {
            v = v2;
            dist = d;
        }
        d = length_squared(position - v3);
        if (d < dist) {
            v = v3;
            dist = d;
        }
        d = length_squared(position - v4);
        if (d < dist) {
            v = v4;
            dist = d;
        }
        d = length_squared(position - v5);
        if (d < dist) {
            v = v5;
            dist = d;
        }
        position = scale * position - v * (scale - 1.0);
    }
    return sd_pyramid(position, h) * pow(scale, -float(FRACTAL_ITERATIONS));
}

// GetDistanceFromSignedDistancePrimitive.
float signed_distance(float3 position, uint type) {
    switch (type) {
    case SD_MINI_SPHERES:
        return op_i(sd_sphere(op_rep(position + 1.0, float3(2.0, 2.0, 2.0) / 4.0), 0.65 / 4.0), sd_box(position, float3(1.0, 1.0, 1.0)));
    case SD_INTERSECTED_ROUND_CUBE:
        return op_s(op_s(ud_round_box(position, float3(0.75, 0.75, 0.75), 0.2), sd_sphere(position, 1.20)), -sd_sphere(position, 1.32));
    case SD_SQUARE_TORUS:
        return sd_torus82(position, float2(0.75, 0.15));
    case SD_TWISTED_TORUS:
        return sd_torus(op_twist(position), float2(0.6, 0.2));
    case SD_COG:
        return op_s(sd_torus82(position, float2(0.60, 0.3)),
                    sd_cylinder(op_rep(float3(atan2(position.z, position.x) / 6.2831, 1.0, 0.015 + 0.25 * length(position)) + 1.0,
                                       float3(0.05, 1.0, 0.075)),
                                float2(0.02, 0.8)));
    case SD_CYLINDER:
        return op_i(sd_cylinder(op_rep(position + float3(1.0, 1.0, 1.0), float3(1.0, 2.0, 1.0)), float2(0.3, 2.0)),
                    sd_box(position + float3(1.0, 1.0, 1.0), float3(2.0, 2.0, 2.0)));
    case SD_FRACTAL_PYRAMID:
        // Its base at the AABB's y = -1; 63.435 degrees at the base, height 2.
        return sd_fractal_pyramid(position + float3(0.0, 1.0, 0.0), float3(0.894, 0.447, 2.0), 2.0);
    default:
        return 0.0;
    }
}

float3 signed_distance_normal(float3 p, uint type) {
    const float2 e = float2(1.0, -1.0) * 0.5773 * 0.0001;
    return normalize(e.xyy * signed_distance(p + e.xyy, type) + e.yyx * signed_distance(p + e.yyx, type) +
                     e.yxy * signed_distance(p + e.yxy, type) + e.xxx * signed_distance(p + e.xxx, type));
}

// Sphere tracing through the AABB.
bool ray_signed_distance(Ray ray, uint type, HitRange range, float step_scale, out float thit, out float3 normal) {
    thit = 0.0;
    normal = float3(0.0, 0.0, 0.0);
    const float threshold = 0.0001;
    const uint max_steps = 512;
    float t = range.tmin;
    for (uint i = 0; i < max_steps && t <= range.tcurrent; ++i) {
        const float3 position = ray.origin + t * ray.direction;
        const float distance = signed_distance(position, type);
        if (distance <= threshold * t) {
            const float3 n = signed_distance_normal(position, type);
            if (is_valid_hit(ray, t, n, range)) {
                thit = t;
                normal = n;
                return true;
            }
        }
        // The distance is the least to the surface, in any direction: a
        // step that long can't cross it, scaled down where the primitive's
        // transform doesn't keep distances.
        t += step_scale * distance;
    }
    return false;
}

// ---------------------------------------------------------------------------

// What an intersection shader of [kind] finds of primitive [type] along
// [ray], in the primitive's local space: the hit's t and its local normal.
// [elapsed] animates the metaballs; [step_scale] shortens a signed
// distance primitive's steps.
public bool intersect_primitive(Ray ray, uint kind, uint type, HitRange range, float elapsed, float step_scale, out float thit,
                                out float3 normal) {
    thit = 0.0;
    normal = float3(0.0, 0.0, 0.0);
    switch (kind) {
    case KIND_ANALYTIC:
        if (type == ANALYTIC_AABB)
            return ray_box(ray, range, thit, normal);
        if (type == ANALYTIC_SPHERES)
            return ray_spheres(ray, range, thit, normal);
        return false;
    case KIND_VOLUMETRIC:
        return ray_metaballs(ray, range, elapsed, thit, normal);
    case KIND_SIGNED_DISTANCE:
        return ray_signed_distance(ray, type, range, step_scale, thit, normal);
    default:
        return false;
    }
}

// The original's analytically integrated checkerboard (a box filter), its
// lines one fiftieth of a square wide. From Inigo Quilez's
// http://iquilezles.org/www/articles/filterableprocedurals/filterableprocedurals.htm
public float checkers_box_filter(float2 uv, float2 dpdx, float2 dpdy, float ratio) {
    const float2 w = max(abs(dpdx), abs(dpdy));
    const float2 a = uv + 0.5 * w;
    const float2 b = uv - 0.5 * w;
    const float2 i = (floor(a) + min(frac(a) * ratio, 1.0) - floor(b) - min(frac(b) * ratio, 1.0)) / (ratio * w);
    return (1.0 - i.x) * (1.0 - i.y);
}
