// "Courtyard" (legacy tier): an arcaded stone plaza with a fountain on rolling terrain with
// trees and rocks, lit by the sun with a PCF shadow map, forward shaded with procedural
// materials, fog and a sky. Uses only what OpenGL 3.3 / D3D11 feature level 10 offer
// (vertex + pixel shaders, constant buffers, textures, comparison samplers); the terrain gets
// hull/domain-shader displacement where tessellation exists.
//
// Objects: position + rotation about Y, non-uniform scale + material, colour — either from a
// per-draw constant buffer (engine-like: one draw per object) or from per-instance vertex
// attributes (CPU-light: one draw per mesh type).
#include "common/bindings.slang"

struct FrameData {
    float4x4 viewProj;
    float4x4 sunViewProj;
    float4 cameraPos; // xyz, w = time
    float4 sunDir;    // xyz towards the sun, w = intensity
    float4 sunColor;  // rgb, a = shadow-map texel size
    float4 fog;       // rgb, a = density
    float4 tess;      // x = max tessellation factor, y = full-detail distance, z = no-detail distance, w = amplitude
};
ConstantBuffer<FrameData> frame : register(b0);

struct ObjectData {
    float4 posRot;   // xyz position, w = rotation about Y (radians)
    float4 scaleMat; // xyz scale, w = material id
    float4 color;    // rgb albedo
};
ConstantBuffer<ObjectData> object : register(b1);

Texture2D<float> shadowMap : register(t0);
SamplerComparisonState shadowSampler : register(s0);

struct VSIn {
    [[vk::location(0)]] float3 pos : ATTRIB0;
    [[vk::location(1)]] float3 normal : ATTRIB1;
    [[vk::location(2)]] float2 uv : ATTRIB2;
};

// One struct for all instanced inputs: Slang offsets explicit locations of a second input struct.
struct VSInstIn {
    [[vk::location(0)]] float3 pos : ATTRIB0;
    [[vk::location(1)]] float3 normal : ATTRIB1;
    [[vk::location(2)]] float2 uv : ATTRIB2;
    [[vk::location(3)]] float4 posRot : ATTRIB3;
    [[vk::location(4)]] float4 scaleMat : ATTRIB4;
    [[vk::location(5)]] float4 color : ATTRIB5;
};

VSIn vertexOf(VSInstIn v) {
    VSIn o;
    o.pos = v.pos;
    o.normal = v.normal;
    o.uv = v.uv;
    return o;
}

struct VSOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation float4 color : TEXCOORD3; // rgb albedo, a = material
};

static const float kStone = 0.0, kPlaster = 1.0, kBark = 2.0, kLeaves = 3.0, kWater = 4.0, kRoof = 5.0, kTerrain = 6.0,
                   kMetal = 7.0, kLamp = 8.0;

float3x3 rotY(float a) {
    float s = sin(a), c = cos(a);
    return float3x3(c, 0, s, 0, 1, 0, -s, 0, c);
}

VSOut transformObject(VSIn v, float4 posRot, float4 scaleMat, float4 color) {
    float3x3 r = rotY(posRot.w);
    VSOut o;
    o.world = mul(r, v.pos * scaleMat.xyz) + posRot.xyz;
    o.normal = mul(r, v.normal / scaleMat.xyz);
    o.pos = mul(frame.viewProj, float4(o.world, 1.0));
    o.uv = v.uv;
    o.color = float4(color.rgb, scaleMat.w);
    return o;
}

[shader("vertex")]
VSOut vsObject(VSIn v) { return transformObject(v, object.posRot, object.scaleMat, object.color); }

[shader("vertex")]
VSOut vsInstanced(VSInstIn v) { return transformObject(vertexOf(v), v.posRot, v.scaleMat, v.color); }

[shader("vertex")]
VSOut vsTerrain(VSIn v) {
    VSOut o;
    o.world = v.pos;
    o.normal = v.normal;
    o.pos = mul(frame.viewProj, float4(v.pos, 1.0));
    o.uv = v.uv;
    o.color = float4(1.0, 1.0, 1.0, kTerrain);
    return o;
}

// --- shadow pass (depth only) -------------------------------------------------------------
float4 shadowPos(float3 p, float4 posRot, float4 scaleMat) {
    return mul(frame.sunViewProj, float4(mul(rotY(posRot.w), p * scaleMat.xyz) + posRot.xyz, 1.0));
}

[shader("vertex")]
float4 vsShadowObject(VSIn v) : SV_Position { return shadowPos(v.pos, object.posRot, object.scaleMat); }

[shader("vertex")]
float4 vsShadowInstanced(VSInstIn v) : SV_Position { return shadowPos(v.pos, v.posRot, v.scaleMat); }

[shader("vertex")]
float4 vsShadowTerrain(VSIn v) : SV_Position { return mul(frame.sunViewProj, float4(v.pos, 1.0)); }

// --- procedural materials -----------------------------------------------------------------
float hash2(float2 p) {
    uint2 q = uint2(int2(floor(p)) + 32768);
    return gtHashFloat(q.x * 73856093u ^ q.y * 19349663u);
}

float valueNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash2(i), hash2(i + float2(1, 0)), u.x), lerp(hash2(i + float2(0, 1)), hash2(i + float2(1, 1)), u.x), u.y);
}

float fbm(float2 p) {
    float s = 0.0, a = 0.5;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        s += a * valueNoise(p);
        p = p * 2.03 + 11.7;
        a *= 0.5;
    }
    return s;
}

/// Albedo multiplier and specular strength for a material at a world position.
float2 materialDetail(float material, float3 w, float3 n, float2 uv) {
    float2 planar = abs(n.y) > 0.7 ? w.xz : (abs(n.x) > abs(n.z) ? w.zy : w.xy);
    if (material == kStone) {
        float blocks = step(0.06, frac(planar.x * 0.5)) * step(0.06, frac(planar.y * 0.5 + 0.5 * floor(planar.x * 0.5)));
        return float2((0.75 + 0.35 * fbm(planar * 3.0)) * lerp(0.7, 1.0, blocks), 0.15);
    }
    if (material == kPlaster) return float2(0.85 + 0.2 * fbm(planar * 1.5), 0.05);
    if (material == kBark) return float2(0.6 + 0.5 * valueNoise(float2(uv.x * 30.0, uv.y * 4.0)), 0.02);
    if (material == kLeaves) return float2(0.55 + 0.6 * fbm(w.xz * 2.5 + w.y), 0.1);
    if (material == kWater) return float2(0.9 + 0.1 * sin(w.x * 3.0 + frame.cameraPos.w * 2.0), 1.2);
    if (material == kRoof) return float2(0.7 + 0.3 * step(0.5, frac(planar.y * 2.0)), 0.25);
    if (material == kMetal) return float2(0.9, 0.9);
    if (material == kLamp) return float2(1.0, 0.0);
    return float2(1.0, 0.1);
}

float3 terrainAlbedo(float3 w, float3 n) {
    float grassNoise = fbm(w.xz * 0.35);
    float3 grass = lerp(float3(0.20, 0.32, 0.10), float3(0.34, 0.42, 0.16), grassNoise);
    float3 rock = float3(0.42, 0.40, 0.37) * (0.8 + 0.4 * fbm(w.xz * 1.3 + w.y));
    float3 path = float3(0.55, 0.48, 0.38) * (0.85 + 0.3 * valueNoise(w.xz * 4.0));
    float slope = 1.0 - n.y;
    float3 c = lerp(grass, rock, smoothstep(0.25, 0.45, slope));
    return lerp(c, path, smoothstep(31.0, 29.0, length(w.xz)) * (1.0 - smoothstep(0.2, 0.4, slope)));
}

float sunShadow(float3 world, float3 n) {
    float4 sp = mul(frame.sunViewProj, float4(world + n * 0.05, 1.0));
    float3 c = sp.xyz / sp.w;
    float2 uv = c.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || c.z > 1.0) return 1.0;
    float texel = frame.sunColor.a, sum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            sum += shadowMap.SampleCmpLevelZero(shadowSampler, uv + float2(x, y) * texel, c.z - 0.0015);
    return sum / 9.0;
}

float3 skyColor(float3 dir) {
    float h = saturate(dir.y);
    float3 sky = lerp(float3(0.72, 0.78, 0.86), float3(0.30, 0.48, 0.78), pow(h, 0.6));
    float sun = pow(saturate(dot(dir, frame.sunDir.xyz)), 600.0) * 12.0 + pow(saturate(dot(dir, frame.sunDir.xyz)), 8.0) * 0.25;
    return sky + frame.sunColor.rgb * sun;
}

// Filmic tone curve (Narkowicz's ACES fit) and gamma: more contrast than Reinhard in sunlight.
float3 tonemap(float3 x) {
    x *= 0.6;
    x = saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
    return pow(x, 1.0 / 2.2);
}

float3 finish(float3 color, float3 world) {
    float dist = length(frame.cameraPos.xyz - world);
    color = lerp(color, frame.fog.rgb, 1.0 - exp(-dist * frame.fog.a));
    return tonemap(color);
}

[shader("fragment")]
float4 psScene(VSOut i) : SV_Target {
    float3 n = normalize(i.normal);
    float material = i.color.a;
    float3 albedo;
    float spec;
    if (material == kTerrain) {
        albedo = terrainAlbedo(i.world, n);
        spec = 0.03;
    } else {
        float2 d = materialDetail(material, i.world, n, i.uv);
        albedo = i.color.rgb * d.x;
        spec = d.y;
    }
    float3 v = normalize(frame.cameraPos.xyz - i.world);
    if (material == kLeaves && dot(n, v) < 0.0) n = -n; // two-sided foliage
    float3 l = frame.sunDir.xyz;
    float shadow = sunShadow(i.world, n);
    float ndl = saturate(dot(n, l));
    float3 h = normalize(l + v);
    float power = material == kWater || material == kMetal ? 180.0 : 32.0;
    float specular = pow(saturate(dot(n, h)), power) * spec;
    float3 ambient = lerp(float3(0.12, 0.11, 0.10), float3(0.22, 0.28, 0.38), n.y * 0.5 + 0.5);
    float3 color = albedo * (ambient + frame.sunColor.rgb * frame.sunDir.w * ndl * shadow) + frame.sunColor.rgb * specular * shadow;
    if (material == kWater) color = lerp(color, skyColor(reflect(-v, n)) * 0.8, 0.35);
    if (material == kLamp) color = albedo * 2.5;
    return float4(finish(color, i.world), 1.0);
}

// --- sky (full-screen triangle at the far plane) --------------------------------------------
struct SkyOut {
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

[shader("vertex")]
SkyOut vsSky(uint vid: SV_VertexID) {
    float2 uv = float2(float((vid << 1u) & 2u), float(vid & 2u));
    SkyOut o;
    o.ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}

struct SkyParams {
    float4x4 invViewProj;
};
ConstantBuffer<SkyParams> sky : register(b2);

[shader("fragment")]
float4 psSky(SkyOut i) : SV_Target {
    float4 p = mul(sky.invViewProj, float4(i.ndc, 1.0, 1.0));
    float3 dir = normalize(p.xyz / p.w - frame.cameraPos.xyz);
    float3 color = lerp(skyColor(dir), frame.fog.rgb, saturate(1.0 - dir.y * 4.0) * 0.5);
    return float4(tonemap(color), 1.0);
}

// --- terrain tessellation (where available) -------------------------------------------------
struct TessCP {
    float3 pos : WORLDPOS;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct TessFactors {
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

[shader("vertex")]
TessCP vsTerrainTess(VSIn v) {
    TessCP o;
    o.pos = v.pos;
    o.normal = v.normal;
    o.uv = v.uv;
    return o;
}

float tessFactor(float3 a, float3 b) {
    float d = distance(frame.cameraPos.xyz, (a + b) * 0.5);
    return lerp(frame.tess.x, 1.0, saturate((d - frame.tess.y) / (frame.tess.z - frame.tess.y)));
}

TessFactors terrainPatch(InputPatch<TessCP, 3> p) {
    TessFactors f;
    f.edges[0] = tessFactor(p[1].pos, p[2].pos);
    f.edges[1] = tessFactor(p[2].pos, p[0].pos);
    f.edges[2] = tessFactor(p[0].pos, p[1].pos);
    f.inside = (f.edges[0] + f.edges[1] + f.edges[2]) / 3.0;
    return f;
}

[shader("hull")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(3)]
[patchconstantfunc("terrainPatch")]
TessCP hsTerrain(InputPatch<TessCP, 3> p, uint i: SV_OutputControlPointID) { return p[i]; }

float detailHeight(float2 xz) { return (fbm(xz * 0.9) - 0.5) * frame.tess.w; }

[shader("domain")]
[domain("tri")]
VSOut dsTerrain(TessFactors f, float3 bary: SV_DomainLocation, const OutputPatch<TessCP, 3> p) {
    float3 w = p[0].pos * bary.x + p[1].pos * bary.y + p[2].pos * bary.z;
    float3 n = normalize(p[0].normal * bary.x + p[1].normal * bary.y + p[2].normal * bary.z);
    float2 uv = p[0].uv * bary.x + p[1].uv * bary.y + p[2].uv * bary.z;
    // Detail displacement fades out near the plaza so the paving stays flat.
    float fade = smoothstep(26.0, 34.0, length(w.xz));
    float e = 0.25;
    float h0 = detailHeight(w.xz) * fade;
    float hx = detailHeight(w.xz + float2(e, 0.0)) * fade, hz = detailHeight(w.xz + float2(0.0, e)) * fade;
    w.y += h0;
    n = normalize(n + float3(h0 - hx, 0.0, h0 - hz) / e);
    VSOut o;
    o.world = w;
    o.normal = n;
    o.pos = mul(frame.viewProj, float4(w, 1.0));
    o.uv = uv;
    o.color = float4(1.0, 1.0, 1.0, kTerrain);
    return o;
}
