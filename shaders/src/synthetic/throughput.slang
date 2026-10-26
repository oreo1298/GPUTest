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

// fxc (D3D11) refuses to unroll small loops nested in a dynamic loop, so the eight
// chains are spelled out with macros instead of arrays and [unroll] loops.
#define GT_CHAINS(OP) OP(0) OP(1) OP(2) OP(3) OP(4) OP(5) OP(6) OP(7)

[shader("compute")]
[numthreads(256, 1, 1)]
void csFp32(uint3 tid: SV_DispatchThreadID) {
#define INIT(k) float4 x##k = float4(tid.x & 255u, k, 1, 2) * 1e-3;
#define STEP(k) x##k = mad(x##k, m, c);
#define SUM(k) s += x##k;
    GT_CHAINS(INIT)
    float4 m = pc.mulF, c = pc.addF;
    [loop]
    for (uint i = 0; i < pc.iterations; ++i) {
        GT_CHAINS(STEP)
    }
    float4 s = 0;
    GT_CHAINS(SUM)
    sink[tid.x & 1023u] = asuint(s.x + s.y + s.z + s.w);
#undef INIT
#undef STEP
#undef SUM
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csFp16(uint3 tid: SV_DispatchThreadID) {
#define INIT(k) half4 x##k = half4(half(tid.x & 7u), half(k), 1.0h, 2.0h) * 0.01h;
#define STEP(k) x##k = mad(x##k, m, c);
#define SUM(k) s += x##k;
    GT_CHAINS(INIT)
    half4 m = half4(half(pc.mulF)), c = half4(half(pc.addF));
    [loop]
    for (uint i = 0; i < pc.iterations; ++i) {
        GT_CHAINS(STEP)
    }
    half4 s = half4(0.0h);
    GT_CHAINS(SUM)
    sink[tid.x & 1023u] = asuint(float(s.x + s.y + s.z + s.w));
#undef INIT
#undef STEP
#undef SUM
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csFp64(uint3 tid: SV_DispatchThreadID) {
#define INIT(k) double x##k = double(tid.x & 255u) * 1e-3 + double(k);
#define STEP(k) x##k = x##k * m + c;
#define SUM(k) s += x##k;
    GT_CHAINS(INIT)
    double m = double(pc.mulF), c = double(pc.addF);
    [loop]
    for (uint i = 0; i < pc.iterations; ++i) {
        GT_CHAINS(STEP)
    }
    double s = 0;
    GT_CHAINS(SUM)
    sink[tid.x & 1023u] = asuint(float(s));
#undef INIT
#undef STEP
#undef SUM
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csInt32(uint3 tid: SV_DispatchThreadID) {
#define INIT(k) uint4 x##k = uint4(tid.x, k, tid.x ^ k, 7u);
#define STEP(k) x##k = x##k * m + c;
#define SUM(k) s ^= x##k;
    GT_CHAINS(INIT)
    uint4 m = pc.mulI, c = pc.addI;
    [loop]
    for (uint i = 0; i < pc.iterations; ++i) {
        GT_CHAINS(STEP)
    }
    uint4 s = 0;
    GT_CHAINS(SUM)
    sink[tid.x & 1023u] = s.x ^ s.y ^ s.z ^ s.w;
#undef INIT
#undef STEP
#undef SUM
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csDot8(uint3 tid: SV_DispatchThreadID) {
#define INIT(k) int x##k = int(tid.x * 2654435761u + k);
#define STEP(k) x##k = dot4add_i8packed(uint(x##k), pc.packedB, x##k);
#define SUM(k) s ^= x##k;
    GT_CHAINS(INIT)
    [loop]
    for (uint i = 0; i < pc.iterations; ++i) {
        // The accumulator is also the packed operand: a non-linear chain no compiler can shortcut.
        GT_CHAINS(STEP)
    }
    int s = 0;
    GT_CHAINS(SUM)
    sink[tid.x & 1023u] = uint(s);
#undef INIT
#undef STEP
#undef SUM
}
