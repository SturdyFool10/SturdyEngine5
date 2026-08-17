#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#pragma endregion

namespace SFT::Renderer {

    template <class Tag>
    struct Handle {
        u64 value = 0;

        /// Reports whether valid holds for this `Handle`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
        /// Converts the `Handle` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_valid(); }

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(Handle, Handle) noexcept = default;
    };

    using MeshHandle = Handle<struct MeshTag>;
    using MaterialHandle = Handle<struct MaterialTag>;
    using TextureHandle = Handle<struct TextureTag>;


    using MaterialTemplateHandle = Handle<struct MaterialTemplateTag>;
    using MaterialInstanceHandle = Handle<struct MaterialInstanceTag>;

} // namespace SFT::Renderer
