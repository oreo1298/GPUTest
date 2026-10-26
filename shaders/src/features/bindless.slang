// Bindless textures (descriptor indexing): reading from a large table of textures with an
// index chosen per workgroup (uniform) or per thread (divergent), against one ordinarily
// bound texture. csBindlessCheck reads every table entry once for a correctness check.
#include "common/bindings.slang"

struct BindlessParams {
    uint tableSize;
    uint samples; // texture reads per thread
    uint pad0;
    uint pad1;
};
GT_PUSH_CONSTANTS(BindlessParams, pc);

[[vk::binding(0, 1)]] Texture2D<float4> table[] : register(t0, space1);
Texture2D<float4> single : register(t0);
SamplerState samp : register(s0);
RWStructuredBuffer<uint> output : register(u0);

[shader("compute")]
[numthreads(64, 1, 1)]
void csBindlessCheck(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.tableSize) return;
    float4 c = table[NonUniformResourceIndex(tid.x)].SampleLevel(samp, float2(0.5, 0.5), 0.0);
    output[tid.x] = uint(c.r * 255.0 + 0.5) | (uint(c.g * 255.0 + 0.5) << 8u);
}

float2 uvFor(uint t, uint k) {
    uint h = gtHash(t * 131u + k);
    return float2(float(h & 1023u), float(h >> 22u)) / 1024.0;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csFetchBound(uint3 tid: SV_DispatchThreadID) {
    float4 acc = 0;
    [loop]
    for (uint k = 0u; k < pc.samples; ++k) acc += single.SampleLevel(samp, uvFor(tid.x, k), 0.0);
    output[tid.x & 1023u] = asuint(acc.x + acc.y + acc.z + acc.w);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csFetchUniform(uint3 tid: SV_DispatchThreadID, uint3 gid: SV_GroupID) {
    float4 acc = 0;
    [loop]
    for (uint k = 0u; k < pc.samples; ++k) acc += table[(gid.x + k) % pc.tableSize].SampleLevel(samp, uvFor(tid.x, k), 0.0);
    output[tid.x & 1023u] = asuint(acc.x + acc.y + acc.z + acc.w);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csFetchDivergent(uint3 tid: SV_DispatchThreadID) {
    float4 acc = 0;
    [loop]
    for (uint k = 0u; k < pc.samples; ++k) {
        uint index = gtHash(tid.x * 7u + k) % pc.tableSize;
        acc += table[NonUniformResourceIndex(index)].SampleLevel(samp, uvFor(tid.x, k), 0.0);
    }
    output[tid.x & 1023u] = asuint(acc.x + acc.y + acc.z + acc.w);
}
