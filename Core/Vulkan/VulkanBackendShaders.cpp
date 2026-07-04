// VulkanBackend shader pipeline: Slang → SPIR-V compilation of the engine's uncompiled
// shaders, VkShaderModule creation per entry point, and keyed module lookup.
module;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"

#include <chrono>
#include <format>
#include <memory>
#include <span>

module Sturdy.Core;

import :VulkanBackend;
import :VulkanDevice;
import :VulkanShaderModule;
import :RendererError;
import :Renderer;
import :Shader;
import :ShaderDiscovery;
import Sturdy.Foundation;

using std::format;
using std::make_shared;
using std::span;
using std::chrono::duration;
using std::chrono::steady_clock;

namespace SFT::Core::Vulkan {

    namespace {

        // Drops a trailing ".slang" so shader module keys are addressed by bare source name, e.g.
        // "Shaders/triangle" rather than "Shaders/triangle.slang". Used both when filing modules and
        // when looking them up, so callers may pass the path with or without the extension.
        //
        // Also normalizes '\\' to '/': discovery keys come from fs::path::string(), which uses the
        // platform's native separator ('\\' on Windows), while call sites like createPipeline() use
        // hardcoded forward slashes. Without normalizing, the two disagree and the map lookup misses
        // on Windows even though the shader compiled successfully.
        [[nodiscard]] UString strip_slang_extension(const ustr &path) {
            // ".slang" is 6 ASCII scalars, so scalar and byte counts coincide for these paths.
            // FRICTION: ends_with(".slang") is ambiguous — a const char* literal converts equally well to
            // ustr or string_view, and both have overloads — so the suffix must be spelled ustr{...}.
            const ustr trimmed = path.ends_with(ustr{".slang"}) ? path.substr(0, path.size() - 6) : path.substr();
            UString result{trimmed};
            result.replace_all(ustr{"\\"}, ustr{"/"});
            return result;
        }

    } // namespace

    RendererResult VulkanBackend::createShaders(const RendererCreateInfo &init) {
        // The backend owns turning the engine's reflected-but-uncompiled shaders into its native
        // format. For Vulkan that means recompiling each source to SPIR-V 1.6 with maximal
        // optimization, then producing one VkShaderModule per entry point. Each module retains a
        // shared handle to its source file's reflection and is filed under (source file, entry point).
        Slang::ShaderCompiler compiler;

        Slang::ShaderCompileOptions options{};
        options.targets = {Slang::ShaderTarget{.format = Slang::ShaderTargetFormat::Spirv, .profile = "spirv_1_6"}};
        options.optimization = Slang::ShaderOptimizationLevel::Maximal;

        const auto total_start = steady_clock::now();

        for (const Slang::UnCompiledShader &uncompiled : init.uncompiled_shaders) {
            const UString source_path{uncompiled.source.path};        // real path — for logs/errors
            // FRICTION: strip_slang_extension(source_path) is ambiguous — UString->ustr can go via either
            // ustr(const UString&) or UString::operator ustr(), so the conversion must be forced with
            // .as_ustr(). This bites any UString passed where a `const ustr&` is expected.
            const UString source_file = strip_slang_extension(source_path.as_ustr()); // key + stored provenance

            const auto frontend_start = steady_clock::now();
            auto compiled = compiler.compile(uncompiled.source, options);
            const duration<double, std::milli> frontend_elapsed = steady_clock::now() - frontend_start;
            if (!compiled) [[unlikely]] {
                return renderer_error(RendererErrorCode::OperationFailed,
                                      format("Failed to compile shader '{}' to SPIR-V: {}",
                                             source_path, compiled.error().message));
            }

            // One reflection per source file, shared by every entry point's module.
            auto reflection = make_shared<const Slang::ShaderReflection>(compiled->reflection());

            for (usize entry_index = 0; entry_index < reflection->entry_points.size(); ++entry_index) {
                const Slang::ShaderEntryPointReflection &entry = reflection->entry_points[entry_index];

                const VkShaderStageFlagBits stage = to_vk_shader_stage(entry.stage);
                if (stage == 0) [[unlikely]] {
                    return renderer_error(RendererErrorCode::Unsupported,
                                          format("Shader '{}' entry point '{}' has no Vulkan stage mapping.",
                                                 source_path, entry.name));
                }

                // Times the part of the pipeline that produces a shader Vulkan can actually call:
                // target codegen for this entry point plus building its VkShaderModule. The Slang
                // front-end parse/reflect above (frontend_elapsed) is shared across every entry
                // point in the file, so it's reported separately rather than folded in here.
                const auto entry_start = steady_clock::now();

                auto bytecode = compiled->entry_point_code(entry_index, 0);
                if (!bytecode) [[unlikely]] {
                    return renderer_error(RendererErrorCode::OperationFailed,
                                          format("Failed to emit SPIR-V for '{}' entry point '{}': {}",
                                                 source_path, entry.name, bytecode.error().message));
                }

                // SPIR-V is a stream of 32-bit words; the byte count is always a multiple of 4.
                if (bytecode->bytes.size() % sizeof(u32) != 0) [[unlikely]] {
                    return renderer_error(RendererErrorCode::OperationFailed,
                                          format("SPIR-V for '{}' entry point '{}' is not word-aligned.",
                                                 source_path, entry.name));
                }
                const span<const u32> words{
                    reinterpret_cast<const u32 *>(bytecode->bytes.data()),
                    bytecode->bytes.size() / sizeof(u32),
                };

                auto module = VulkanShaderModule::create(
                    this->logicalDevice.vk_handle(),
                    words,
                    source_file,
                    entry.name,
                    stage,
                    reflection);
                if (!module) [[unlikely]] {
                    return renderer_error(module.error().code,
                                          format("Failed to create VkShaderModule for '{}' entry point '{}': {}",
                                                 source_path, entry.name, module.error().message));
                }

                const duration<double, std::milli> entry_elapsed = steady_clock::now() - entry_start;
                Foundation::log_info("Shader '{}' entry point '{}': codegen+module {:.3f} ms (+{:.3f} ms front-end)",
                                     source_path, entry.name, entry_elapsed.count(), frontend_elapsed.count());

                VulkanShaderModuleKey key{source_file, entry.name};
                if (auto [it, inserted] = shader_modules_.try_emplace(std::move(key), std::move(*module)); !inserted) [[unlikely]] {
                    return renderer_error(RendererErrorCode::OperationFailed,
                                          format("Duplicate shader entry point '{}' in '{}'.", entry.name, source_path));
                }
            }

            Foundation::log_info("Compiled shader '{}' to SPIR-V 1.6: {} entry point(s)",
                                 source_path, reflection->entry_points.size());
        }

        const duration<double, std::milli> total_elapsed = steady_clock::now() - total_start;
        Foundation::log_info("Shader compilation complete: {} module(s) from {} source file(s) in {:.3f} ms",
                             shader_modules_.size(), init.uncompiled_shaders.size(), total_elapsed.count());
        return {};
    }

    const VulkanShaderModule *VulkanBackend::find_shader_module(const ustr &source_file,
                                                               const ustr &entry_point) const noexcept {
        const auto it = shader_modules_.find(VulkanShaderModuleKey{strip_slang_extension(source_file), UString{entry_point}});
        return it != shader_modules_.end() ? &it->second : nullptr;
    }

} // namespace SFT::Core::Vulkan
