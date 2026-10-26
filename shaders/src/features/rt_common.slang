// Shared by the ray-tracing feature shaders: ray generation for coherent camera rays and
// incoherent rays (random origin and direction inside a box), and the hit encoding.
#pragma once
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
