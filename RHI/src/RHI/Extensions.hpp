#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string_view>
#pragma endregion

using std::span;
using std::string_view;

namespace SFT::RHI {


    struct ExtensionId {
        string_view name_space;
        string_view name;
        u32 version = 1;

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const ExtensionId &, const ExtensionId &) noexcept = default;
    };

    /// Performs the extension matches operation using the supplied arguments.
    ///
    /// @param supported `supported` value used by the operation.
    /// @param requested `requested` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool extension_matches(ExtensionId supported, ExtensionId requested) noexcept {
        return supported.name_space == requested.name_space && supported.name == requested.name &&
               supported.version >= requested.version;
    }

    /// Reports whether extension holds.
    ///
    /// @param supported `supported` value used by the operation.
    /// @param requested `requested` value used by the operation.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool contains_extension(span<const ExtensionId> supported,
                                                 ExtensionId requested) noexcept;


    class RhiDeviceExtension {
      public:
        /// Destroys the `RhiDeviceExtension` and releases resources owned by it.
        ///
        /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
        virtual ~RhiDeviceExtension() = default;
        /// Returns the current or globally available extension ID value.
        ///
        /// @return Returns the current extension ID value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual ExtensionId extension_id() const noexcept = 0;
    };

} // namespace SFT::RHI
