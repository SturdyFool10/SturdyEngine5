// The settings cross language boundaries by value (CxxApi -> Rust via bindgen, the C ABI by memcpy-like copies), so every
// struct must stay plain data. This test pins that down at compile time and checks a few defaults at run time.

#include <RenderSettings/RenderSettings.hpp>

#include <cstdio>
#include <type_traits>

using namespace SFT::RenderSettings;

namespace {
    template <class T>
    constexpr bool plain = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>;

    static_assert(plain<SceneSettings> && plain<ShadowSettings> && plain<AmbientOcclusionSettings> && plain<AntiAliasingSettings>);
    static_assert(plain<BloomSettings> && plain<ToneMappingSettings> && plain<HermiteSplineSettings> && plain<PsychoVSettings>);
    static_assert(plain<RestirGiSettings> && plain<ScreenSpaceGiSettings> && plain<LightingSettings> && plain<VolumetricFogSettings> &&
                  plain<ReflectionSettings>);
    static_assert(plain<MotionBlurSettings> && plain<CameraEmulationSettings> && plain<AutoExposureSettings>);
    static_assert(plain<TemporalUpscalerSettings> && plain<FrameSettings>);
    static_assert(sizeof(SceneIntegrator) == 4 && sizeof(ShadowDebugView) == 4 && sizeof(ToneMappingOperator) == 4 && sizeof(FisheyeMode) == 4,
                  "enums are 32-bit so bindgen and the C ABI agree on their size");

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
} // namespace

int main() {
    const FrameSettings defaults{};
    check(defaults.shadows.enabled && defaults.shadows.cascade_resolutions[0] == 2048u && defaults.shadows.cascade_resolutions[3] == 1024u,
          "shadow defaults");
    check(defaults.tone_mapping.operation == ToneMappingOperator::Agx && defaults.tone_mapping.psycho_v.adapted_gray_bt709[2] == 0.18f,
          "tone-mapping defaults");
    check(defaults.lighting.clustered && defaults.lighting.max_lights >= 256, "clustered lighting is on by default");
    check(!defaults.volumetric_fog.enabled && defaults.volumetric_fog.slice_count == 64, "fog is opt-in");
    check(defaults.resolution_scale == 1.0f && defaults.execution_mode == ExecutionMode::FireAndForget, "frame defaults");
    FrameSettings copy = defaults;
    copy.camera_emulation.tint[1] = 0.5f;
    check(defaults.camera_emulation.tint[1] == 1.0f && copy.camera_emulation.tint[1] == 0.5f, "settings copy by value");
    return failures == 0 ? 0 : 1;
}
