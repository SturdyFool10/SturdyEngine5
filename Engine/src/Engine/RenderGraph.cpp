#include "RenderGraph.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace SFT::Engine {

    namespace {
        [[nodiscard]] bool finite(glm::vec4 value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z) && std::isfinite(value.w);
        }

        [[nodiscard]] u32 next_graph_generation() noexcept {
            static std::atomic<u32> next{1};
            u32 generation = next.fetch_add(1, std::memory_order_relaxed);
            if (generation == 0) {
                generation = next.fetch_add(1, std::memory_order_relaxed);
            }
            return generation;
        }
    } // namespace

    RenderGraph::RenderGraph(EmptyTag, RenderGraphDescription description) noexcept
        : description_(std::move(description)), generation_(next_graph_generation()) {}

    RenderGraph::RenderGraph() noexcept
        : RenderGraph(EmptyTag{}, RenderGraphDescription{}) {
        build_standard_topology();
    }

    RenderGraph::RenderGraph(RenderGraphDescription description) noexcept
        : RenderGraph(EmptyTag{}, std::move(description)) {
        build_standard_topology();
    }

    RenderGraph::RenderGraph(const RenderGraph &other)
        : description_(other.description_),
          generation_(other.generation_),
          textures_(other.textures_),
          passes_(other.passes_),
          presented_texture_(other.presented_texture_) {
        rebase_handles();
    }

    RenderGraph &RenderGraph::operator=(const RenderGraph &other) {
        if (this == &other) {
            return *this;
        }
        description_ = other.description_;
        generation_ = other.generation_;
        textures_ = other.textures_;
        passes_ = other.passes_;
        presented_texture_ = other.presented_texture_;
        rebase_handles();
        return *this;
    }

    void RenderGraph::rebase_handles() noexcept {
        generation_ = next_graph_generation();
        for (RenderGraphTextureDescription &texture : textures_) {
            if (texture.extent.input) {
                texture.extent.input.generation = generation_;
            }
        }
        for (usize index = 0; index < passes_.size(); ++index) {
            RenderGraphPassDescription &pass = passes_[index];
            pass.handle = RenderGraphPassHandle{
                .index = static_cast<u32>(index),
                .generation = generation_,
            };
            if (pass.input) {
                pass.input.generation = generation_;
            }
            if (pass.output) {
                pass.output.generation = generation_;
            }
        }
        if (presented_texture_) {
            presented_texture_.generation = generation_;
        }
    }

    RenderGraph RenderGraph::standard() noexcept { return {}; }

    RenderGraph RenderGraph::empty(RenderGraphDescription description) noexcept {
        return RenderGraph{EmptyTag{}, std::move(description)};
    }

    RenderGraph RenderGraph::overlay_only() noexcept {
        RenderGraph graph;
        graph.description_.scene.enabled = false;
        graph.description_.shadows.enabled = false;
        graph.description_.ambient_occlusion.enabled = false;
        graph.description_.anti_aliasing.post_process = PostProcessAntiAliasing::None;
        graph.description_.bloom.enabled = false;
        graph.description_.debug_overlay.enabled = true;
        return graph;
    }

    const RenderGraphDescription &RenderGraph::description() const noexcept { return description_; }
    RenderGraphDescription &RenderGraph::description() noexcept { return description_; }
    const SceneRenderSettings &RenderGraph::scene() const noexcept { return description_.scene; }
    SceneRenderSettings &RenderGraph::scene() noexcept { return description_.scene; }
    const ShadowSettings &RenderGraph::shadows() const noexcept { return description_.shadows; }
    ShadowSettings &RenderGraph::shadows() noexcept { return description_.shadows; }
    const AmbientOcclusionSettings &RenderGraph::ambient_occlusion() const noexcept { return description_.ambient_occlusion; }
    AmbientOcclusionSettings &RenderGraph::ambient_occlusion() noexcept { return description_.ambient_occlusion; }
    const AntiAliasingSettings &RenderGraph::anti_aliasing() const noexcept { return description_.anti_aliasing; }
    AntiAliasingSettings &RenderGraph::anti_aliasing() noexcept { return description_.anti_aliasing; }
    const BloomSettings &RenderGraph::bloom() const noexcept { return description_.bloom; }
    BloomSettings &RenderGraph::bloom() noexcept { return description_.bloom; }
    const ToneMappingSettings &RenderGraph::tone_mapping() const noexcept { return description_.tone_mapping; }
    ToneMappingSettings &RenderGraph::tone_mapping() noexcept { return description_.tone_mapping; }
    const DebugOverlayRenderSettings &RenderGraph::debug_overlay() const noexcept { return description_.debug_overlay; }
    DebugOverlayRenderSettings &RenderGraph::debug_overlay() noexcept { return description_.debug_overlay; }
    RenderGraphExecutionMode RenderGraph::execution_mode() const noexcept { return description_.execution_mode; }

    const std::vector<RenderGraphTextureDescription> &RenderGraph::textures() const noexcept { return textures_; }
    const std::vector<RenderGraphPassDescription> &RenderGraph::passes() const noexcept { return passes_; }
    RenderGraphTextureHandle RenderGraph::presented_texture() const noexcept { return presented_texture_; }

    bool RenderGraph::contains_pass(RenderGraphPassKind kind) const noexcept {
        return std::ranges::any_of(passes_, [kind](const RenderGraphPassDescription &pass) {
            return pass.kind == kind;
        });
    }

    std::vector<RenderGraphPassHandle> RenderGraph::presentation_path() const {
        const auto valid_texture = [this](RenderGraphTextureHandle handle) noexcept {
            return handle && handle.generation == generation_ && handle.index < textures_.size();
        };

        std::vector<i32> producer(textures_.size(), -1);
        usize present_index = passes_.size();
        for (usize index = 0; index < passes_.size(); ++index) {
            const RenderGraphPassDescription &pass = passes_[index];
            if (valid_texture(pass.output)) {
                producer[pass.output.index] = static_cast<i32>(index);
            }
            if (pass.kind == RenderGraphPassKind::Present) {
                present_index = index;
            }
        }
        if (present_index == passes_.size() || !valid_texture(passes_[present_index].input)) {
            return {};
        }

        std::vector<RenderGraphPassHandle> reverse_path;
        reverse_path.reserve(passes_.size());
        reverse_path.push_back(passes_[present_index].handle);
        RenderGraphTextureHandle cursor = passes_[present_index].input;
        while (valid_texture(cursor) && reverse_path.size() <= passes_.size()) {
            const i32 producer_index = producer[cursor.index];
            if (producer_index < 0 || static_cast<usize>(producer_index) >= passes_.size()) {
                return {};
            }
            const RenderGraphPassDescription &pass = passes_[static_cast<usize>(producer_index)];
            reverse_path.push_back(pass.handle);
            if (!pass.input) {
                std::ranges::reverse(reverse_path);
                return reverse_path;
            }
            cursor = pass.input;
        }
        return {};
    }

    bool RenderGraph::presentation_contains_pass(RenderGraphPassKind kind) const {
        const std::vector<RenderGraphPassHandle> path = presentation_path();
        return std::ranges::any_of(path, [this, kind](RenderGraphPassHandle handle) {
            return handle.generation == generation_ && handle.index < passes_.size() &&
                   passes_[handle.index].kind == kind;
        });
    }

    RenderGraphTextureHandle RenderGraph::create_texture(RenderGraphTextureDescription description) {
        const RenderGraphTextureHandle handle{
            .index = static_cast<u32>(textures_.size()),
            .generation = generation_,
        };
        textures_.push_back(std::move(description));
        return handle;
    }

    RenderGraphTextureHandle RenderGraph::add_builtin_pass(
        RenderGraphPassKind kind,
        RenderGraphTextureHandle input,
        RenderGraphTextureDescription output,
        UString label) {
        const RenderGraphTextureHandle output_handle = create_texture(std::move(output));
        const RenderGraphPassHandle pass_handle{
            .index = static_cast<u32>(passes_.size()),
            .generation = generation_,
        };
        passes_.push_back(RenderGraphPassDescription{
            .handle = pass_handle,
            .kind = kind,
            .input = input,
            .output = output_handle,
            .label = std::move(label),
        });
        return output_handle;
    }

    RenderGraphTextureHandle RenderGraph::add_fullscreen_effect(
        RenderGraphTextureHandle input,
        const FullscreenEffectDescription &effect) {
        const RenderGraphTextureHandle output = create_texture(RenderGraphTextureDescription{
            .format = RenderGraphTextureFormat::Inherit,
            .extent = RenderGraphExtent::relative_to(input),
            .label = effect.label,
        });
        const RenderGraphPassHandle pass_handle{
            .index = static_cast<u32>(passes_.size()),
            .generation = generation_,
        };
        passes_.push_back(RenderGraphPassDescription{
            .handle = pass_handle,
            .kind = RenderGraphPassKind::FullscreenEffect,
            .input = input,
            .output = output,
            .fullscreen_effect = effect,
            .label = effect.label,
        });
        return output;
    }

    RenderGraphPassHandle RenderGraph::add_present_pass(RenderGraphTextureHandle input) {
        const RenderGraphPassHandle pass_handle{
            .index = static_cast<u32>(passes_.size()),
            .generation = generation_,
        };
        passes_.push_back(RenderGraphPassDescription{
            .handle = pass_handle,
            .kind = RenderGraphPassKind::Present,
            .input = input,
            .label = UString{"present"_ustr},
        });
        presented_texture_ = input;
        return pass_handle;
    }

    void RenderGraph::build_standard_topology() {
        RenderGraphTextureHandle color = compose(RenderModules::DeferredScene{});
        color = compose(RenderModules::AntiAliasing{.input = color});
        color = compose(RenderModules::Bloom{.input = color});
        color = compose(RenderModules::ToneMapping{.input = color});
        color = compose(RenderModules::DebugOverlay{.input = color});
        (void)compose(RenderModules::Present{.input = color});
    }

    bool RenderGraph::enabled(RenderFeature feature) const noexcept {
        switch (feature) {
            case RenderFeature::Scene:
                return description_.scene.enabled;
            case RenderFeature::Shadows:
                return description_.shadows.enabled;
            case RenderFeature::AmbientOcclusion:
                return description_.ambient_occlusion.enabled;
            case RenderFeature::AntiAliasing:
                return description_.anti_aliasing.msaa_samples > 1 ||
                       description_.anti_aliasing.post_process != PostProcessAntiAliasing::None;
            case RenderFeature::Bloom:
                return description_.bloom.enabled;
            case RenderFeature::ToneMapping:
                return description_.tone_mapping.enabled;
            case RenderFeature::DebugOverlay:
                return description_.debug_overlay.enabled;
        }
        return false;
    }

    RenderGraph &RenderGraph::set_enabled(RenderFeature feature, bool enabled_value) noexcept {
        switch (feature) {
            case RenderFeature::Scene:
                description_.scene.enabled = enabled_value;
                break;
            case RenderFeature::Shadows:
                description_.shadows.enabled = enabled_value;
                break;
            case RenderFeature::AmbientOcclusion:
                description_.ambient_occlusion.enabled = enabled_value;
                break;
            case RenderFeature::AntiAliasing:
                description_.anti_aliasing.post_process =
                    enabled_value ? PostProcessAntiAliasing::Fxaa : PostProcessAntiAliasing::None;
                if (!enabled_value) {
                    description_.anti_aliasing.msaa_samples = 1;
                }
                break;
            case RenderFeature::Bloom:
                description_.bloom.enabled = enabled_value;
                break;
            case RenderFeature::ToneMapping:
                description_.tone_mapping.enabled = enabled_value;
                break;
            case RenderFeature::DebugOverlay:
                description_.debug_overlay.enabled = enabled_value;
                break;
        }
        return *this;
    }

    RenderGraph &RenderGraph::enable(RenderFeature feature) noexcept { return set_enabled(feature, true); }
    RenderGraph &RenderGraph::disable(RenderFeature feature) noexcept { return set_enabled(feature, false); }

    RenderGraph &RenderGraph::set_execution_mode(RenderGraphExecutionMode mode) noexcept {
        description_.execution_mode = mode;
        return *this;
    }

    RenderGraph &RenderGraph::set_resolution_scale(f32 scale) noexcept {
        description_.resolution_scale = scale;
        return *this;
    }

    RenderGraph &RenderGraph::set_background_color(glm::vec4 color) noexcept {
        description_.scene.background_color = color;
        return *this;
    }

    RenderGraph &RenderGraph::inherit_camera_background() noexcept {
        description_.scene.background_color.reset();
        return *this;
    }

    RenderGraph &RenderGraph::set_tone_mapping(ToneMappingOperator operation,
                                               f32 exposure,
                                               f32 white_point,
                                               f32 saturation) noexcept {
        description_.tone_mapping.enabled = operation != ToneMappingOperator::None;
        description_.tone_mapping.operation = operation;
        description_.tone_mapping.exposure = exposure;
        description_.tone_mapping.white_point = white_point;
        description_.tone_mapping.saturation = saturation;
        return *this;
    }

    RenderGraphResult RenderGraph::validate_topology() const noexcept {
        if (passes_.empty() || textures_.empty()) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidPassGraph,
                .message = UString{"Render graph must contain at least one module and one texture."_ustr},
            });
        }

        const auto valid_texture = [this](RenderGraphTextureHandle handle) noexcept {
            return handle && handle.generation == generation_ && handle.index < textures_.size();
        };
        std::vector<i32> producer(textures_.size(), -1);
        usize scene_count = 0;
        usize present_count = 0;
        RenderGraphTextureHandle present_input{};

        // Structural validation is graph-wide: every edge must reference a texture produced by an
        // earlier node, every output has one producer, and every pass/texture handle belongs to this
        // graph generation. Branches are legal and do not need to contribute to presentation.
        for (usize index = 0; index < passes_.size(); ++index) {
            const RenderGraphPassDescription &pass = passes_[index];
            if (!pass.handle || pass.handle.generation != generation_ || pass.handle.index != index) {
                return std::unexpected(RenderGraphError{
                    .code = RenderGraphErrorCode::InvalidPassGraph,
                    .message = UString{"Render graph contains a stale or foreign pass handle."_ustr},
                });
            }

            if (pass.kind == RenderGraphPassKind::DeferredScene) {
                if (++scene_count != 1 || index != 0 || pass.input || !valid_texture(pass.output) ||
                    producer[pass.output.index] >= 0) {
                    return std::unexpected(RenderGraphError{
                        .code = RenderGraphErrorCode::UnsupportedPassOrder,
                        .message = UString{"The deferred-scene module must be the graph's first and only scene producer."_ustr},
                    });
                }
                producer[pass.output.index] = static_cast<i32>(index);
                continue;
            }

            if (pass.kind == RenderGraphPassKind::Present) {
                if (++present_count != 1 || !valid_texture(pass.input) || pass.output ||
                    producer[pass.input.index] < 0) {
                    return std::unexpected(RenderGraphError{
                        .code = RenderGraphErrorCode::InvalidPassGraph,
                        .message = UString{"Present must consume a texture produced earlier by this graph and may not produce another texture."_ustr},
                    });
                }
                present_input = pass.input;
                continue;
            }

            if (!valid_texture(pass.input) || producer[pass.input.index] < 0 ||
                !valid_texture(pass.output) || producer[pass.output.index] >= 0) {
                return std::unexpected(RenderGraphError{
                    .code = RenderGraphErrorCode::InvalidPassGraph,
                    .message = UString{"Each module input must have an earlier producer and each output must have exactly one producer."_ustr},
                });
            }

            const RenderGraphTextureDescription &output = textures_[pass.output.index];
            if (output.mip_levels == 0 || !std::isfinite(output.extent.scale_x) ||
                !std::isfinite(output.extent.scale_y) || output.extent.scale_x <= 0.0f ||
                output.extent.scale_y <= 0.0f ||
                (output.extent.mode == RenderGraphExtentMode::RelativeToInput &&
                 output.extent.input != pass.input) ||
                (output.extent.mode == RenderGraphExtentMode::Absolute &&
                 (output.extent.width == 0 || output.extent.height == 0))) {
                return std::unexpected(RenderGraphError{
                    .code = RenderGraphErrorCode::InvalidPassGraph,
                    .message = UString{"Render graph texture extent or mip declaration is invalid."_ustr},
                });
            }
            if (pass.kind == RenderGraphPassKind::FullscreenEffect &&
                (pass.fullscreen_effect.shader_path.empty() ||
                 pass.fullscreen_effect.module_name.empty() ||
                 pass.fullscreen_effect.fragment_entry_point.empty())) {
                return std::unexpected(RenderGraphError{
                    .code = RenderGraphErrorCode::InvalidFullscreenEffect,
                    .message = UString{"Fullscreen effects require a shader path, module name, and fragment entry point."_ustr},
                });
            }
            producer[pass.output.index] = static_cast<i32>(index);
        }

        if (scene_count != 1 || present_count != 1 || !valid_texture(presented_texture_) ||
            presented_texture_ != present_input) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidPassGraph,
                .message = UString{"The graph requires exactly one scene producer and one Present node with a matching output."_ustr},
            });
        }

        // Renderer lowering currently consumes one presentation ancestry. Validate only that live path's
        // built-in ordering; unrelated branches remain valid declarative data and are not lowered.
        const std::vector<RenderGraphPassHandle> path = presentation_path();
        if (path.empty()) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidPassGraph,
                .message = UString{"Present does not have a complete producer path back to the scene module."_ustr},
            });
        }

        bool saw_scene = false;
        bool saw_anti_aliasing = false;
        bool saw_bloom = false;
        bool saw_tone_mapping = false;
        bool saw_debug_overlay = false;
        bool saw_present = false;
        for (usize path_index = 0; path_index < path.size(); ++path_index) {
            const RenderGraphPassDescription &pass = passes_[path[path_index].index];
            switch (pass.kind) {
                case RenderGraphPassKind::DeferredScene:
                    if (saw_scene || path_index != 0) {
                        return std::unexpected(RenderGraphError{
                            .code = RenderGraphErrorCode::UnsupportedPassOrder,
                            .message = UString{"The presentation path must begin with the deferred scene module."_ustr},
                        });
                    }
                    saw_scene = true;
                    break;
                case RenderGraphPassKind::AntiAliasing:
                    if (saw_anti_aliasing || saw_bloom || saw_tone_mapping) {
                        return std::unexpected(RenderGraphError{
                            .code = RenderGraphErrorCode::UnsupportedPassOrder,
                            .message = UString{"Anti-aliasing may appear once before bloom and tone mapping on the presentation path."_ustr},
                        });
                    }
                    saw_anti_aliasing = true;
                    break;
                case RenderGraphPassKind::Bloom:
                    if (saw_bloom || saw_tone_mapping) {
                        return std::unexpected(RenderGraphError{
                            .code = RenderGraphErrorCode::UnsupportedPassOrder,
                            .message = UString{"Bloom may appear once before tone mapping on the presentation path."_ustr},
                        });
                    }
                    saw_bloom = true;
                    break;
                case RenderGraphPassKind::FullscreenEffect:
                    if (saw_tone_mapping) {
                        return std::unexpected(RenderGraphError{
                            .code = RenderGraphErrorCode::UnsupportedPassOrder,
                            .message = UString{"Fullscreen HDR effects must run before tone mapping on the presentation path."_ustr},
                        });
                    }
                    break;
                case RenderGraphPassKind::ToneMapping:
                    if (saw_tone_mapping) {
                        return std::unexpected(RenderGraphError{
                            .code = RenderGraphErrorCode::UnsupportedPassOrder,
                            .message = UString{"Tone mapping may appear only once on the presentation path."_ustr},
                        });
                    }
                    saw_tone_mapping = true;
                    break;
                case RenderGraphPassKind::DebugOverlay:
                    if (!saw_tone_mapping || saw_debug_overlay) {
                        return std::unexpected(RenderGraphError{
                            .code = RenderGraphErrorCode::UnsupportedPassOrder,
                            .message = UString{"Debug overlay may appear once after tone mapping on the presentation path."_ustr},
                        });
                    }
                    saw_debug_overlay = true;
                    break;
                case RenderGraphPassKind::Present:
                    if (saw_present || path_index + 1 != path.size()) {
                        return std::unexpected(RenderGraphError{
                            .code = RenderGraphErrorCode::UnsupportedPassOrder,
                            .message = UString{"Present must terminate the presentation path."_ustr},
                        });
                    }
                    saw_present = true;
                    break;
            }
        }

        if (!saw_scene || !saw_tone_mapping || !saw_present) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidPassGraph,
                .message = UString{"The current renderer lowering requires scene, tone-mapping, and present modules on the presentation path."_ustr},
            });
        }
        return {};
    }

    RenderGraphResult RenderGraph::validate() const noexcept {
        if (!std::isfinite(description_.resolution_scale) ||
            description_.resolution_scale < 0.1f || description_.resolution_scale > 2.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidResolutionScale,
                .message = UString{"Render graph resolution_scale must be finite and in [0.1, 2.0]."_ustr},
            });
        }
        if (description_.scene.background_color && !finite(*description_.scene.background_color)) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidBackgroundColor,
                .message = UString{"Render graph background color must contain only finite values."_ustr},
            });
        }
        if (!std::isfinite(description_.scene.background_intensity) ||
            description_.scene.background_intensity < 0.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidBackgroundColor,
                .message = UString{"Render graph background intensity must be finite and non-negative."_ustr},
            });
        }
        const ShadowSettings &shadows = description_.shadows;
        if (shadows.atlas_size < 512 || shadows.atlas_size > 16384 || shadows.atlas_size % 8 != 0 ||
            shadows.cascade_count < 1 || shadows.cascade_count > 4 ||
            !std::isfinite(shadows.max_distance) || shadows.max_distance <= 0.0f ||
            !std::isfinite(shadows.cascade_split_lambda) || shadows.cascade_split_lambda < 0.0f || shadows.cascade_split_lambda > 1.0f ||
            !std::isfinite(shadows.cascade_blend) || shadows.cascade_blend < 0.0f || shadows.cascade_blend > 0.5f ||
            !std::isfinite(shadows.depth_bias) || shadows.depth_bias < 0.0f ||
            !std::isfinite(shadows.slope_bias) || shadows.slope_bias < 0.0f ||
            !std::isfinite(shadows.normal_bias) || shadows.normal_bias < 0.0f || shadows.normal_bias > 4.0f ||
            shadows.max_shadowed_spot_lights > 8 || shadows.max_shadowed_point_lights > 4) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidShadowSettings,
                .message = UString{"Shadow settings are outside their supported atlas, cascade, bias, or local-light ranges."_ustr},
            });
        }
        const AmbientOcclusionSettings &ao = description_.ambient_occlusion;
        if (!std::isfinite(ao.radius) || ao.radius <= 0.0f ||
            !std::isfinite(ao.falloff) || ao.falloff < 0.0f || ao.falloff >= 1.0f ||
            !std::isfinite(ao.thickness) || ao.thickness < 0.0f ||
            !std::isfinite(ao.intensity) || ao.intensity < 0.0f || ao.intensity > 4.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidAmbientOcclusionSettings,
                .message = UString{"GTAO requires a positive radius, falloff in [0, 1), and finite non-negative thickness/intensity."_ustr},
            });
        }
        const AntiAliasingSettings &aa = description_.anti_aliasing;
        if ((aa.msaa_samples != 1 && aa.msaa_samples != 2 &&
             aa.msaa_samples != 4 && aa.msaa_samples != 8) ||
            !std::isfinite(aa.subpixel_quality) || aa.subpixel_quality < 0.0f || aa.subpixel_quality > 1.0f ||
            !std::isfinite(aa.edge_threshold) || aa.edge_threshold < 0.0312f || aa.edge_threshold > 0.5f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidAntiAliasingSettings,
                .message = UString{"Anti-aliasing requires 1/2/4/8 MSAA samples, subpixel quality in [0, 1], and edge threshold in [0.0312, 0.5]."_ustr},
            });
        }
        const BloomSettings &bloom = description_.bloom;
        if (!std::isfinite(bloom.threshold) || bloom.threshold < 0.0f ||
            !std::isfinite(bloom.soft_knee) || bloom.soft_knee < 0.0f || bloom.soft_knee > 1.0f ||
            !std::isfinite(bloom.intensity) || bloom.intensity < 0.0f ||
            !std::isfinite(bloom.scatter) || bloom.scatter < 0.0f || bloom.scatter > 1.0f ||
            bloom.max_levels < 1 || bloom.max_levels > 10) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidBloomSettings,
                .message = UString{"Render graph bloom requires a non-negative threshold/intensity, soft_knee and scatter in [0, 1], and max_levels in [1, 10]."_ustr},
            });
        }
        const ToneMappingSettings &tone = description_.tone_mapping;
        if (!std::isfinite(tone.exposure) || tone.exposure < 0.0f ||
            !std::isfinite(tone.white_point) || tone.white_point <= 0.0f ||
            !std::isfinite(tone.saturation) || tone.saturation < 0.0f || tone.saturation > 4.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidToneMappingSettings,
                .message = UString{"Render graph tonemapping requires finite non-negative exposure, positive white point, and saturation in [0, 4]."_ustr},
            });
        }
        if (!std::isfinite(tone.hdr_paper_white_nits) || tone.hdr_paper_white_nits <= 0.0f ||
            !std::isfinite(tone.hdr_peak_nits) || tone.hdr_peak_nits < tone.hdr_paper_white_nits) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidToneMappingSettings,
                .message = UString{"Render graph HDR output requires a positive paper-white and a peak nits value at or above it."_ustr},
            });
        }
        if (!std::isfinite(tone.hermite_spline.toe_strength) || tone.hermite_spline.toe_strength < 0.0f || tone.hermite_spline.toe_strength > 1.0f ||
            !std::isfinite(tone.hermite_spline.toe_length) || tone.hermite_spline.toe_length < 0.0f || tone.hermite_spline.toe_length > 1.0f ||
            !std::isfinite(tone.hermite_spline.shoulder_strength) || tone.hermite_spline.shoulder_strength < 0.0f ||
            !std::isfinite(tone.hermite_spline.shoulder_length) || tone.hermite_spline.shoulder_length < 0.0f || tone.hermite_spline.shoulder_length > 1.0f ||
            !std::isfinite(tone.hermite_spline.shoulder_angle) || tone.hermite_spline.shoulder_angle < 0.0f || tone.hermite_spline.shoulder_angle > 1.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidToneMappingSettings,
                .message = UString{"Render graph Hermite spline settings must be finite; toe/shoulder length and shoulder angle must lie in [0, 1], and shoulder strength must be non-negative."_ustr},
            });
        }
        if (!std::isfinite(tone.psycho_v.highlights) || tone.psycho_v.highlights <= 0.0f ||
            !std::isfinite(tone.psycho_v.shadows) || tone.psycho_v.shadows <= 0.0f ||
            !std::isfinite(tone.psycho_v.contrast) || tone.psycho_v.contrast <= 0.0f ||
            !std::isfinite(tone.psycho_v.purity_scale) || tone.psycho_v.purity_scale < 0.0f ||
            !std::isfinite(tone.psycho_v.gamut_compression) || tone.psycho_v.gamut_compression < 0.0f || tone.psycho_v.gamut_compression > 1.0f ||
            !std::isfinite(tone.psycho_v.compression) || tone.psycho_v.compression < 0.0f ||
            !finite(glm::vec4{tone.psycho_v.adapted_gray_bt709, 1.0f}) ||
            !finite(glm::vec4{tone.psycho_v.background_gray_bt709, 1.0f}) ||
            tone.psycho_v.adapted_gray_bt709.x <= 0.0f || tone.psycho_v.adapted_gray_bt709.y <= 0.0f ||
            tone.psycho_v.adapted_gray_bt709.z <= 0.0f || tone.psycho_v.background_gray_bt709.x <= 0.0f ||
            tone.psycho_v.background_gray_bt709.y <= 0.0f || tone.psycho_v.background_gray_bt709.z <= 0.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidToneMappingSettings,
                .message = UString{"Render graph PsychoV settings must be finite, with positive highlights/shadows/contrast, non-negative purity/compression, gamut_compression in [0, 1], and positive adapted/background gray points."_ustr},
            });
        }

        return validate_topology();
    }

    RenderGraph RenderGraph::normalized() const noexcept {
        RenderGraph result = *this;
        RenderGraphDescription &desc = result.description_;
        desc.resolution_scale = std::isfinite(desc.resolution_scale)
                                    ? std::clamp(desc.resolution_scale, 0.1f, 2.0f)
                                    : 1.0f;
        if (desc.scene.background_color && !finite(*desc.scene.background_color)) {
            desc.scene.background_color.reset();
        }
        desc.scene.background_intensity = std::isfinite(desc.scene.background_intensity)
                                              ? std::max(desc.scene.background_intensity, 0.0f)
                                              : 1.0f;
        ShadowSettings &shadows = desc.shadows;
        shadows.atlas_size = std::clamp(shadows.atlas_size, 512u, 16384u);
        shadows.atlas_size -= shadows.atlas_size % 8u;
        shadows.cascade_count = std::clamp(shadows.cascade_count, 1u, 4u);
        shadows.max_distance = std::isfinite(shadows.max_distance) && shadows.max_distance > 0.0f
                                   ? shadows.max_distance : 250.0f;
        shadows.cascade_split_lambda = std::isfinite(shadows.cascade_split_lambda)
                                           ? std::clamp(shadows.cascade_split_lambda, 0.0f, 1.0f) : 0.65f;
        shadows.cascade_blend = std::isfinite(shadows.cascade_blend)
                                    ? std::clamp(shadows.cascade_blend, 0.0f, 0.5f) : 0.10f;
        shadows.depth_bias = std::isfinite(shadows.depth_bias) ? std::max(shadows.depth_bias, 0.0f) : 0.75f;
        shadows.slope_bias = std::isfinite(shadows.slope_bias) ? std::max(shadows.slope_bias, 0.0f) : 1.0f;
        shadows.normal_bias = std::isfinite(shadows.normal_bias)
                                  ? std::clamp(shadows.normal_bias, 0.0f, 4.0f)
                                  : 0.75f;
        shadows.max_shadowed_spot_lights = std::min(shadows.max_shadowed_spot_lights, 8u);
        shadows.max_shadowed_point_lights = std::min(shadows.max_shadowed_point_lights, 4u);
        AmbientOcclusionSettings &ao = desc.ambient_occlusion;
        ao.radius = std::isfinite(ao.radius) && ao.radius > 0.0f ? ao.radius : 1.0f;
        ao.falloff = std::isfinite(ao.falloff) ? std::clamp(ao.falloff, 0.0f, 0.999f) : 0.8f;
        ao.thickness = std::isfinite(ao.thickness) ? std::max(ao.thickness, 0.0f) : 0.15f;
        ao.intensity = std::isfinite(ao.intensity) ? std::clamp(ao.intensity, 0.0f, 4.0f) : 1.0f;
        AntiAliasingSettings &aa = desc.anti_aliasing;
        if (aa.msaa_samples != 1 && aa.msaa_samples != 2 &&
            aa.msaa_samples != 4 && aa.msaa_samples != 8) {
            aa.msaa_samples = 1;
        }
        aa.subpixel_quality = std::isfinite(aa.subpixel_quality)
                                  ? std::clamp(aa.subpixel_quality, 0.0f, 1.0f) : 0.75f;
        aa.edge_threshold = std::isfinite(aa.edge_threshold)
                                ? std::clamp(aa.edge_threshold, 0.0312f, 0.5f) : 0.125f;
        desc.bloom.threshold = std::isfinite(desc.bloom.threshold) ? std::max(desc.bloom.threshold, 0.0f) : 1.0f;
        desc.bloom.soft_knee = std::isfinite(desc.bloom.soft_knee) ? std::clamp(desc.bloom.soft_knee, 0.0f, 1.0f) : 0.5f;
        desc.bloom.intensity = std::isfinite(desc.bloom.intensity) ? std::max(desc.bloom.intensity, 0.0f) : 0.08f;
        desc.bloom.scatter = std::isfinite(desc.bloom.scatter) ? std::clamp(desc.bloom.scatter, 0.0f, 1.0f) : 0.7f;
        desc.bloom.max_levels = std::clamp(desc.bloom.max_levels, 1u, 10u);
        desc.tone_mapping.exposure = std::isfinite(desc.tone_mapping.exposure)
                                         ? std::max(desc.tone_mapping.exposure, 0.0f)
                                         : 1.0f;
        desc.tone_mapping.white_point = std::isfinite(desc.tone_mapping.white_point) && desc.tone_mapping.white_point > 0.0f
                                            ? desc.tone_mapping.white_point
                                            : 1.0f;
        desc.tone_mapping.saturation = std::isfinite(desc.tone_mapping.saturation)
                                           ? std::clamp(desc.tone_mapping.saturation, 0.0f, 4.0f)
                                           : 1.0f;
        desc.tone_mapping.hdr_paper_white_nits = std::isfinite(desc.tone_mapping.hdr_paper_white_nits) && desc.tone_mapping.hdr_paper_white_nits > 0.0f
                                                     ? desc.tone_mapping.hdr_paper_white_nits
                                                     : 203.0f;
        desc.tone_mapping.hdr_peak_nits = std::isfinite(desc.tone_mapping.hdr_peak_nits)
                                              ? std::max(desc.tone_mapping.hdr_peak_nits, desc.tone_mapping.hdr_paper_white_nits)
                                              : 1000.0f;
        HermiteSplineSettings &hermite = desc.tone_mapping.hermite_spline;
        hermite.toe_strength = std::isfinite(hermite.toe_strength) ? std::clamp(hermite.toe_strength, 0.0f, 1.0f) : 0.5f;
        hermite.toe_length = std::isfinite(hermite.toe_length) ? std::clamp(hermite.toe_length, 0.0f, 1.0f) : 0.5f;
        hermite.shoulder_strength = std::isfinite(hermite.shoulder_strength) ? std::max(hermite.shoulder_strength, 0.0f) : 2.0f;
        hermite.shoulder_length = std::isfinite(hermite.shoulder_length) ? std::clamp(hermite.shoulder_length, 0.0f, 1.0f) : 0.5f;
        hermite.shoulder_angle = std::isfinite(hermite.shoulder_angle) ? std::clamp(hermite.shoulder_angle, 0.0f, 1.0f) : 1.0f;
        PsychoVSettings &psycho_v = desc.tone_mapping.psycho_v;
        psycho_v.highlights = std::isfinite(psycho_v.highlights) && psycho_v.highlights > 0.0f ? psycho_v.highlights : 1.0f;
        psycho_v.shadows = std::isfinite(psycho_v.shadows) && psycho_v.shadows > 0.0f ? psycho_v.shadows : 1.0f;
        psycho_v.contrast = std::isfinite(psycho_v.contrast) && psycho_v.contrast > 0.0f ? psycho_v.contrast : 1.0f;
        psycho_v.purity_scale = std::isfinite(psycho_v.purity_scale) ? std::max(psycho_v.purity_scale, 0.0f) : 1.0f;
        psycho_v.gamut_compression = std::isfinite(psycho_v.gamut_compression) ? std::clamp(psycho_v.gamut_compression, 0.0f, 1.0f) : 1.0f;
        psycho_v.compression = std::isfinite(psycho_v.compression) ? std::max(psycho_v.compression, 0.0f) : 0.0f;
        if (!finite(glm::vec4{psycho_v.adapted_gray_bt709, 1.0f}) ||
            psycho_v.adapted_gray_bt709.x <= 0.0f || psycho_v.adapted_gray_bt709.y <= 0.0f || psycho_v.adapted_gray_bt709.z <= 0.0f) {
            psycho_v.adapted_gray_bt709 = glm::vec3{0.18f};
        }
        if (!finite(glm::vec4{psycho_v.background_gray_bt709, 1.0f}) ||
            psycho_v.background_gray_bt709.x <= 0.0f || psycho_v.background_gray_bt709.y <= 0.0f || psycho_v.background_gray_bt709.z <= 0.0f) {
            psycho_v.background_gray_bt709 = glm::vec3{0.18f};
        }

        if (!result.validate_topology()) {
            return RenderGraph{desc};
        }
        return result;
    }

} // namespace SFT::Engine
