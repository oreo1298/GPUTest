// Pipeline-compilation test material: a realistically sized forward-shading pixel shader
// (several lights, normal perturbation, fog). The test patches the 32-bit salt constant in
// the SPIR-V so every pipeline it creates is new to the driver and its shader caches.
#include "common/bindings.slang"

struct MaterialParams {
    float4x4 viewProj;
    float4 cameraPos;
    uint flags;
    uint lightCount;
    float time;
    float pad;
};
GT_PUSH_CONSTANTS(MaterialParams, pc);

static const uint kSalt = 0x5A17C0DEu;

struct VSOut {
    float4 pos : SV_Position;
    float3 world : WORLDPOS;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMaterial(float3 pos: POSITION, float3 normal: NORMAL, float2 uv: TEXCOORD0) {
    VSOut o;
    o.world = pos;
    o.pos = mul(pc.viewProj, float4(pos, 1.0));
    o.normal = normal;
    o.uv = uv;
    return o;
}

float noise(float2 p) {
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = gtHashFloat(uint(i.x) * 73856093u ^ uint(i.y) * 19349663u);
    float b = gtHashFloat(uint(i.x + 1.0) * 73856093u ^ uint(i.y) * 19349663u);
    float c = gtHashFloat(uint(i.x) * 73856093u ^ uint(i.y + 1.0) * 19349663u);
    float d = gtHashFloat(uint(i.x + 1.0) * 73856093u ^ uint(i.y + 1.0) * 19349663u);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

[shader("fragment")]
float4 psMaterial(VSOut v) : SV_Target {
    float3 n = normalize(v.normal);
    float h = noise(v.uv * 32.0) * 0.6 + noise(v.uv * 64.0) * 0.3 + noise(v.uv * 128.0) * 0.1;
    n = normalize(n + float3(ddx(h), 0.0, ddy(h)) * 4.0);
    float3 albedo = lerp(float3(0.55, 0.42, 0.3), float3(0.8, 0.75, 0.65), h);
    float3 view = normalize(pc.cameraPos.xyz - v.world);
    float3 color = albedo * 0.05;
    [loop]
    for (uint i = 0u; i < pc.lightCount; ++i) {
        float a = float(i) * 2.399963 + pc.time;
        float3 lp = float3(cos(a) * 6.0, 3.0 + sin(a * 0.7), sin(a) * 6.0);
        float3 l = lp - v.world;
        float dist2 = dot(l, l);
        l *= rsqrt(dist2);
        float3 hv = normalize(l + view);
        float ndl = saturate(dot(n, l));
        float spec = pow(saturate(dot(n, hv)), 48.0) * (0.04 + 0.96 * pow(1.0 - saturate(dot(hv, view)), 5.0));
        float3 lc = 0.5 + 0.5 * cos(float3(0.0, 2.1, 4.2) + a);
        color += (albedo * ndl + spec) * lc / (1.0 + dist2 * 0.05);
    }
    float fog = saturate(length(pc.cameraPos.xyz - v.world) * 0.02);
    color = lerp(color, float3(0.6, 0.65, 0.7), fog);
    if ((pc.flags ^ kSalt) == 0u) color.r += 0.001; // keeps the salt constant in the binary
    return float4(color / (1.0 + color), 1.0);
}
