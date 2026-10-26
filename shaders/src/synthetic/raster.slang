// Fixed-function throughput kernels: pixel fill (ROP), texel fill (texture units),
// triangle setup (tiny triangles), tessellation and MSAA. Shaders are deliberately
// trivial so the unit under test is the bottleneck.
#include "common/bindings.slang"

struct RasterParams {
    float4 color;
    uint samples;      // texel test: texture reads per pixel
    float tessFactor;  // tessellation test
    float triSize;     // triangle test: triangle edge in clip-space units
    uint gridW;        // triangle test: triangles per row
    float2 texScale;   // texel test: uv scale (minification -> mips / anisotropy)
    float lodBias;
    uint pad;
};
GT_PUSH_CONSTANTS(RasterParams, pc);

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
float4 psSolid(VSOut v) : SV_Target { return pc.color; }

Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);

[shader("fragment")]
float4 psTexel(VSOut v) : SV_Target {
    float4 acc = 0;
    float2 uv = v.uv * pc.texScale;
    // Explicit gradients (taken once, scaled by the LOD bias) so the reads can sit in a dynamic loop.
    float bias = exp2(pc.lodBias);
    float2 dx = ddx(uv) * bias, dy = ddy(uv) * bias;
    [loop]
    for (uint i = 0; i < pc.samples; ++i) {
        // Small, distinct offsets per read: realistic cache behaviour, no read can be merged.
        acc += tex.SampleGrad(samp, uv + float2(float(i) * 0.0137, float(i) * 0.0071), dx, dy);
    }
    return acc * (1.0 / float(max(pc.samples, 1u)));
}

// Tiny triangles laid out on a grid, generated from the vertex index (no buffers).
[shader("vertex")]
VSOut vsTiny(uint vid: SV_VertexID, uint iid: SV_InstanceID) {
    uint tri = vid / 3u + iid * 65536u;
    uint corner = vid % 3u;
    uint gx = tri % pc.gridW, gy = (tri / pc.gridW) % pc.gridW;
    float cell = 2.0 / float(pc.gridW);
    float2 base = float2(-1.0 + (float(gx) + 0.25) * cell, -1.0 + (float(gy) + 0.25) * cell);
    float2 offs = corner == 0u ? float2(0, 0) : corner == 1u ? float2(pc.triSize, 0) : float2(0, pc.triSize);
    VSOut o;
    o.pos = float4(base + offs, 0.5, 1.0);
    o.uv = offs;
    return o;
}

[shader("fragment")]
float4 psFlat(VSOut v) : SV_Target { return float4(pc.color.rgb, 1.0); }

[shader("fragment")]
float4 psShade(VSOut v) : SV_Target {
    // A little arithmetic so MSAA costs are measured on a realistic pixel shader.
    float3 c = pc.color.rgb;
    c = frac(c * 1.37 + v.uv.xyx * 0.11);
    c = frac(c * 1.37 + v.uv.xyx * 0.11);
    c = frac(c * 1.37 + v.uv.xyx * 0.11);
    c = frac(c * 1.37 + v.uv.xyx * 0.11);
    return float4(c, 1.0);
}

// --- tessellation ---------------------------------------------------------------
struct CP {
    float4 pos : POSITION;
};
struct PatchConst {
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

[shader("vertex")]
CP vsPatch(uint vid: SV_VertexID) {
    VSOut v = vsTiny(vid, 0u);
    CP o;
    o.pos = v.pos;
    return o;
}

PatchConst hsPatchConstants(InputPatch<CP, 3> patch) {
    PatchConst o;
    o.edges[0] = o.edges[1] = o.edges[2] = pc.tessFactor;
    o.inside = pc.tessFactor;
    return o;
}

[shader("hull")]
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hsPatchConstants")]
CP hsTess(InputPatch<CP, 3> patch, uint i: SV_OutputControlPointID) { return patch[i]; }

[shader("domain")]
[domain("tri")]
VSOut dsTess(PatchConst c, float3 bary: SV_DomainLocation, const OutputPatch<CP, 3> patch) {
    VSOut o;
    o.pos = patch[0].pos * bary.x + patch[1].pos * bary.y + patch[2].pos * bary.z;
    o.uv = bary.xy;
    return o;
}
