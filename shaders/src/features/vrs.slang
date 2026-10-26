// Variable-rate shading: an ALU-heavy full-screen pixel shader (escape-time fractal) drawn
// at several per-draw shading rates, plus a probe shader whose output differs for every
// pixel, so the test can verify that coarse rates really share one invocation per block.
#include "common/bindings.slang"

struct VrsParams {
    uint width;
    uint height;
    uint iterations;
    float time;
};
GT_PUSH_CONSTANTS(VrsParams, pc);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsFull(uint vid: SV_VertexID) {
    float2 uv = float2(float((vid << 1u) & 2u), float(vid & 2u));
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 psHeavy(VSOut v) : SV_Target {
    float aspect = float(pc.width) / float(pc.height);
    float2 z = (v.uv - 0.5) * float2(3.0 * aspect, 3.0);
    float2 c = float2(-0.8 + 0.05 * sin(pc.time), 0.156);
    uint i = 0u;
    [loop]
    for (; i < pc.iterations; ++i) {
        z = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
        if (dot(z, z) > 4.0) break;
    }
    float t = float(i) / float(pc.iterations);
    return float4(0.5 + 0.5 * cos(6.2831853 * (t + float3(0.0, 0.33, 0.67))), 1.0);
}

[shader("fragment")]
float4 psRateProbe(VSOut v) : SV_Target {
    uint2 p = uint2(v.pos.xy);
    uint h = gtHash(p.x * 7919u + p.y * 104729u);
    return float4(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u), 255.0) / 255.0;
}
