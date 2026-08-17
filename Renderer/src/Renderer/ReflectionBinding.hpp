#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::optional;
using std::string;
using std::vector;

namespace SFT::Renderer {


    namespace slang = Core::Slang;


    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param stage `stage` value used by the operation.
    ///
    /// @return Returns the value converted to RHI shader stage representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::ShaderStage to_rhi_shader_stage(slang::ShaderStage stage) noexcept;


    /// Performs the reflected stage mask operation using the supplied arguments.
    ///
    /// @param reflection `reflection` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::ShaderStage reflected_stage_mask(const slang::ShaderReflection &reflection) noexcept;


    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    [[nodiscard]] optional<RHI::BindingType> to_rhi_binding_type(slang::ShaderBindingType type) noexcept;


    struct GeneratedBindGroupLayout {
        u32 set = 0;
        vector<RHI::BindGroupLayoutEntry> entries;
    };


    /// Performs the generate bind group layouts operation using the supplied arguments.
    ///
    /// @param reflection `reflection` value used by the operation.
    /// @param visibility `visibility` value used by the operation.
    /// @param bindless_array_max_count Number of elements or operations to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<GeneratedBindGroupLayout> generate_bind_group_layouts(
        const slang::ShaderReflection &reflection,
        RHI::ShaderStage visibility,
        u32 bindless_array_max_count = 4096);


    /// Performs the generate push constant ranges operation using the supplied arguments.
    ///
    /// @param reflection `reflection` value used by the operation.
    /// @param stages `stages` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<RHI::PushConstantRange> generate_push_constant_ranges(const slang::ShaderReflection &reflection,
                                                                                RHI::ShaderStage stages);


    struct ReflectedUniform {
        string name;
        u64 offset = 0;
        u64 size = 0;
        slang::ShaderScalarType scalar = slang::ShaderScalarType::None;
        u32 rows = 0;
        u32 columns = 0;
    };

    namespace detail {

        /// Reports whether numeric leaf holds.
        ///
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_numeric_leaf(const slang::ShaderTypeReflection &type) noexcept;


        /// Collects uniform leaves using the supplied arguments and current state.
        ///
        /// @param type Type value to inspect, select, or convert.
        /// @param name Name used to identify or label the target.
        /// @param base_offset Offset from the beginning of the relevant range or buffer.
        /// @param out `out` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void collect_uniform_leaves(const slang::ShaderTypeReflection &type,
                                           const string &name,
                                           u64 base_offset,
                                           vector<ReflectedUniform> &out);

    } // namespace detail


    /// Collects uniform fields using the supplied arguments and current state.
    ///
    /// @param reflection `reflection` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<ReflectedUniform> collect_uniform_fields(const slang::ShaderReflection &reflection);


    struct ReflectedResource {
        string name;
        u32 set = 0;
        u32 binding = 0;
        RHI::BindingType type = RHI::BindingType::SampledTexture;
    };


    /// Collects resource bindings using the supplied arguments and current state.
    ///
    /// @param reflection `reflection` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<ReflectedResource> collect_resource_bindings(const slang::ShaderReflection &reflection);

} // namespace SFT::Renderer
