// D3D12 work graphs (shader model 6.8), HLSL because Slang has no node shaders; compiled with DXC
// (see cmake/Shaders.cmake). The graph: a broadcasting entry node whose grid size comes with the
// CPU record; every entry thread emits one record to a thread-launch leaf node that does a fixed
// amount of ALU work and writes its result (work_graphs_baseline.hlsl does the same work as one
// plain dispatch).
#include "features/work_graphs_common.hlsli"

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
