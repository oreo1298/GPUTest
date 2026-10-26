// BVH build / refit test helpers: csDisplace animates a height-field grid in place
// (same topology, so the BLAS can be refit), csProbe shoots one ray straight down onto
// every cell and records what it hit, for comparison with a CPU reference.
#include "common/bindings.slang"

struct GridParams {
    uint cells;    // cells per side
    float size;    // world size of the grid
    float time;    // displacement phase
    float amplitude;
    float probeHeight;
    uint pad0, pad1, pad2;
};
GT_PUSH_CONSTANTS(GridParams, pc);

RWStructuredBuffer<float> vertices : register(u0); // xyz per vertex, tightly packed
RWStructuredBuffer<uint2> probes : register(u1);   // (asuint(t), primitive index)
RaytracingAccelerationStructure scene : register(t0);

[shader("compute")]
[numthreads(64, 1, 1)]
void csDisplace(uint3 tid: SV_DispatchThreadID) {
    uint n = pc.cells + 1u;
    if (tid.x >= n * n) return;
    uint ix = tid.x % n, iz = tid.x / n;
    float x = (float(ix) / float(pc.cells) - 0.5) * pc.size;
    float z = (float(iz) / float(pc.cells) - 0.5) * pc.size;
    float y = pc.amplitude * (sin(x * 0.9 + pc.time) * cos(z * 0.7 - pc.time * 1.3) + 0.5 * sin((x + z) * 2.1 + pc.time * 0.5));
    vertices[tid.x * 3u + 0u] = x;
    vertices[tid.x * 3u + 1u] = y;
    vertices[tid.x * 3u + 2u] = z;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csProbe(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.cells * pc.cells) return;
    uint cx = tid.x % pc.cells, cz = tid.x / pc.cells;
    // Off-centre so the ray stays clear of the cell's diagonal edge.
    float x = ((float(cx) + 0.37) / float(pc.cells) - 0.5) * pc.size;
    float z = ((float(cz) + 0.61) / float(pc.cells) - 0.5) * pc.size;
    RayDesc r;
    r.Origin = float3(x, pc.probeHeight, z);
    r.Direction = float3(0, -1, 0);
    r.TMin = 0.0;
    r.TMax = pc.probeHeight * 4.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    uint2 o = uint2(asuint(-1.0), 0xFFFFFFFFu);
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) o = uint2(asuint(q.CommittedRayT()), q.CommittedPrimitiveIndex());
    probes[tid.x] = o;
}
