// "Gallery" (ultimate tier): a real-time path tracer. A sculpture hall lit by the sun and sky
// through skylights and by a warm light panel, with glass, polished metals, a lacquered floor and
// coloured walls for bleeding. Everything is traced with ray queries in compute: one primary hit
// per pixel, then N paths from it with next-event estimation towards the sun and towards the
// rectangular lights (skylight portals, the panel) for the diffuse lobe. The noisy result
// is demodulated by the primary albedo, filtered by an edge-aware a-trous wavelet (normals and
// distance as edge stops), re-modulated and tone-mapped.
#include "common/bindings.slang"
#include "scenes/rt_scene.slang"

/// A rectangle centre +- u +- v: a skylight portal (sky radiance) or an emitter.
struct RectLight {
    float4 center; // xyz, w = 0 portal, 1 emitter
    float4 u;      // half extent
    float4 v;      // half extent
    float4 radiance; // emitter radiance
};

struct FrameData {
    float4x4 invViewProj;
    float4 cameraPos; // xyz, w = time
    float4 sunDir;    // xyz towards the sun, w = irradiance
    float4 sunColor;  // rgb, a = sun angular radius
    float4 screen;    // width, height, 1/width, 1/height
    uint4 params;     // x = frame seed, y = paths per pixel, z = bounces per path, w = light count
    RectLight lights[8];
};
ConstantBuffer<FrameData> frame : register(b0);

static const float kPlaster = 0.0, kFloor = 1.0, kWood = 2.0, kGold = 3.0, kChrome = 4.0, kCopper = 5.0,
                   kGlass = 6.0, kEmissive = 7.0, kPaint = 8.0, kStone = 9.0;

float hash2(float2 p) {
    uint2 q = uint2(int2(floor(p)) + 32768);
    return gtHashFloat(q.x * 73856093u ^ q.y * 19349663u);
}

float valueNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash2(i), hash2(i + float2(1, 0)), u.x), lerp(hash2(i + float2(0, 1)), hash2(i + float2(1, 1)), u.x), u.y);
}

struct Surface {
    float3 albedo;   // diffuse colour, or F0 for metals, or tint for glass
    float3 emission;
    float rough;
    float metal;     // 1 = conductor
    float glass;     // 1 = dielectric, transmissive
    float coat;      // 1 = dielectric clear coat over the diffuse base
};

Surface surfaceAt(float m, float3 base, float3 p) {
    Surface s;
    s.albedo = base;
    s.emission = float3(0, 0, 0);
    s.rough = 1.0;
    s.metal = 0.0;
    s.glass = 0.0;
    s.coat = 0.0;
    if (m == kFloor) {
        // Polished stone: large tiles with soft veining under a clear coat.
        float2 t = floor(p.xz * 0.5);
        float tile = fmod(abs(t.x + t.y), 2.0);
        float vein = valueNoise(p.xz * 1.7 + float2(p.z * 0.4, 0.0)) * 0.6 + valueNoise(p.xz * 6.0) * 0.4;
        s.albedo = lerp(float3(0.62, 0.60, 0.56), float3(0.36, 0.35, 0.34), tile) * (0.8 + 0.3 * vein);
        s.rough = 0.12;
        s.coat = 1.0;
    } else if (m == kWood) {
        s.albedo = base * (0.75 + 0.35 * valueNoise(float2(p.x * 1.5 + p.z * 1.5, p.y * 40.0)));
        s.rough = 0.35;
        s.coat = 0.5;
    } else if (m == kGold || m == kChrome || m == kCopper) {
        s.metal = 1.0;
        s.rough = m == kChrome ? 0.02 : m == kGold ? 0.22 : 0.4;
    } else if (m == kGlass) {
        s.glass = 1.0;
        s.rough = 0.0;
    } else if (m == kEmissive) {
        s.emission = base;
        s.albedo = float3(0, 0, 0);
    } else if (m == kStone) {
        s.albedo = base * (0.85 + 0.2 * valueNoise(p.xz * 3.0 + p.y * 2.0));
    }
    return s;
}

float schlick(float f0, float cosine) { return f0 + (1.0 - f0) * pow(1.0 - saturate(cosine), 5.0); }
float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

/// Sky seen through the skylights (the sun itself is only sampled explicitly).
float3 skyRadiance(float3 dir) {
    float h = saturate(dir.y);
    float3 sky = lerp(float3(0.95, 0.97, 1.0), float3(0.35, 0.55, 0.95), pow(h, 0.45)) * 1.6;
    float glow = pow(saturate(dot(dir, frame.sunDir.xyz)), 24.0);
    return sky + frame.sunColor.rgb * glow * 2.0;
}

/// Glossy reflection: the mirror direction perturbed inside a cone that widens with roughness.
float3 glossyDir(float3 r, float rough, float u1, float u2, float u3) {
    float3 jitter = float3(u1, u2, u3) * 2.0 - 1.0;
    return normalize(r + jitter * rough);
}

/// BRDF times cosine for light arriving from l (next-event estimation). `minRough` widens glossy
/// lobes once a path has been diffuse: sun glints off mirrors seen through a diffuse bounce are
/// caustics this estimator cannot sample, and would only appear as fireflies.
float3 evalDirect(Surface s, float3 n, float3 v, float3 l, float minRough) {
    float ndl = saturate(dot(n, l));
    float3 h = normalize(v + l);
    float rough = max(s.rough, minRough);
    float shin = clamp(2.0 / max(rough * rough, 1e-4) - 2.0, 2.0, 8192.0);
    float lobe = (shin + 2.0) / (2.0 * GT_PI) * pow(saturate(dot(n, h)), shin);
    if (s.metal > 0.5) {
        float3 f = s.albedo + (1.0 - s.albedo) * pow(1.0 - saturate(dot(v, h)), 5.0);
        return f * lobe * ndl;
    }
    float fc = s.coat * schlick(0.04, dot(v, h));
    return (s.albedo / GT_PI * (1.0 - fc) + fc * lobe) * ndl;
}

struct Hit {
    float3 pos;
    float3 normal; // facing the incoming ray
    bool inside;   // the ray came from inside the mesh (glass exit)
    float t;
    Surface surface;
};

bool traceClosest(float3 origin, float3 dir, out Hit hit) {
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = 500.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    hit.pos = float3(0, 0, 0);
    hit.normal = float3(0, 1, 0);
    hit.inside = false;
    hit.t = 0.0;
    hit.surface = surfaceAt(kPlaster, float3(0, 0, 0), float3(0, 0, 0));
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    Instance inst = instances[q.CommittedInstanceID()];
    float3 n = outwardNormal(inst, q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());
    hit.inside = dot(n, dir) > 0.0;
    hit.normal = hit.inside ? -n : n;
    hit.t = q.CommittedRayT();
    hit.pos = origin + dir * hit.t;
    hit.surface = surfaceAt(inst.color.w, inst.color.rgb, hit.pos);
    return true;
}

/// Next-event estimation of the diffuse lobe towards one randomly chosen rectangular light.
/// Diffuse bounces therefore never count the sky or emitters they hit (no double counting).
float3 sampleRectLight(Hit h, float3 v, inout uint seed) {
    Surface s = h.surface;
    uint count = frame.params.w;
    seed = gtHash(seed);
    uint i = min(uint(gtHashFloat(seed) * float(count)), count - 1u);
    RectLight light = frame.lights[i];
    float a = gtHashFloat(seed ^ 0x27D4EB2Du) * 2.0 - 1.0, b = gtHashFloat(seed ^ 0x165667B1u) * 2.0 - 1.0;
    float3 d = light.center.xyz + light.u.xyz * a + light.v.xyz * b - h.pos;
    float dist2 = dot(d, d);
    float dist = sqrt(dist2);
    float3 l = d / dist;
    float ndl = dot(h.normal, l);
    float cosL = abs(dot(normalize(cross(light.u.xyz, light.v.xyz)), l));
    if (ndl <= 0.0 || cosL < 1e-3 || occluded(h.pos + h.normal * 0.002, l, dist - 0.01)) return float3(0, 0, 0);
    float3 le = light.center.w > 0.5 ? light.radiance.rgb : skyRadiance(l);
    float area = 4.0 * length(light.u.xyz) * length(light.v.xyz);
    float fc = s.coat * schlick(0.04, dot(v, h.normal));
    // 1/pdf in solid angle; bounded by the hemisphere so points beside a light do not spike.
    float invPdf = min(cosL * area / dist2, 2.0 * GT_PI) * float(count);
    return s.albedo / GT_PI * (1.0 - fc) * ndl * le * invPdf;
}

/// Samples the next direction. Returns false when the path is absorbed; `weight` is
/// BSDF * cosine / pdf and `diffuse` tells whether the diffuse lobe was chosen.
bool sampleBsdf(Hit h, float3 v, inout uint seed, out float3 wi, out float3 weight, out bool diffuse) {
    Surface s = h.surface;
    float3 n = h.normal;
    seed = gtHash(seed);
    float u0 = gtHashFloat(seed), u1 = gtHashFloat(seed ^ 0x9E3779B9u), u2 = gtHashFloat(seed ^ 0x85EBCA6Bu),
          u3 = gtHashFloat(seed ^ 0xC2B2AE35u);
    weight = float3(1, 1, 1);
    wi = n;
    diffuse = false;
    if (s.glass > 0.5) {
        float eta = h.inside ? 1.5 : 1.0 / 1.5;
        float cosi = dot(v, n);
        float3 refr = refract(-v, n, eta);
        bool tir = dot(refr, refr) < 1e-6;
        float f = tir ? 1.0 : schlick(0.04, cosi);
        if (u0 < f) {
            wi = reflect(-v, n);
        } else {
            wi = normalize(refr);
            weight = s.albedo;
        }
        return true;
    }
    if (s.metal > 0.5) {
        wi = glossyDir(reflect(-v, n), s.rough, u1, u2, u3);
        weight = s.albedo + (1.0 - s.albedo) * pow(1.0 - saturate(dot(v, n)), 5.0);
        return dot(wi, n) > 0.0;
    }
    // The coat reflects only a few percent; picking it at least a quarter of the time (with the
    // weights compensating) keeps its rare bright reflections from turning into fireflies.
    float fc = s.coat * schlick(0.04, dot(v, n));
    float pc = fc > 0.0 ? max(fc, 0.25) : 0.0;
    if (u0 < pc) {
        wi = glossyDir(reflect(-v, n), s.rough, u1, u2, u3);
        weight = float3(fc, fc, fc) / pc;
        return dot(wi, n) > 0.0;
    }
    wi = cosineDir(n, u1, u2);
    weight = s.albedo * (1.0 - fc) / (1.0 - pc);
    diffuse = true;
    return true;
}

/// One path continuing from the primary hit: next-event estimation at every non-glass vertex
/// (sun for every lobe, rectangular lights for the diffuse one), then BSDF sampling up to the
/// bounce limit (Russian roulette past two). Specular bounces pick up the sky and emitters.
float3 tracePath(Hit h, float3 dir, uint seed) {
    float3 direct = float3(0, 0, 0);   // light reaching the primary vertex
    float3 indirect = float3(0, 0, 0); // everything after the first bounce
    float3 throughput = float3(1, 1, 1);
    float3 v = -dir;
    float minRough = 0.0;
    uint bounces = frame.params.z;
    [loop]
    for (uint b = 0; b < bounces; ++b) {
        Surface s = h.surface;
        if (dot(s.emission, s.emission) > 0.0) break;
        float3 radiance = float3(0, 0, 0);
        if (s.glass < 0.5) {
            seed = gtHash(seed);
            float3 l = frame.sunDir.xyz;
            float3 lt = normalize(cross(l, float3(0, 1, 0))), lb = cross(l, lt);
            float rr = frame.sunColor.a * sqrt(gtHashFloat(seed)), ph = 6.2831853 * gtHashFloat(seed ^ 0x68E31DA4u);
            float3 ls = normalize(l + lt * (rr * cos(ph)) + lb * (rr * sin(ph)));
            if (dot(h.normal, ls) > 0.0 && !occluded(h.pos + h.normal * 0.002, ls, 200.0))
                radiance += throughput * evalDirect(s, h.normal, v, ls, minRough) * frame.sunColor.rgb * frame.sunDir.w;
            if (s.metal < 0.5) radiance += throughput * sampleRectLight(h, v, seed);
        }
        float3 wi, weight;
        bool diffuse;
        // A diffuse last bounce would only find light that was already sampled.
        bool more = sampleBsdf(h, v, seed, wi, weight, diffuse) && !(diffuse && b + 1u == bounces);
        if (more) {
            if (diffuse) minRough = 0.3;
            throughput *= weight;
            if (b >= 2u) {
                float survive = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.1, 1.0);
                seed = gtHash(seed);
                more = gtHashFloat(seed) <= survive;
                throughput /= survive;
            }
        }
        if (more) {
            float3 origin = h.pos + (dot(wi, h.normal) > 0.0 ? h.normal : -h.normal) * 0.002;
            Hit next;
            more = traceClosest(origin, wi, next);
            if (!diffuse) radiance += throughput * (more ? next.surface.emission : skyRadiance(wi));
            h = next;
            v = -wi;
        }
        if (b == 0u) direct += radiance;
        else indirect += radiance;
        if (!more) break;
    }
    // Clamp rare high-energy paths (fireflies) that no filter could average away; indirect light
    // is clamped harder, as is common in real-time path tracers.
    return min(direct, float3(24, 24, 24)) + min(indirect, float3(4, 4, 4));
}

// --- path tracing ----------------------------------------------------------------------------------
RWTexture2D<float4> radianceOut : register(u0); // demodulated radiance
RWTexture2D<float4> gbufferOut : register(u1);  // primary normal, w = distance (0 = sky)
RWTexture2D<float4> albedoOut : register(u2);   // primary demodulation albedo

[shader("compute")]
[numthreads(8, 8, 1)]
void csPathTrace(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= uint(frame.screen.x) || tid.y >= uint(frame.screen.y)) return;
    float2 ndc = ((float2(tid.xy) + 0.5) * frame.screen.zw) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 farP = mul(frame.invViewProj, float4(ndc, 1.0, 1.0));
    float3 origin = frame.cameraPos.xyz;
    float3 dir = normalize(farP.xyz / farP.w - origin);
    Hit h;
    if (!traceClosest(origin, dir, h)) {
        radianceOut[tid.xy] = float4(skyRadiance(dir), 1.0);
        gbufferOut[tid.xy] = float4(0, 0, 0, 0);
        albedoOut[tid.xy] = float4(1, 1, 1, 1);
        return;
    }
    Surface s = h.surface;
    bool emissive = dot(s.emission, s.emission) > 0.0;
    float3 demod = emissive || s.glass > 0.5 ? float3(1, 1, 1) : max(s.albedo, float3(0.03, 0.03, 0.03));
    uint pixel = gtHash(tid.x * 1973u + tid.y * 9277u + frame.params.x * 26699u);
    uint paths = frame.params.y;
    float3 sum = float3(0, 0, 0);
    [loop]
    for (uint k = 0; k < paths; ++k) sum += tracePath(h, dir, gtHash(pixel + k * 0x9E3779B9u));
    float3 radiance = s.emission + sum / float(max(paths, 1u));
    radianceOut[tid.xy] = float4(radiance / demod, 1.0);
    gbufferOut[tid.xy] = float4(h.normal, h.t);
    albedoOut[tid.xy] = float4(demod, 1.0);
}

// --- a-trous wavelet filter ---------------------------------------------------------------------
struct FilterParams {
    int stepSize;
    float phiLuminance;
    float phiDistance;
    float pad;
};
GT_PUSH_CONSTANTS(FilterParams, fp);
Texture2D<float4> filterIn : register(t5);
Texture2D<float4> gbufferIn : register(t6);
RWTexture2D<float4> filterOut : register(u3);

[shader("compute")]
[numthreads(8, 8, 1)]
void csFilter(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= uint(frame.screen.x) || tid.y >= uint(frame.screen.y)) return;
    int2 px = int2(tid.xy);
    float4 g = gbufferIn.Load(int3(px, 0));
    float4 c = filterIn.Load(int3(px, 0));
    if (g.w <= 0.0) {
        filterOut[tid.xy] = c;
        return;
    }
    float lum = luminance(c.rgb);
    int2 maxPx = int2(frame.screen.xy) - 1;
    float3 sum = float3(0, 0, 0);
    float wsum = 0.0;
    [loop]
    for (int y = -2; y <= 2; ++y) {
        [loop]
        for (int x = -2; x <= 2; ++x) {
            int2 q = px + int2(x, y) * fp.stepSize;
            if (any(q < int2(0, 0)) || any(q > maxPx)) continue;
            float4 gq = gbufferIn.Load(int3(q, 0));
            if (gq.w <= 0.0) continue;
            float4 cq = filterIn.Load(int3(q, 0));
            // B3-spline kernel taps: 3/8, 1/4, 1/16.
            float kx = x == 0 ? 0.375 : abs(x) == 1 ? 0.25 : 0.0625;
            float ky = y == 0 ? 0.375 : abs(y) == 1 ? 0.25 : 0.0625;
            float wn = pow(saturate(dot(g.xyz, gq.xyz)), 32.0);
            float wd = exp(-abs(g.w - gq.w) / (fp.phiDistance * g.w * float(fp.stepSize) + 0.01));
            float wl = exp(-abs(lum - luminance(cq.rgb)) / (fp.phiLuminance * (lum + 0.05)));
            float w = kx * ky * wn * wd * wl;
            sum += cq.rgb * w;
            wsum += w;
        }
    }
    filterOut[tid.xy] = float4(wsum > 1e-6 ? sum / wsum : c.rgb, 1.0);
}

// --- composite --------------------------------------------------------------------------------------
Texture2D<float4> finalIn : register(t8);
Texture2D<float4> albedoIn : register(t7);

struct FsOut {
    float4 pos : SV_Position;
};

[shader("vertex")]
FsOut vsFullscreen(uint vid: SV_VertexID) {
    float2 uv = float2(float((vid << 1u) & 2u), float(vid & 2u));
    FsOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 psComposite(FsOut i) : SV_Target {
    int3 px = int3(int2(i.pos.xy), 0);
    float3 x = finalIn.Load(px).rgb * albedoIn.Load(px).rgb * 1.9;
    x = saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
    return float4(pow(x, 1.0 / 2.2), 1.0);
}
