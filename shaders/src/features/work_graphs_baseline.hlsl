// Plain-dispatch baseline for feature.work_graphs: the leaf node's work for every record, as
// one ordinary compute dispatch (compiled with DXC, cs_6_8).
#include "features/work_graphs_common.hlsli"

[numthreads(64, 1, 1)]
void csBaseline(uint tid : SV_DispatchThreadID) {
    if (tid < gRecords) finish(tid, tid * 3u + 1u);
}
