// Ray-tracing feature tests: the same rays traced with inline ray queries (compute) and
// with a ray-tracing pipeline, so their speed and their results can be compared.
// Every ray writes one word: (InstanceID << 20) | PrimitiveIndex, or 0xFFFFFFFF on a miss.
#include "common/bindings.slang"

struct RayParams {
    float4 camPos;    // xyz; w = tan(vertical fov / 2)
    float4 camFwd;    // xyz; w = aspect ratio
    float4 camRight;
    float4 camUp;
    float4 boundsMin; // incoherent rays start inside this box
    float4 boundsMax;
    uint width;
    uint height;
    uint mode; // 0 = coherent camera rays, 1 = incoherent (random origin and direction)
    uint seed;
};
GT_PUSH_CONSTANTS(RayParams, pc);

RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> hits : register(u0);

static const uint kMiss = 0xFFFFFFFFu;

uint encodeHit(uint instance, uint primitive) { return (instance << 20u) | (primitive & 0xFFFFFu); }

RayDesc makeRay(uint2 id) {
    RayDesc r;
    r.TMin = 0.001;
    r.TMax = 1000.0;
    if (pc.mode == 0u) {
        float2 ndc = (float2(id) + 0.5) / float2(pc.width, pc.height) * 2.0 - 1.0;
        r.Origin = pc.camPos.xyz;
        r.Direction = normalize(pc.camFwd.xyz + pc.camRight.xyz * (ndc.x * pc.camPos.w * pc.camFwd.w) -
                                pc.camUp.xyz * (ndc.y * pc.camPos.w));
    } else {
        uint h0 = gtHash((id.y * pc.width + id.x) ^ pc.seed);
        uint h1 = gtHash(h0), h2 = gtHash(h1), h3 = gtHash(h2), h4 = gtHash(h3);
        float3 u = float3(h0, h1, h2) * (1.0 / 4294967296.0);
        r.Origin = lerp(pc.boundsMin.xyz, pc.boundsMax.xyz, u);
        float z = float(h3) * (2.0 / 4294967296.0) - 1.0;
        float phi = float(h4) * (2.0 * GT_PI / 4294967296.0);
        float s = sqrt(max(0.0, 1.0 - z * z));
        r.Direction = float3(s * cos(phi), z, s * sin(phi));
    }
    return r;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csRayQuery(uint3 id: SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, makeRay(id.xy));
    q.Proceed(); // opaque geometry only: traversal finishes in one call
    uint v = kMiss;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) v = encodeHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex());
    hits[id.y * pc.width + id.x] = v;
}

struct Payload {
    uint value;
};

[shader("raygeneration")]
void rgRays() {
    uint2 id = DispatchRaysIndex().xy;
    Payload p;
    p.value = kMiss;
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, makeRay(id), p);
    hits[id.y * pc.width + id.x] = p.value;
}

[shader("miss")]
void msRays(inout Payload p) { p.value = kMiss; }

[shader("closesthit")]
void chRays(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    p.value = encodeHit(InstanceID(), PrimitiveIndex());
}
