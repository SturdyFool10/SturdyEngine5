// Depth of field, pass 2 of 4: the largest blur in each 8x8-texel tile (half resolution), far and near separately.
//
// The gather uses it twice: to skip tiles whose whole neighbourhood is sharp (most of a frame with a shallow focus band), and as
// the search radius for everything else, so a pixel only looks as far as the largest blur that could reach it.
import sturdy_common;
import dof_common;

ConstantBuffer<DofConstants> dof;
Texture2D<float4> cocHalf;
// rgba16f rather than rg16f: WGSL has no rg16f storage format. Only .xy is used.
[format("rgba16f")]
WTexture2D<float4> tileMaxOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void tileMaxMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= dof.gather.yz)) {
        return;
    }
    int2 maxTexel = int2(dof.extents.zw) - 1;
    int2 origin = int2(id.xy) * int(DOF_TILE_SIZE);
    float2 result = float2(0.0, 0.0);
    [loop]
    for (int y = 0; y < int(DOF_TILE_SIZE); ++y) {
        [loop]
        for (int x = 0; x < int(DOF_TILE_SIZE); ++x) {
            result = max(result, cocHalf.Load(int3(min(origin + int2(x, y), maxTexel), 0)).xy);
        }
    }
    tileMaxOut.Store(int2(id.xy), float4(result, 0.0, 0.0));
}
