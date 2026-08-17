#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string_view>
#pragma endregion

namespace SFT::Core::Slang {


    struct EmbeddedShaderSource {
        std::string_view relative_path;
        std::string_view module_name;
        Foundation::EmbeddedText source;
    };


    /// Returns the current or globally available embedded shaders value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::span<const EmbeddedShaderSource> embedded_shaders() noexcept;


    /// Finds embedded shader in the available state.
    ///
    /// @param path_or_module_name Name used to identify or label the target.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const EmbeddedShaderSource *find_embedded_shader(std::string_view path_or_module_name) noexcept;

} // namespace SFT::Core::Slang
