#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <bit>
#include <cstring>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#pragma endregion

namespace SFT::UI {

    enum class UiQuadKind : u32 { Rect = 0, Image = 1, ImageNearest = 2 };

                                                                                                       
                                                                                                
                                                                                                    
                                                                                                    
                                                                                                      
                                                                                                 
                                                                                                  
                                                                                                     
                                                                                                         
                                                                                       
       
                                                                                           
                                                                                                 
                                                                                                    
                                                                                                     
                                                                                                    
                                                                                                 
                                                    
    struct UiQuadInstance {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
                                                                                       
        glm::vec4 corner_radius{0.0f};
                                                                         
        glm::vec4 border_width{0.0f};
        glm::vec4 fill_color{1.0f};
        glm::vec4 border_color{0.0f};
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{1.0f};
        f32 kind = 0.0f;
        f32 _pad0 = 0.0f;
        f32 _pad1 = 0.0f;
        f32 _pad2 = 0.0f;
    };

    /// The record `UiQuadPipeline` uploads for one quad: `UiQuadInstance` with its radii, widths and colours stored as
    /// half floats and the quad's kind and texture index packed in one word (72 bytes instead of 112). The field order
    /// matches `UiQuadInstance` in `Shaders/ui_quad.slang`. The pipeline fills it in; nothing else builds one.
    struct UiQuadInstanceGpu {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        u32 corner_radius_half[2]{};
        u32 border_width_half[2]{};
        u32 fill_color_half[2]{};
        u32 border_color_half[2]{};
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{1.0f};
        /// Low 8 bits: the kind (`UiQuadKind`). The rest: the index into the bound texture array (bindless draws only).
        u32 kind_and_texture = 0;
        /// Fills the record out to a multiple of 8 bytes, which is the stride a structured buffer of it has on the GPU
        /// (the 8-byte `float2`/`uint2` members make the whole struct 8-byte aligned).
        u32 reserved = 0;
    };
    static_assert(sizeof(UiQuadInstanceGpu) == 72 && alignof(UiQuadInstanceGpu) == 4,
                  "UiQuadInstanceGpu must match the 18-word record in ui_quad.slang");

    /// `value` as an IEEE 754 half float (round to nearest even; overflow becomes infinity, NaN stays NaN).
    [[nodiscard]] inline u32 float_to_half(f32 value) noexcept {
        const u32 bits = std::bit_cast<u32>(value);
        const u32 sign = (bits >> 16u) & 0x8000u;
        const u32 exponent = (bits >> 23u) & 0xFFu;
        u32 mantissa = bits & 0x7FFFFFu;
        if (exponent == 0xFFu) {
            return sign | 0x7C00u | (mantissa != 0u ? 0x200u : 0u);
        }
        const i32 half_exponent = static_cast<i32>(exponent) - 127 + 15;
        if (half_exponent >= 0x1F) {
            return sign | 0x7C00u;
        }
        if (half_exponent <= 0) {
            if (half_exponent < -10) {
                return sign;
            }
            mantissa |= 0x800000u;
            const u32 shift = static_cast<u32>(14 - half_exponent);
            u32 result = mantissa >> shift;
            const u32 remainder = mantissa & ((1u << shift) - 1u);
            const u32 halfway = 1u << (shift - 1u);
            if (remainder > halfway || (remainder == halfway && (result & 1u) != 0u)) {
                ++result;
            }
            return sign | result;
        }
        u32 result = (static_cast<u32>(half_exponent) << 10u) | (mantissa >> 13u);
        const u32 remainder = mantissa & 0x1FFFu;
        if (remainder > 0x1000u || (remainder == 0x1000u && (result & 1u) != 0u)) {
            ++result;
        }
        return sign | result;
    }

    /// Packs `instance` for upload. `texture_index` is only read by shaders that bind their textures as an array.
    [[nodiscard]] inline UiQuadInstanceGpu pack_quad_instance(const UiQuadInstance &instance, u32 texture_index) noexcept {
        UiQuadInstanceGpu packed;
        packed.position = instance.position;
        packed.size = instance.size;
        const auto half4 = [](const glm::vec4 &value, u32 (&out)[2]) noexcept {
            out[0] = float_to_half(value.x) | (float_to_half(value.y) << 16u);
            out[1] = float_to_half(value.z) | (float_to_half(value.w) << 16u);
        };
        half4(instance.corner_radius, packed.corner_radius_half);
        half4(instance.border_width, packed.border_width_half);
        half4(instance.fill_color, packed.fill_color_half);
        half4(instance.border_color, packed.border_color_half);
        packed.uv_min = instance.uv_min;
        packed.uv_max = instance.uv_max;
        packed.kind_and_texture = (static_cast<u32>(instance.kind) & 0xFFu) | (texture_index << 8u);
        return packed;
    }

} // namespace SFT::UI
