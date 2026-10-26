// Ray-tracing feature tests: the same rays traced with inline ray queries (compute) and
// with a ray-tracing pipeline, so their speed and their results can be compared.
// Every ray writes one word: (InstanceID << 20) | PrimitiveIndex, or 0xFFFFFFFF on a miss.
#include "features/rt_common.slang"

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
