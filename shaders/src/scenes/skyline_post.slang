// Skyline post-processing on the async compute queue: ambient occlusion and a bloom chain.
#include "scenes/skyline_common.slang"

// --- async compute: ambient occlusion and bloom -----------------------------------------------
struct PostParams {
    uint2 size;      // destination size
    float2 invSrc;   // 1 / source size
    float threshold; // bloom: bright-pass threshold (first downsample only)
    float strength;  // bloom upsample: weight of the coarser level
    uint frameIndex;
    uint pad;
};
GT_PUSH_CONSTANTS(PostParams, post);

Texture2D<float> depthTex : register(t4);
Texture2D<float4> normalTex : register(t5);
Texture2D<float4> srcTex : register(t6);
Texture2D<float4> coarseTex : register(t7);
SamplerState linearClamp : register(s1);
RWTexture2D<float> aoOut : register(u2);
RWTexture2D<float4> bloomOut : register(u3);


[shader("compute")]
[numthreads(8, 8, 1)]
void csAmbientOcclusion(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= post.size)) return;
    int2 px = int2(tid.xy);
    float depth = depthTex.Load(int3(px, 0));
    if (depth >= 1.0) {
        aoOut[tid.xy] = 1.0;
        return;
    }
    float3 p = skylineViewPos(px, depth);
    float3 n = normalize(normalTex.Load(int3(px, 0)).xyz * 2.0 - 1.0);
    float radius = 1.2, occlusion = 0.0;
    uint h = gtHash(tid.x * 1973u + tid.y * 9277u + post.frameIndex * 26699u);
    [unroll]
    for (uint k = 0; k < 8; ++k) {
        h = gtHash(h);
        float3 dir = normalize(float3(float(h & 1023u), float((h >> 10) & 1023u), float((h >> 20) & 1023u)) / 511.5 - 1.0);
        if (dot(dir, n) < 0.0) dir = -dir;
        float3 q = p + dir * radius * (0.25 + 0.75 * float(k + 1) / 8.0);
        float4 c = mul(frame.viewProj, float4(q, 1.0));
        float2 uv = (c.xy / c.w) * float2(0.5, -0.5) + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0)) continue;
        int2 sp = int2(uv * frame.screen.xy);
        float sd = depthTex.Load(int3(sp, 0));
        float3 sceneP = skylineViewPos(sp, sd);
        float distQ = distance(frame.cameraPos.xyz, q), distS = distance(frame.cameraPos.xyz, sceneP);
        float range = saturate(radius * 2.0 / max(abs(distQ - distS), 1e-3));
        occlusion += (distS < distQ - 0.05 ? 1.0 : 0.0) * range;
    }
    aoOut[tid.xy] = saturate(1.0 - occlusion / 8.0 * 1.2);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csBloomDown(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= post.size)) return;
    float2 uv = (float2(tid.xy) + 0.5) * 2.0 * post.invSrc;
    float2 o = post.invSrc;
    // 13-tap downsample (Call of Duty / Jimenez style), with a soft bright-pass on the first level.
    float3 c = srcTex.SampleLevel(linearClamp, uv, 0).rgb * 0.125;
    c += (srcTex.SampleLevel(linearClamp, uv + float2(-o.x, -o.y), 0).rgb + srcTex.SampleLevel(linearClamp, uv + float2(o.x, -o.y), 0).rgb +
          srcTex.SampleLevel(linearClamp, uv + float2(-o.x, o.y), 0).rgb + srcTex.SampleLevel(linearClamp, uv + float2(o.x, o.y), 0).rgb) * 0.125;
    c += (srcTex.SampleLevel(linearClamp, uv + float2(-2.0 * o.x, 0), 0).rgb + srcTex.SampleLevel(linearClamp, uv + float2(2.0 * o.x, 0), 0).rgb +
          srcTex.SampleLevel(linearClamp, uv + float2(0, -2.0 * o.y), 0).rgb + srcTex.SampleLevel(linearClamp, uv + float2(0, 2.0 * o.y), 0).rgb) * 0.0625;
    c += (srcTex.SampleLevel(linearClamp, uv + 2.0 * float2(-o.x, -o.y), 0).rgb + srcTex.SampleLevel(linearClamp, uv + 2.0 * float2(o.x, -o.y), 0).rgb +
          srcTex.SampleLevel(linearClamp, uv + 2.0 * float2(-o.x, o.y), 0).rgb + srcTex.SampleLevel(linearClamp, uv + 2.0 * float2(o.x, o.y), 0).rgb) * 0.03125;
    if (post.threshold > 0.0) {
        float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
        c *= saturate((lum - post.threshold) / max(lum, 1e-4));
    }
    bloomOut[tid.xy] = float4(c, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csBloomUp(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= post.size)) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(post.size);
    float2 o = post.invSrc;
    // 3x3 tent filter over the coarser level, added to this level's downsampled colour.
    float3 up = coarseTex.SampleLevel(linearClamp, uv, 0).rgb * 4.0;
    up += (coarseTex.SampleLevel(linearClamp, uv + float2(o.x, 0), 0).rgb + coarseTex.SampleLevel(linearClamp, uv - float2(o.x, 0), 0).rgb +
           coarseTex.SampleLevel(linearClamp, uv + float2(0, o.y), 0).rgb + coarseTex.SampleLevel(linearClamp, uv - float2(0, o.y), 0).rgb) * 2.0;
    up += coarseTex.SampleLevel(linearClamp, uv + o, 0).rgb + coarseTex.SampleLevel(linearClamp, uv - o, 0).rgb +
          coarseTex.SampleLevel(linearClamp, uv + float2(o.x, -o.y), 0).rgb + coarseTex.SampleLevel(linearClamp, uv + float2(-o.x, o.y), 0).rgb;
    float3 base = srcTex.SampleLevel(linearClamp, uv, 0).rgb;
    bloomOut[tid.xy] = float4(base + up / 16.0 * post.strength, 1.0);
}

