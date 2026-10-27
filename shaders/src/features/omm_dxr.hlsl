// DXR 1.2 variant of features/omm.slang's csAlphaTrace: identical logic, but the ray query is
// declared with RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS, without which D3D12 ignores micromaps.
// HLSL because Slang's RayQuery has no second template parameter yet; compiled with DXC
// (cs_6_9). Bindings match the RHI layout of the test: t0 scene, u0 hits, u1 counters,
// push constants at b13 (see common/bindings.slang and features/rt_common.slang).

struct RayParams {
    float4 camPos; // xyz; w = tan(vertical fov / 2)
    float4 camFwd; // xyz; w = aspect ratio
    float4 camRight;
    float4 camUp;
    float4 boundsMin;
    float4 boundsMax;
    uint width;
    uint height;
    uint mode;
    uint seed;
};
ConstantBuffer<RayParams> pc : register(b13);

RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> hits : register(u0);
RWStructuredBuffer<uint> counters : register(u1); // [0] = alpha tests run

static const uint kMiss = 0xFFFFFFFFu;

uint encodeHit(uint instance, uint primitive) { return (instance << 20u) | (primitive & 0xFFFFFu); }

// Coherent camera rays only (the OMM test uses mode 0).
RayDesc makeRay(uint2 id) {
    RayDesc r;
    r.TMin = 0.001;
    r.TMax = 1000.0;
    float2 ndc = (float2(id) + 0.5) / float2(pc.width, pc.height) * 2.0 - 1.0;
    r.Origin = pc.camPos.xyz;
    r.Direction = normalize(pc.camFwd.xyz + pc.camRight.xyz * (ndc.x * pc.camPos.w * pc.camFwd.w) -
                            pc.camUp.xyz * (ndc.y * pc.camPos.w));
    return r;
}

bool leafOpaque(float2 uv) { return length(uv - 0.5) < 0.45; }

[numthreads(8, 8, 1)]
void csAlphaTrace(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;
    RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> q;
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
