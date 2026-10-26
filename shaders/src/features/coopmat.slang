// Cooperative matrices (matrix / tensor cores): each subgroup loads one A and one B tile and
// runs `iterations` multiply-accumulates into a C tile held in registers, then stores it.
// Compiled once per element type and shape (see the manifest): AB_TYPE, C_TYPE and
// TILE_M/N/K come from defines. Offsets and strides of Load/Store count elements.
#include "common/bindings.slang"
using namespace linalg;

struct CoopParams {
    uint iterations;
    uint pad0;
    uint pad1;
    uint pad2;
};
GT_PUSH_CONSTANTS(CoopParams, pc);

ByteAddressBuffer matA : register(t0);
ByteAddressBuffer matB : register(t1);
RWByteAddressBuffer matC : register(u0);

typealias MatA = CoopMat<AB_TYPE, MemoryScope.Subgroup, TILE_M, TILE_K, CoopMatMatrixUse.MatrixA>;
typealias MatB = CoopMat<AB_TYPE, MemoryScope.Subgroup, TILE_K, TILE_N, CoopMatMatrixUse.MatrixB>;
typealias MatC = CoopMat<C_TYPE, MemoryScope.Subgroup, TILE_M, TILE_N, CoopMatMatrixUse.MatrixAccumulator>;

[shader("compute")]
[numthreads(64, 1, 1)]
void csCoopMat(uint3 gid: SV_GroupID, uint3 tid: SV_GroupThreadID) {
    let a = MatA.Load<CoopMatMatrixLayout.RowMajor>(matA, 0, TILE_K);
    let b = MatB.Load<CoopMatMatrixLayout.RowMajor>(matB, 0, TILE_N);
    var c = MatC(C_TYPE(0));
    [loop]
    for (uint i = 0u; i < pc.iterations; ++i) c = coopMatMulAdd<C_TYPE, false>(a, b, c);
    // Each subgroup stores its own tile (up to 1024 slots) so no work can be dropped.
    uint subgroup = WaveReadLaneFirst(gid.x * 64u + tid.x) / WaveGetLaneCount();
    c.Store<CoopMatMatrixLayout.RowMajor>(matC, (subgroup & 1023u) * (TILE_M * TILE_N), TILE_N);
}
