#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Features.hpp"
#include "Extensions.hpp"
#include "Queues.hpp"
#include "Device.hpp"

using std::span;
using std::string_view;
using std::unique_ptr;
using std::vector;

namespace SFT::RHI {


    struct InstanceDesc {
        string_view application_name;
        u32 application_version = 0;
        string_view engine_name = "Sturdy";
        u32 engine_version = 0;
        bool enable_validation = false;
        bool enable_debug_utils = false;


        bool headless = false;
        const char *label = nullptr;
    };


    struct DeviceRequest {
        FeatureSet required_features;
        FeatureSet optional_features;


        span<const ExtensionId> required_extensions;
        span<const ExtensionId> optional_extensions;


        span<const QueueRequest> queue_requests;
        const char *label = nullptr;
    };


    class RhiAdapter {
      public:
        /// Destroys the `RhiAdapter` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~RhiAdapter() = default;

        /// Disables this construction form for `RhiAdapter`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiAdapter(const RhiAdapter &) = delete;
        /// Assigns a new value to this `RhiAdapter`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiAdapter &operator=(const RhiAdapter &) = delete;
        /// Disables this construction form for `RhiAdapter`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiAdapter(RhiAdapter &&) = delete;
        /// Assigns a new value to this `RhiAdapter`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiAdapter &operator=(RhiAdapter &&) = delete;

        /// Returns the current or globally available info value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const AdapterInfo &info() const noexcept = 0;


        /// Returns the current or globally available supported features value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const FeatureSet &supported_features() const noexcept = 0;


        /// Returns the current or globally available feature properties value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const FeatureProperties &feature_properties() const noexcept = 0;


        /// Returns the current or globally available supported extensions value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual span<const ExtensionId> supported_extensions() const noexcept = 0;
        /// Returns the current or globally available queue infos value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual span<const QueueInfo> queue_infos() const noexcept = 0;

        /// Returns the current or globally available limits value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const DeviceLimits &limits() const noexcept = 0;


        /// Creates a device from the supplied parameters.
        ///
        /// @param request `request` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<unique_ptr<RhiDevice>> create_device(const DeviceRequest &request) = 0;

      protected:
        /// Constructs a `RhiAdapter` in its default state.
        ///
        /// @note This function does not throw exceptions.
        RhiAdapter() = default;
    };


    class RhiInstance {
      public:
        /// Destroys the `RhiInstance` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~RhiInstance() = default;

        /// Disables this construction form for `RhiInstance`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiInstance(const RhiInstance &) = delete;
        /// Assigns a new value to this `RhiInstance`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiInstance &operator=(const RhiInstance &) = delete;
        /// Disables this construction form for `RhiInstance`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiInstance(RhiInstance &&) = delete;
        /// Assigns a new value to this `RhiInstance`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiInstance &operator=(RhiInstance &&) = delete;

        /// Returns the current or globally available backend type value.
        ///
        /// @return Returns the current backend type value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual BackendType backend_type() const noexcept = 0;


        /// Enumerates adapters using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<vector<unique_ptr<RhiAdapter>>> enumerate_adapters() = 0;

      protected:
        /// Constructs a `RhiInstance` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RhiInstance() = default;
    };

} // namespace SFT::RHI
