#include <Renderer/ReflectionBinding.hpp>

#include <iostream>

namespace {

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    bool implicit_global_constant_buffer_is_in_layout() {
        SFT::Core::Slang::ShaderReflection reflection{};
        reflection.global_constant_buffer_binding = 0;
        reflection.global_constant_buffer_size = 64;
        reflection.descriptor_sets.push_back(SFT::Core::Slang::ShaderDescriptorSetReflection{
            .space = 0,
            .ranges = {
                SFT::Core::Slang::ShaderDescriptorRangeReflection{
                    .type = SFT::Core::Slang::ShaderBindingType::ConstantBuffer,
                    .category = SFT::Core::Slang::ShaderParameterCategory::DescriptorTableSlot,
                    .binding = 0,
                    .count = 1,
                },
                SFT::Core::Slang::ShaderDescriptorRangeReflection{
                    .type = SFT::Core::Slang::ShaderBindingType::CombinedTextureSampler,
                    .category = SFT::Core::Slang::ShaderParameterCategory::DescriptorTableSlot,
                    .binding = 1,
                    .count = 1,
                },
            },
        });

        SFT::Core::Slang::ShaderParameterReflection texture{};
        texture.name = "base_color_texture";
        texture.category = SFT::Core::Slang::ShaderParameterCategory::DescriptorTableSlot;
        texture.binding = 1;
        texture.binding_space = 0;
        texture.binding_ranges.push_back(SFT::Core::Slang::ShaderBindingRangeReflection{
            .type = SFT::Core::Slang::ShaderBindingType::CombinedTextureSampler,
            .binding = 0,
            .count = 1,
        });
        reflection.global_parameters.push_back(std::move(texture));

        const auto layouts = SFT::Renderer::generate_bind_group_layouts(
            reflection, SFT::RHI::ShaderStage::Fragment);
        bool passed = check(layouts.size() == 1, "reflection produced the wrong bind-group layout count");
        if (layouts.size() != 1) {
            return false;
        }
        passed &= check(layouts[0].set == 0, "global material descriptors did not remain in set zero");
        passed &= check(layouts[0].entries.size() == 2, "implicit uniform buffer was omitted from the reflected layout");
        if (layouts[0].entries.size() == 2) {
            passed &= check(layouts[0].entries[0].binding == 0 &&
                            layouts[0].entries[0].type == SFT::RHI::BindingType::UniformBuffer,
                            "implicit uniform buffer was not emitted at binding zero");
            passed &= check(layouts[0].entries[1].binding == 1 &&
                            layouts[0].entries[1].type == SFT::RHI::BindingType::CombinedImageSampler,
                            "material texture binding changed while adding the implicit uniform buffer");
        }
        return passed;
    }

} // namespace

int main() {
    return implicit_global_constant_buffer_is_in_layout() ? 0 : 1;
}
