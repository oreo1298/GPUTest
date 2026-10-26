// Subgroup (wave) operations: a correctness kernel whose results the CPU re-derives from
// the reported lane layout, and three ways of reducing 256 values per workgroup — shared-
// memory atomics, a shared-memory tree, and subgroup sums — to show what wave intrinsics save.
#include "common/bindings.slang"

struct SubgroupParams {
    uint count;
    uint iterations;
    uint pad0;
    uint pad1;
};
GT_PUSH_CONSTANTS(SubgroupParams, pc);

RWStructuredBuffer<uint4> lanes : register(u0); // per thread: lane index, first thread of its subgroup, lane count
RWStructuredBuffer<uint4> ops : register(u1);   // per thread: sum, exclusive prefix sum, ballot count, max
RWStructuredBuffer<uint> sink : register(u2);

uint inputValue(uint i) { return gtHash(i) & 0xFFFFu; }

[shader("compute")]
[numthreads(64, 1, 1)]
void csWaveCheck(uint3 tid: SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.count) return;
    uint x = inputValue(i);
    lanes[i] = uint4(WaveGetLaneIndex(), WaveReadLaneFirst(i), WaveGetLaneCount(), 0u);
    ops[i] = uint4(WaveActiveSum(x), WavePrefixSum(x), WaveActiveCountBits((x & 1u) != 0u), WaveActiveMax(x));
}

groupshared uint shared[256];
groupshared uint counter;

[shader("compute")]
[numthreads(256, 1, 1)]
void csReduceAtomic(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex, uint3 gid: SV_GroupID) {
    uint acc = 0u;
    [loop]
    for (uint k = 0u; k < pc.iterations; ++k) {
        if (gi == 0u) counter = 0u;
        GroupMemoryBarrierWithGroupSync();
        InterlockedAdd(counter, gtHash(tid.x ^ (k * 0x9E3779B9u)));
        GroupMemoryBarrierWithGroupSync();
        acc += counter;
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u) sink[gid.x & 1023u] = acc;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csReduceShared(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex, uint3 gid: SV_GroupID) {
    uint acc = 0u;
    [loop]
    for (uint k = 0u; k < pc.iterations; ++k) {
        shared[gi] = gtHash(tid.x ^ (k * 0x9E3779B9u));
        GroupMemoryBarrierWithGroupSync();
        [unroll]
        for (uint s = 128u; s > 0u; s >>= 1u) {
            if (gi < s) shared[gi] += shared[gi + s];
            GroupMemoryBarrierWithGroupSync();
        }
        acc += shared[0];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u) sink[gid.x & 1023u] = acc;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csReduceWave(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex, uint3 gid: SV_GroupID) {
    uint acc = 0u;
    uint laneCount = WaveGetLaneCount();
    uint waves = (256u + laneCount - 1u) / laneCount;
    [loop]
    for (uint k = 0u; k < pc.iterations; ++k) {
        uint s = WaveActiveSum(gtHash(tid.x ^ (k * 0x9E3779B9u)));
        if (WaveIsFirstLane()) shared[gi / laneCount] = s;
        GroupMemoryBarrierWithGroupSync();
        // Second level: the first subgroup sums the partial results (in chunks when subgroups are narrow).
        uint total = 0u;
        if (gi < laneCount) {
            [loop]
            for (uint w = gi; w < waves; w += laneCount) total += shared[w];
            total = WaveActiveSum(total);
        }
        if (gi == 0u) counter = total;
        GroupMemoryBarrierWithGroupSync();
        acc += counter;
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u) sink[gid.x & 1023u] = acc;
}
