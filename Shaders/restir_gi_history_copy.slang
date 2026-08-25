// Copies this frame's final composited scene color into ReSTIR GI's dedicated history texture, run
// once per frame after the deferred shadow lighting pass has written `sceneColorIn`. Kept as its own
// small persistent texture (rather than re-reading `slot.deferred_targets.scene_color` a second time
// mid-frame) so the render graph has an explicit, single-writer dependency chain: next frame's
// restir_gi_initial_sample.slang reads this copy for multi-bounce feedback with no risk of racing this
// frame's own lighting-pass write to the live scene color target.
Texture2D<float4> sceneColorIn;
RWTexture2D<float4> historyOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void historyCopyMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width, height;
    historyOut.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height) return;
    historyOut[dispatchThreadId.xy] = sceneColorIn.Load(int3(dispatchThreadId.xy, 0));
}
