module;

#pragma region Imports
#pragma endregion

export module Sturdy.Renderer:Handles;

import Sturdy.Foundation;

export namespace SFT::Renderer {

    template <class Tag>
    struct Handle {
        u64 value = 0;

        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_valid(); }

        friend constexpr bool operator==(Handle, Handle) noexcept = default;
    };

    using MeshHandle = Handle<struct MeshTag>;
    using MaterialHandle = Handle<struct MaterialTag>;
    using TextureHandle = Handle<struct TextureTag>;
    // A reflection-derived material shader + layout (see :Material). Templates are shared; the thing
    // you actually draw with and set parameters on is a MaterialInstance minted from one.
    using MaterialTemplateHandle = Handle<struct MaterialTemplateTag>;
    using MaterialInstanceHandle = Handle<struct MaterialInstanceTag>;

} // namespace SFT::Renderer
