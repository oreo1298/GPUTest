// Test image for feature.upscale: an analytic scene (gradient, disks, rotated squares, thin spokes,
// "text" rows) rendered with 4x4 supersampling, so the same picture can be produced at any size —
// a ground truth at the output resolution and anti-aliased inputs at render resolutions.
#include "common/bindings.slang"

struct PatternParams {
    uint2 size;
    uint pad0, pad1;
};
GT_PUSH_CONSTANTS(PatternParams, pc);

[format("rgba8")] RWTexture2D<float4> outImage : register(u0);

float boxMask(float2 p, float2 c, float2 half, float angle) {
    float s = sin(angle), co = cos(angle);
    float2 d = p - c;
    d = float2(co * d.x + s * d.y, -s * d.x + co * d.y);
    return all(abs(d) <= half) ? 1.0 : 0.0;
}

float3 scene(float2 p) { // p: x in [0, aspect], y in [0, 1]
    float3 c = lerp(float3(0.18, 0.28, 0.45), float3(0.85, 0.80, 0.70), p.y);
    // Disks.
    for (uint i = 0; i < 6; ++i) {
        float2 cen = float2(0.15 + 0.27 * float(i), 0.22 + 0.06 * float(i & 1u));
        float r = 0.035 + 0.012 * float(i);
        if (length(p - cen) < r) c = float3(0.9, 0.35 + 0.1 * float(i), 0.2);
    }
    // Rotated squares.
    for (uint j = 0; j < 6; ++j) {
        float2 cen = float2(0.18 + 0.27 * float(j), 0.52);
        if (boxMask(p, cen, float2(0.05, 0.05), 0.15 + 0.25 * float(j)) > 0.0) c = float3(0.15, 0.6, 0.3 + 0.1 * float(j));
    }
    // Thin spokes around a hub.
    float2 hub = float2(1.45, 0.82);
    float2 d = p - hub;
    float r = length(d);
    if (r < 0.15 && r > 0.02) {
        float a = atan2(d.y, d.x) * (24.0 / (2.0 * GT_PI));
        if (abs(a - round(a)) * r * (2.0 * GT_PI / 24.0) < 0.0015) c = float3(0.05);
    }
    // "Text": rows of small dark blocks of varying width.
    if (p.y > 0.72 && p.y < 0.95 && p.x > 0.08 && p.x < 1.2) {
        float row = floor((p.y - 0.72) / 0.032);
        float yIn = frac((p.y - 0.72) / 0.032);
        float cell = floor(p.x / 0.018);
        float xIn = frac(p.x / 0.018);
        float w = 0.3 + 0.6 * gtHashFloat(uint(row) * 131u + uint(cell));
        if (yIn > 0.25 && yIn < 0.75 && xIn < w) c = float3(0.08, 0.08, 0.1);
    }
    return c;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csPattern(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= pc.size)) return;
    const float scale = 1.0 / float(pc.size.y);
    float3 acc = 0.0;
    for (uint sy = 0; sy < 4; ++sy)
        for (uint sx = 0; sx < 4; ++sx) {
            float2 p = (float2(id.xy) + (float2(sx, sy) + 0.5) / 4.0) * scale;
            acc += scene(p);
        }
    outImage[id.xy] = float4(acc / 16.0, 1.0);
}
