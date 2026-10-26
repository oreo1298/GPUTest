// ALU throughput kernels. Each thread runs `iterations` steps of 8 independent
// dependency chains (enough instruction-level parallelism to fill the pipelines).
// Multipliers/addends come from push constants so nothing can be constant-folded,
// and every chain feeds the stored result so nothing can be eliminated.
#include "common/bindings.slang"

struct ThroughputParams {
    uint iterations;
    float mulF;   // close to 1 so values stay finite
    float addF;
    uint mulI;
    uint addI;
    uint packedB; // int8x4 operand for the dot-product kernel
    uint pad0, pad1;
};
GT_PUSH_CONSTANTS(ThroughputParams, pc);
RWStructuredBuffer<uint> sink : register(u0);

[shader("compute")]
[numthreads(256, 1, 1)]
void csFp32(uint3 tid: SV_DispatchThreadID) {
    float4 x[8];
    [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = float4(tid.x & 255u, k, 1, 2) * 1e-3;
    float4 m = pc.mulF, c = pc.addF;
    for (uint i = 0; i < pc.iterations; ++i) {
        [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = mad(x[k], m, c);
    }
    float4 s = 0;
    [[unroll]] for (uint k = 0; k < 8; ++k) s += x[k];
    sink[tid.x & 1023u] = asuint(s.x + s.y + s.z + s.w);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csFp16(uint3 tid: SV_DispatchThreadID) {
    half4 x[8];
    [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = half4(half(tid.x & 7u), half(k), 1.0h, 2.0h) * 0.01h;
    half4 m = half4(half(pc.mulF)), c = half4(half(pc.addF));
    for (uint i = 0; i < pc.iterations; ++i) {
        [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = mad(x[k], m, c);
    }
    half4 s = half4(0.0h);
    [[unroll]] for (uint k = 0; k < 8; ++k) s += x[k];
    sink[tid.x & 1023u] = asuint(float(s.x + s.y + s.z + s.w));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csFp64(uint3 tid: SV_DispatchThreadID) {
    double x[8];
    [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = double(tid.x & 255u) * 1e-3 + double(k);
    double m = double(pc.mulF), c = double(pc.addF);
    for (uint i = 0; i < pc.iterations; ++i) {
        [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = x[k] * m + c;
    }
    double s = 0;
    [[unroll]] for (uint k = 0; k < 8; ++k) s += x[k];
    sink[tid.x & 1023u] = asuint(float(s));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csInt32(uint3 tid: SV_DispatchThreadID) {
    uint4 x[8];
    [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = uint4(tid.x, k, tid.x ^ k, 7u);
    uint4 m = pc.mulI, c = pc.addI;
    for (uint i = 0; i < pc.iterations; ++i) {
        [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = x[k] * m + c;
    }
    uint4 s = 0;
    [[unroll]] for (uint k = 0; k < 8; ++k) s ^= x[k];
    sink[tid.x & 1023u] = s.x ^ s.y ^ s.z ^ s.w;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csDot8(uint3 tid: SV_DispatchThreadID) {
    int x[8];
    [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = int(tid.x * 2654435761u + k);
    for (uint i = 0; i < pc.iterations; ++i) {
        // The accumulator is also the packed operand: a non-linear chain no compiler can shortcut.
        [[unroll]] for (uint k = 0; k < 8; ++k) x[k] = dot4add_i8packed(uint(x[k]), pc.packedB, x[k]);
    }
    int s = 0;
    [[unroll]] for (uint k = 0; k < 8; ++k) s ^= x[k];
    sink[tid.x & 1023u] = uint(s);
}
