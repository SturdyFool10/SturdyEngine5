// The engine's model of space: how a mesh vertex becomes a world position and a world position becomes a clip
// position. The built-in geometry shaders (G-buffer, history, instanced, colour) call only these functions, so a
// game whose space is not the usual Euclidean one (hyperbolic, spherical, portals, ...) replaces this module with
// its own -- `Renderer::set_space_model(SpaceModel{.shader_source = ...})` -- and every built-in geometry pass
// follows without a fork of the shader set.
//
// The defaults are the ordinary affine transform and matrix projection. Row-vector convention: `mul(v, M)`.
//
// Passes that read the depth buffer back (deferred lighting, ReSTIR GI, the spectral integrators) call
// `sturdySpaceClipToWorld` to turn a screen position + depth into a world position, and the lighting/shadow lookups
// call `sturdySpaceWorldToClip` with the *light's* matrix, so a replaced module also changes shadow projection.
// (GTAO works in linear view depth and needs no world position.)

/// Local (mesh) position -> world position.
float3 sturdySpaceModelToWorld(float3 localPosition, float4x4 model)
{
    return mul(float4(localPosition, 1.0), model).xyz;
}

/// World position -> clip position (before the perspective divide).
float4 sturdySpaceWorldToClip(float3 worldPosition, float4x4 viewProjection)
{
    return mul(float4(worldPosition, 1.0), viewProjection);
}

/// Local direction (normal, tangent) -> world direction, normalised. Uses the model's linear part.
float3 sturdySpaceTransformDirection(float3 localDirection, float4x4 model)
{
    return normalize(mul(localDirection, (float3x3)model));
}

/// Clip-space point (ndc.xy, depth, 1) -> homogeneous world position (xyz, w) via the view's inverse view-projection.
float4 sturdySpaceClipToWorldH(float4 clip, float4x4 inverseViewProjection)
{
    return mul(clip, inverseViewProjection);
}

/// Clip-space point (ndc.xy, depth, 1) -> world position. Keeps the sign of w so points behind the camera stay behind.
float3 sturdySpaceClipToWorld(float4 clip, float4x4 inverseViewProjection)
{
    float4 worldH = sturdySpaceClipToWorldH(clip, inverseViewProjection);
    float denominator = max(abs(worldH.w), 1.0e-6) * (worldH.w < 0.0 ? -1.0 : 1.0);
    return worldH.xyz / denominator;
}
