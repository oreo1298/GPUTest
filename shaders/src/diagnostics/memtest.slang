// VRAM tester kernels. Each thread walks its words with a grid stride (coalesced),
// ascending or descending, so moving-inversion passes keep a per-thread order.
// Errors are counted, the flipped bits OR-ed together, and the first records kept
// for failing-range reports: errors[0] = count, errors[1] = flipped-bit mask,
// then kMaxRecords records of {chunk, word, expected, actual}.
#include "common/bindings.slang"

struct MemtestParams {
    uint mode;       // 0 = write, 1 = verify, 2 = verify then write the complement
    uint pattern;    // 0 walking ones, 1 walking zeros, 2 random, 3 address-in-address, 4 checkerboard
    uint seed;       // varies per pass so stuck bits cannot hide behind one value
    uint invert;     // 0 or 0xFFFFFFFF, XOR-ed into the pattern
    uint wordCount;  // words in this chunk
    uint baseWord;   // global word index of the chunk's first word (low 32 bits)
    uint chunk;
    uint flags;      // bit 0: descending order
    uint threads;    // total threads dispatched (grid stride)
};
GT_PUSH_CONSTANTS(MemtestParams, pc);

RWStructuredBuffer<uint> data : register(u0);
RWStructuredBuffer<uint> errors : register(u1);
static const uint kMaxRecords = 1024u;

uint patternAt(uint word) {
    uint g = pc.baseWord + word;
    uint v;
    switch (pc.pattern) {
    case 0u: v = 1u << ((g + pc.seed) & 31u); break;
    case 1u: v = ~(1u << ((g + pc.seed) & 31u)); break;
    case 2u: v = gtHash(g ^ (pc.seed * 0x9E3779B9u)); break;
    case 3u: v = g ^ (pc.seed * 0x01000193u); break;
    default: v = (((g >> 0u) ^ (g >> 5u)) & 1u) != 0u ? 0xAAAAAAAAu : 0x55555555u; break;
    }
    return v ^ pc.invert;
}

void report(uint word, uint expected, uint actual) {
    uint slot;
    InterlockedAdd(errors[0], 1u, slot);
    InterlockedOr(errors[1], expected ^ actual);
    if (slot < kMaxRecords) {
        uint b = 2u + slot * 4u;
        errors[b] = pc.chunk;
        errors[b + 1u] = word;
        errors[b + 2u] = expected;
        errors[b + 3u] = actual;
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csMemtest(uint3 tid: SV_DispatchThreadID) {
    uint t = tid.x;
    if (t >= pc.wordCount) return;
    uint n = (pc.wordCount - 1u - t) / pc.threads + 1u; // words owned by this thread
    bool descending = (pc.flags & 1u) != 0u;
    [loop]
    for (uint k = 0u; k < n; ++k) {
        uint idx = descending ? (n - 1u - k) : k;
        uint w = t + idx * pc.threads;
        uint expected = patternAt(w);
        if (pc.mode == 0u) {
            data[w] = expected;
        } else {
            uint actual = data[w];
            if (actual != expected) report(w, expected, actual);
            if (pc.mode == 2u) data[w] = ~expected;
        }
    }
}
