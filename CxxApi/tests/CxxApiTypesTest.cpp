// Every plain-data type must stay standard-layout and trivially copyable: cxx binds them as
// `ExternType` with `kind = Trivial` and bindgen mirrors their layout, so both properties are load-bearing.

#include <CxxApi/Types.hpp>

#include <cstdio>
#include <type_traits>

namespace {

    template <typename... Ts>
    constexpr bool all_trivial_pods = ((std::is_standard_layout_v<Ts> && std::is_trivially_copyable_v<Ts> &&
                                        std::is_trivially_destructible_v<Ts>) && ...);

    using namespace SFT::CxxApi;

    static_assert(all_trivial_pods<
                  // Common
                  Vec3Result, Ray, RayResult, Aabb,
                  // Runtime
                  WindowMode, VSync, VariableRefresh, LatencyMode, PresentationPreference, HdrColorSpace, FrameRateLimit,
                  GraphicsBackend, PresentationOptions, RuntimeOptions, FrameInfo, SceneLightingDesc, GpuDescription, GpuString,
                  RendererCapabilities,
                  // Camera
                  CameraProjection, Containment, CameraDesc>);

} // namespace

int main() {
    std::printf("CxxApiTypesTest: all plain-data types are trivial.\n");
    return 0;
}
