// Copies this frame's final composited scene color into ReSTIR GI's dedicated history texture, run
// once per frame after the deferred shadow lighting pass has written `sceneColorIn`. Kept as its own
// small persistent texture (rather than re-reading `slot.deferred_targets.scene_color` a second time
// mid-frame) so the render graph has an explicit, single-writer dependency chain: next frame's
// restir_gi_initial_sample.slang reads this copy for multi-bounce feedback with no risk of racing this
// frame's own lighting-pass write to the live scene color target.
Texture2D<float4> sceneColorIn;
// WGSL's texture_storage_2d requires a format in the type itself; must match the RHI::Format the
// ReSTIR GI history texture is actually created with (RHI::Format::RGBA16Float). Write-only: this
// pass only ever overwrites the whole history texture, never reads it back.
[format("rgba16f")]
WTexture2D<float4> historyOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void historyCopyMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width, height;
    historyOut.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height) return;
    historyOut.Store(dispatchThreadId.xy, sceneColorIn.Load(int3(dispatchThreadId.xy, 0)));
}
