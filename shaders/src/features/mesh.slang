// Mesh-shader feature test: the same field of rotating spheres drawn three ways —
// a classic indexed, instanced vertex pipeline; mesh shaders (one workgroup per meshlet
// per instance); and task + mesh shaders where the task stage culls meshlets that face
// away from the camera or lie outside the view. All three must produce the same image.
#include "common/bindings.slang"

struct SceneData {
    float4x4 viewProj;
    float4 planes[5]; // world-space frustum planes (left, right, bottom, top, near), normals point inwards
    float4 cameraPos; // xyz; w = time
    float4 lightDir;  // xyz
    uint4 grid;       // x = instances per row, y = meshlets per mesh
    float4 layout;    // x = spacing
};
ConstantBuffer<SceneData> scene : register(b0);

struct Meshlet {
    uint vertexOffset;
    uint vertexCount;
    uint primOffset;
    uint primCount;
    float4 sphere; // object-space bounding sphere (centre, radius)
    float4 cone;   // object-space normal cone (axis, cutoff); cutoff >= 1 = never cull
};

StructuredBuffer<float4> positions : register(t0);
StructuredBuffer<Meshlet> meshlets : register(t1);
StructuredBuffer<uint> meshletVertices : register(t2);
StructuredBuffer<uint> meshletPrims : register(t3); // three 8-bit local indices per triangle
RWStructuredBuffer<uint> stats : register(u0);      // [0] = meshlets that survived task-shader culling

static const uint kMaxVerts = 64;
static const uint kMaxPrims = 124;

struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

struct Instance {
    float3x3 rot;
    float3 offset;
    float3 color;
};

Instance instanceData(uint inst) {
    uint side = scene.grid.x;
    float spacing = scene.layout.x;
    float halfWidth = spacing * float(side) * 0.5;
    uint h = gtHash(inst);
    float a = scene.cameraPos.w * 0.6 + float(h & 1023u) * (6.2831853 / 1024.0);
    float c = cos(a), s = sin(a);
    Instance o;
    o.rot = float3x3(c, 0, s, 0, 1, 0, -s, 0, c);
    o.offset = float3((float(inst % side) + 0.5) * spacing - halfWidth, 1.0, (float(inst / side) + 0.5) * spacing - halfWidth);
    o.color = 0.35 + 0.6 * float3(float(h >> 8 & 255u), float(h >> 16 & 255u), float(h >> 24)) / 255.0;
    return o;
}

VSOut shadeVertex(float3 p, Instance inst) {
    VSOut o;
    float3 world = mul(inst.rot, p) + inst.offset;
    o.pos = mul(scene.viewProj, float4(world, 1.0));
    o.normal = mul(inst.rot, p); // unit sphere: the position is the normal
    o.color = inst.color;
    return o;
}

// --- classic vertex pipeline ------------------------------------------------------------
[shader("vertex")]
VSOut vsSpheres(float3 pos: POSITION, uint inst: SV_InstanceID) { return shadeVertex(pos, instanceData(inst)); }

// --- mesh shaders -----------------------------------------------------------------------
void emitMeshlet(uint meshletIndex, uint inst, uint tid, OutputVertices<VSOut, kMaxVerts> verts,
                 OutputIndices<uint3, kMaxPrims> tris) {
    Meshlet m = meshlets[meshletIndex];
    SetMeshOutputCounts(m.vertexCount, m.primCount);
    Instance data = instanceData(inst);
    if (tid < m.vertexCount) verts[tid] = shadeVertex(positions[meshletVertices[m.vertexOffset + tid]].xyz, data);
    for (uint p = tid; p < m.primCount; p += kMaxVerts) {
        uint packed = meshletPrims[m.primOffset + p];
        tris[p] = uint3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u);
    }
}

[shader("mesh")]
[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void msDirect(uint tid: SV_GroupThreadID, uint3 gid: SV_GroupID, OutputVertices<VSOut, kMaxVerts> verts,
              OutputIndices<uint3, kMaxPrims> tris) {
    emitMeshlet(gid.x, gid.y, tid, verts, tris);
}

struct TaskPayload {
    uint instance;
    uint meshlets[64];
};
groupshared TaskPayload payload;
groupshared uint visibleCount;

bool meshletVisible(Meshlet m, Instance inst) {
    float3 centre = mul(inst.rot, m.sphere.xyz) + inst.offset;
    float radius = m.sphere.w;
    [unroll]
    for (uint i = 0; i < 5; ++i)
        if (dot(scene.planes[i].xyz, centre) + scene.planes[i].w < -radius) return false;
    if (m.cone.w < 1.0) {
        // Conservative normal-cone test (meshoptimizer): every triangle faces away from the camera.
        float3 axis = mul(inst.rot, m.cone.xyz);
        float3 v = centre - scene.cameraPos.xyz;
        if (dot(v, axis) >= m.cone.w * length(v) + radius) return false;
    }
    return true;
}

[shader("amplification")]
[numthreads(64, 1, 1)]
void tsCull(uint tid: SV_GroupThreadID, uint3 gid: SV_GroupID) {
    if (tid == 0) {
        visibleCount = 0;
        payload.instance = gid.x;
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid < scene.grid.y && meshletVisible(meshlets[tid], instanceData(gid.x))) {
        uint slot;
        InterlockedAdd(visibleCount, 1u, slot);
        payload.meshlets[slot] = tid;
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) InterlockedAdd(stats[0], visibleCount);
    DispatchMesh(visibleCount, 1, 1, payload);
}

[shader("mesh")]
[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void msFromTask(uint tid: SV_GroupThreadID, uint3 gid: SV_GroupID, in payload TaskPayload p,
                OutputVertices<VSOut, kMaxVerts> verts, OutputIndices<uint3, kMaxPrims> tris) {
    emitMeshlet(p.meshlets[gid.x], p.instance, tid, verts, tris);
}

// --- shading ----------------------------------------------------------------------------
[shader("fragment")]
float4 psSpheres(VSOut v) : SV_Target {
    float3 n = normalize(v.normal);
    float d = saturate(dot(n, scene.lightDir.xyz));
    return float4(v.color * (0.2 + 0.8 * d), 1.0);
}
