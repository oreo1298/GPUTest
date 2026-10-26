// Draw-call overhead: each draw is one tiny triangle, so the CPU cost of issuing draws (and
// the GPU front end) dominates. The draw index comes from push constants, from a per-draw
// uniform buffer, or (indirect draws) from the vertex index.
#include "common/bindings.slang"

struct DrawParams {
    uint drawIndex;
    uint mode; // 0 = index from push constants, 1 = from the uniform, 2 = from the vertex index (indirect)
    uint gridW;
    uint pad;
};
GT_PUSH_CONSTANTS(DrawParams, pc);

struct PerDraw {
    uint drawIndex;
    uint pad0, pad1, pad2;
};
ConstantBuffer<PerDraw> perDraw : register(b0);

struct VSOut {
    float4 pos : SV_Position;
    float3 color : COLOR;
};

VSOut makeVertex(uint index, uint corner) {
    float2 cell = float2(float(index % pc.gridW), float((index / pc.gridW) % pc.gridW)) / float(pc.gridW);
    float2 offs = float2(float(corner == 1u), float(corner == 2u)) * (1.5 / float(pc.gridW));
    VSOut o;
    o.pos = float4((cell + offs) * 2.0 - 1.0, 0.0, 1.0);
    o.color = float3(gtHashFloat(index), gtHashFloat(index + 7u), 0.5);
    return o;
}

[shader("vertex")]
VSOut vsPush(uint vid: SV_VertexID) { return makeVertex(pc.drawIndex, vid % 3u); }

[shader("vertex")]
VSOut vsUniform(uint vid: SV_VertexID) { return makeVertex(perDraw.drawIndex, vid % 3u); }

[shader("vertex")]
VSOut vsIndirect(uint vid: SV_VertexID) { return makeVertex(vid / 3u, vid % 3u); }

[shader("fragment")]
float4 psDraw(VSOut v) : SV_Target { return float4(v.color, 1.0); }
