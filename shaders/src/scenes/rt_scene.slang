// Ray-traced scene tables shared by the ultimate-tier scenes (src/bench/tests/scenes/rt_scene.h):
// the TLAS, per-instance transform/colour/material, per-mesh index ranges and the scene-toolkit
// vertex and index buffers, plus helpers to recover a hit's shading normal.

struct Instance {
    float4 row0, row1, row2; // object-to-world 3x4 (the same matrix as the ray-tracing instance)
    float4 color;            // rgb albedo, w = material
    float4 info;             // x = mesh index
};

struct MeshInfo {
    uint firstIndex;
    uint indexCount;
    int baseVertex;
    uint pad;
};

RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<Instance> instances : register(t1);
StructuredBuffer<MeshInfo> meshes : register(t2);
StructuredBuffer<float> vertices : register(t3); // scene::Vertex: 8 floats
StructuredBuffer<uint> indices : register(t4);

float3 transformPoint(float4 r0, float4 r1, float4 r2, float3 p) {
    return float3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
}

/// Normal transform by the cofactor matrix (inverse transpose up to scale) of the 3x3 part.
float3 transformNormal(float4 r0, float4 r1, float4 r2, float3 n) {
    float3 c0 = float3(r0.x, r1.x, r2.x), c1 = float3(r0.y, r1.y, r2.y), c2 = float3(r0.z, r1.z, r2.z);
    return normalize(cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z);
}

float3 vertexNormal(uint index) { return float3(vertices[index * 8u + 3u], vertices[index * 8u + 4u], vertices[index * 8u + 5u]); }

/// World-space interpolated normal of a committed hit, pointing out of the mesh.
float3 outwardNormal(Instance inst, uint primitive, float2 bc) {
    MeshInfo m = meshes[uint(inst.info.x)];
    uint tri = m.firstIndex + primitive * 3u;
    float3 n0 = vertexNormal(uint(int(indices[tri]) + m.baseVertex));
    float3 n1 = vertexNormal(uint(int(indices[tri + 1u]) + m.baseVertex));
    float3 n2 = vertexNormal(uint(int(indices[tri + 2u]) + m.baseVertex));
    return transformNormal(inst.row0, inst.row1, inst.row2, n0 * (1.0 - bc.x - bc.y) + n1 * bc.x + n2 * bc.y);
}

/// The same, facing against the ray direction.
float3 hitNormal(Instance inst, uint primitive, float2 bc, float3 dir) {
    float3 n = outwardNormal(inst, primitive, bc);
    return dot(n, dir) > 0.0 ? -n : n;
}

/// True when anything lies along the ray before tMax.
bool occluded(float3 origin, float3 dir, float tMax) {
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    return q.CommittedStatus() != COMMITTED_NOTHING;
}

/// Cosine-distributed direction around n.
float3 cosineDir(float3 n, float u1, float u2) {
    float r = sqrt(u1), phi = 6.2831853 * u2;
    float3 t = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    float3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}
