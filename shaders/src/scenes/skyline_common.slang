// Shared declarations of the Skyline scene (scenes/skyline.slang, scenes/skyline_post.slang).
#pragma once
#include "common/bindings.slang"

static const uint kMeshes = 4;  // box, cylinder, sphere, cone
static const uint kViews = 5;   // camera + 4 shadow cascades
static const uint kCascades = 4;

struct Instance {
    float4 posRot;    // xyz position, w = rotation about Y
    float4 scaleMesh; // xyz scale, w = mesh index
    float4 color;     // rgb albedo, w = material
};

struct MeshInfo {
    uint indexCount;
    uint firstIndex;
    int baseVertex;
    uint instanceBase; // first slot of this mesh's instances in a view's visible list
    float4 sphere;     // object-space bounding sphere
};

struct ViewData {
    float4 planes[6];
};

struct FrameData {
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 cascadeViewProj[4];
    float4 cascadeSplits; // view-space distances where cascades 1..4 end
    float4 cameraPos;     // xyz, w = time
    float4 cameraFwd;     // xyz, w = unused
    float4 sunDir;        // xyz towards the sun, w = intensity
    float4 sunColor;      // rgb, a = shadow-atlas texel size
    float4 screen;        // width, height, 1/width, 1/height
    ViewData views[5];
    uint4 counts;         // x = instance count, y = visible-list stride per view
};
ConstantBuffer<FrameData> frame : register(b0);

float3 skylineViewPos(int2 px, float depth) {
    float2 ndc = ((float2(px) + 0.5) * frame.screen.zw) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 p = mul(frame.invViewProj, float4(ndc, depth, 1.0));
    return p.xyz / p.w;
}
