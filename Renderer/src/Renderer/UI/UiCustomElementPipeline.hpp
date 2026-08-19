#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <Renderer/UI/CustomElement.hpp>

using std::span;
using std::string;
using std::vector;

namespace SFT::UI {


    class UiCustomElementPipeline {
      public:
        /// Constructs a `UiCustomElementPipeline` in its default state.
        ///
        /// @note This function does not throw exceptions.
        UiCustomElementPipeline() noexcept = default;


        /// Prepares the required state or resources for a later operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param draws Draw descriptions processed in submission order.
        /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, RHI::Format color_format,
                                                    span<const CustomDraw> draws, bool enable_shader_disk_cache = true);

        /// Issues one draw call per entry of `draws`, in order.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param draws Draw descriptions processed in submission order — every shader referenced
        ///        must have already been prepared this frame via prepare() above (same
        ///        `color_format`, which is what the shader cache is keyed by).
        /// @param viewport_size Requested or available size for the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note See class doc comment for why this doesn't batch.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, RHI::Format color_format,
                                                 span<const CustomDraw> draws, glm::vec2 viewport_size);

        /// Destroys or releases the `UiCustomElementPipeline` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
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


            u32 push_constant_size = 0;
        };

        /// Finds or creates the shader required by the operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param shader Shader used or affected by the operation.
        /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<CachedShader *> ensure_shader(RHI::RhiDevice &device,
                                                                            RHI::Format color_format,
                                                                            const CustomShaderRef &shader,
                                                                            bool enable_shader_disk_cache);
        /// Finds shader in the available state.
        ///
        /// @param shader Shader used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CachedShader *find_shader(const CustomShaderRef &shader, RHI::Format color_format) noexcept;

        vector<CachedShader> shaders_;
    };

} // namespace SFT::UI
