// HDR10 test pattern for feature.hdr, written PQ-encoded (SMPTE ST 2084) in BT.2020 primaries into an
// RGB10A2 image: the top 70 % holds 16 grey bars from 0 to 10000 nits (the brightest bar a display still
// tells apart is about its peak), the bottom row six colour patches at 100 nits — BT.2020 red, green and
// blue, then BT.709's red, green and blue converted to BT.2020 (a wide-gamut display shows the first three
// more saturated). The bar luminances are in hdr.cpp too, which checks the encoded values.
#include "common/bindings.slang"

struct HdrParams {
    uint2 size;
    uint pad0, pad1;
};
GT_PUSH_CONSTANTS(HdrParams, pc);

[format("rgb10_a2")] RWTexture2D<float4> outImage : register(u0);

static const float kBarNits[16] = {0.0, 0.1, 1.0, 5.0, 20.0, 50.0, 100.0, 203.0,
                                   400.0, 600.0, 1000.0, 1500.0, 2000.0, 4000.0, 6000.0, 10000.0};

float pq(float nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
float3 pq3(float3 nits) { return float3(pq(nits.r), pq(nits.g), pq(nits.b)); }

// BT.709 -> BT.2020 (linear light).
float3 rec709To2020(float3 c) {
    return float3(dot(c, float3(0.6274, 0.3293, 0.0433)), dot(c, float3(0.0691, 0.9195, 0.0114)),
                  dot(c, float3(0.0164, 0.0880, 0.8956)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csPattern(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= pc.size)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.size);
    float3 nits;
    if (uv.y < 0.7) {
        uint bar = min(uint(uv.x * 16.0), 15u);
        nits = kBarNits[bar];
    } else {
        uint patch = min(uint(uv.x * 6.0), 5u);
        float3 c = float3(patch % 3u == 0u, patch % 3u == 1u, patch % 3u == 2u);
        nits = 100.0 * (patch < 3u ? c : rec709To2020(c));
    }
    outImage[id.xy] = float4(pq3(nits), 1.0);
}
