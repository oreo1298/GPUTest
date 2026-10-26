// Fullscreen triangle + texture blit (presents the fixed internal render
// resolution into the window with letterboxing done via the viewport).
#include "common/bindings.slang"

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsFullscreen(uint vid: SV_VertexID) {
    VSOut o;
    float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

struct BlitParams {
    float4 uvScaleBias; // uv' = uv * xy + zw
    float4 colorScale;  // multiplied into the result (exposure / fade)
};
GT_PUSH_CONSTANTS(BlitParams, pc);

Texture2D srcTex : register(t0);
SamplerState srcSampler : register(s0);

[shader("fragment")]
float4 psBlit(VSOut i) : SV_Target0 {
    float2 uv = i.uv * pc.uvScaleBias.xy + pc.uvScaleBias.zw;
    return srcTex.SampleLevel(srcSampler, uv, 0.0) * pc.colorScale;
}
