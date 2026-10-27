// Shared by the work-graph library (work_graphs.hlsl) and its plain-dispatch baseline
// (work_graphs_baseline.hlsl). Bound through a native root signature
// (src/bench/tests/features/work_graphs.cpp): b0 root constants, u0 results (uint per record),
// u1 counters (64 x uint).
#pragma once

cbuffer Params : register(b0) {
    uint gRecords;    // total leaf records (= entry threads)
    uint gIterations; // hash rounds per record
    uint gPad0;
    uint gPad1;
};
RWStructuredBuffer<uint> gResults : register(u0);
RWByteAddressBuffer gCounters : register(u1);

uint hashChain(uint v) {
    for (uint i = 0; i < gIterations; ++i) {
        uint state = v * 747796405u + 2891336453u;
        uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        v = (word >> 22u) ^ word;
    }
    return v;
}

void finish(uint index, uint value) {
    gResults[index] = hashChain(value);
    gCounters.InterlockedAdd((index % 64u) * 4u, 1u);
}
