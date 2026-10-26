// Compute-correctness kernels. The integer chain is exact everywhere, so the CPU
// checks it bit for bit; the float chain is checked against the GPU's own first
// pass (same GPU, same shader: any difference later means unstable hardware).
#include "common/bindings.slang"

struct ChainParams {
    uint count;
    uint iterations;
    uint seed;
    uint pad;
};
GT_PUSH_CONSTANTS(ChainParams, pc);

RWStructuredBuffer<uint> results : register(u0);

uint rotl(uint x, uint r) { return (x << r) | (x >> (32u - r)); }

[shader("compute")]
[numthreads(256, 1, 1)]
void csIntChain(uint3 tid: SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.count) return;
    uint x = i * 2654435761u + pc.seed;
    uint y = x ^ 0x6A09E667u;
    for (uint k = 0u; k < pc.iterations; ++k) {
        x = x * 1664525u + 1013904223u;
        y ^= rotl(x, 7u) + (y >> 3u);
        x ^= y * 0x9E3779B9u;
    }
    results[i] = x ^ y;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csFloatChain(uint3 tid: SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.count) return;
    float a = float(i % 1024u) * (1.0 / 1024.0) + 0.5;
    float b = float((i * 7u + pc.seed) % 997u) * (1.0 / 997.0);
    for (uint k = 0u; k < pc.iterations; ++k) {
        a = mad(a, 0.999, b * 0.001);
        b = sqrt(abs(sin(a * 3.1) * 0.5 + b * 0.5)) + 0.25;
        a = a * b - floor(a * b) + 0.5;
    }
    results[i] = asuint(a + b);
}
