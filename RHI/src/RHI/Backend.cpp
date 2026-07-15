#include "Backend.hpp"

namespace SFT::RHI {

void BackendRegistry::register_backend(const BackendRegistration &registration) {
            for (BackendRegistration &existing : backends_) {
                if (existing.backend == registration.backend) {
                    existing = registration;
                    return;
                }
            }
            backends_.push_back(registration);
        }

[[nodiscard]] span<const BackendRegistration> BackendRegistry::backends() const noexcept { return backends_; }

[[nodiscard]] bool BackendRegistry::empty() const noexcept { return backends_.empty(); }

[[nodiscard]] bool BackendRegistry::is_available(BackendType backend) const noexcept { return find(backend) != nullptr; }

[[nodiscard]] const BackendRegistration *BackendRegistry::find(BackendType backend) const noexcept {
            for (const BackendRegistration &registration : backends_) {
                if (registration.backend == backend) {
                    return &registration;
                }
            }
            return nullptr;
        }

[[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> BackendRegistry::create_instance(
            BackendType backend, const InstanceDesc &desc) const {
            const BackendRegistration *registration = find(backend);
            if (registration == nullptr || registration->create_instance == nullptr) {
                return rhi_error(RhiErrorCode::Unsupported,
                                 string("No RHI backend registered for ") + backend_type_name(backend));
            }
            return registration->create_instance(desc);
        }

[[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> BackendRegistry::create_preferred_instance(
            const InstanceDesc &desc) const {
            optional<BackendType> backend = preferred_backend();
            if (!backend.has_value()) {
                return rhi_error(RhiErrorCode::Unsupported, "No RHI graphics backend is registered.");
            }
            return create_instance(*backend, desc);
        }

string_view backend_display_name(const BackendRegistration &registration) noexcept {
        return registration.name.empty() ? string_view{backend_type_name(registration.backend)}
                                         : registration.name;
    }

} // namespace SFT::RHI
