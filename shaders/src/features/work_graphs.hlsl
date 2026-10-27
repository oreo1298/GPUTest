// D3D12 work graphs (shader model 6.8), HLSL because Slang has no node shaders; compiled with DXC
// (see cmake/Shaders.cmake). The graph: a broadcasting entry node whose grid size comes with the
// CPU record; every entry thread emits one record to a thread-launch leaf node that does a fixed
// amount of ALU work and writes its result. csBaseline does the same work as one plain dispatch.
// Bound through a native root signature (src/bench/tests/features/work_graphs.cpp):
//   b0 root constants, u0 results (uint per record), u1 counters (64 x uint).

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

struct EntryRecord {
    uint groups : SV_DispatchGrid;
};

struct LeafRecord {
    uint index;
    uint value;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(64, 1, 1)]
void entry(DispatchNodeInputRecord<EntryRecord> input, uint tid : SV_DispatchThreadID,
           [MaxRecords(64)] NodeOutput<LeafRecord> leaf) {
    const bool active = tid < gRecords;
    ThreadNodeOutputRecords<LeafRecord> rec = leaf.GetThreadNodeOutputRecords(active ? 1 : 0);
    if (active) {
        rec.Get().index = tid;
        rec.Get().value = tid * 3u + 1u;
    }
    rec.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void leaf(ThreadNodeInputRecord<LeafRecord> input) {
    finish(input.Get().index, input.Get().value);
}

[numthreads(64, 1, 1)]
void csBaseline(uint tid : SV_DispatchThreadID) {
    if (tid < gRecords) finish(tid, tid * 3u + 1u);
}
