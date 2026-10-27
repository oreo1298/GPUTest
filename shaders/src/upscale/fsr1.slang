// AMD FidelityFX Super Resolution 1.0: EASU (edge-adaptive spatial upsampling) and RCAS (robust
// contrast-adaptive sharpening), the non-packed 32-bit paths of ffx_fsr1.h ported to Slang, plus a
// bilinear baseline. One thread per output pixel; constants come from gt::bench::fsr1 (upscale.cpp).
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#include "common/bindings.slang"

struct FsrParams {
    uint4 con0, con1, con2, con3; // EASU constants (FsrEasuCon); RCAS uses con0.x (sharpness)
    uint2 outSize;
    uint2 pad;
};
GT_PUSH_CONSTANTS(FsrParams, pc);

Texture2D<float4> src : register(t0);
SamplerState linearClamp : register(s0);
[format("rgba8")] RWTexture2D<float4> dst : register(u0); // RGBA8 UNORM output (OpenGL needs the format)

// Approximations from ffx_a.h (bit tricks on the float representation).
float prxLoRcp(float a) { return asfloat(0x7ef07ebbu - asuint(a)); }
float prxMedRcp(float a) {
    float b = asfloat(0x7ef19fffu - asuint(a));
    return b * (-b * a + 2.0);
}
float prxLoRsq(float a) { return asfloat(0x5f347d74u - (asuint(a) >> 1u)); }
float3 min3(float3 a, float3 b, float3 c) { return min(a, min(b, c)); }
float3 max3(float3 a, float3 b, float3 c) { return max(a, max(b, c)); }
float min3(float a, float b, float c) { return min(a, min(b, c)); }
float max3(float a, float b, float c) { return max(a, max(b, c)); }

// --- EASU ------------------------------------------------------------------------------------------------
void easuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    // Lanczos-2 approximation: (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lob * x^2 - 1)^2
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void easuSet(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = prxLoRcp(max(abs(dc), abs(cb)));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    len += lenX * lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = prxLoRcp(max(abs(ec), abs(ca)));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    len += lenY * lenY * w;
}

float3 easu(uint2 ip) {
    float2 pp = float2(ip) * asfloat(pc.con0.xy) + asfloat(pc.con0.zw);
    float2 fp = floor(pp);
    pp -= fp;
    // 12-tap kernel from four gathers:
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    float2 p0 = fp * asfloat(pc.con1.xy) + asfloat(pc.con1.zw);
    float2 p1 = p0 + asfloat(pc.con2.xy);
    float2 p2 = p0 + asfloat(pc.con2.zw);
    float2 p3 = p0 + asfloat(pc.con3.xy);
    float4 bczzR = src.GatherRed(linearClamp, p0), bczzG = src.GatherGreen(linearClamp, p0),
           bczzB = src.GatherBlue(linearClamp, p0);
    float4 ijfeR = src.GatherRed(linearClamp, p1), ijfeG = src.GatherGreen(linearClamp, p1),
           ijfeB = src.GatherBlue(linearClamp, p1);
    float4 klhgR = src.GatherRed(linearClamp, p2), klhgG = src.GatherGreen(linearClamp, p2),
           klhgB = src.GatherBlue(linearClamp, p2);
    float4 zzonR = src.GatherRed(linearClamp, p3), zzonG = src.GatherGreen(linearClamp, p3),
           zzonB = src.GatherBlue(linearClamp, p3);
    // Luma times 2.
    float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x, cL = bczzL.y, iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w, oL = zzonL.z, nL = zzonL.w;
    // Direction and length, bilinearly weighted over the four nearest taps.
    float2 dir = 0.0;
    float len = 0.0;
    easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : prxLoRsq(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * prxLoRcp(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = prxLoRcp(lob);
    // Accumulate, then clamp to the 2x2 neighbourhood (de-ringing).
    float3 min4 = min(min3(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w),
                           float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      float3(klhgR.x, klhgG.x, klhgB.x));
    float3 max4 = max(max3(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w),
                           float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      float3(klhgR.x, klhgG.x, klhgB.x));
    float3 aC = 0.0;
    float aW = 0.0;
    easuTap(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x)); // b
    easuTap(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y)); // c
    easuTap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x)); // i
    easuTap(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y));  // j
    easuTap(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z));  // f
    easuTap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w)); // e
    easuTap(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x));  // k
    easuTap(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));  // l
    easuTap(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));  // h
    easuTap(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w));  // g
    easuTap(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));  // o
    easuTap(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));  // n
    return min(max4, max(min4, aC * (1.0 / aW)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csEasu(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= pc.outSize)) return;
    dst[id.xy] = float4(easu(id.xy), 1.0);
}

// --- RCAS ------------------------------------------------------------------------------------------------
static const float kRcasLimit = 0.25 - 1.0 / 16.0;

float3 rcas(int2 sp) {
    //    b
    //  d e f
    //    h
    int2 size = int2(pc.outSize) - 1;
    float3 b = src.Load(int3(clamp(sp + int2(0, -1), 0, size), 0)).rgb;
    float3 d = src.Load(int3(clamp(sp + int2(-1, 0), 0, size), 0)).rgb;
    float3 e = src.Load(int3(sp, 0)).rgb;
    float3 f = src.Load(int3(clamp(sp + int2(1, 0), 0, size), 0)).rgb;
    float3 h = src.Load(int3(clamp(sp + int2(0, 1), 0, size), 0)).rgb;
    float3 mn4 = min(min3(b, d, f), h);
    float3 mx4 = max(max3(b, d, f), h);
    const float2 peakC = float2(1.0, -4.0);
    // The guards keep black and white neighbourhoods finite (0 * inf would be NaN, and GLSL's min/max
    // do not promise to drop NaNs as HLSL's do).
    float3 hitMin = min(mn4, e) * (1.0 / max(4.0 * mx4, 1.0 / 65536.0));
    float3 hitMax = (peakC.x - max(mx4, e)) * (1.0 / min(4.0 * mn4 + peakC.y, -1.0 / 65536.0));
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-kRcasLimit, min(max3(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * asfloat(pc.con0.x);
    float rcpL = prxMedRcp(4.0 * lobe + 1.0);
    return (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csRcas(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= pc.outSize)) return;
    dst[id.xy] = float4(rcas(int2(id.xy)), 1.0);
}

// --- bilinear baseline ------------------------------------------------------------------------------------
[shader("compute")]
[numthreads(8, 8, 1)]
void csBilinear(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= pc.outSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.outSize);
    dst[id.xy] = float4(src.SampleLevel(linearClamp, uv, 0.0).rgb, 1.0);
}
