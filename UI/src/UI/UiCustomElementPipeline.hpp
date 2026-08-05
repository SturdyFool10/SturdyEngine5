#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include "CustomElement.hpp"

using std::span;
using std::string;
using std::vector;

namespace SFT::UI {

    // GPU-side cache + draw path for Context::custom_element() (Clay's CUSTOM render command,
    // plans/clay-ui-renderer.md's Phase 3) — one compiled pipeline per distinct
    // (shader_path, module_name, fragment_entry_point, color_format), built the first time
    // UiRenderer::prepare() sees it and reused every frame after (same cache shape as
    // Renderer::Renderer::ensure_custom_post_process()). Unlike UiQuadPipeline, there is no
    // batching: an arbitrary user fragment shader can't share the generic rect/image pipeline's
    // fixed fragment stage, and custom elements are expected to be rare (a handful of animated
    // panels), not per-widget, so a draw call each is an acceptable cost.
    class UiCustomElementPipeline {
      public:
        UiCustomElementPipeline() noexcept = default;

        // Compiles/caches every distinct shader referenced by `draws` (a no-op for ones already
        // cached this pipeline's lifetime). Fails on the first compile/reflection/authoring error it
        // hits — a bad shader_path or a shader that doesn't follow CustomShaderRef's documented
        // contract (UiElementConstants-prefixed single push-constant struct, no resource bindings)
        // is an app bug, not something to draw around silently.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, RHI::Format color_format,
                                                    span<const CustomDraw> draws, bool enable_shader_disk_cache = true);

        // Issues one draw call per entry of `draws`, in order — see class doc comment for why this
        // doesn't batch. Every shader referenced must have already been prepared this frame via
        // prepare() above (same `color_format`, which is what the shader cache is keyed by).
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, RHI::Format color_format,
                                                 span<const CustomDraw> draws, glm::vec2 viewport_size);

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct CachedShader {
            string shader_path;
            string module_name;
            string fragment_entry_point;
            RHI::Format color_format{};
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle fragment_module{};
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::RenderPipelineHandle pipeline{};
            // The shader's own reflected push-constant range — draw() validates
            // UiElementConstants + CustomShaderRef::push_constants adds up to exactly this before
            // uploading, rather than overrunning into whatever follows.
            u32 push_constant_size = 0;
        };

        [[nodiscard]] Core::RendererExpected<CachedShader *> ensure_shader(RHI::RhiDevice &device,
                                                                            RHI::Format color_format,
                                                                            const CustomShaderRef &shader,
                                                                            bool enable_shader_disk_cache);
        [[nodiscard]] CachedShader *find_shader(const CustomShaderRef &shader, RHI::Format color_format) noexcept;

        vector<CachedShader> shaders_;
    };

} // namespace SFT::UI
