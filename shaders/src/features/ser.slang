// Shader execution reordering: incoherent rays into the sphere field, where each instance
// has one of eight "materials" whose closest-hit shading costs 1x..8x (128 to 1024 iterations: heavy
// enough that regrouping pays for itself, as it does with real materials). Neighbouring rays hit
// different materials, so without reordering the GPU runs every material's loop on mostly
// idle lanes. rgShadeSer traces into a HitObject, regroups threads by material with
// ReorderThread, then invokes the shading; rgShade is the same work without reordering.
// Both write the shaded value of every ray, which must match bit for bit.
#include "features/rt_common.slang"

struct ShadePayload {
    float value;
};

static const uint kMaterials = 8u;

[shader("raygeneration")]
void rgShade() {
    uint2 id = DispatchRaysIndex().xy;
    ShadePayload p;
    p.value = 0.0;
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, makeRay(id), p);
    hits[id.y * pc.width + id.x] = asuint(p.value);
}

[shader("raygeneration")]
void rgShadeSer() {
    uint2 id = DispatchRaysIndex().xy;
    ShadePayload p;
    p.value = 0.0;
    HitObject hit = HitObject::TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, makeRay(id), p);
    uint material = hit.IsHit() ? hit.GetInstanceID() % kMaterials : kMaterials;
    ReorderThread(hit, material, 4);
    HitObject::Invoke(hit, p);
    hits[id.y * pc.width + id.x] = asuint(p.value);
}

[shader("miss")]
void msShade(inout ShadePayload p) { p.value = -1.0; }

[shader("closesthit")]
void chShade(inout ShadePayload p, BuiltInTriangleIntersectionAttributes attr) {
    uint material = InstanceID() % kMaterials;
    float v = float(PrimitiveIndex() & 1023u) * 0.001 + attr.barycentrics.x;
    [loop]
    for (uint i = 0u; i < 128u * (material + 1u); ++i) v = frac(sin(v * 12.9898 + float(i)) * 43758.5453);
    p.value = v + float(material);
}
