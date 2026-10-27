// "Skyline" (modern tier): a procedural city at dusk. GPU-driven: a compute pass frustum-culls
// every instance against the camera and four shadow cascades and writes compacted instance
// lists plus indirect draw arguments, so the CPU records a fixed handful of draws. Forward PBR
// shading with cascaded shadow maps into an HDR target; ambient occlusion and bloom run on the
// async compute queue while the GPU renders the next frame; a composite pass tone-maps.
#include "scenes/skyline_common.slang"

struct DrawConstants {
    uint visibleBase; // offset into the visible list for this view + mesh
    uint view;        // 0 = camera, 1..4 = cascades
    uint pad0;
    uint pad1;
};
GT_PUSH_CONSTANTS(DrawConstants, pc);

StructuredBuffer<Instance> instances : register(t0);
StructuredBuffer<MeshInfo> meshes : register(t1);
StructuredBuffer<uint> visible : register(t2);
RWStructuredBuffer<uint> visibleOut : register(u0);
RWStructuredBuffer<uint> drawArgs : register(u1); // kViews x kMeshes x {indexCount, instanceCount, firstIndex, baseVertex, firstInstance}

float3x3 rotY(float a) {
    float s = sin(a), c = cos(a);
    return float3x3(c, 0, s, 0, 1, 0, -s, 0, c);
}

// --- culling --------------------------------------------------------------------------------
[shader("compute")]
[numthreads(64, 1, 1)]
void csResetArgs(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= kViews * kMeshes) return;
    MeshInfo m = meshes[tid.x % kMeshes];
    uint o = tid.x * 5u;
    drawArgs[o + 0] = m.indexCount;
    drawArgs[o + 1] = 0u;
    drawArgs[o + 2] = m.firstIndex;
    drawArgs[o + 3] = asuint(m.baseVertex);
    drawArgs[o + 4] = 0u;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csCull(uint3 tid: SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= frame.counts.x) return;
    Instance inst = instances[i];
    uint mesh = uint(inst.scaleMesh.w);
    MeshInfo m = meshes[mesh];
    float3 centre = mul(rotY(inst.posRot.w), m.sphere.xyz * inst.scaleMesh.xyz) + inst.posRot.xyz;
    float radius = m.sphere.w * max(inst.scaleMesh.x, max(inst.scaleMesh.y, inst.scaleMesh.z));
    [loop]
    for (uint v = 0; v < kViews; ++v) {
        bool inside = true;
        [unroll]
        for (uint p = 0; p < 6; ++p) {
            float4 pl = frame.views[v].planes[p];
            inside = inside && dot(pl.xyz, centre) + pl.w >= -radius;
        }
        // Tiny objects far from the camera are not drawn into the camera view (detail culling).
        if (v == 0 && radius < distance(centre, frame.cameraPos.xyz) * 0.002) inside = false;
        if (inside) {
            uint slot;
            InterlockedAdd(drawArgs[(v * kMeshes + mesh) * 5u + 1u], 1u, slot);
            visibleOut[v * frame.counts.y + m.instanceBase + slot] = i;
        }
    }
}

// --- geometry -------------------------------------------------------------------------------
struct VSIn {
    [[vk::location(0)]] float3 pos : ATTRIB0;
    [[vk::location(1)]] float3 normal : ATTRIB1;
    [[vk::location(2)]] float2 uv : ATTRIB2;
};

struct VSOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 local : TEXCOORD2; // object-space position * scale (facade coordinates in metres)
    nointerpolation float4 color : TEXCOORD3;
    nointerpolation float3 scale : TEXCOORD4;
};

Instance fetch(uint instanceId) { return instances[visible[pc.visibleBase + instanceId]]; }

[shader("vertex")]
VSOut vsScene(VSIn v, uint iid: SV_InstanceID) {
    Instance inst = fetch(iid);
    float3x3 r = rotY(inst.posRot.w);
    VSOut o;
    o.local = v.pos * inst.scaleMesh.xyz;
    o.world = mul(r, o.local) + inst.posRot.xyz;
    o.normal = mul(r, v.normal / inst.scaleMesh.xyz);
    o.pos = mul(frame.viewProj, float4(o.world, 1.0));
    o.color = inst.color;
    o.scale = inst.scaleMesh.xyz;
    return o;
}

[shader("vertex")]
float4 vsShadow(VSIn v, uint iid: SV_InstanceID) : SV_Position {
    Instance inst = fetch(iid);
    float3 world = mul(rotY(inst.posRot.w), v.pos * inst.scaleMesh.xyz) + inst.posRot.xyz;
    return mul(frame.cascadeViewProj[pc.view - 1], float4(world, 1.0));
}

// --- shading --------------------------------------------------------------------------------
Texture2D<float> shadowAtlas : register(t3);
SamplerComparisonState shadowSampler : register(s0);

static const float kConcrete = 0.0, kGlassTower = 1.0, kBrick = 2.0, kRoad = 3.0, kFoliage = 4.0, kCarPaint = 5.0,
                   kMetal = 6.0, kLight = 7.0;

float hash3(float3 p) {
    uint3 q = uint3(int3(floor(p)) + 65536);
    return gtHashFloat(q.x * 73856093u ^ q.y * 19349663u ^ q.z * 83492791u);
}

float cascadeShadow(float3 world, float3 n) {
    float viewDepth = dot(world - frame.cameraPos.xyz, frame.cameraFwd.xyz);
    uint c = 0;
    [unroll]
    for (uint k = 0; k < kCascades - 1; ++k)
        if (viewDepth > frame.cascadeSplits[k]) c = k + 1;
    if (viewDepth > frame.cascadeSplits[kCascades - 1]) return 1.0;
    float4 sp = mul(frame.cascadeViewProj[c], float4(world + n * (0.05 + 0.1 * float(c)), 1.0));
    float3 s = sp.xyz / sp.w;
    float2 uv = s.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    float2 atlas = (uv + float2(float(c & 1u), float(c >> 1))) * 0.5; // 2 x 2 tiles
    float texel = frame.sunColor.a, sum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            sum += shadowAtlas.SampleCmpLevelZero(shadowSampler, atlas + float2(x, y) * texel, s.z - 0.0008);
    return sum / 9.0;
}

float3 skyColor(float3 dir) {
    float h = saturate(dir.y);
    float3 horizon = float3(1.0, 0.55, 0.32), zenith = float3(0.16, 0.24, 0.48);
    float3 sky = lerp(horizon, zenith, pow(h, 0.45));
    float sd = saturate(dot(dir, frame.sunDir.xyz));
    return sky + frame.sunColor.rgb * (pow(sd, 800.0) * 30.0 + pow(sd, 12.0) * 0.6);
}

float D_GGX(float nh, float a) {
    float a2 = a * a, d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (GT_PI * d * d);
}

float3 brdf(float3 albedo, float metal, float rough, float3 n, float3 v, float3 l) {
    float3 h = normalize(v + l);
    float nl = saturate(dot(n, l)), nv = max(dot(n, v), 1e-3), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float a = max(rough * rough, 0.02);
    float k = a * 0.5;
    float g = (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 f = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    float3 spec = D_GGX(nh, a) * g * f / (4.0 * nv + 1e-3);
    float3 diff = (1.0 - f) * (1.0 - metal) * albedo / GT_PI;
    return (diff + spec / max(nl, 1e-3) * nl) * nl;
}

struct SceneOut {
    float4 hdr : SV_Target0;
    float4 normal : SV_Target1; // xyz view-space-agnostic world normal * 0.5 + 0.5, w = roughness
};

[shader("fragment")]
SceneOut psScene(VSOut i) {
    float3 n = normalize(i.normal);
    float3 v = normalize(frame.cameraPos.xyz - i.world);
    float material = i.color.w;
    float3 albedo = i.color.rgb;
    float metal = 0.0, rough = 0.7;
    float3 emissive = 0.0;
    if (material == kConcrete || material == kGlassTower || material == kBrick) {
        // Facade: window grid on the walls (floor height 3.4 m, bays 2.6 m); roofs stay plain.
        if (abs(n.y) < 0.5) {
            float along = abs(n.x) > abs(n.z) ? i.local.z : i.local.x;
            float2 cell = float2(along / 2.6, (i.local.y) / 3.4);
            float2 f = frac(cell);
            bool window = f.x > 0.18 && f.x < 0.82 && f.y > 0.25 && f.y < 0.85 && i.local.y > 3.0;
            float glassRatio = material == kGlassTower ? 1.0 : 0.0;
            if (window || glassRatio > 0.5) {
                float lit = step(0.62, hash3(float3(floor(cell), i.color.r * 97.0 + i.color.g * 13.0)));
                albedo = float3(0.04, 0.05, 0.06);
                metal = 0.0;
                rough = 0.08;
                emissive = lit * float3(1.6, 1.15, 0.7) * (window ? 1.0 : 0.35);
            } else if (material == kBrick) {
                albedo *= 0.8 + 0.2 * hash3(float3(floor(cell * float2(6.0, 12.0)), 3.0));
            }
        }
    } else if (material == kRoad) {
        float2 p = i.world.xz;
        float2 g = abs(frac(p / 60.0 + 0.5) - 0.5) * 60.0; // distance to the street centre lines
        // Masks from step(), not bool ?: — Slang's optimizer turns (b ? 1 : 0) * vector into a vector select
        // on a scalar condition, which SPIR-V 1.3 does not allow.
        float lane = max(step(g.x, 0.12), step(g.y, 0.12)) * step(frac((p.x + p.y) * 0.25), 0.5);
        bool street = g.x < 7.0 || g.y < 7.0;
        float streetMask = max(step(g.x, 7.0), step(g.y, 7.0));
        albedo = lerp(float3(0.30, 0.30, 0.29), float3(0.06, 0.06, 0.065) + lane * float3(0.7, 0.62, 0.3), streetMask);
        rough = street ? 0.5 : 0.85;
    } else if (material == kCarPaint) {
        metal = 0.6;
        rough = 0.25;
    } else if (material == kMetal) {
        metal = 1.0;
        rough = 0.35;
    } else if (material == kLight) {
        emissive = albedo * 6.0;
    }
    float shadow = cascadeShadow(i.world, n);
    float3 direct = brdf(albedo, metal, rough, n, v, frame.sunDir.xyz) * frame.sunColor.rgb * frame.sunDir.w * shadow;
    float3 ambientSky = lerp(float3(0.10, 0.08, 0.08), float3(0.20, 0.24, 0.34), n.y * 0.5 + 0.5);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float fres = pow(1.0 - saturate(dot(n, v)), 5.0);
    float3 reflection = skyColor(reflect(-v, n)) * (f0 + (1.0 - f0) * fres) * (1.0 - rough) * 0.6;
    float3 color = direct + albedo * (1.0 - metal) * ambientSky + reflection + emissive;
    float dist = length(frame.cameraPos.xyz - i.world);
    float fog = (1.0 - exp(-dist * 0.0009)) * 0.85;
    color = lerp(color, skyColor(-v) * 0.9, fog);
    SceneOut o;
    o.hdr = float4(color, 1.0);
    o.normal = float4(n * 0.5 + 0.5, rough);
    return o;
}

// --- sky (graphics, full-screen at the far plane) ---------------------------------------------
struct FsOut {
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

[shader("vertex")]
FsOut vsFullscreen(uint vid: SV_VertexID) {
    float2 uv = float2(float((vid << 1u) & 2u), float(vid & 2u));
    FsOut o;
    o.ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}

[shader("fragment")]
SceneOut psSky(FsOut i) {
    float4 p = mul(frame.invViewProj, float4(i.ndc, 1.0, 1.0));
    SceneOut o;
    o.hdr = float4(skyColor(normalize(p.xyz / p.w - frame.cameraPos.xyz)), 1.0);
    o.normal = float4(0.5, 1.0, 0.5, 1.0);
    return o;
}

// --- composite (graphics, after the async work) ----------------------------------------------
SamplerState linearClamp : register(s1);
Texture2D<float4> hdrTex : register(t8);
Texture2D<float> aoTex : register(t9);
Texture2D<float4> bloomTex : register(t10);

[shader("fragment")]
float4 psComposite(FsOut i) : SV_Target {
    int2 px = int2(i.pos.xy);
    float2 uv = i.pos.xy * frame.screen.zw;
    float3 hdr = hdrTex.Load(int3(px, 0)).rgb;
    // 3x3 blur of the (noisy, 8-sample) occlusion.
    float ao = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x) ao += aoTex.Load(int3(clamp(px + int2(x, y), int2(0, 0), int2(frame.screen.xy) - 1), 0));
    ao /= 9.0;
    float3 bloom = bloomTex.SampleLevel(linearClamp, uv, 0).rgb;
    float3 x = (hdr * lerp(1.0, ao, 0.7) + bloom * 0.06) * 0.9;
    x = saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
    float vignette = 1.0 - 0.3 * dot(uv - 0.5, uv - 0.5);
    return float4(pow(x * vignette, 1.0 / 2.2), 1.0);
}
