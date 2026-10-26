// Memory-system kernels: streaming bandwidth (read / write / copy with 16-byte
// accesses and a grid stride), a single-thread pointer chase for latency, and
// atomics throughput (one hot address, spread addresses, 64-bit).
#include "common/bindings.slang"

struct MemoryParams {
    uint elements;   // float4 elements (bandwidth) / steps (chase) / iterations (atomics)
    uint threads;    // grid stride
    uint start;
    uint spread;     // atomics: number of distinct addresses (power of two)
};
GT_PUSH_CONSTANTS(MemoryParams, pc);

RWStructuredBuffer<float4> dst : register(u0);
RWStructuredBuffer<float4> src : register(u1);

[shader("compute")]
[numthreads(256, 1, 1)]
void csRead(uint3 tid: SV_DispatchThreadID) {
    float4 acc = 0;
    [loop]
    for (uint i = tid.x; i < pc.elements; i += pc.threads) acc += src[i];
    if (acc.x == 123456.0) dst[tid.x] = acc; // practically never true, but the loads must happen
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csWrite(uint3 tid: SV_DispatchThreadID) {
    float4 v = float4(tid.x, 1, 2, 3);
    [loop]
    for (uint i = tid.x; i < pc.elements; i += pc.threads) dst[i] = v;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csCopy(uint3 tid: SV_DispatchThreadID) {
    [loop]
    for (uint i = tid.x; i < pc.elements; i += pc.threads) dst[i] = src[i];
}

RWStructuredBuffer<uint> chain : register(u2);
RWStructuredBuffer<uint> result : register(u3);

[shader("compute")]
[numthreads(1, 1, 1)]
void csChase(uint3 tid: SV_DispatchThreadID) {
    uint idx = pc.start;
    [loop]
    for (uint i = 0; i < pc.elements; ++i) idx = chain[idx];
    result[0] = idx;
}

RWStructuredBuffer<uint> counters : register(u4);
RWStructuredBuffer<uint64_t> counters64 : register(u5);

[shader("compute")]
[numthreads(256, 1, 1)]
void csAtomicAdd(uint3 tid: SV_DispatchThreadID) {
    uint slot = (tid.x * 2654435761u) & (pc.spread - 1u);
    [loop]
    for (uint i = 0; i < pc.elements; ++i) InterlockedAdd(counters[(slot + i) & (pc.spread - 1u)], 1u);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csAtomicAdd64(uint3 tid: SV_DispatchThreadID) {
    uint slot = (tid.x * 2654435761u) & (pc.spread - 1u);
    [loop]
    for (uint i = 0; i < pc.elements; ++i) {
        uint64_t prev;
        InterlockedAdd(counters64[(slot + i) & (pc.spread - 1u)], uint64_t(1), prev);
    }
}
