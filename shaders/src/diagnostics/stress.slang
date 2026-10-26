// Deterministic, ALU-heavy full-screen render used for artifact detection and the
// thermal soak: a ray-marched field of blended spheres with soft shadows. The same
// inputs always produce bit-identical pixels on a healthy GPU, so a reference frame
// re-rendered under load must match exactly. csCompare counts mismatching pixels.
#include "common/bindings.slang"

struct StressParams {
    float time;
    uint steps;      // ray-march steps (quality)
    uint shadowSteps;
    uint width;
    uint height;
    uint checkSlot;  // csCompare: which per-check counter to increment
    uint pad0;
    uint pad1;
};
GT_PUSH_CONSTANTS(StressParams, pc);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsStress(uint vid: SV_VertexID) {
    float2 uv = float2(float((vid << 1u) & 2u), float(vid & 2u));
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); // uv (0,0) = top left, as in blit.slang
    o.uv = uv;
    return o;
}

float sdScene(float3 p, float t) {
    float d = p.y + 1.0; // floor
    [[unroll]]
    for (uint i = 0u; i < 6u; ++i) {
        float fi = float(i);
        float3 c = float3(sin(t * 0.7 + fi * 1.7) * 1.6, 0.2 + 0.5 * sin(t * 1.1 + fi), cos(t * 0.5 + fi * 2.3) * 1.6);
        float r = 0.45 + 0.15 * sin(fi * 3.1 + t);
        float s = length(p - c) - r;
        float k = 0.35; // smooth union
        float h = saturate(0.5 + 0.5 * (s - d) / k);
        d = lerp(s, d, h) - k * h * (1.0 - h);
    }
    return d;
}

float3 normalAt(float3 p, float t) {
    const float e = 0.002;
    return normalize(float3(sdScene(p + float3(e, 0, 0), t) - sdScene(p - float3(e, 0, 0), t),
                            sdScene(p + float3(0, e, 0), t) - sdScene(p - float3(0, e, 0), t),
                            sdScene(p + float3(0, 0, e), t) - sdScene(p - float3(0, 0, e), t)));
}

[shader("fragment")]
float4 psStress(VSOut v) : SV_Target {
    float t = pc.time;
    float2 q = v.uv * 2.0 - 1.0;
    q.x *= float(pc.width) / float(pc.height);
    float3 ro = float3(sin(t * 0.2) * 4.5, 1.6, cos(t * 0.2) * 4.5);
    float3 fw = normalize(-ro + float3(0, 0.2, 0));
    float3 rt = normalize(cross(float3(0, 1, 0), fw));
    float3 up = cross(fw, rt);
    float3 rd = normalize(fw * 1.6 + rt * q.x - up * q.y);
    float dist = 0.0;
    bool hit = false;
    for (uint i = 0u; i < pc.steps; ++i) {
        float d = sdScene(ro + rd * dist, t);
        if (d < 0.0005) {
            hit = true;
            break;
        }
        dist += d * 0.9;
        if (dist > 30.0) break;
    }
    float3 sky = lerp(float3(0.55, 0.65, 0.8), float3(0.15, 0.2, 0.35), saturate(rd.y * 1.5 + 0.3));
    if (!hit) return float4(sky, 1.0);
    float3 p = ro + rd * dist;
    float3 n = normalAt(p, t);
    float3 l = normalize(float3(0.5, 0.8, 0.3));
    float shade = 1.0, s = 0.02;
    for (uint j = 0u; j < pc.shadowSteps; ++j) {
        float h = sdScene(p + l * s, t);
        shade = min(shade, 12.0 * h / s);
        s += clamp(h, 0.01, 0.3);
        if (h < 0.0005 || s > 8.0) break;
    }
    shade = saturate(shade);
    float3 albedo = p.y < -0.99 ? (((uint(floor(p.x * 2.0)) + uint(floor(p.z * 2.0))) & 1u) != 0u ? float3(0.7, 0.7, 0.7)
                                                                                                    : float3(0.35, 0.35, 0.38))
                                : 0.5 + 0.45 * cos(6.28318 * (float3(0.0, 0.33, 0.67) + p.y * 0.4 + t * 0.05));
    float diff = saturate(dot(n, l)) * shade;
    float3 col = albedo * (0.12 + 0.88 * diff) + 0.25 * pow(saturate(dot(reflect(rd, n), l)), 32.0) * shade;
    col = lerp(col, sky, saturate(dist / 30.0));
    return float4(col, 1.0);
}

Texture2D<float4> reference : register(t0);
Texture2D<float4> candidate : register(t1);
RWStructuredBuffer<uint> mismatches : register(u0); // [0] total pixels, [1] first x, [2] first y, [3+slot] per check

[shader("compute")]
[numthreads(8, 8, 1)]
void csCompare(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.width || tid.y >= pc.height) return;
    uint4 a = asuint(reference.Load(int3(tid.xy, 0)));
    uint4 b = asuint(candidate.Load(int3(tid.xy, 0)));
    if (any(a != b)) {
        uint before;
        InterlockedAdd(mismatches[0], 1u, before);
        if (before == 0u) {
            mismatches[1] = tid.x;
            mismatches[2] = tid.y;
        }
        InterlockedAdd(mismatches[3u + pc.checkSlot], 1u);
    }
}
