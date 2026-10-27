// Sampler feedback (D3D12 only): compute shaders that record which mip levels of a texture
// were requested, per mip region, in a MinMip feedback map. Bound through a native root
// signature in the test (src/bench/tests/features/sampler_feedback.cpp):
//   b0 root constants, t0/u0 descriptor table, u1 root UAV, s0 static point sampler.
#include "common/bindings.slang"

struct FeedbackParams {
    uint textureSize; ///< paired texture width = height (texels)
    uint regionSize;  ///< mip region width = height (texels)
    uint maxLod;      ///< finest mip level a region can hold whole: log2(regionSize)
    uint samples;     ///< samples per thread (throughput entries)
};
ConstantBuffer<FeedbackParams> gParams : register(b0);
Texture2D<float4> gTexture : register(t0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> gFeedback : register(u0);
RWStructuredBuffer<float4> gSink : register(u1);
SamplerState gPoint : register(s0);

static const uint kRegions = 16; // regions per axis in the verification pattern

/// Mip level written to region (x, y) of the verification pattern, or ~0u for "not sampled".
uint expectedLod(uint x, uint y) {
    if ((x + y) % 7u == 0u) return ~0u;
    return (x * 3u + y * 5u) % (gParams.maxLod + 1u);
}

// One thread per region: a single point sample inside the region at the pattern's mip level.
// Mip texels of levels <= log2(regionSize) never straddle a region boundary, and the offset of
// a third of a region keeps the sample off every texel edge.
[shader("compute")]
[numthreads(64, 1, 1)]
void csPattern(uint3 id: SV_DispatchThreadID) {
    if (id.x >= kRegions * kRegions) return;
    uint x = id.x % kRegions, y = id.x / kRegions;
    uint lod = expectedLod(x, y);
    if (lod == ~0u) return;
    float region = float(gParams.regionSize);
    float2 uv = (float2(x, y) * region + region / 3.0) / float(gParams.textureSize);
    gFeedback.WriteSamplerFeedbackLevel(gTexture, gPoint, uv, float(lod));
}

float4 sampleAt(uint id, uint i, out float2 uv, out float lod) {
    uint h = gtHash(id * 7919u + i * 104729u);
    uv = float2(float(h & 0xffffu), float(h >> 16u)) / 65536.0;
    lod = float((h >> 5u) % (gParams.maxLod + 1u));
    return gTexture.SampleLevel(gPoint, uv, lod);
}

// Throughput: the same pseudo-random samples with and without feedback writes.
[shader("compute")]
[numthreads(64, 1, 1)]
void csSample(uint3 id: SV_DispatchThreadID) {
    float4 acc = 0.0;
    for (uint i = 0; i < gParams.samples; ++i) {
        float2 uv;
        float lod;
        acc += sampleAt(id.x, i, uv, lod);
    }
    if (acc.x < -1.0) gSink[0] = acc; // never true: keeps the samples alive
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csSampleFeedback(uint3 id: SV_DispatchThreadID) {
    float4 acc = 0.0;
    for (uint i = 0; i < gParams.samples; ++i) {
        float2 uv;
        float lod;
        acc += sampleAt(id.x, i, uv, lod);
        gFeedback.WriteSamplerFeedbackLevel(gTexture, gPoint, uv, lod);
    }
    if (acc.x < -1.0) gSink[0] = acc;
}
