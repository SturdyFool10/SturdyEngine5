#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <expected>
#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        /// Grid granularity of the shared spot/point ("punctual") shadow atlas.
        ///
        /// Directional cascades are not allocated from this grid; they own a dedicated atlas so a
        /// crowded scene full of shadowed local lights can never shrink the sun cascades.
        constexpr u32 kPunctualAtlasGridSize = 8;
        constexpr f32 kMinimumLightRange = 0.05f;

        constexpr f32 kPointShadowFaceFovRadians = 1.8151424f;

        /// Multiplicative step of the cascade-extent quantization ladder (2^(1/4)).
        ///
        /// Cascade edge length is snapped up onto powers of this base. A coarse multiplicative
        /// ladder is what makes the fit stable under rotation: the tight light-space extent of a
        /// frustum slice varies continuously as the camera turns, but the quantized extent is
        /// piecewise constant, so the world-space texel size (and therefore every texel-relative
        /// bias and filter radius) holds still across the vast majority of camera orientations.
        /// Worst-case waste is one rung, ~19% per axis.
        constexpr f32 kCascadeExtentLadderBase = 1.18920711500272f;

        /// Extra filter/bias guard reserved inside each cascade tile, in texels.
        ///
        /// The receiver region is inset by this many texels on every side so that no PCF/PCSS tap
        /// and no normal-offset displacement can reach outside the cascade's own tile. Sized from
        /// the configured PCF radius, the PCSS penumbra cap and the maximum normal offset.
        constexpr f32 kCascadeGuardSafetyTexels = 2.0f;

        /// Upper bound on the PCSS penumbra radius relative to the base PCF radius.
        constexpr f32 kCascadePcssRadiusScale = 6.0f;

        /// Largest normal-offset displacement the resolve applies, in shadow texels.
        ///
        /// Mirrors the clamp in `deferred_shadow_lighting.slang`; both must move together or the
        /// guard region stops being a guarantee.
        constexpr f32 kMaxNormalOffsetTexels = 3.0f;

        /// Creates an error result describing the supplied shadow failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError shadow_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        /// Performs the safe normalize operation for `Renderer` using the supplied arguments.
        ///
        /// @param value Value consumed by the operation.
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 safe_normalize(glm::vec3 value, glm::vec3 fallback) noexcept {
            const f32 length_squared = glm::dot(value, value);
            return std::isfinite(length_squared) && length_squared > 1.0e-12f
                       ? value * glm::inversesqrt(length_squared)
                       : fallback;
        }

        /// Returns finite when available, otherwise uses the supplied fallback.
        ///
        /// @param value Value consumed by the operation.
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 finite_or(f32 value, f32 fallback) noexcept {
            return std::isfinite(value) ? value : fallback;
        }

        /// Chooses the reference up axis used to build a light basis.
        ///
        /// @param direction Normalized direction the light travels.
        ///
        /// @return Returns an axis that is never close to parallel with `direction`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 light_up(glm::vec3 direction) noexcept {
            return std::abs(direction.y) < 0.98f ? glm::vec3{0.0f, 1.0f, 0.0f}
                                                 : glm::vec3{0.0f, 0.0f, 1.0f};
        }

        /// Orthonormal basis of a directional light, matching `glm::lookAtRH` exactly.
        ///
        /// `forward` is the direction the light travels (increasing light-space depth). `right` and
        /// `up` span the plane the shadow map rasterizes. Because the basis depends only on the sun
        /// direction, every cascade shares one light-space lattice, which is what lets texel
        /// snapping be expressed as a plain quantization of the cascade centre.
        struct LightBasis {
            glm::vec3 right{1.0f, 0.0f, 0.0f};
            glm::vec3 up{0.0f, 1.0f, 0.0f};
            glm::vec3 forward{0.0f, 0.0f, -1.0f};
            glm::vec3 reference_up{0.0f, 1.0f, 0.0f};
        };

        /// Builds the light basis for a directional light.
        ///
        /// @param direction Normalized direction the light travels.
        ///
        /// @return Returns a right-handed orthonormal basis identical to the one `glm::lookAtRH`
        ///         derives from the same direction and reference up axis.
        /// @note This function does not throw exceptions.
        [[nodiscard]] LightBasis make_light_basis(glm::vec3 direction) noexcept {
            LightBasis basis;
            basis.forward = direction;
            basis.reference_up = light_up(direction);
            basis.right = safe_normalize(glm::cross(direction, basis.reference_up),
                                         glm::vec3{1.0f, 0.0f, 0.0f});
            basis.up = glm::cross(basis.right, direction);
            return basis;
        }

        /// Transforms a world-space point into light space.
        ///
        /// @param basis Light basis produced by `make_light_basis`.
        /// @param point World-space point.
        ///
        /// @return Returns `(right, up, depth)` coordinates; depth grows along the light direction.
        /// @note The basis is orthonormal and unshifted, so the transform is a pure rotation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 to_light_space(const LightBasis &basis, glm::vec3 point) noexcept {
            return glm::vec3{glm::dot(basis.right, point), glm::dot(basis.up, point),
                             glm::dot(basis.forward, point)};
        }

        /// Transforms a light-space point back into world space.
        ///
        /// @param basis Light basis produced by `make_light_basis`.
        /// @param point Light-space point.
        ///
        /// @return Returns the corresponding world-space point.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 from_light_space(const LightBasis &basis, glm::vec3 point) noexcept {
            return basis.right * point.x + basis.up * point.y + basis.forward * point.z;
        }

        /// Computes the world-space corners of one camera-frustum depth slice.
        ///
        /// @param projection Camera projection matrix defining the frustum.
        /// @param camera_world Inverse camera view matrix (camera-to-world).
        /// @param near_depth Positive view-space distance to the slice near plane.
        /// @param far_depth Positive view-space distance to the slice far plane.
        ///
        /// @return Returns the four near corners followed by the four far corners, in world space.
        /// @note Handles both perspective and orthographic projections. Corners are exact, not
        ///       conservative: padding for filtering and bias is applied later, by the caller.
        /// @note This function does not throw exceptions.
        [[nodiscard]] array<glm::vec3, 8> calculate_frustum_slice_corners(
            const glm::mat4 &projection, const glm::mat4 &camera_world, f32 near_depth,
            f32 far_depth) noexcept {
            const bool perspective = std::abs(projection[2][3]) > 0.5f &&
                                     std::abs(projection[3][3]) <= 1.0e-6f;
            const glm::mat4 inverse_projection = glm::inverse(projection);
            const array<glm::vec2, 4> ndc_xy{
                glm::vec2{-1.0f, -1.0f}, glm::vec2{1.0f, -1.0f},
                glm::vec2{1.0f, 1.0f}, glm::vec2{-1.0f, 1.0f},
            };
            array<glm::vec3, 8> corners{};
            for (usize corner = 0; corner < ndc_xy.size(); ++corner) {
                glm::vec4 point_h = inverse_projection * glm::vec4{ndc_xy[corner], 1.0f, 1.0f};
                if (std::abs(point_h.w) > 1.0e-6f) {
                    point_h /= point_h.w;
                }
                const glm::vec3 point = glm::vec3{point_h};
                glm::vec3 near_view;
                glm::vec3 far_view;
                if (perspective) {
                    const f32 inverse_abs_z = 1.0f / std::max(std::abs(point.z), 1.0e-6f);
                    near_view = point * (near_depth * inverse_abs_z);
                    far_view = point * (far_depth * inverse_abs_z);
                } else {
                    near_view = glm::vec3{point.x, point.y, -near_depth};
                    far_view = glm::vec3{point.x, point.y, -far_depth};
                }
                corners[corner] = glm::vec3{camera_world * glm::vec4{near_view, 1.0f}};
                corners[corner + 4] = glm::vec3{camera_world * glm::vec4{far_view, 1.0f}};
            }
            return corners;
        }

        /// Computes a rotation-invariant bounding sphere for a perspective-frustum slice.
        ///
        /// @param projection Camera projection matrix defining the frustum.
        /// @param near_depth Positive view-space distance to the slice near plane.
        /// @param far_depth Positive view-space distance to the slice far plane.
        ///
        /// @return Returns the sphere center in camera view space followed by its radius in world units.
        /// @note Symmetric perspective projections use the analytic minimum sphere; other projections use a conservative corner fit.
        /// @note The radius depends only on the slice depths and the field of view, never on camera
        ///       orientation. It is retained purely as the rotation-invariant upper bound that the
        ///       tight fit is clamped against, so the tight fit can never be the larger of the two.
        [[nodiscard]] std::pair<glm::vec3, f32> stable_frustum_slice_sphere(
            const glm::mat4 &projection, f32 near_depth, f32 far_depth) noexcept {
            const bool perspective = std::abs(projection[2][3]) > 0.5f &&
                                     std::abs(projection[3][3]) <= 1.0e-6f;
            const bool symmetric = std::abs(projection[2][0]) <= 1.0e-6f &&
                                   std::abs(projection[2][1]) <= 1.0e-6f;
            if (perspective && symmetric) {
                const f32 tan_half_x = 1.0f / std::max(std::abs(projection[0][0]), 1.0e-6f);
                const f32 tan_half_y = 1.0f / std::max(std::abs(projection[1][1]), 1.0e-6f);
                const f32 diagonal_slope_squared = tan_half_x * tan_half_x + tan_half_y * tan_half_y;
                const f32 depth_span = std::max(far_depth - near_depth, 1.0e-6f);
                const f32 depth_sum = far_depth + near_depth;

                f32 center_depth = far_depth;
                f32 radius = far_depth * std::sqrt(diagonal_slope_squared);
                if (diagonal_slope_squared < depth_span / std::max(depth_sum, 1.0e-6f)) {
                    center_depth = 0.5f * depth_sum * (1.0f + diagonal_slope_squared);
                    const f32 axial = center_depth - near_depth;
                    radius = std::sqrt(axial * axial +
                                       near_depth * near_depth * diagonal_slope_squared);
                }
                return {glm::vec3{0.0f, 0.0f, -center_depth}, std::max(radius, 0.25f)};
            }

            const glm::mat4 inverse_projection = glm::inverse(projection);
            const array<glm::vec2, 4> ndc_xy{
                glm::vec2{-1.0f, -1.0f}, glm::vec2{1.0f, -1.0f},
                glm::vec2{1.0f, 1.0f}, glm::vec2{-1.0f, 1.0f},
            };
            array<glm::vec3, 8> corners{};
            glm::vec3 center{0.0f};
            for (usize corner = 0; corner < ndc_xy.size(); ++corner) {
                glm::vec4 point_h = inverse_projection * glm::vec4{ndc_xy[corner], 1.0f, 1.0f};
                if (std::abs(point_h.w) > 1.0e-6f) {
                    point_h /= point_h.w;
                }
                const glm::vec3 point = glm::vec3{point_h};
                if (perspective) {
                    const f32 inverse_abs_z = 1.0f / std::max(std::abs(point.z), 1.0e-6f);
                    corners[corner] = point * (near_depth * inverse_abs_z);
                    corners[corner + 4] = point * (far_depth * inverse_abs_z);
                } else {
                    corners[corner] = glm::vec3{point.x, point.y, -near_depth};
                    corners[corner + 4] = glm::vec3{point.x, point.y, -far_depth};
                }
                center += corners[corner] + corners[corner + 4];
            }
            center *= 1.0f / 8.0f;
            f32 radius = 0.25f;
            for (const glm::vec3 corner : corners) {
                radius = std::max(radius, glm::distance(center, corner));
            }
            return {center, radius};
        }

        /// Light-space bounds of the region a cascade must be able to *receive* shadows in.
        ///
        /// Derived from the camera frustum slice alone. Caster geometry never widens these bounds;
        /// it only influences the depth range (see `calculate_caster_depth_range`). All members are
        /// light-space (see `to_light_space`) and exact, in world units.
        struct ReceiverBounds {
            glm::vec2 minimum{0.0f};
            glm::vec2 maximum{0.0f};
            f32 minimum_depth = 0.0f;
            f32 maximum_depth = 0.0f;
            glm::vec2 center{0.0f};
            f32 tight_extent = 0.0f;
        };

        /// Computes the exact light-space receiver bounds of a frustum slice.
        ///
        /// @param corners World-space frustum-slice corners.
        /// @param basis Light basis the cascade rasterizes in.
        ///
        /// @return Returns the tight light-space XY bounds and depth range of the slice.
        /// @note Bounds are exact rather than conservative; `tight_extent` is the larger of the two
        ///       XY spans so callers can build an isotropic (square-texel) cascade from it.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ReceiverBounds calculate_receiver_bounds(span<const glm::vec3> corners,
                                                              const LightBasis &basis) noexcept {
            ReceiverBounds bounds;
            bounds.minimum = glm::vec2{std::numeric_limits<f32>::max()};
            bounds.maximum = glm::vec2{std::numeric_limits<f32>::lowest()};
            bounds.minimum_depth = std::numeric_limits<f32>::max();
            bounds.maximum_depth = std::numeric_limits<f32>::lowest();
            for (const glm::vec3 corner : corners) {
                const glm::vec3 light_space = to_light_space(basis, corner);
                bounds.minimum = glm::min(bounds.minimum, glm::vec2{light_space});
                bounds.maximum = glm::max(bounds.maximum, glm::vec2{light_space});
                bounds.minimum_depth = std::min(bounds.minimum_depth, light_space.z);
                bounds.maximum_depth = std::max(bounds.maximum_depth, light_space.z);
            }
            bounds.center = (bounds.minimum + bounds.maximum) * 0.5f;
            bounds.tight_extent = std::max(bounds.maximum.x - bounds.minimum.x,
                                           bounds.maximum.y - bounds.minimum.y);
            return bounds;
        }

        /// Snaps a cascade edge length up onto the quantization ladder.
        ///
        /// @param extent Tight light-space edge length in world units.
        ///
        /// @return Returns the smallest ladder rung greater than or equal to `extent`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 quantize_cascade_extent(f32 extent) noexcept {
            if (!(extent > 0.0f) || !std::isfinite(extent)) {
                return 0.0f;
            }
            const f32 rung = std::ceil(std::log(extent) / std::log(kCascadeExtentLadderBase));
            return std::pow(kCascadeExtentLadderBase, rung);
        }

        /// Picks a cascade edge length that is stable under both camera translation and rotation.
        ///
        /// @param tight_extent Exact light-space edge length required by the receiver bounds.
        /// @param conservative_extent Rotation-invariant upper bound (the slice sphere diameter).
        /// @param persistent Per-cascade edge length accepted on the previous frame; updated here.
        ///
        /// @return Returns the edge length to build the cascade projection from, in world units.
        /// @note Never returns less than `tight_extent`, so receiver coverage is preserved.
        /// @note The previous value is reused whenever the tight extent still sits within one full
        ///       ladder rung below it. That deadband is what prevents a camera parked on a
        ///       quantization threshold from flipping between two rungs (and therefore two texel
        ///       sizes) on alternating frames.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 stabilize_cascade_extent(f32 tight_extent, f32 conservative_extent,
                                                   f32 &persistent) noexcept {
            const f32 clamped_conservative = std::max(conservative_extent, tight_extent);
            const f32 target = std::min(quantize_cascade_extent(tight_extent), clamped_conservative);
            const f32 previous = persistent;
            constexpr f32 deadband = kCascadeExtentLadderBase * kCascadeExtentLadderBase;
            const bool reuse_previous = std::isfinite(previous) && previous >= tight_extent &&
                                        previous <= clamped_conservative &&
                                        previous <= tight_extent * deadband;
            persistent = reuse_previous ? previous : target;
            return persistent;
        }

        /// Light-space depth range that a cascade's shadow map must cover.
        struct CascadeDepthRange {
            f32 minimum = 0.0f;
            f32 maximum = 0.0f;
        };

        /// Determines the depth range and caster set relevant to one cascade.
        ///
        /// @param draws All submitted render items, in submission order.
        /// @param basis Light basis the cascade rasterizes in.
        /// @param minimum_xy Light-space lower XY corner of the cascade's rasterized region.
        /// @param maximum_xy Light-space upper XY corner of the cascade's rasterized region.
        /// @param receiver_minimum_depth Light-space depth of the nearest receiver in the cascade.
        /// @param receiver_maximum_depth Light-space depth of the farthest receiver in the cascade.
        /// @param casters Cleared and filled with indices into `draws` for the relevant casters.
        ///
        /// @return Returns the light-space depth interval to build the orthographic projection from.
        /// @note For a directional light the shadow of a caster is a pure translation along the
        ///       light direction, so its light-space XY footprint never moves. A caster can
        ///       therefore only affect this cascade when its bounding sphere overlaps the cascade's
        ///       XY rectangle *and* it is not entirely behind the last receiver. Casters failing
        ///       either test are excluded, which is what stops one enormous or distant caster from
        ///       destroying every cascade's depth precision.
        /// @note Bounds are conservative (bounding spheres, inflated rectangle): the result may
        ///       include casters that turn out not to matter, but never excludes one that does.
        /// @note The returned range always contains the full receiver range, so a receiver can
        ///       never fall outside the depth clip planes and silently read as unshadowed.
        /// @note This function does not throw exceptions.
        template <typename RenderItem>
        [[nodiscard]] CascadeDepthRange calculate_caster_depth_range(
            const vector<RenderItem> &draws, const LightBasis &basis, glm::vec2 minimum_xy,
            glm::vec2 maximum_xy, f32 receiver_minimum_depth, f32 receiver_maximum_depth,
            vector<u32> &casters) {
            casters.clear();
            CascadeDepthRange range{receiver_minimum_depth, receiver_maximum_depth};
            for (usize index = 0; index < draws.size(); ++index) {
                const RenderItem &item = draws[index];
                if (!item.casts_shadows) {
                    continue;
                }
                const f32 world_radius = item.world_bounds_radius;
                const glm::vec3 world_center = item.world_bounds_center;
                if (!std::isfinite(world_radius) || !std::isfinite(world_center.x) ||
                    !std::isfinite(world_center.y) || !std::isfinite(world_center.z)) {
                    continue;
                }
                const f32 radius = std::max(world_radius, 0.0f);
                const glm::vec3 light_space = to_light_space(basis, world_center);
                if (light_space.x + radius < minimum_xy.x || light_space.x - radius > maximum_xy.x ||
                    light_space.y + radius < minimum_xy.y || light_space.y - radius > maximum_xy.y) {
                    continue;
                }
                if (light_space.z - radius > receiver_maximum_depth) {
                    continue;
                }
                casters.push_back(static_cast<u32>(index));
                range.minimum = std::min(range.minimum, light_space.z - radius);
            }
            return range;
        }
        /// Performs the luminance operation for `Renderer` using the supplied arguments.
        ///
        /// @param radiance `radiance` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 luminance(glm::vec3 radiance) noexcept {
            return glm::dot(glm::max(radiance, glm::vec3{0.0f}), glm::vec3{0.2126f, 0.7152f, 0.0722f});
        }

        /// Performs the punctual importance operation for `Renderer` using the supplied arguments.
        ///
        /// @param position `position` value used by the operation.
        /// @param radiance `radiance` value used by the operation.
        /// @param range Range of values to process.
        /// @param camera_position `camera_position` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 punctual_importance(glm::vec3 position, glm::vec3 radiance, f32 range, glm::vec3 camera_position) noexcept {
            const glm::vec3 offset = position - camera_position;
            const f32 distance_squared = glm::dot(offset, offset);
            const f32 safe_range = std::max(range, kMinimumLightRange);
            return luminance(radiance) * safe_range * safe_range /
                   (1.0f + distance_squared / (safe_range * safe_range));
        }


        /// Selects a positional-light shadow tile size from projected screen coverage.
        ///
        /// @param position World-space light position.
        /// @param range Effective light range in world units.
        /// @param camera_position World-space camera position.
        /// @param projection Camera projection matrix.
        /// @param render_extent Current render resolution.
        /// @param atlas_size Shadow atlas edge size in texels.
        ///
        /// @return Returns the requested atlas-grid width in cells.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 positional_shadow_tile_cells(
            glm::vec3 position, f32 range, glm::vec3 camera_position, const glm::mat4 &projection,
            Core::Extent2D render_extent, u32 atlas_size) noexcept {
            if (atlas_size < kPunctualAtlasGridSize * 2u) {
                return 1u;
            }
            const f32 distance = glm::distance(position, camera_position);
            if (distance <= std::max(range, kMinimumLightRange)) {
                return 2u;
            }
            const f32 projection_scale = std::abs(projection[1][1]);
            const f32 radius_pixels =
                std::max(range, kMinimumLightRange) * projection_scale /
                std::max(distance, kMinimumLightRange) *
                (0.5f * static_cast<f32>(std::max(render_extent.y, 1u)));
            const f32 diameter_pixels = radius_pixels * 2.0f;
            const f32 cell_pixels = static_cast<f32>(atlas_size / kPunctualAtlasGridSize);
            return diameter_pixels > cell_pixels * 0.75f ? 2u : 1u;
        }

        struct AtlasTile {
            u32 x = 0;
            u32 y = 0;
            u32 cells = 0;
            /// Converts the `AtlasTile` to `bool`.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function does not throw exceptions.
            explicit operator bool() const noexcept { return cells != 0; }
        };

        class AtlasAllocator {
          public:
            /// Allocates storage or a resource.
            ///
            /// @param cells `cells` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            [[nodiscard]] AtlasTile allocate(u32 cells) noexcept {
                if (cells == 0 || cells > kPunctualAtlasGridSize) {
                    return {};
                }
                for (u32 y = 0; y + cells <= kPunctualAtlasGridSize; ++y) {
                    for (u32 x = 0; x + cells <= kPunctualAtlasGridSize; ++x) {
                        bool free = true;
                        for (u32 row = 0; row < cells && free; ++row) {
                            for (u32 column = 0; column < cells; ++column) {
                                free &= !used_[(y + row) * kPunctualAtlasGridSize + x + column];
                            }
                        }
                        if (!free) {
                            continue;
                        }
                        for (u32 row = 0; row < cells; ++row) {
                            for (u32 column = 0; column < cells; ++column) {
                                used_[(y + row) * kPunctualAtlasGridSize + x + column] = true;
                            }
                        }
                        return AtlasTile{.x = x, .y = y, .cells = cells};
                    }
                }
                return {};
            }

          private:
            array<bool, kPunctualAtlasGridSize * kPunctualAtlasGridSize> used_{};
        };

        /// Performs the tile viewport operation for `Renderer` using the supplied arguments.
        ///
        /// @param tile `tile` value used by the operation.
        /// @param atlas_size Requested or available size for the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::Rect2D tile_viewport(AtlasTile tile, u32 atlas_size) noexcept {
            const u32 cell_size = atlas_size / kPunctualAtlasGridSize;
            return RHI::Rect2D{
                .x = static_cast<i32>(tile.x * cell_size),
                .y = static_cast<i32>(tile.y * cell_size),
                .width = tile.cells * cell_size,
                .height = tile.cells * cell_size,
            };
        }

        /// Performs the tile scale bias operation for `Renderer` using the supplied arguments.
        ///
        /// @param tile `tile` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec4 tile_scale_bias(AtlasTile tile) noexcept {
            const f32 inv_grid = 1.0f / static_cast<f32>(kPunctualAtlasGridSize);
            return glm::vec4{
                static_cast<f32>(tile.cells) * inv_grid,
                static_cast<f32>(tile.cells) * inv_grid,
                static_cast<f32>(tile.x) * inv_grid,
                static_cast<f32>(tile.y) * inv_grid,
            };
        }

        /// Binds group layout index for set for subsequent operations.
        ///
        /// @param sets `sets` value used by the operation.
        /// @param set `set` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize bind_group_layout_index_for_set(span<const u32> sets, u32 set) noexcept {
            for (usize i = 0; i < sets.size(); ++i) {
                if (sets[i] == set) {
                    return i;
                }
            }
            return sets.size();
        }
    } // namespace

    /// Builds the deliberate directional cascade allocation for the supplied settings.
    ///
    /// @param requested_resolutions Per-cascade edge resolutions in texels, near cascade first.
    /// @param cascade_count Number of cascades that will be rendered.
    /// @param device_max_texture_dimension Largest 2D texture edge the device supports.
    ///
    /// @return Returns a packed layout whose atlas fits inside the device limit.
    /// @note Resolutions are sanitized to powers of two and forced non-increasing, then packed into
    ///       full-height columns beside the near cascade. For power-of-two, non-increasing inputs
    ///       that packing has no gaps, so atlas utilization is exactly 100%: with the default
    ///       {2048, 1024, 1024, 1024} the atlas is 4096x2048 with cascade 0 occupying the left half.
    /// @note If the packed atlas would exceed the device limit every resolution is halved and the
    ///       packing retried, so the function always returns something allocatable (or an empty
    ///       layout when no cascades were requested).
    /// @note This function does not throw exceptions.
    Renderer::DirectionalAtlasLayout Renderer::build_directional_atlas_layout(
        span<const u32> requested_resolutions, u32 cascade_count,
        u32 device_max_texture_dimension) noexcept {
        DirectionalAtlasLayout layout;
        const u32 count = std::min({cascade_count, max_directional_shadow_cascades,
                                    static_cast<u32>(requested_resolutions.size())});
        if (count == 0) {
            return layout;
        }
        const u32 device_max = std::max(device_max_texture_dimension, 1024u);

        const auto floor_power_of_two = [](u32 value) noexcept {
            u32 result = 256u;
            while (result * 2u <= value) {
                result *= 2u;
            }
            return result;
        };

        array<u32, max_directional_shadow_cascades> resolutions{};
        for (u32 cascade = 0; cascade < count; ++cascade) {
            u32 resolution = floor_power_of_two(std::clamp(requested_resolutions[cascade], 256u, 8192u));
            if (cascade > 0) {
                resolution = std::min(resolution, resolutions[cascade - 1]);
            }
            resolutions[cascade] = resolution;
        }

        for (u32 attempt = 0; attempt < 8; ++attempt) {
            const u32 height = resolutions[0];
            array<DirectionalCascadeTile, max_directional_shadow_cascades> tiles{};
            tiles[0] = DirectionalCascadeTile{.x = 0, .y = 0, .resolution = resolutions[0]};
            u32 cursor_x = resolutions[0];
            u32 cursor_y = 0;
            u32 column_width = 0;
            for (u32 cascade = 1; cascade < count; ++cascade) {
                if (cursor_y + resolutions[cascade] > height) {
                    cursor_x += column_width;
                    column_width = 0;
                    cursor_y = 0;
                }
                tiles[cascade] = DirectionalCascadeTile{
                    .x = cursor_x, .y = cursor_y, .resolution = resolutions[cascade]};
                cursor_y += resolutions[cascade];
                column_width = std::max(column_width, resolutions[cascade]);
            }
            const u32 width = cursor_x + column_width;
            if (width <= device_max && height <= device_max) {
                layout.tiles = tiles;
                layout.cascade_count = count;
                layout.width = width;
                layout.height = height;
                return layout;
            }
            if (resolutions[count - 1] <= 256u) {
                break;
            }
            for (u32 cascade = 0; cascade < count; ++cascade) {
                resolutions[cascade] = std::max(resolutions[cascade] / 2u, 256u);
            }
        }
        return layout;
    }

    /// Finds or creates the frame shadow targets required by the operation.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param requested_atlas_size Edge size requested for the shared spot/point atlas, or zero to release it.
    /// @param directional_layout Directional cascade allocation, or an empty layout to release that atlas.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note The two atlases are separate textures so punctual shadow demand can never reduce
    ///       directional cascade resolution, and either can be released independently.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_frame_shadow_targets(
        FrameInFlight &slot, u32 requested_atlas_size,
        const DirectionalAtlasLayout &directional_layout) {
        ZoneScopedN("Renderer::ensure_frame_shadow_targets");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(shadow_error("Cannot allocate shadow targets without an RHI device."));
        }

        const u32 device_max = std::max(device->limits().max_texture_dimension_2d, 512u);
        u32 atlas_size = requested_atlas_size == 0
                             ? 0u
                             : std::clamp(requested_atlas_size, 512u, std::min(16384u, device_max));
        atlas_size -= atlas_size % kPunctualAtlasGridSize;
        if (Core::RendererResult directional = ensure_directional_shadow_atlas(slot, directional_layout);
            !directional.has_value()) {
            return directional;
        }
        if (!slot.shadow_targets.lighting_buffer) {
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = sizeof(ShadowLightingGpuData),
                .usage = RHI::BufferUsage::Uniform,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "shadow lighting constants",
            });
            if (!buffer) {
                return unexpected(graphics_error_from_rhi(buffer.error(), "create shadow lighting constants buffer"));
            }
            slot.shadow_targets.lighting_buffer = *buffer;
        }

        if (atlas_size == 0) {
            if (slot.shadow_targets.atlas_view) {
                device->destroy_texture_view(slot.shadow_targets.atlas_view);
            }
            if (slot.shadow_targets.atlas) {
                device->destroy_texture(slot.shadow_targets.atlas);
            }
            slot.shadow_targets.atlas = {};
            slot.shadow_targets.atlas_view = {};
            slot.shadow_targets.atlas_size = 0;
            return {};
        }
        if (slot.shadow_targets.atlas && slot.shadow_targets.atlas_size == atlas_size) {
            return {};
        }

        if (slot.shadow_targets.atlas_view) {
            device->destroy_texture_view(slot.shadow_targets.atlas_view);
        }
        if (slot.shadow_targets.atlas) {
            device->destroy_texture(slot.shadow_targets.atlas);
        }
        slot.shadow_targets.atlas = {};
        slot.shadow_targets.atlas_view = {};

        auto atlas = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = slot.shadow_targets.format,
            .extent = RHI::Extent3D{.width = atlas_size, .height = atlas_size, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::DepthStencilAttachment | RHI::TextureUsage::Sampled,
            .label = "raster shadow atlas",
        });
        if (!atlas) {
            return unexpected(graphics_error_from_rhi(atlas.error(), "create raster shadow atlas"));
        }
        auto atlas_view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *atlas,
            .view_type = RHI::TextureViewType::View2D,
            .label = "raster shadow atlas view",
        });
        if (!atlas_view) {
            device->destroy_texture(*atlas);
            return unexpected(graphics_error_from_rhi(atlas_view.error(), "create raster shadow atlas view"));
        }
        slot.shadow_targets.atlas = *atlas;
        slot.shadow_targets.atlas_view = *atlas_view;
        slot.shadow_targets.atlas_size = atlas_size;
        return {};
    }

    /// Finds or creates the dedicated directional cascade atlas for a frame slot.
    ///
    /// @param slot Frame-in-flight slot owning the shadow targets.
    /// @param layout Directional cascade allocation, or an empty layout to release the atlas.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Reallocates only when the packed atlas dimensions actually change, so toggling other
    ///       shadow settings never churns the texture.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_directional_shadow_atlas(FrameInFlight &slot,
                                                                   const DirectionalAtlasLayout &layout) {
        ZoneScopedN("Renderer::ensure_directional_shadow_atlas");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(shadow_error("Cannot allocate the directional shadow atlas without an RHI device."));
        }
        FrameShadowTargets &targets = slot.shadow_targets;
        const bool matches = static_cast<bool>(targets.directional_atlas) &&
                             targets.directional_layout.width == layout.width &&
                             targets.directional_layout.height == layout.height;
        targets.directional_layout = layout;
        if (!layout) {
            if (targets.directional_atlas_view) {
                device->destroy_texture_view(targets.directional_atlas_view);
            }
            if (targets.directional_atlas) {
                device->destroy_texture(targets.directional_atlas);
            }
            targets.directional_atlas = {};
            targets.directional_atlas_view = {};
            targets.directional_layout = {};
            return {};
        }
        if (matches) {
            return {};
        }
        if (targets.directional_atlas_view) {
            device->destroy_texture_view(targets.directional_atlas_view);
        }
        if (targets.directional_atlas) {
            device->destroy_texture(targets.directional_atlas);
        }
        targets.directional_atlas = {};
        targets.directional_atlas_view = {};

        auto atlas = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = targets.format,
            .extent = RHI::Extent3D{.width = layout.width, .height = layout.height, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::DepthStencilAttachment | RHI::TextureUsage::Sampled,
            .label = "directional shadow atlas",
        });
        if (!atlas) {
            return unexpected(graphics_error_from_rhi(atlas.error(), "create directional shadow atlas"));
        }
        auto atlas_view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *atlas,
            .view_type = RHI::TextureViewType::View2D,
            .label = "directional shadow atlas view",
        });
        if (!atlas_view) {
            device->destroy_texture(*atlas);
            return unexpected(graphics_error_from_rhi(atlas_view.error(), "create directional shadow atlas view"));
        }
        targets.directional_atlas = *atlas;
        targets.directional_atlas_view = *atlas_view;
        return {};
    }

    /// Destroys the frame shadow targets identified by the supplied parameters.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_frame_shadow_targets(FrameInFlight &slot) noexcept {
        ZoneScopedN("Renderer::destroy_frame_shadow_targets");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            if (slot.shadow_targets.atlas_view) {
                device->destroy_texture_view(slot.shadow_targets.atlas_view);
            }
            if (slot.shadow_targets.atlas) {
                device->destroy_texture(slot.shadow_targets.atlas);
            }
            if (slot.shadow_targets.directional_atlas_view) {
                device->destroy_texture_view(slot.shadow_targets.directional_atlas_view);
            }
            if (slot.shadow_targets.directional_atlas) {
                device->destroy_texture(slot.shadow_targets.directional_atlas);
            }
            if (slot.shadow_targets.lighting_buffer) {
                device->destroy_buffer(slot.shadow_targets.lighting_buffer);
            }
        }
        slot.shadow_targets = {};
    }

    /// Prepares shadow frame for a later operation.
    ///
    /// @param submission Frame submission holding the camera, lighting and draw list.
    /// @param targets Per-frame shadow atlases and the lighting constant buffer.
    /// @param directional_state Per-window cascade stabilization history; read and updated here.
    /// @param prepared Cleared and filled with the shadow views and GPU constants for this frame.
    /// @param render_extent Current render resolution, used for punctual tile sizing.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Directional cascades occupy shadow-view indices `[0, cascade_count)` and sample the
    ///       dedicated directional atlas; spot/point views follow and sample the punctual atlas.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::prepare_shadow_frame(const FrameSubmission &submission,
                                                        FrameShadowTargets &targets,
                                                        DirectionalShadowState &directional_state,
                                                        PreparedShadowFrame &prepared,
                                                        Core::Extent2D render_extent) {
        ZoneScopedN("Renderer::prepare_shadow_frame");
        static_assert(sizeof(ShadowViewGpuData) == 112);
        static_assert(sizeof(DirectionalLightGpuData) == 80);
        static_assert(sizeof(SpotLightGpuData) == 64);
        static_assert(sizeof(PointLightGpuData) == 48);
        static_assert(sizeof(ShadowLightingGpuData) == 5344);
        static_assert(offsetof(ShadowLightingGpuData, sun) == 336);
        static_assert(offsetof(ShadowLightingGpuData, spot_lights) == 416);
        static_assert(offsetof(ShadowLightingGpuData, point_lights) == 928);
        static_assert(offsetof(ShadowLightingGpuData, shadow_views) == 1312);
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !targets.lighting_buffer) {
            return unexpected(shadow_error("Cannot prepare shadow lighting without its per-frame constant buffer."));
        }

        prepared = {};
        ShadowLightingGpuData &gpu = prepared.gpu;
        const glm::mat4 view_projection = submission.camera.projection * submission.camera.view;
        gpu.inverse_view_projection = glm::inverse(view_projection);
        gpu.view_projection = view_projection;
        gpu.view = submission.camera.view;
        gpu.camera_position_near = glm::vec4{submission.camera.world_position,
                                             std::max(submission.camera.near_plane, 0.0001f)};
        gpu.ambient_radiance_exposure = glm::vec4{
            glm::max(submission.lighting.ambient_radiance, glm::vec3{0.0f}),
            std::max(submission.lighting.exposure, 0.0f),
        };
        gpu.background_color = submission.render_graph.background_color *
                               glm::vec4{submission.render_graph.background_intensity,
                                         submission.render_graph.background_intensity,
                                         submission.render_graph.background_intensity,
                                         1.0f};
        gpu.spectral_params.x = static_cast<f32>(submission.render_graph.spectral_path_tracing.mode);
        // .y/.z are repurposed here for surfel GI enable/intensity rather than adding a new packed
        // vec4 field — spectral_params previously only used .x, leaving these lanes reserved.
        gpu.spectral_params.y = submission.render_graph.surfel_gi.enabled ? 1.0f : 0.0f;
        gpu.spectral_params.z = std::max(finite_or(submission.render_graph.surfel_gi.intensity, 1.0f), 0.0f);
        gpu.viewport_params = glm::vec4{
            1.0f / static_cast<f32>(std::max(render_extent.x, 1u)),
            1.0f / static_cast<f32>(std::max(render_extent.y, 1u)),
            std::abs(submission.camera.projection[1][1]),
            submission.render_graph.ambient_occlusion ? 1.0f : 0.0f,
        };

        const DirectionalLight &sun = submission.lighting.sun;
        const glm::vec3 sun_direction = safe_normalize(sun.direction, glm::vec3{0.0f, -1.0f, 0.0f});
        gpu.sun.direction_angular_radius = glm::vec4{
            sun_direction,
            glm::radians(std::clamp(finite_or(sun.angular_radius_degrees, 0.27f), 0.0f, 10.0f)),
        };
        gpu.sun.radiance_shadow = glm::vec4{glm::max(sun.radiance, glm::vec3{0.0f}), 0.0f};

        const bool shadows_enabled = submission.render_graph.shadows;
        const bool punctual_shadows_enabled = shadows_enabled && static_cast<bool>(targets.atlas);
        const bool directional_shadows_enabled =
            shadows_enabled && static_cast<bool>(targets.directional_atlas) &&
            static_cast<bool>(targets.directional_layout);
        const u32 atlas_size = targets.atlas_size;
        const f32 filter_radius_texels = std::clamp(
            finite_or(submission.render_graph.shadow_filter_radius_texels, 2.0f), 0.5f, 8.0f);
        gpu.shadow_params = glm::vec4{
            filter_radius_texels,
            std::clamp(finite_or(submission.render_graph.shadow_normal_bias, 0.75f), 0.0f, 4.0f),
            submission.render_graph.shadow_contact_hardening ? 1.0f : 0.0f,
            std::max(finite_or(submission.render_graph.shadow_max_distance, 250.0f), submission.camera.near_plane),
        };
        // Kept in sync with Engine::ShadowDebugView's final `UnshadowedSunLighting` value.
        gpu.spectral_params.w = static_cast<f32>(std::min(submission.render_graph.shadow_debug_view, 25u));
        gpu.contact_shadow_params = glm::vec4{
            std::clamp(finite_or(submission.render_graph.contact_shadow_distance, 0.5f), 0.0f, 5.0f),
            std::clamp(finite_or(submission.render_graph.contact_shadow_thickness, 0.05f), 0.0f, 1.0f),
            static_cast<f32>(std::clamp(submission.render_graph.contact_shadow_steps, 2u, 12u)),
            submission.render_graph.contact_shadows && directional_shadows_enabled && sun.casts_shadows ? 1.0f : 0.0f,
        };
        gpu.contact_shadow_params_extra = glm::vec4{
            std::clamp(finite_or(submission.render_graph.contact_shadow_intensity, 0.85f), 0.0f, 1.0f),
            std::max(finite_or(submission.render_graph.contact_shadow_fade_distance, 40.0f), 0.01f),
            0.0f,
            0.0f,
        };

        AtlasAllocator allocator;
        const auto has_shadow_caster_in_sphere = [&](glm::vec3 position, f32 range) noexcept {
            const f32 safe_range = std::max(range, kMinimumLightRange);
            for (const RenderItem &item : submission.draws) {
                if (!item.casts_shadows || !std::isfinite(item.world_bounds_radius)) {
                    continue;
                }
                const glm::vec3 delta = item.world_bounds_center - position;
                const f32 combined_radius = safe_range + std::max(item.world_bounds_radius, 0.0f);
                if (glm::dot(delta, delta) <= combined_radius * combined_radius) {
                    return true;
                }
            }
            return false;
        };
        // Punctual (spot/point) views are appended after the directional cascades, so their
        // shadow-view index is the running total across both atlases.
        auto append_shadow_view = [&](const glm::mat4 &matrix, AtlasTile tile, f32 near_plane,
                                      f32 far_plane, bool perspective, f32 light_radius_uv,
                                      f32 world_span_at_unit_depth, f32 max_filter_radius_local,
                                      f32 max_search_radius_local) -> i32 {
            const usize index = prepared.directional_views.size() + prepared.punctual_views.size();
            if (!tile || index >= max_shadow_views) {
                return -1;
            }
            const RHI::Rect2D viewport = tile_viewport(tile, atlas_size);
            prepared.punctual_views.push_back(ShadowRenderView{
                .view_projection = matrix,
                .frustum = frustum_from_view_projection(matrix),
                .viewport = viewport,
            });
            gpu.shadow_views[index] = ShadowViewGpuData{
                .view_projection = matrix,
                .atlas_scale_bias = tile_scale_bias(tile),
                .depth_params = glm::vec4{near_plane, far_plane, perspective ? 1.0f : 0.0f, std::max(light_radius_uv, 0.0f)},
                .filter_params = glm::vec4{world_span_at_unit_depth,
                                           static_cast<f32>(viewport.width),
                                           std::max(max_filter_radius_local, 0.0f),
                                           std::max(max_search_radius_local, 0.0f)},
            };
            return static_cast<i32>(index);
        };


        const bool has_any_shadow_caster = std::any_of(
            submission.draws.begin(), submission.draws.end(),
            [](const RenderItem &item) noexcept { return item.casts_shadows; });
        if (directional_shadows_enabled && has_any_shadow_caster && sun.casts_shadows &&
            luminance(sun.radiance) > 0.0f) {
            const DirectionalAtlasLayout &layout = targets.directional_layout;
            const u32 cascade_count = std::min(
                std::clamp(submission.render_graph.shadow_cascade_count, 1u, max_directional_shadow_cascades),
                layout.cascade_count);
            const f32 camera_near = std::max(submission.camera.near_plane, 0.0001f);
            const f32 camera_far = std::max(camera_near + 0.01f,
                                            std::min(submission.camera.far_plane,
                                                     finite_or(submission.render_graph.shadow_max_distance, 250.0f)));

            // Practical split scheme: a blend of uniform and logarithmic distributions. Logarithmic
            // alone starves the far cascades on a large far plane; uniform alone wastes almost all
            // of cascade 0 on distant geometry.
            const f32 split_lambda = std::clamp(
                finite_or(submission.render_graph.shadow_cascade_split_lambda, 0.65f), 0.0f, 1.0f);
            array<f32, max_directional_shadow_cascades> splits{};
            for (u32 cascade = 0; cascade < cascade_count; ++cascade) {
                const f32 p = static_cast<f32>(cascade + 1) / static_cast<f32>(cascade_count);
                const f32 logarithmic = camera_near * std::pow(camera_far / camera_near, p);
                const f32 uniform = camera_near + (camera_far - camera_near) * p;
                splits[cascade] = cascade + 1u == cascade_count
                                      ? camera_far
                                      : glm::mix(uniform, logarithmic, split_lambda);
            }

            // Explicit fade band per cascade, in view-space distance. Cascade `c` is authoritative
            // from `starts[c]` to `fade_starts[c]` and cross-fades into cascade `c + 1` from there
            // to `splits[c]`. Cascade `c + 1` is fitted from `fade_starts[c]`, not from `splits[c]`,
            // so both cascades are valid everywhere in the band.
            const f32 cascade_blend = std::clamp(
                finite_or(submission.render_graph.shadow_cascade_blend, 0.10f), 0.0f, 0.5f);
            array<f32, max_directional_shadow_cascades> starts{};
            array<f32, max_directional_shadow_cascades> fade_starts{};
            for (u32 cascade = 0; cascade < cascade_count; ++cascade) {
                starts[cascade] = cascade == 0 ? camera_near : splits[cascade - 1];
                fade_starts[cascade] =
                    splits[cascade] - (splits[cascade] - starts[cascade]) * cascade_blend;
            }

            const glm::mat4 camera_world = glm::inverse(submission.camera.view);
            const LightBasis basis = make_light_basis(sun_direction);
            // Only while a shadow debug view is selected, and at most a few times a second, so the
            // instrumentation can never become a per-frame cost in normal operation.
            static std::atomic<u64> last_cascade_log_ms{0};
            bool log_cascade_stats = false;
            if (submission.render_graph.shadow_debug_view != 0) {
                const u64 now_ms = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                u64 previous = last_cascade_log_ms.load(std::memory_order_relaxed);
                if (now_ms - previous >= 2000 &&
                    last_cascade_log_ms.compare_exchange_strong(previous, now_ms,
                                                                std::memory_order_relaxed)) {
                    log_cascade_stats = true;
                }
            }
            const f32 sun_angular_radius = gpu.sun.direction_angular_radius.w;
            u32 emitted_cascades = 0;
            for (u32 cascade = 0; cascade < cascade_count; ++cascade) {
                if (prepared.directional_views.size() >= max_shadow_views) {
                    break;
                }
                const DirectionalCascadeTile &tile = layout.tiles[cascade];
                const f32 resolution = static_cast<f32>(std::max(tile.resolution, 1u));

                // --- Guard region -------------------------------------------------------------
                // Every tap of every filter, after the receiver has been displaced along its normal,
                // must land inside this cascade's own tile. Reserving the worst case up front is
                // what makes "no filter sample may cross into another allocation" an invariant
                // rather than a hope, and it is why the receiver region is inset rather than the
                // filter clamped at sample time.
                const f32 penumbra_cap_texels = filter_radius_texels * kCascadePcssRadiusScale;
                const f32 guard_texels = std::min(
                    std::ceil(penumbra_cap_texels + kMaxNormalOffsetTexels + kCascadeGuardSafetyTexels),
                    resolution * 0.125f);
                const f32 usable_resolution = std::max(resolution - guard_texels * 2.0f, 1.0f);
                const f32 max_filter_radius_local = penumbra_cap_texels / resolution;

                // --- Receiver bounds ----------------------------------------------------------
                const f32 fit_near = cascade == 0 ? camera_near : fade_starts[cascade - 1];
                const f32 fit_far = splits[cascade];
                const array<glm::vec3, 8> corners = calculate_frustum_slice_corners(
                    submission.camera.projection, camera_world, fit_near, fit_far);
                const ReceiverBounds receiver = calculate_receiver_bounds(
                    span<const glm::vec3>{corners.data(), corners.size()}, basis);

                // --- Stable extent ------------------------------------------------------------
                // The tight light-space extent is what we want; the slice's bounding sphere
                // diameter is the rotation-invariant ceiling we clamp against so the stabilized
                // value can never exceed the old sphere fit. See `stabilize_cascade_extent` for
                // why quantizing (rather than using the tight value directly) is what removes
                // rotational crawl.
                const auto [sphere_center_view, sphere_radius] =
                    stable_frustum_slice_sphere(submission.camera.projection, fit_near, fit_far);
                const f32 extent = stabilize_cascade_extent(
                    receiver.tight_extent, 2.0f * sphere_radius, directional_state.stable_extent[cascade]);
                if (!(extent > 0.0f) || !std::isfinite(extent)) {
                    break;
                }

                // --- Texel snapping -----------------------------------------------------------
                // `world_texel` is constant while `extent` sits on the same ladder rung, so
                // quantizing the cascade centre onto multiples of it in light space locks the
                // shadow-map lattice to the world. Camera translation below one texel produces a
                // bit-identical projection matrix, which is exactly the no-swimming condition.
                const f32 world_texel = extent / usable_resolution;
                const f32 padded_extent = world_texel * resolution;
                const f32 half_extent = padded_extent * 0.5f;
                const glm::vec2 snapped_center{
                    std::round(receiver.center.x / world_texel) * world_texel,
                    std::round(receiver.center.y / world_texel) * world_texel,
                };
                const glm::vec2 minimum_xy = snapped_center - glm::vec2{half_extent};
                const glm::vec2 maximum_xy = snapped_center + glm::vec2{half_extent};

                // --- Caster bounds ------------------------------------------------------------
                ShadowRenderView view{};
                const CascadeDepthRange depth_range = calculate_caster_depth_range(
                    submission.draws, basis, minimum_xy, maximum_xy, receiver.minimum_depth,
                    receiver.maximum_depth, view.caster_indices);
                view.has_caster_list = true;

                const f32 depth_margin = std::max(0.05f, world_texel * 4.0f);
                const f32 near_depth_light = depth_range.minimum - depth_margin;
                const f32 depth_span = std::max(
                    (depth_range.maximum + depth_margin) - near_depth_light, 0.01f);

                const glm::vec3 eye = from_light_space(
                    basis, glm::vec3{snapped_center.x, snapped_center.y, near_depth_light});
                const glm::mat4 light_view =
                    glm::lookAtRH(eye, eye + basis.forward, basis.reference_up);
                const glm::mat4 light_projection = glm::orthoRH_ZO(
                    -half_extent, half_extent, -half_extent, half_extent, 0.0f, depth_span);
                const glm::mat4 light_view_projection = light_projection * light_view;

                const usize index = prepared.directional_views.size();
                view.view_projection = light_view_projection;
                view.frustum = frustum_from_view_projection(light_view_projection);
                view.viewport = RHI::Rect2D{
                    .x = static_cast<i32>(tile.x),
                    .y = static_cast<i32>(tile.y),
                    .width = tile.resolution,
                    .height = tile.resolution,
                };
                prepared.directional_views.push_back(std::move(view));

                // Sun penumbra half-angle expressed as tile-local UV per world unit of
                // receiver-to-blocker separation, so PCSS scales with the cascade's own footprint.
                const f32 local_radius_per_world =
                    std::tan(sun_angular_radius) / std::max(padded_extent, 0.001f);
                gpu.shadow_views[index] = ShadowViewGpuData{
                    .view_projection = light_view_projection,
                    .atlas_scale_bias = glm::vec4{
                        static_cast<f32>(tile.resolution) / static_cast<f32>(layout.width),
                        static_cast<f32>(tile.resolution) / static_cast<f32>(layout.height),
                        static_cast<f32>(tile.x) / static_cast<f32>(layout.width),
                        static_cast<f32>(tile.y) / static_cast<f32>(layout.height),
                    },
                    .depth_params = glm::vec4{0.0f, depth_span, 0.0f, local_radius_per_world},
                    .filter_params = glm::vec4{padded_extent, resolution, max_filter_radius_local,
                                               max_filter_radius_local},
                };
                gpu.sun.cascade_splits[cascade] = splits[cascade];
                gpu.sun.cascade_fade_starts[cascade] = fade_starts[cascade];
                ++emitted_cascades;

                if (log_cascade_stats) {
                    // Quantitative counterpart to the cascade debug views: utilization is how much
                    // of the tight receiver extent survives the ladder quantization and the guard
                    // band, so a persistently low number means the fit (not the resolution) is what
                    // is costing shadow detail.
                    const f32 utilization = receiver.tight_extent / std::max(padded_extent, 1.0e-6f);
                    Foundation::log_info(
                        "CSM c{}: view [{:.2f}, {:.2f}] tight {:.2f}m stable {:.2f}m padded {:.2f}m "
                        "texel {:.4f}m res {} util {:.0f}% casters {}/{} depth {:.2f}m",
                        cascade, fit_near, fit_far, receiver.tight_extent, extent, padded_extent,
                        world_texel, tile.resolution, utilization * 100.0f,
                        prepared.directional_views.back().caster_indices.size(),
                        submission.draws.size(), depth_span);
                }
            }
            if (emitted_cascades > 0) {
                // If the view budget truncated the cascade set, the last surviving cascade has to
                // own the remaining range and fade out at the shadow distance rather than leaving
                // a hard edge where the missing cascade would have started.
                gpu.sun.cascade_splits[emitted_cascades - 1] = camera_far;
                gpu.sun.cascade_fade_starts[emitted_cascades - 1] = std::min(
                    gpu.sun.cascade_fade_starts[emitted_cascades - 1],
                    camera_far - (camera_far - starts[emitted_cascades - 1]) * cascade_blend);
                gpu.sun.radiance_shadow.w = 1.0f;
                gpu.sun.cascade_params = glm::vec4{
                    static_cast<f32>(emitted_cascades),
                    cascade_blend,
                    0.0f,
                    0.0f,
                };
            }
        }

        vector<usize> spot_order(submission.lighting.spot_lights.size());
        std::iota(spot_order.begin(), spot_order.end(), usize{0});
        std::stable_sort(spot_order.begin(), spot_order.end(), [&](usize a, usize b) {
            const SpotLight &left = submission.lighting.spot_lights[a];
            const SpotLight &right = submission.lighting.spot_lights[b];
            return punctual_importance(left.position, left.radiance, left.range, submission.camera.world_position) >
                   punctual_importance(right.position, right.radiance, right.range, submission.camera.world_position);
        });
        const u32 spot_count = static_cast<u32>(std::min<usize>(spot_order.size(), max_lighting_spot_lights));
        u32 shadowed_spots = 0;
        for (u32 output_index = 0; output_index < spot_count; ++output_index) {
            const SpotLight &light = submission.lighting.spot_lights[spot_order[output_index]];
            const f32 range = std::max(light.range, kMinimumLightRange);
            const glm::vec3 direction = safe_normalize(light.direction, glm::vec3{0.0f, -1.0f, 0.0f});
            SpotLightGpuData &output = gpu.spot_lights[output_index];
            output.position_range = glm::vec4{light.position, range};
            output.direction_outer_cos =
                glm::vec4{direction, std::clamp(light.outer_cone_cos, 0.001f, 0.9999f)};
            output.radiance_inner_cos = glm::vec4{glm::max(light.radiance, glm::vec3{0.0f}),
                                                  std::max(light.inner_cone_cos, output.direction_outer_cos.w + 0.0001f)};
            output.shadow_params = glm::vec4{-1.0f, std::max(light.source_radius, 0.0f), 0.0f, 0.0f};
            if (!punctual_shadows_enabled || !light.casts_shadows ||
                shadowed_spots >= std::min(submission.render_graph.max_shadowed_spot_lights,
                                           max_lighting_spot_lights) ||
                !has_shadow_caster_in_sphere(light.position, range)) {
                continue;
            }
            const u32 desired_cells = positional_shadow_tile_cells(
                light.position, range, submission.camera.world_position, submission.camera.projection,
                render_extent, atlas_size);
            AtlasTile tile = allocator.allocate(desired_cells);
            if (!tile && desired_cells > 1u) {
                tile = allocator.allocate(1u);
            }
            if (!tile) {
                continue;
            }
            const f32 outer_angle = std::acos(output.direction_outer_cos.w);
            const f32 shadow_near = std::max(0.02f, range * 0.001f);
            glm::mat4 projection = glm::perspectiveRH_ZO(std::max(outer_angle * 2.0f, 0.02f), 1.0f, shadow_near, range);
            const glm::mat4 view = glm::lookAtRH(light.position, light.position + direction, light_up(direction));
            const glm::mat4 matrix = projection * view;
            const f32 radius_uv_world = std::max(light.source_radius, 0.0f) /
                                        std::max(2.0f * std::tan(outer_angle), 0.001f);
            const i32 view_index = append_shadow_view(
                matrix, tile, shadow_near, range, true, radius_uv_world,
                2.0f * std::tan(outer_angle), 0.10f, 0.075f);
            if (view_index >= 0) {
                output.shadow_params.x = static_cast<f32>(view_index);
                ++shadowed_spots;
            }
        }

        vector<usize> point_order(submission.lighting.point_lights.size());
        std::iota(point_order.begin(), point_order.end(), usize{0});
        std::stable_sort(point_order.begin(), point_order.end(), [&](usize a, usize b) {
            const PointLight &left = submission.lighting.point_lights[a];
            const PointLight &right = submission.lighting.point_lights[b];
            return punctual_importance(left.position, left.radiance, left.range, submission.camera.world_position) >
                   punctual_importance(right.position, right.radiance, right.range, submission.camera.world_position);
        });
        const u32 point_count = static_cast<u32>(std::min<usize>(point_order.size(), max_lighting_point_lights));
        u32 shadowed_points = 0;
        const array<glm::vec3, 6> face_directions{
            glm::vec3{1.0f, 0.0f, 0.0f},
            glm::vec3{-1.0f, 0.0f, 0.0f},
            glm::vec3{0.0f, 1.0f, 0.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
            glm::vec3{0.0f, 0.0f, -1.0f},
        };
        const array<glm::vec3, 6> face_ups{
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
            glm::vec3{0.0f, 0.0f, -1.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
        };
        for (u32 output_index = 0; output_index < point_count; ++output_index) {
            const PointLight &light = submission.lighting.point_lights[point_order[output_index]];
            const f32 range = std::max(light.range, kMinimumLightRange);
            PointLightGpuData &output = gpu.point_lights[output_index];
            output.position_range = glm::vec4{light.position, range};
            output.radiance_source_radius = glm::vec4{glm::max(light.radiance, glm::vec3{0.0f}),
                                                      std::max(light.source_radius, 0.0f)};
            output.shadow_params = glm::vec4{-1.0f, 0.0f, 0.0f, 0.0f};
            if (!punctual_shadows_enabled || !light.casts_shadows ||
                shadowed_points >= std::min(submission.render_graph.max_shadowed_point_lights,
                                            max_shadowed_point_lights) ||
                !has_shadow_caster_in_sphere(light.position, range)) {
                continue;
            }


            const u32 desired_cells = positional_shadow_tile_cells(
                light.position, range, submission.camera.world_position, submission.camera.projection,
                render_extent, atlas_size);
            AtlasAllocator candidate_allocator = allocator;
            array<AtlasTile, 6> tiles{};
            bool allocated = true;
            for (AtlasTile &tile : tiles) {
                tile = candidate_allocator.allocate(desired_cells);
                allocated &= static_cast<bool>(tile);
            }
            if (!allocated && desired_cells > 1u) {
                candidate_allocator = allocator;
                allocated = true;
                for (AtlasTile &tile : tiles) {
                    tile = candidate_allocator.allocate(1u);
                    allocated &= static_cast<bool>(tile);
                }
            }
            if (!allocated ||
                prepared.directional_views.size() + prepared.punctual_views.size() + 6 > max_shadow_views) {
                continue;
            }
            allocator = candidate_allocator;
            const f32 shadow_near = std::max(0.02f, range * 0.001f);
            const glm::mat4 projection = glm::perspectiveRH_ZO(kPointShadowFaceFovRadians, 1.0f, shadow_near, range);
            const i32 first_view = static_cast<i32>(prepared.directional_views.size() +
                                                     prepared.punctual_views.size());
            const f32 face_span_at_unit_depth = 2.0f * std::tan(kPointShadowFaceFovRadians * 0.5f);
            const f32 radius_uv_world = std::max(light.source_radius, 0.0f) /
                                        std::max(face_span_at_unit_depth, 0.001f);
            for (usize face = 0; face < face_directions.size(); ++face) {
                const glm::mat4 view = glm::lookAtRH(light.position, light.position + face_directions[face], face_ups[face]);
                (void)append_shadow_view(projection * view, tiles[face], shadow_near, range,
                                         true, radius_uv_world, face_span_at_unit_depth, 0.10f, 0.075f);
            }
            output.shadow_params.x = static_cast<f32>(first_view);
            ++shadowed_points;
        }

        prepared.atlas_used = !prepared.punctual_views.empty();
        prepared.directional_atlas_used = !prepared.directional_views.empty();
        const usize total_views = prepared.directional_views.size() + prepared.punctual_views.size();
        gpu.counts = glm::vec4{static_cast<f32>(spot_count), static_cast<f32>(point_count),
                               static_cast<f32>(total_views),
                               total_views != 0 ? 1.0f : 0.0f};
        const span<const ShadowLightingGpuData> data{&gpu, 1};
        auto written = device->write_buffer(targets.lighting_buffer, 0, std::as_bytes(data));
        if (!written) {
            return unexpected(graphics_error_from_rhi(written.error(), "write shadow lighting constants"));
        }
        return {};
    }

    /// Finds or creates the shadow lighting resources required by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_shadow_lighting_resources() {
        ZoneScopedN("Renderer::ensure_shadow_lighting_resources");
        auto guard = shadow_lighting_.lock();
        if (guard->ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(shadow_error("Cannot build shadow lighting resources without an RHI device."));
        }

        const auto shader_target = shader_target_for_device(*device);
        if (!shader_target) return unexpected(shader_target.error());

        const slang::ShaderCompileOptions options{
            .targets = shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file("Shaders/deferred_shadow_lighting.slang", "deferred_shadow_lighting"),
            options,
            slang::ShaderCompiler{},
            recovery_create_info_.enable_shader_disk_cache};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) {
            return unexpected(shadow_error("compile deferred shadow lighting shader failed: " +
                                           shader.error().message + "\n" + shader.error().diagnostics));
        }
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->fragment_entry_point = "fragmentMain";

        auto create_module = [&](string_view entry, const char *label) -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto code = guard->shader.entry_point_code(entry, shader_target->slang_target.format);
            if (!code) {
                string message = string{"generate shadow lighting bytecode failed: "} + code.error().message;
                if (!code.error().diagnostics.empty()) {
                    message += "\n" + code.error().diagnostics;
                }
                return unexpected(shadow_error(std::move(message)));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = shader_target->module_language,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) {
                return unexpected(graphics_error_from_rhi(module.error(), label));
            }
            return *module;
        };
        auto vertex_module = create_module(guard->vertex_entry_point, "shadow lighting vertex module");
        if (!vertex_module) {
            return unexpected(vertex_module.error());
        }
        guard->vertex_module = *vertex_module;
        auto fragment_module = create_module(guard->fragment_entry_point, "shadow lighting fragment module");
        if (!fragment_module) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(fragment_module.error());
        }
        guard->fragment_module = *fragment_module;

        const slang::ShaderReflection &reflection = guard->shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(
            reflection,
            reflected_stage_mask(reflection));
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "shadow lighting bind group layout",
            });
            if (!handle) {
                destroy_shadow_lighting_resources_locked(*guard);
                return unexpected(graphics_error_from_rhi(handle.error(), "create shadow lighting bind group layout"));
            }
            guard->bind_group_layouts.push_back(*handle);
            guard->bind_group_layout_sets.push_back(layout.set);
        }
        if (guard->bind_group_layouts.empty()) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(shadow_error("Shadow lighting reflection produced no bind-group layout."));
        }
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{guard->bind_group_layouts.data(),
                                                                         guard->bind_group_layouts.size()},
            .push_constant_ranges = {},
            .label = "shadow lighting pipeline layout",
        });
        if (!pipeline_layout) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create shadow lighting pipeline layout"));
        }
        guard->pipeline_layout = *pipeline_layout;

        const RHI::SamplerDesc nearest_sampler_desc{
            .min_filter = RHI::Filter::Nearest,
            .mag_filter = RHI::Filter::Nearest,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "shadow lighting nearest sampler",
        };
        auto gbuffer_sampler = device->create_sampler(nearest_sampler_desc);
        if (!gbuffer_sampler) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(gbuffer_sampler.error(), "create G-buffer sampler"));
        }
        guard->gbuffer_sampler = *gbuffer_sampler;

        RHI::SamplerDesc shadow_depth_sampler_desc = nearest_sampler_desc;
        shadow_depth_sampler_desc.label = "shadow blocker depth sampler";
        auto shadow_depth_sampler = device->create_sampler(shadow_depth_sampler_desc);
        if (!shadow_depth_sampler) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(shadow_depth_sampler.error(), "create raw shadow depth sampler"));
        }
        guard->shadow_depth_sampler = *shadow_depth_sampler;

        const RHI::SamplerDesc shadow_compare_sampler_desc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .compare_enable = true,
            .compare = RHI::CompareOp::LessEqual,
            .label = "shadow hardware PCF comparison sampler",
        };
        auto shadow_compare_sampler = device->create_sampler(shadow_compare_sampler_desc);
        if (!shadow_compare_sampler) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(shadow_compare_sampler.error(), "create shadow comparison sampler"));
        }
        guard->shadow_compare_sampler = *shadow_compare_sampler;
        const RHI::SamplerDesc atmosphere_sampler_desc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "atmosphere lut linear sampler",
        };
        auto atmosphere_sampler = device->create_sampler(atmosphere_sampler_desc);
        if (!atmosphere_sampler) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(atmosphere_sampler.error(), "create atmosphere lut sampler"));
        }
        guard->atmosphere_sampler = *atmosphere_sampler;
        guard->shader.release_compiler_state();
        guard->ready = true;
        return {};
    }

    /// Resolves the shadow lighting pipeline associated with the supplied key, handle, or resource.
    ///
    /// @param color_format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::shadow_lighting_pipeline_for(
        RHI::Format color_format) {
        ZoneScopedN("Renderer::shadow_lighting_pipeline_for");
        if (Core::RendererResult ready = ensure_shadow_lighting_resources(); !ready) {
            return unexpected(ready.error());
        }
        auto guard = shadow_lighting_.lock();
        for (const ShadowLightingPipelineVariant &variant : guard->pipeline_variants) {
            if (variant.color_format == color_format) {
                return variant.pipeline;
            }
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(shadow_error("Cannot build a shadow lighting pipeline without an RHI device."));
        }
        const RHI::ColorTargetState color_target{
            .format = color_format,
            .blend_enable = false,
            .write_mask = RHI::ColorWriteMask::All,
        };
        auto pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = guard->pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->vertex_module,
                                       .entry_point = guard->vertex_entry_point.c_str(),
                                       .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = guard->fragment_module,
                                         .entry_point = guard->fragment_entry_point.c_str(),
                                         .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "deferred shadow lighting pipeline",
        });
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create deferred shadow lighting pipeline"));
        }
        guard->pipeline_variants.push_back(ShadowLightingPipelineVariant{
            .color_format = color_format,
            .pipeline = *pipeline,
        });
        return *pipeline;
    }

    /// Records shadow lighting using the supplied arguments and current state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param albedo_view `albedo_view` value used by the operation.
    /// @param normal_view `normal_view` value used by the operation.
    /// @param material_view `material_view` value used by the operation.
    /// @param emissive_view `emissive_view` value used by the operation.
    /// @param depth_view `depth_view` value used by the operation.
    /// @param spectral_effect_view `spectral_effect_view` value used by the operation.
    /// @param shadow_atlas_view Punctual (spot/point) shadow atlas view.
    /// @param directional_shadow_atlas_view Dedicated directional cascade atlas view.
    /// @param lighting_buffer Buffer used or affected by the operation.
    /// @param transmittance_lut_view `transmittance_lut_view` value used by the operation.
    /// @param multi_scattering_lut_view `multi_scattering_lut_view` value used by the operation.
    /// @param sky_view_lut_view `sky_view_lut_view` value used by the operation.
    /// @param atmosphere_buffer Buffer used or affected by the operation.
    /// @param color_format Format used for the resource, render target, or conversion.
    /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_shadow_lighting(
        RHI::RenderPassEncoder &pass,
        RHI::TextureViewHandle albedo_view,
        RHI::TextureViewHandle normal_view,
        RHI::TextureViewHandle material_view,
        RHI::TextureViewHandle emissive_view,
        RHI::TextureViewHandle depth_view,
        RHI::TextureViewHandle spectral_effect_view,
        RHI::TextureViewHandle shadow_atlas_view,
        RHI::TextureViewHandle directional_shadow_atlas_view,
        RHI::BufferHandle lighting_buffer,
        RHI::TextureViewHandle transmittance_lut_view,
        RHI::TextureViewHandle multi_scattering_lut_view,
        RHI::TextureViewHandle sky_view_lut_view,
        RHI::TextureViewHandle surfel_irradiance_view,
        RHI::TextureViewHandle gtao_ambient_occlusion_view,
        RHI::BufferHandle atmosphere_buffer,
        RHI::Format color_format,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_shadow_lighting");
        auto pipeline = shadow_lighting_pipeline_for(color_format);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !albedo_view || !normal_view || !material_view || !emissive_view || !depth_view ||
            !spectral_effect_view || !shadow_atlas_view || !directional_shadow_atlas_view || !lighting_buffer || !transmittance_lut_view || !multi_scattering_lut_view ||
            !sky_view_lut_view || !surfel_irradiance_view || !gtao_ambient_occlusion_view || !atmosphere_buffer) {
            return unexpected(shadow_error("Deferred shadow lighting received an invalid G-buffer, atlas, or constants resource."));
        }

        vector<ReflectedResource> resources;
        RHI::SamplerHandle gbuffer_sampler{};
        RHI::SamplerHandle shadow_depth_sampler{};
        RHI::SamplerHandle shadow_compare_sampler{};
        RHI::SamplerHandle atmosphere_sampler{};
        vector<u32> bind_group_layout_sets;
        vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
        {
            auto guard = shadow_lighting_.lock();
            resources = collect_resource_bindings(guard->shader.reflection());
            gbuffer_sampler = guard->gbuffer_sampler;
            shadow_depth_sampler = guard->shadow_depth_sampler;
            shadow_compare_sampler = guard->shadow_compare_sampler;
            atmosphere_sampler = guard->atmosphere_sampler;
            bind_group_layout_sets = guard->bind_group_layout_sets;
            bind_group_layouts = guard->bind_group_layouts;
        }
        if (resources.empty()) {
            return unexpected(shadow_error("Shadow lighting reflection produced no resource bindings."));
        }
        const u32 set = resources.front().set;
        vector<RHI::BindGroupEntry> entries;
        entries.reserve(resources.size());
        for (const ReflectedResource &resource : resources) {
            if (resource.set != set) {
                return unexpected(shadow_error("Shadow lighting resources unexpectedly span multiple bind groups."));
            }
            RHI::BindGroupEntry entry{.binding = resource.binding};
            if (resource.name == "lightingData") {
                entry.buffer = lighting_buffer;
                entry.size = sizeof(ShadowLightingGpuData);
            } else if (resource.name == "atmosphereData") {
                entry.buffer = atmosphere_buffer;
                entry.size = sizeof(AtmosphereGpuData);
            } else if (resource.name == "gbufferAlbedo") {
                entry.texture_view = albedo_view;
            } else if (resource.name == "gbufferNormal") {
                entry.texture_view = normal_view;
            } else if (resource.name == "gbufferMaterial") {
                entry.texture_view = material_view;
            } else if (resource.name == "gbufferEmissive") {
                entry.texture_view = emissive_view;
            } else if (resource.name == "gbufferDepth") {
                entry.texture_view = depth_view;
            } else if (resource.name == "spectralEffect") {
                entry.texture_view = spectral_effect_view;
            } else if (resource.name == "shadowAtlas") {
                entry.texture_view = shadow_atlas_view;
            } else if (resource.name == "directionalShadowAtlas") {
                entry.texture_view = directional_shadow_atlas_view;
            } else if (resource.name == "transmittanceLut") {
                entry.texture_view = transmittance_lut_view;
            } else if (resource.name == "multiScatteringLut") {
                entry.texture_view = multi_scattering_lut_view;
            } else if (resource.name == "skyViewLut") {
                entry.texture_view = sky_view_lut_view;
            } else if (resource.name == "surfelIrradiance") {
                entry.texture_view = surfel_irradiance_view;
            } else if (resource.name == "gtaoAmbientOcclusion") {
                entry.texture_view = gtao_ambient_occlusion_view;
            } else if (resource.name == "gbufferSampler") {
                entry.sampler = gbuffer_sampler;
            } else if (resource.name == "shadowDepthSampler") {
                entry.sampler = shadow_depth_sampler;
            } else if (resource.name == "shadowCompareSampler") {
                entry.sampler = shadow_compare_sampler;
            } else if (resource.name == "atmosphereSampler") {
                entry.sampler = atmosphere_sampler;
            } else {
                return unexpected(shadow_error("Shadow lighting reflection contains an unknown resource: " + resource.name));
            }
            entries.push_back(entry);
        }
        const usize layout_index = bind_group_layout_index_for_set(bind_group_layout_sets, set);
        if (layout_index >= bind_group_layouts.size()) {
            return unexpected(shadow_error("Shadow lighting bind group has no matching generated layout."));
        }
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = bind_group_layouts[layout_index],
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "deferred shadow lighting bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create deferred shadow lighting bind group"));
        }


        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }
        pass.set_pipeline(*pipeline);
        pass.set_bind_group(set, *bind_group);
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    /// Destroys the shadow lighting resources identified by the supplied parameters.
    ///
    /// @return Returns the current destroy shadow lighting resources value.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_shadow_lighting_resources() noexcept {
        ZoneScopedN("Renderer::destroy_shadow_lighting_resources");
        auto guard = shadow_lighting_.lock();
        destroy_shadow_lighting_resources_locked(*guard);
    }

    /// Destroys the shadow lighting resources locked identified by the supplied parameters.
    ///
    /// @param resources `resources` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_shadow_lighting_resources_locked(ShadowLightingResources &resources) noexcept {
        ZoneScopedN("Renderer::destroy_shadow_lighting_resources_locked");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            resources = {};
            return;
        }
        for (const ShadowLightingPipelineVariant &variant : resources.pipeline_variants) {
            if (variant.pipeline) {
                device->destroy_render_pipeline(variant.pipeline);
            }
        }
        if (resources.shadow_compare_sampler)
            device->destroy_sampler(resources.shadow_compare_sampler);
        if (resources.shadow_depth_sampler)
            device->destroy_sampler(resources.shadow_depth_sampler);
        if (resources.gbuffer_sampler)
            device->destroy_sampler(resources.gbuffer_sampler);
        if (resources.atmosphere_sampler)
            device->destroy_sampler(resources.atmosphere_sampler);
        if (resources.pipeline_layout)
            device->destroy_pipeline_layout(resources.pipeline_layout);
        for (RHI::BindGroupLayoutHandle layout : resources.bind_group_layouts) {
            device->destroy_bind_group_layout(layout);
        }
        if (resources.fragment_module)
            device->destroy_shader_module(resources.fragment_module);
        if (resources.vertex_module)
            device->destroy_shader_module(resources.vertex_module);
        resources = {};
    }

} // namespace SFT::Renderer
