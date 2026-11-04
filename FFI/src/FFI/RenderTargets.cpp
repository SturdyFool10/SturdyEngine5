/// C ABI for off-screen render targets: create, resize, reconfigure, describe, read back the texture, and render a view of the ECS scene into one.
///
/// Everything forwards to the `Engine::*offscreen_render_target*` family. `sturdy_render_target_render_view` mirrors what the Rust bridge does for
/// `offscreen_render_view`, with the view described by the same `sturdy_frame_*` setters a window frame uses.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <map>
#include <mutex>
#include <string>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    namespace E = SFT::Engine;

    using SFT::Ffi::ScopedHandle;
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    [[nodiscard]] SturdyResult invalid(const char *what) noexcept { return set_error(STURDY_ERROR_INVALID_ARGUMENT, what); }

    template <class Body>
    [[nodiscard]] SturdyResult with_engine(SturdyEngine engine, Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            E::Engine *resolved = nullptr;
            const SturdyResult result = resolve_engine(engine, &resolved);
            if (result != STURDY_OK) {
                return result;
            }
            return body(*resolved);
        });
    }

    // --- enum translation (explicit switches: a value this build does not know is an error, never a cast) ---

    template <class From, class To>
    struct Mapping {
        From foreign;
        To native;
    };

    template <class To, class From, std::size_t N>
    [[nodiscard]] bool to_native(const Mapping<From, To> (&table)[N], From value, To *out) noexcept {
        for (const auto &entry : table) {
            if (entry.foreign == value) {
                *out = entry.native;
                return true;
            }
        }
        return false;
    }

    template <class From, class To, std::size_t N>
    [[nodiscard]] bool to_foreign(const Mapping<From, To> (&table)[N], To value, From *out) noexcept {
        for (const auto &entry : table) {
            if (entry.native == value) {
                *out = entry.foreign;
                return true;
            }
        }
        return false;
    }

    constexpr Mapping<SturdyRenderTargetFormat, E::RenderTargetFormat> format_table[] = {
        {STURDY_RENDER_TARGET_FORMAT_AUTO, E::RenderTargetFormat::Auto},
        {STURDY_RENDER_TARGET_FORMAT_BGRA8_SRGB, E::RenderTargetFormat::Bgra8Srgb},
        {STURDY_RENDER_TARGET_FORMAT_RGBA8_SRGB, E::RenderTargetFormat::Rgba8Srgb},
        {STURDY_RENDER_TARGET_FORMAT_RGBA16_FLOAT, E::RenderTargetFormat::Rgba16Float},
        {STURDY_RENDER_TARGET_FORMAT_RGB10A2, E::RenderTargetFormat::Rgb10A2},
    };
    constexpr Mapping<SturdyRenderTargetUse, E::RenderTargetUse> use_table[] = {
        {STURDY_RENDER_TARGET_USE_SCREEN, E::RenderTargetUse::Screen},
        {STURDY_RENDER_TARGET_USE_WORLD, E::RenderTargetUse::World},
        {STURDY_RENDER_TARGET_USE_COMPUTE, E::RenderTargetUse::Compute},
    };
    constexpr Mapping<SturdyRenderTargetFeature, E::RenderTargetFeature> feature_table[] = {
        {STURDY_RENDER_TARGET_FEATURE_AUTO, E::RenderTargetFeature::Auto},
        {STURDY_RENDER_TARGET_FEATURE_OFF, E::RenderTargetFeature::Off},
        {STURDY_RENDER_TARGET_FEATURE_IF_SUPPORTED, E::RenderTargetFeature::IfSupported},
        {STURDY_RENDER_TARGET_FEATURE_REQUIRED, E::RenderTargetFeature::Required},
    };
    constexpr Mapping<SturdyRenderTargetFilter, E::RenderTargetFilter> filter_table[] = {
        {STURDY_RENDER_TARGET_FILTER_LINEAR, E::RenderTargetFilter::Linear},
        {STURDY_RENDER_TARGET_FILTER_NEAREST, E::RenderTargetFilter::Nearest},
    };
    constexpr Mapping<SturdyRenderTargetAddressMode, E::RenderTargetAddressMode> address_table[] = {
        {STURDY_RENDER_TARGET_ADDRESS_CLAMP_TO_EDGE, E::RenderTargetAddressMode::ClampToEdge},
        {STURDY_RENDER_TARGET_ADDRESS_REPEAT, E::RenderTargetAddressMode::Repeat},
        {STURDY_RENDER_TARGET_ADDRESS_MIRRORED_REPEAT, E::RenderTargetAddressMode::MirroredRepeat},
    };

    [[nodiscard]] SturdyResult translate(const SturdyRenderTargetDesc &desc, E::OffscreenRenderTargetDescription *out) {
        if (desc.struct_size != sizeof(SturdyRenderTargetDesc)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyRenderTargetDesc::struct_size does not match this build");
        }
        E::OffscreenRenderTargetDescription native;
        native.width = desc.width;
        native.height = desc.height;
        if (desc.label != nullptr) {
            native.label = SFT::UString::from_c_str(desc.label);
        }
        if (!to_native(format_table, desc.format, &native.format) || !to_native(use_table, desc.use, &native.use) ||
            !to_native(feature_table, desc.storage, &native.storage) || !to_native(filter_table, desc.filter, &native.sampling.filter) ||
            !to_native(address_table, desc.address, &native.sampling.address)) {
            return invalid("unrecognized render target enum value");
        }
        if (std::isnan(desc.max_anisotropy)) {
            return invalid("max_anisotropy must not be NaN");
        }
        native.sampling.max_anisotropy = desc.max_anisotropy;
        native.mip_levels = desc.mip_levels;
        native.msaa_samples = desc.msaa_samples;
        native.strict = desc.strict != 0;
        *out = std::move(native);
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult translate(const E::OffscreenRenderTargetDescription &native, SturdyRenderTargetDesc *out) noexcept {
        SturdyRenderTargetDesc desc{};
        desc.struct_size = sizeof(SturdyRenderTargetDesc);
        desc.width = native.width;
        desc.height = native.height;
        desc.label = nullptr;
        if (!to_foreign(format_table, native.format, &desc.format) || !to_foreign(use_table, native.use, &desc.use) ||
            !to_foreign(feature_table, native.storage, &desc.storage) || !to_foreign(filter_table, native.sampling.filter, &desc.filter) ||
            !to_foreign(address_table, native.sampling.address, &desc.address)) {
            return set_error(STURDY_ERROR_INTERNAL, "the render target uses a value the ABI cannot spell");
        }
        desc.max_anisotropy = native.sampling.max_anisotropy;
        desc.mip_levels = native.mip_levels;
        desc.msaa_samples = native.msaa_samples;
        desc.strict = native.strict ? STURDY_TRUE : STURDY_FALSE;
        *out = desc;
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult failure(const SFT::Core::GraphicsBackendError &error) {
        return set_error(STURDY_ERROR_INITIALIZATION_FAILED, std::string{error.message});
    }

    // Per-target camera, kept across views so motion vectors and blur see the previous view-projection.
    std::mutex g_history_mutex;
    std::map<uint64_t, E::Camera> g_camera_history;
    std::map<uint64_t, uint64_t> g_frame_index;

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_render_target_desc_init(SturdyRenderTargetDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return invalid("desc must not be null");
        }
        const E::OffscreenRenderTargetDescription defaults;
        return translate(defaults, desc);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_create(SturdyEngine engine, const SturdyRenderTargetDesc *desc, SturdyRenderTarget *out_target) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (desc == nullptr || out_target == nullptr) {
            return invalid("desc and output pointer must not be null");
        }
        E::OffscreenRenderTargetDescription native;
        if (const SturdyResult translated = translate(*desc, &native); translated != STURDY_OK) {
            return translated;
        }
        auto created = e.create_offscreen_render_target(native);
        if (!created) {
            return failure(created.error());
        }
        out_target->value = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_destroy(SturdyEngine engine, SturdyRenderTarget target) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        e.destroy_offscreen_render_target(E::RenderTargetHandle{target.value});
        const std::lock_guard lock{g_history_mutex};
        g_camera_history.erase(target.value);
        g_frame_index.erase(target.value);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_resize(SturdyEngine engine, SturdyRenderTarget target, uint32_t width, uint32_t height) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (width == 0 || height == 0) {
            return invalid("a render target needs a non-zero size");
        }
        const auto result = e.resize_offscreen_render_target(E::RenderTargetHandle{target.value}, width, height);
        return result ? STURDY_OK : failure(result.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_reconfigure(SturdyEngine engine, SturdyRenderTarget target, const SturdyRenderTargetDesc *desc) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (desc == nullptr) {
            return invalid("desc must not be null");
        }
        E::OffscreenRenderTargetDescription native;
        if (const SturdyResult translated = translate(*desc, &native); translated != STURDY_OK) {
            return translated;
        }
        const auto result = e.reconfigure_offscreen_render_target(E::RenderTargetHandle{target.value}, native);
        return result ? STURDY_OK : failure(result.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_description(SturdyEngine engine, SturdyRenderTarget target, SturdyRenderTargetDesc *out_desc) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_desc == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto native = e.offscreen_render_target_description(E::RenderTargetHandle{target.value});
        if (!native) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "unknown or destroyed render target");
        }
        return translate(*native, out_desc);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_label(SturdyEngine engine, SturdyRenderTarget target, char *buffer, size_t capacity, size_t *out_length) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        const auto native = e.offscreen_render_target_description(E::RenderTargetHandle{target.value});
        if (!native) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "unknown or destroyed render target");
        }
        return copy_string_out(native->label.cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_config(SturdyEngine engine, SturdyRenderTarget target, SturdyRenderTargetConfig *out_config) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_config == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto native = e.offscreen_render_target_config(E::RenderTargetHandle{target.value});
        if (!native) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "unknown or destroyed render target");
        }
        SturdyRenderTargetConfig config{};
        config.width = native->width;
        config.height = native->height;
        config.mip_levels = native->mip_levels;
        config.max_anisotropy = native->max_anisotropy;
        config.storage = native->storage ? STURDY_TRUE : STURDY_FALSE;
        config.msaa_samples = native->msaa_samples;
        config.adjustment_count = static_cast<uint32_t>(native->adjustments.size());
        if (!to_foreign(format_table, native->format, &config.format) || !to_foreign(filter_table, native->filter, &config.filter) ||
            !to_foreign(address_table, native->address, &config.address)) {
            return set_error(STURDY_ERROR_INTERNAL, "the render target uses a value the ABI cannot spell");
        }
        *out_config = config;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_adjustment(SturdyEngine engine, SturdyRenderTarget target, uint32_t index, char *buffer, size_t capacity,
                                                             size_t *out_length) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        const auto native = e.offscreen_render_target_config(E::RenderTargetHandle{target.value});
        if (!native) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "unknown or destroyed render target");
        }
        if (index >= native->adjustments.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "adjustment index is out of range");
        }
        return copy_string_out(native->adjustments[index], buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_texture(SturdyEngine engine, SturdyRenderTarget target, uint64_t *out_texture) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_texture == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_texture = e.offscreen_render_target_texture(E::RenderTargetHandle{target.value}).value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_target_render_view(SturdyEngine engine, SturdyRenderTarget target, SturdyViewCallback callback, void *user) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (callback == nullptr) {
            return invalid("callback must not be null");
        }
        const E::RenderTargetHandle handle{target.value};
        const auto description = e.offscreen_render_target_description(handle);
        if (!description) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "unknown or destroyed render target");
        }
        SFT::WindowManager::Window *primary = e.primary_window();
        if (primary == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "an offscreen view needs the primary window to exist");
        }

        // The camera starts from the last committed one so its previous view-projection survives; the callback edits it through the frame setters.
        E::RenderFrameParameters parameters{};
        parameters.render_graph = E::RenderGraph::standard();
        uint64_t frame_index = 0;
        {
            const std::lock_guard lock{g_history_mutex};
            const auto found = g_camera_history.find(target.value);
            if (found != g_camera_history.end()) {
                parameters.camera = found->second;
            }
            frame_index = g_frame_index[target.value];
        }
        parameters.camera.set_viewport_size(description->width, description->height);

        SturdyFrameInput input{};
        input.struct_size = static_cast<uint32_t>(sizeof(SturdyFrameInput));
        input.delta_seconds = 1.0 / 60.0;
        input.frame_index = frame_index;
        input.framebuffer_width = description->width;
        input.framebuffer_height = description->height;
        input.live_resize = STURDY_FALSE;

        SturdyBool render = STURDY_FALSE;
        {
            const ScopedHandle frame_handle{HandleKind::Frame, &parameters};
            render = callback(engine, &input, SturdyFrame{frame_handle.token()}, user);
        }
        if (render == STURDY_FALSE) {
            return STURDY_OK;
        }

        {
            const std::lock_guard lock{g_history_mutex};
            E::Camera &stored = g_camera_history[target.value];
            stored = parameters.camera;
            stored.commit_frame();
            ++g_frame_index[target.value];
        }
        parameters.debug_label = SFT::UString::from_c_str("offscreen view");
        if (!parameters.render_graph.set_present_target(handle)) {
            return invalid("the render graph has no single Present pass to aim at the offscreen target");
        }
        const SFT::Core::FrameInput frame{
            .delta_seconds = input.delta_seconds,
            .frame_index = frame_index,
            .framebuffer_width = description->width,
            .framebuffer_height = description->height,
        };
        const E::PreparedRenderFrame prepared = e.prepare_render_frame(SFT::Core::RenderSurfaceHandle{primary->id()}, frame, parameters);
        const SFT::Core::RendererResult result = e.render(prepared);
        e.record_frame_result(result);
        if (!result) {
            return failure(result.error());
        }
        return STURDY_OK;
    });
}

} // extern "C"
