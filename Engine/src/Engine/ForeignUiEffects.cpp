#include <Engine/ForeignUiEffects.hpp>

#include <unordered_map>

#include <Async/Mutex.hpp>

namespace SFT::Engine {

    namespace {

        struct EffectRegistry {
            std::unordered_map<ForeignUiEffectId, std::shared_ptr<UI::UiEffect>> effects;
            ForeignUiEffectId next_id = 1;
        };

        Async::Mutex<EffectRegistry> &registry() {
            static Async::Mutex<EffectRegistry> instance;
            return instance;
        }

        [[nodiscard]] std::unexpected<ForeignUiError> invalid(std::string message) {
            return std::unexpected(ForeignUiError{.kind = ForeignUiErrorKind::InvalidArgument, .message = std::move(message)});
        }

        [[nodiscard]] std::optional<UI::ColorSpace> to_space(u32 value) {
            if (value > static_cast<u32>(UI::ColorSpace::Oklch)) {
                return std::nullopt;
            }
            return static_cast<UI::ColorSpace>(value);
        }

        [[nodiscard]] std::optional<UI::ColorInterpolation> to_interpolation(u32 space, u32 hue) {
            const auto resolved = to_space(space);
            if (!resolved || hue > static_cast<u32>(UI::HueInterpolation::Decreasing)) {
                return std::nullopt;
            }
            return UI::ColorInterpolation{.space = *resolved, .hue = static_cast<UI::HueInterpolation>(hue)};
        }

        [[nodiscard]] glm::vec4 vec4_of(const f32 (&values)[4]) { return glm::vec4{values[0], values[1], values[2], values[3]}; }

        [[nodiscard]] ForeignUiResult<void> apply_options(UI::UiEffect &effect, const ForeignUiEffectOptions &options) {
            if (options.composite > static_cast<u32>(UI::UiEffectBlend::Replace)) {
                return invalid("unknown composite mode");
            }
            if (options.layer > 255) {
                return invalid("effect layer must be 0..255");
            }
            effect.composite = static_cast<UI::UiEffectBlend>(options.composite);
            effect.opacity = options.opacity;
            effect.outset = UI::UiEffectOutset{options.outset[0], options.outset[1], options.outset[2], options.outset[3]};
            effect.clip_to_bounds = options.clip_to_bounds;
            effect.layer = static_cast<u8>(options.layer);
            effect.content.mip_levels = options.content_mip_levels;
            return {};
        }

        /// A library effect keeps its own outset unless the caller asked for one.
        [[nodiscard]] bool outset_is_default(const ForeignUiEffectOptions &options) {
            return options.outset[0] == 0.0f && options.outset[1] == 0.0f && options.outset[2] == 0.0f && options.outset[3] == 0.0f;
        }

        [[nodiscard]] ForeignUiEffectId store(std::shared_ptr<UI::UiEffect> effect) {
            auto guard = registry().lock();
            const ForeignUiEffectId id = guard->next_id++;
            guard->effects.emplace(id, std::move(effect));
            return id;
        }

        [[nodiscard]] std::shared_ptr<UI::UiEffect> find_mutable(ForeignUiEffectId id) {
            auto guard = registry().lock();
            const auto found = guard->effects.find(id);
            return found == guard->effects.end() ? nullptr : found->second;
        }

    } // namespace

    ForeignUiResult<ForeignUiEffectId> create_foreign_ui_effect(const ForeignUiEffectSpec &spec, const ForeignUiEffectOptions &options) {
        const auto space = to_space(spec.color_space);
        if (!space) {
            return invalid("unknown color space");
        }
        const glm::vec4 color = vec4_of(spec.color);
        const f32(&v)[16] = spec.values;
        std::shared_ptr<UI::UiEffect> effect;
        using Kind = ForeignUiEffectKind;
        switch (spec.kind) {
            case Kind::ColorAdjust:
                effect = UI::Effects::color_adjust(UI::Effects::ColorAdjust{.brightness = v[0], .contrast = v[1], .saturation = v[2], .grayscale = v[3],
                                                                             .sepia = v[4], .invert = v[5], .hue_radians = v[6], .opacity = v[7]});
                break;
            case Kind::Tint: effect = UI::Effects::tint(color, *space); break;
            case Kind::Blur: effect = UI::Effects::blur(v[0]); break;
            case Kind::FastBlur: effect = UI::Effects::fast_blur(v[0]); break;
            case Kind::DropShadow: effect = UI::Effects::drop_shadow({v[0], v[1]}, v[2], color, v[3], *space); break;
            case Kind::Glow: effect = UI::Effects::glow(color, v[0], v[1], *space); break;
            case Kind::Outline: effect = UI::Effects::outline(v[0], color, v[1], *space); break;
            case Kind::InsetShadow: effect = UI::Effects::inset_shadow({v[0], v[1]}, v[2], color, v[3], *space); break;
            case Kind::FrostedGlass: effect = UI::Effects::frosted_glass(v[0], glm::vec3{color}, v[1], v[2]); break;
            case Kind::Layer: effect = UI::Effects::layer(1); break;
            case Kind::Transform:
                effect = UI::Effects::transform({v[0], v[1]}, v[2], {v[3], v[4]}, {v[5], v[6]});
                break;
            case Kind::ClipInset: effect = UI::Effects::clip_inset(v[0], v[1], v[2], v[3], v[4]); break;
            case Kind::ClipCircle: effect = UI::Effects::clip_circle(v[0], {v[1], v[2]}); break;
            case Kind::ClipEllipse: effect = UI::Effects::clip_ellipse({v[0], v[1]}, {v[2], v[3]}); break;
            case Kind::ClipPolygon: {
                const usize points = std::min<usize>(spec.value_count / 2, 6);
                if (points < 3) {
                    return invalid("a clip polygon needs at least three points");
                }
                std::vector<glm::vec2> list;
                for (usize i = 0; i < points; ++i) {
                    list.emplace_back(v[i * 2], v[i * 2 + 1]);
                }
                effect = UI::Effects::clip_polygon(list);
                break;
            }
            case Kind::MaskLinear: effect = UI::Effects::mask_linear(v[0], v[1], v[2], v[3], v[4], v[5] > 0.5f); break;
            case Kind::MaskImage: {
                if (spec.texture == 0) {
                    return invalid("an image mask needs a texture");
                }
                Renderer::TextureHandle texture;
                texture.value = spec.texture;
                effect = UI::Effects::mask_image(texture, v[0] > 0.5f, v[1] > 0.5f);
                break;
            }
            case Kind::LinearGradient:
            case Kind::RadialGradient:
            case Kind::ConicGradient: {
                if (spec.stops.empty()) {
                    return invalid("a gradient needs at least one stop");
                }
                const auto interpolation = to_interpolation(spec.interpolation_space, spec.interpolation_hue);
                if (!interpolation) {
                    return invalid("unknown gradient interpolation space or hue method");
                }
                std::vector<UI::Effects::GradientStop> stops;
                for (const ForeignUiGradientStop &stop : spec.stops) {
                    const auto stop_space = to_space(stop.space);
                    if (!stop_space) {
                        return invalid("unknown gradient stop color space");
                    }
                    std::optional<UI::ColorInterpolation> toward;
                    if (stop.has_toward_next) {
                        toward = to_interpolation(stop.toward_space, stop.toward_hue_method);
                        if (!toward) {
                            return invalid("unknown stop-to-stop interpolation space or hue method");
                        }
                    }
                    stops.emplace_back(vec4_of(stop.color), stop.position, *stop_space, toward);
                }
                if (spec.kind == Kind::LinearGradient) {
                    effect = UI::Effects::linear_gradient(v[0], stops, *interpolation);
                } else if (spec.kind == Kind::RadialGradient) {
                    effect = UI::Effects::radial_gradient({v[0], v[1]}, v[2], stops, *interpolation);
                } else {
                    effect = UI::Effects::conic_gradient({v[0], v[1]}, v[2], stops, *interpolation);
                }
                break;
            }
            default: return invalid("unknown effect kind");
        }
        const UI::UiEffectOutset library_outset = effect->outset;
        if (auto applied = apply_options(*effect, options); !applied) {
            return std::unexpected(applied.error());
        }
        if (outset_is_default(options)) {
            effect->outset = library_outset;
            if (spec.kind == Kind::Transform) {
                effect->clip_to_bounds = false; // no room was named, so the transformed element may land anywhere on the surface
            }
        }
        if (const auto problem = UI::validate_effect(*effect)) {
            return invalid(problem->cpp_string());
        }
        return store(std::move(effect));
    }

    ForeignUiResult<ForeignUiEffectId> create_custom_foreign_ui_effect(std::string_view name, const ForeignUiEffectOptions &options) {
        auto effect = std::make_shared<UI::UiEffect>();
        effect->name = UString{name};
        if (auto applied = apply_options(*effect, options); !applied) {
            return std::unexpected(applied.error());
        }
        return store(std::move(effect));
    }

    ForeignUiResult<void> add_foreign_ui_effect_pass(ForeignUiEffectId id, const ForeignUiEffectPass &pass) {
        auto effect = find_mutable(id);
        if (!effect) {
            return invalid("unknown effect id");
        }
        if (effect->passes.size() >= UI::kUiEffectMaxPasses) {
            return invalid("an effect holds at most " + std::to_string(UI::kUiEffectMaxPasses) + " passes");
        }
        UI::UiEffectPass out;
        out.label = UString{pass.label};
        out.shader_path = UString{pass.shader_path};
        out.module_name = UString{pass.module_name};
        out.fragment_entry_point = UString{pass.fragment_entry_point};
        for (const ForeignUiEffectInput &input : pass.inputs) {
            if (input.kind > static_cast<u32>(UI::UiEffectInputKind::Backdrop)) {
                return invalid("unknown effect input kind");
            }
            UI::UiEffectInput in;
            // The C ABI numbers inputs group content, pass output, texture, backdrop; the enum is group content, pass output, texture, backdrop.
            in.kind = static_cast<UI::UiEffectInputKind>(input.kind);
            in.pass = input.pass;
            in.texture.value = input.texture;
            in.base_mip = input.base_mip;
            in.mip_count = input.mip_count;
            out.inputs.push_back(in);
        }
        if (pass.address > static_cast<u32>(UI::UiEffectAddress::Border)) {
            return invalid("unknown address mode");
        }
        const auto filter = [](bool linear) { return linear ? UI::UiEffectFilter::Linear : UI::UiEffectFilter::Nearest; };
        out.sampler.min_filter = out.sampler.mag_filter = filter(pass.linear_filter);
        out.sampler.mip_filter = filter(pass.linear_mips);
        out.sampler.address_u = out.sampler.address_v = static_cast<UI::UiEffectAddress>(pass.address);
        out.sampler.lod_bias = pass.lod_bias;
        out.sampler.min_lod = pass.min_lod;
        out.sampler.max_lod = pass.max_lod;
        out.sampler.max_anisotropy = pass.max_anisotropy;
        if (pass.output_format != 0) {
            out.output.format = static_cast<RHI::Format>(pass.output_format);
        }
        out.output.scale = pass.output_scale;
        out.output.mip_levels = pass.output_mip_levels;
        out.output.mip_filter = pass.output_mip_filter == 0 ? UI::UiMipFilter::Box : UI::UiMipFilter::Tent;
        out.parameters = pass.parameters;
        // Validate with the pass in place so the message names it; roll back on a problem so a bad pass is not left behind.
        effect->passes.push_back(std::move(out));
        if (const auto problem = UI::validate_effect(*effect)) {
            effect->passes.pop_back();
            return invalid(problem->cpp_string());
        }
        return {};
    }

    ForeignUiResult<void> set_foreign_ui_effect_options(ForeignUiEffectId id, const ForeignUiEffectOptions &options) {
        auto effect = find_mutable(id);
        if (!effect) {
            return invalid("unknown effect id");
        }
        return apply_options(*effect, options);
    }

    ForeignUiResult<void> release_foreign_ui_effect(ForeignUiEffectId id) {
        auto guard = registry().lock();
        if (guard->effects.erase(id) == 0) {
            return invalid("unknown effect id");
        }
        return {};
    }

    std::shared_ptr<const UI::UiEffect> find_foreign_ui_effect(ForeignUiEffectId id) { return find_mutable(id); }

} // namespace SFT::Engine
