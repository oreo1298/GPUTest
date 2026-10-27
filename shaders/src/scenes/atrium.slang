// "Atrium" (legacy tier): a colonnaded hall with two balcony levels under a skylight, rendered
// with classic deferred shading — a G-buffer (albedo + specular, normal + gloss, depth), a sun
// pass with a shadow map through the skylight, hundreds of moving point lights drawn as light
// volumes with additive blending, and a tone-mapping pass. Vertex and pixel shaders, constant
// buffers and texture loads only (OpenGL 3.3 / D3D11 feature level 10).
#include "common/bindings.slang"

struct FrameData {
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 sunViewProj;
    float4 cameraPos; // xyz, w = time
    float4 sunDir;    // xyz towards the sun, w = intensity
    float4 sunColor;  // rgb, a = shadow-map texel size
    float4 screen;    // width, height, 1/width, 1/height
};
ConstantBuffer<FrameData> frame : register(b0);

struct ObjectData {
    float4 posRot;   // xyz position, w = rotation about Y
    float4 scaleMat; // xyz scale, w = material
    float4 color;    // rgb albedo
};
ConstantBuffer<ObjectData> object : register(b1);

// Point light: animated on the GPU from these parameters, so every variant draws the same lights.
struct LightData {
    float4 orbit;  // xyz orbit centre, w = orbit radius
    float4 motion; // x = angular speed, y = phase, z = vertical bob, w = range
    float4 color;  // rgb colour * intensity
};
ConstantBuffer<LightData> light : register(b2);

Texture2D<float4> gAlbedo : register(t0);
Texture2D<float4> gNormal : register(t1);
Texture2D<float> gDepth : register(t2);
Texture2D<float> shadowMap : register(t3);
Texture2D<float4> hdr : register(t4);
SamplerComparisonState shadowSampler : register(s0);

struct VSIn {
    [[vk::location(0)]] float3 pos : ATTRIB0;
    [[vk::location(1)]] float3 normal : ATTRIB1;
    [[vk::location(2)]] float2 uv : ATTRIB2;
};

struct VSInstIn {
    [[vk::location(0)]] float3 pos : ATTRIB0;
    [[vk::location(1)]] float3 normal : ATTRIB1;
    [[vk::location(2)]] float2 uv : ATTRIB2;
    [[vk::location(3)]] float4 a : ATTRIB3; // ObjectData.posRot or LightData.orbit
    [[vk::location(4)]] float4 b : ATTRIB4; // ObjectData.scaleMat or LightData.motion
    [[vk::location(5)]] float4 c : ATTRIB5; // ObjectData.color or LightData.color
};

static const float kMarble = 0.0, kStone = 1.0, kWood = 2.0, kBrass = 3.0, kLeaves = 4.0, kFabric = 5.0, kPlaster = 6.0,
                   kGlow = 7.0;

float3x3 rotY(float a) {
    float s = sin(a), c = cos(a);
    return float3x3(c, 0, s, 0, 1, 0, -s, 0, c);
}

struct GOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    nointerpolation float4 color : TEXCOORD2; // rgb albedo, a = material
};

GOut transformObject(float3 p, float3 n, float4 posRot, float4 scaleMat, float4 color) {
    float3x3 r = rotY(posRot.w);
    GOut o;
    o.world = mul(r, p * scaleMat.xyz) + posRot.xyz;
    o.normal = mul(r, n / scaleMat.xyz);
    o.pos = mul(frame.viewProj, float4(o.world, 1.0));
    o.color = float4(color.rgb, scaleMat.w);
    return o;
}

[shader("vertex")]
GOut vsObject(VSIn v) { return transformObject(v.pos, v.normal, object.posRot, object.scaleMat, object.color); }

[shader("vertex")]
GOut vsInstanced(VSInstIn v) { return transformObject(v.pos, v.normal, v.a, v.b, v.c); }

float4 shadowPos(float3 p, float4 posRot, float4 scaleMat) {
    return mul(frame.sunViewProj, float4(mul(rotY(posRot.w), p * scaleMat.xyz) + posRot.xyz, 1.0));
}

[shader("vertex")]
float4 vsShadowObject(VSIn v) : SV_Position { return shadowPos(v.pos, object.posRot, object.scaleMat); }

[shader("vertex")]
float4 vsShadowInstanced(VSInstIn v) : SV_Position { return shadowPos(v.pos, v.a, v.b); }

// --- G-buffer -------------------------------------------------------------------------------
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

struct GBuffer {
    float4 albedo : SV_Target0; // rgb albedo, a = specular intensity
    float4 normal : SV_Target1; // xyz world normal, w = gloss (0..1)
};

[shader("fragment")]
GBuffer psGBuffer(GOut i) {
    float3 n = normalize(i.normal);
    float m = i.color.a;
    float3 albedo = i.color.rgb;
    float spec = 0.1, gloss = 0.3;
    float2 planar = abs(n.y) > 0.7 ? i.world.xz : (abs(n.x) > abs(n.z) ? i.world.zy : i.world.xy);
    if (m == kMarble) {
        float2 tile = floor(i.world.xz * 0.5);
        float checker = fmod(abs(tile.x + tile.y), 2.0);
        float veins = pow(abs(sin(i.world.x * 1.7 + fbm(i.world.xz * 0.8) * 6.0)), 6.0);
        albedo = lerp(float3(0.82, 0.80, 0.76), float3(0.20, 0.22, 0.25), checker) * (0.9 + 0.1 * fbm(i.world.xz * 3.0));
        albedo = lerp(albedo, albedo * 0.6, veins * 0.6);
        spec = 0.6;
        gloss = 0.85;
    } else if (m == kStone) {
        albedo *= 0.8 + 0.3 * fbm(planar * 2.0);
    } else if (m == kWood) {
        albedo *= 0.7 + 0.3 * sin(planar.x * 18.0 + fbm(planar * 2.0) * 4.0);
        spec = 0.2;
        gloss = 0.5;
    } else if (m == kBrass) {
        spec = 0.9;
        gloss = 0.8;
    } else if (m == kLeaves) {
        albedo *= 0.6 + 0.6 * fbm(i.world.xz * 3.0 + i.world.y);
        spec = 0.05;
    } else if (m == kFabric) {
        albedo *= 0.85 + 0.15 * step(0.5, frac(planar.x * 8.0));
        spec = 0.02;
    } else if (m == kPlaster) {
        albedo *= 0.9 + 0.15 * fbm(planar * 1.2);
        spec = 0.05;
    } else if (m == kGlow) {
        spec = -1.0; // marker: emissive (lit pass adds the albedo as light)
    }
    GBuffer g;
    g.albedo = float4(albedo, spec < 0.0 ? 1.0 : spec * 0.5);
    g.normal = float4(n, spec < 0.0 ? -1.0 : gloss);
    return g;
}

// --- full-screen passes ------------------------------------------------------------------------
struct FsOut {
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

[shader("vertex")]
FsOut vsFullscreen(uint vid: SV_VertexID) {
    float2 uv = float2(float((vid << 1u) & 2u), float(vid & 2u));
    FsOut o;
    o.ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.pos = float4(o.ndc, 0.0, 1.0);
    return o;
}

float3 worldFromDepth(float2 pixel, float depth) {
    float2 ndc = (pixel * frame.screen.zw) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 p = mul(frame.invViewProj, float4(ndc, depth, 1.0));
    return p.xyz / p.w;
}

float sunShadow(float3 world, float3 n) {
    float4 sp = mul(frame.sunViewProj, float4(world + n * 0.04, 1.0));
    float3 c = sp.xyz / sp.w;
    float2 uv = c.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || c.z > 1.0) return 1.0;
    float texel = frame.sunColor.a, sum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            sum += shadowMap.SampleCmpLevelZero(shadowSampler, uv + float2(x, y) * texel, c.z - 0.001);
    return sum / 9.0;
}

float3 shade(float3 albedo, float specI, float3 n, float gloss, float3 l, float3 radiance, float3 v) {
    float ndl = saturate(dot(n, l));
    float3 h = normalize(l + v);
    float power = exp2(2.0 + gloss * 9.0);
    float specular = pow(saturate(dot(n, h)), power) * specI * (power + 8.0) / 25.0;
    return (albedo * ndl + specular * ndl) * radiance;
}

[shader("fragment")]
float4 psSunAmbient(FsOut i) : SV_Target {
    int3 px = int3(int2(i.pos.xy), 0);
    float depth = gDepth.Load(px);
    if (depth >= 1.0) {
        // Sky seen through the skylight.
        float3 dir = normalize(worldFromDepth(i.pos.xy, 1.0) - frame.cameraPos.xyz);
        return float4(lerp(float3(0.55, 0.65, 0.80), float3(0.25, 0.42, 0.75), saturate(dir.y)) * 1.6, 1.0);
    }
    float4 a = gAlbedo.Load(px);
    float4 nn = gNormal.Load(px);
    if (nn.w < 0.0) return float4(a.rgb * 3.0, 1.0); // emissive
    float3 world = worldFromDepth(i.pos.xy, depth);
    float3 n = normalize(nn.xyz);
    float3 v = normalize(frame.cameraPos.xyz - world);
    float shadow = sunShadow(world, n);
    float3 sun = shade(a.rgb, a.a * 2.0, n, nn.w, frame.sunDir.xyz, frame.sunColor.rgb * frame.sunDir.w * shadow, v);
    float3 ambient = a.rgb * lerp(float3(0.05, 0.045, 0.04), float3(0.09, 0.10, 0.12), n.y * 0.5 + 0.5);
    return float4(sun + ambient, 1.0);
}

// --- point-light volumes (spheres, back faces, depth test "behind the scene") ------------------
struct LightOut {
    float4 pos : SV_Position;
    nointerpolation float4 lightPos : TEXCOORD0; // xyz, w = range
    nointerpolation float3 color : TEXCOORD1;
};

LightOut lightVolume(float3 p, float4 orbit, float4 motion, float4 color) {
    float t = frame.cameraPos.w;
    float a = motion.y + t * motion.x;
    float3 centre = orbit.xyz + float3(cos(a) * orbit.w, sin(t * 0.7 + motion.y) * motion.z, sin(a) * orbit.w);
    LightOut o;
    o.lightPos = float4(centre, motion.w);
    o.color = color.rgb;
    o.pos = mul(frame.viewProj, float4(centre + p * motion.w * 1.08, 1.0)); // sphere tessellation margin
    return o;
}

[shader("vertex")]
LightOut vsLight(VSIn v) { return lightVolume(v.pos, light.orbit, light.motion, light.color); }

[shader("vertex")]
LightOut vsLightInstanced(VSInstIn v) { return lightVolume(v.pos, v.a, v.b, v.c); }

[shader("fragment")]
float4 psLight(LightOut i) : SV_Target {
    int3 px = int3(int2(i.pos.xy), 0);
    float depth = gDepth.Load(px);
    float4 nn = gNormal.Load(px);
    if (depth >= 1.0 || nn.w < 0.0) discard;
    float3 world = worldFromDepth(i.pos.xy, depth);
    float3 toLight = i.lightPos.xyz - world;
    float dist = length(toLight);
    float range = i.lightPos.w;
    if (dist >= range) discard;
    float x = dist / range;
    float atten = saturate(1.0 - x * x * x * x);
    atten = atten * atten / (1.0 + dist * dist * 0.6);
    float4 a = gAlbedo.Load(px);
    float3 n = normalize(nn.xyz);
    float3 v = normalize(frame.cameraPos.xyz - world);
    return float4(shade(a.rgb, a.a * 2.0, n, nn.w, toLight / dist, i.color * atten, v), 0.0);
}

// --- tone mapping --------------------------------------------------------------------------------
[shader("fragment")]
float4 psTonemap(FsOut i) : SV_Target {
    float3 x = hdr.Load(int3(int2(i.pos.xy), 0)).rgb * 0.9;
    x = saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
    float2 uv = i.pos.xy * frame.screen.zw;
    float vignette = 1.0 - 0.25 * dot(uv - 0.5, uv - 0.5) * 2.0;
    return float4(pow(x * vignette, 1.0 / 2.2), 1.0);
}
