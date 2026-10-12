// Every plain-data type must stay standard-layout and trivially copyable: cxx binds them as
// `ExternType` with `kind = Trivial` and bindgen mirrors their layout, so both properties are load-bearing.

#include <CxxApi/Types.hpp>

#include <cstdio>
#include <type_traits>

namespace {

    template <typename... Ts>
    constexpr bool all_trivial_pods = ((std::is_standard_layout_v<Ts> && std::is_trivially_copyable_v<Ts> &&
                                        std::is_trivially_destructible_v<Ts> && std::is_trivially_move_constructible_v<Ts>) && ...);

    using namespace SFT::CxxApi;
    using namespace SFT::RenderSettings;

    static_assert(all_trivial_pods<
                  // Common
                  Vec3Result, Ray, RayResult, Aabb,
                  // Runtime
                  WindowMode, VSync, VariableRefresh, LatencyMode, PresentationPreference, HdrColorSpace, FrameRateLimit,
                  GraphicsBackend, PresentationOptions, RuntimeOptions, FrameInfo, SceneLightingDesc, GpuDescription, GpuString,
                  RendererCapabilities,
                  // Camera
                  CameraProjection, Containment, CameraDesc,
                  // Scene
                  Entity, TransformDesc, TransformPropagationStats,
                  // Body rig
                  BodyRigDesc, BodyRigPose,
                  // Ui
                  UiSizing, UiDirection, UiScaleMode, UiElement, UiTextStyle,
                  // Ecs and log
                  FieldKind, ComponentFieldDesc, AccessMode, ComponentAccess, LayoutFingerprint, ResourceId, ResourceAccess, LogLevel,
                  // Window
                  Int2Result, HdrProperties, WindowSnapshot, CursorIcon, WindowEffect, WindowDesc, WindowRequestKind,
                  WindowRequestCompletion,
                  // Render (the RenderSettings records, bound as they are)
                  FrameSettings, SceneSettings, ShadowSettings, AmbientOcclusionSettings, AntiAliasingSettings, BloomSettings,
                  HermiteSplineSettings, PsychoVSettings, ToneMappingSettings, RestirGiSettings, ScreenSpaceGiSettings,
                  LightingSettings, ReflectionSettings, VolumetricFogSettings, ColorGradeSettings, DepthOfFieldSettings,
                  MotionBlurSettings, CameraEmulationSettings, AutoExposureSettings, TemporalUpscalerSettings, SceneIntegrator,
                  ShadowDebugView, AmbientOcclusionQuality, PostProcessAntiAliasing, ToneMappingOperator, AgxLook, RestirGiQuality,
                  RestirGiDenoiser, FisheyeMode, ExecutionMode>);

} // namespace

int main() {
    std::printf("CxxApiTypesTest: all plain-data types are trivial.\n");
    return 0;
}
