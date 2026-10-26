// "Lattice": the vertical-slice scene. A grid of instanced, individually
// animated blocks over a reflective floor. Instance transforms are computed
// procedurally in the vertex shader from SV_InstanceID and simulated time, so
// the CPU only records a handful of draws (CPU-light by construction).
#include "common/bindings.slang"

struct SceneParams {
    float4x4 viewProj;
    float4 cameraPos;   // xyz, w = simulated time (seconds)
    float4 lightDir;    // xyz (towards light), w = intensity
    uint4 grid;         // x, y, z dimensions, w = shading detail (procedural pattern octaves)
    float4 fogColor;    // rgb, a = density
};
ConstantBuffer<SceneParams> scene : register(b0);

struct VSIn {
    [[vk::location(0)]] float3 pos : ATTRIB0;
    [[vk::location(1)]] float3 normal : ATTRIB1;
};

struct VSOut {
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    nointerpolation float3 albedo : TEXCOORD2;
    nointerpolation uint id : TEXCOORD3;
};

float3x3 rotationY(float a) {
    float s = sin(a), c = cos(a);
    return float3x3(c, 0, s, 0, 1, 0, -s, 0, c);
}
float3x3 rotationX(float a) {
    float s = sin(a), c = cos(a);
    return float3x3(1, 0, 0, 0, c, -s, 0, s, c);
}

float3 palette(float t) {
    // cosine palette (Inigo Quilez style), deterministic
    return 0.5 + 0.45 * cos(6.28318 * (float3(0.0, 0.33, 0.67) + t));
}

[shader("vertex")]
VSOut vsLattice(VSIn v, uint iid: SV_InstanceID) {
    uint gx = scene.grid.x, gz = scene.grid.z;
    uint ix = iid % gx;
    uint iy = (iid / gx) / gz;
    uint iz = (iid / gx) % gz;
    float t = scene.cameraPos.w;
    float h = gtHashFloat(iid);
    float3 cell = float3(float(ix) - 0.5 * float(gx - 1), float(iy) * 1.6, float(iz) - 0.5 * float(gz - 1)) * 2.2;
    float wave = sin(t * 1.3 + float(ix) * 0.35 + float(iz) * 0.27) * 0.8 + sin(t * 0.7 + h * 6.28) * 0.3;
    float3 offset = cell + float3(0.0, 1.2 + wave, 0.0);
    float scale = 0.45 + 0.35 * h;
    float3x3 rot = mul(rotationY(t * (0.4 + h) + h * 6.28), rotationX(t * 0.5 * (h - 0.5)));
    float3 world = mul(rot, v.pos * scale) + offset;
    VSOut o;
    o.pos = mul(scene.viewProj, float4(world, 1.0));
    o.worldPos = world;
    o.normal = mul(rot, v.normal);
    o.albedo = palette(h + float(iy) * 0.13);
    o.id = iid;
    return o;
}

// Floor: a single large quad (instance id ignored), shaded with a procedural tile pattern.
[shader("vertex")]
VSOut vsFloor(VSIn v) {
    float extent = float(max(scene.grid.x, scene.grid.z)) * 2.2 + 30.0;
    float3 world = float3(v.pos.x * extent, 0.0, v.pos.z * extent);
    VSOut o;
    o.pos = mul(scene.viewProj, float4(world, 1.0));
    o.worldPos = world;
    o.normal = float3(0, 1, 0);
    o.albedo = float3(0.55, 0.57, 0.6);
    o.id = 0xffffffffu;
    return o;
}

float noise2(float2 p) {
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    uint2 ii = uint2(int2(i) + 4096);
    float a = gtHashFloat(ii.x * 73856093u ^ ii.y * 19349663u);
    float b = gtHashFloat((ii.x + 1u) * 73856093u ^ ii.y * 19349663u);
    float c = gtHashFloat(ii.x * 73856093u ^ (ii.y + 1u) * 19349663u);
    float d = gtHashFloat((ii.x + 1u) * 73856093u ^ (ii.y + 1u) * 19349663u);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p, uint octaves) {
    float sum = 0.0, amp = 0.5;
    for (uint i = 0; i < octaves; ++i) {
        sum += amp * noise2(p);
        p = p * 2.03 + 17.1;
        amp *= 0.5;
    }
    return sum;
}

[shader("fragment")]
float4 psLattice(VSOut i) : SV_Target0 {
    float3 n = normalize(i.normal);
    float3 l = normalize(scene.lightDir.xyz);
    float3 v = normalize(scene.cameraPos.xyz - i.worldPos);
    float3 hv = normalize(l + v);
    float3 albedo = i.albedo;
    if (i.id == 0xffffffffu) {
        float2 tile = floor(i.worldPos.xz * 0.5);
        float checker = fmod(abs(tile.x + tile.y), 2.0);
        albedo *= lerp(0.75, 1.0, checker) * (0.8 + 0.4 * fbm(i.worldPos.xz * 0.35, scene.grid.w));
    } else {
        float stripes = 0.5 + 0.5 * sin(dot(i.worldPos, float3(3.1, 5.3, 2.7)) + scene.cameraPos.w);
        albedo *= 0.75 + 0.25 * stripes * fbm(i.worldPos.xy * 1.7 + i.worldPos.z, scene.grid.w);
    }
    float ndl = saturate(dot(n, l));
    float spec = pow(saturate(dot(n, hv)), 48.0) * 0.6;
    float3 ambient = lerp(float3(0.10, 0.09, 0.08), float3(0.18, 0.22, 0.30), n.y * 0.5 + 0.5);
    float3 color = albedo * (ambient + ndl * scene.lightDir.w) + spec * ndl;
    float dist = length(scene.cameraPos.xyz - i.worldPos);
    float fog = 1.0 - exp(-dist * scene.fogColor.a);
    color = lerp(color, scene.fogColor.rgb, fog);
    return float4(color, 1.0);
}
