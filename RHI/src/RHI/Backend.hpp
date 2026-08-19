#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#pragma endregion

#include <RHI/Error.hpp>
#include <RHI/Device.hpp>
#include <RHI/Adapter.hpp>

using std::optional;
using std::span;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

namespace SFT::RHI {


    using InstanceFactory = RhiExpected<unique_ptr<RhiInstance>> (*)(const InstanceDesc &);

    struct BackendRegistration {
        BackendType backend = BackendType::Vulkan;
        string_view name;
        InstanceFactory create_instance = nullptr;
    };


    inline constexpr BackendType default_backend_priority[] = {
        BackendType::Vulkan,
        BackendType::D3D12,
        BackendType::Metal,
        BackendType::WebGpu,
    };


    class BackendRegistry {
      public:

        /// Registers backend using the supplied arguments and current state.
        ///
        /// @param registration `registration` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void register_backend(const BackendRegistration &registration);

        /// Returns the current or globally available backends value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const BackendRegistration> backends() const noexcept;
        /// Reports whether this `BackendRegistry` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept;
        /// Reports whether available holds for this `BackendRegistry`.
        ///
        /// @param backend Backend value to inspect, select, or convert.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_available(BackendType backend) const noexcept;

        /// Finds the requested entry in the available state.
        ///
        /// @param backend Backend value to inspect, select, or convert.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const BackendRegistration *find(BackendType backend) const noexcept;


        /// Performs the preferred backend operation for `BackendRegistry` using the supplied arguments.
        ///
        /// @param priority `priority` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<BackendType> preferred_backend(
            span<const BackendType> priority = default_backend_priority) const noexcept;


        /// Creates a instance from the supplied parameters.
        ///
        /// @param backend Backend value to inspect, select, or convert.
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
        [[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> create_instance(
            BackendType backend, const InstanceDesc &desc) const;


        /// Creates a preferred instance from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
        [[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> create_preferred_instance(
            const InstanceDesc &desc) const;

      private:
        vector<BackendRegistration> backends_;
    };


    /// Returns a human-readable name for the supplied backend display value.
    ///
    /// @param registration `registration` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] string_view backend_display_name(const BackendRegistration &registration) noexcept;

} // namespace SFT::RHI
