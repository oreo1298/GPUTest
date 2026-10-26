// Text overlay: one instanced quad per character; glyphs come from an 8x8
// bitmap font stored in an R8 texture (16x6 grid of ASCII 32..127).
#include "common/bindings.slang"

struct Glyph {
    float2 pos;   // top-left in pixels
    uint ch;      // ASCII code
    uint color;   // RGBA8
};

struct HudParams {
    float2 invViewport; // 1 / viewport size in pixels
    float scale;        // glyph size multiplier (8 px * scale)
    float pad;
};
GT_PUSH_CONSTANTS(HudParams, pc);

StructuredBuffer<Glyph> glyphs : register(t0);
Texture2D<float> fontTex : register(t1);

struct VSOut {
    float4 pos : SV_Position;
    float2 texel : TEXCOORD0;
    nointerpolation float4 color : COLOR0;
};

float4 unpackColor(uint c) {
    return float4(float(c & 0xffu), float((c >> 8) & 0xffu), float((c >> 16) & 0xffu), float(c >> 24)) / 255.0;
}

[shader("vertex")]
VSOut vsText(uint vid: SV_VertexID, uint iid: SV_InstanceID) {
    Glyph g = glyphs[iid];
    float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    float size = 8.0 * pc.scale;
    float2 px = g.pos + corner * size;
    VSOut o;
    o.pos = float4(px.x * pc.invViewport.x * 2.0 - 1.0, 1.0 - px.y * pc.invViewport.y * 2.0, 0.0, 1.0);
    uint idx = clamp(g.ch, 32u, 127u) - 32u;
    float2 cell = float2(float(idx % 16u), float(idx / 16u)) * 8.0;
    o.texel = cell + corner * 8.0;
    o.color = unpackColor(g.color);
    return o;
}

[shader("fragment")]
float4 psText(VSOut i) : SV_Target0 {
    int2 t = int2(min(i.texel, float2(127.0, 47.0)));
    float a = fontTex.Load(int3(t, 0));
    if (a < 0.5) discard;
    return i.color;
}

// Solid translucent rectangles behind the text (same instancing scheme).
struct Rect {
    float4 rect;  // x, y, w, h in pixels
    uint color;
    uint3 pad;
};
StructuredBuffer<Rect> rects : register(t2);

[shader("vertex")]
VSOut vsRect(uint vid: SV_VertexID, uint iid: SV_InstanceID) {
    Rect r = rects[iid];
    float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    float2 px = r.rect.xy + corner * r.rect.zw;
    VSOut o;
    o.pos = float4(px.x * pc.invViewport.x * 2.0 - 1.0, 1.0 - px.y * pc.invViewport.y * 2.0, 0.0, 1.0);
    o.texel = 0;
    o.color = unpackColor(r.color);
    return o;
}

[shader("fragment")]
float4 psRect(VSOut i) : SV_Target0 { return i.color; }
