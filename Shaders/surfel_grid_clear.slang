// Zeroes the surfel spatial hash grid before `surfel_hash_grid_build.slang` rescatters this frame's
// live surfels into it. Kept as its own tiny pass since a compute dispatch cannot safely clear and
// scatter into the same buffer without a pass boundary between the two.
struct SurfelGridCell {
    uint count;
    uint indices[8];
};

RWStructuredBuffer<SurfelGridCell> gridBuffer;

struct ClearConstants {
    uint bucketCount;
};

[[push_constant]] ConstantBuffer<ClearConstants> clearConstants;

[shader("compute")]
[numthreads(64, 1, 1)]
void clearMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (dispatchThreadId.x >= clearConstants.bucketCount) return;
    gridBuffer[dispatchThreadId.x].count = 0u;
}
