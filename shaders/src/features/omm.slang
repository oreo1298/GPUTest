// Alpha-tested geometry and opacity micromaps: rays through a volume of "leaf" cards whose
// opaque part is a disc. Every non-opaque candidate is alpha-tested in the shader; with an
// opacity micromap the hardware resolves triangles that are entirely inside or outside the
// disc itself, so only triangles on its edge reach the shader. Results must be identical.
#include "features/rt_common.slang"

RWStructuredBuffer<uint> counters : register(u1); // [0] = alpha tests run

bool leafOpaque(float2 uv) { return length(uv - 0.5) < 0.45; }

[shader("compute")]
[numthreads(8, 8, 1)]
void csAlphaTrace(uint3 id: SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, makeRay(id.xy));
    uint tests = 0u;
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            ++tests;
            float3 p = q.CandidateObjectRayOrigin() + q.CandidateObjectRayDirection() * q.CandidateTriangleRayT();
            if (leafOpaque(p.xy)) q.CommitNonOpaqueTriangleHit();
        }
    }
    uint v = kMiss;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) v = encodeHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex());
    hits[id.y * pc.width + id.x] = v;
    uint total = WaveActiveSum(tests);
    if (WaveIsFirstLane()) InterlockedAdd(counters[0], total);
}
