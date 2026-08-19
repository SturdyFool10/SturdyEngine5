#include <RHI/Backend.hpp>

namespace SFT::RHI {

/// Registers backend using the supplied arguments and current state.
///
/// @param registration `registration` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void BackendRegistry::register_backend(const BackendRegistration &registration) {
            for (BackendRegistration &existing : backends_) {
                if (existing.backend == registration.backend) {
                    existing = registration;
                    return;
                }
            }
            backends_.push_back(registration);
        }

/// Returns the current or globally available backends value.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] span<const BackendRegistration> BackendRegistry::backends() const noexcept { return backends_; }

/// Reports whether this `RHI` contains no elements or payload.
///
/// @return Returns the current empty value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool BackendRegistry::empty() const noexcept { return backends_.empty(); }

/// Reports whether available holds for this `RHI`.
///
/// @param backend Backend value to inspect, select, or convert.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool BackendRegistry::is_available(BackendType backend) const noexcept { return find(backend) != nullptr; }

/// Finds the requested entry in the available state.
///
/// @param backend Backend value to inspect, select, or convert.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] const BackendRegistration *BackendRegistry::find(BackendType backend) const noexcept {
            for (const BackendRegistration &registration : backends_) {
                if (registration.backend == backend) {
                    return &registration;
                }
            }
            return nullptr;
        }

/// Creates a instance from the supplied parameters.
///
/// @param backend Backend value to inspect, select, or convert.
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
[[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> BackendRegistry::create_instance(
            BackendType backend, const InstanceDesc &desc) const {
            const BackendRegistration *registration = find(backend);
            if (registration == nullptr || registration->create_instance == nullptr) {
                return rhi_error(RhiErrorCode::Unsupported,
                                 string("No RHI backend registered for ") + backend_type_name(backend));
            }
            return registration->create_instance(desc);
        }

/// Creates a preferred instance from the supplied parameters.
///
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
[[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> BackendRegistry::create_preferred_instance(
            const InstanceDesc &desc) const {
            optional<BackendType> backend = preferred_backend();
            if (!backend.has_value()) {
                return rhi_error(RhiErrorCode::Unsupported, "No RHI graphics backend is registered.");
            }
            return create_instance(*backend, desc);
        }

/// Returns a human-readable name for the supplied backend display value.
///
/// @param registration `registration` value used by the operation.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
string_view backend_display_name(const BackendRegistration &registration) noexcept {
        return registration.name.empty() ? string_view{backend_type_name(registration.backend)}
                                         : registration.name;
    }

} // namespace SFT::RHI

namespace SFT::RHI {

    /// Performs the preferred backend operation for `RHI` using the supplied arguments.
    ///
    /// @param priority `priority` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    optional<BackendType> BackendRegistry::preferred_backend(
        span<const BackendType> priority) const noexcept {
        for (BackendType backend : priority) {
            if (is_available(backend)) {
                return backend;
            }
        }
        if (!backends_.empty()) {
            return backends_.front().backend;
        }
        return std::nullopt;
    }

} // namespace SFT::RHI

