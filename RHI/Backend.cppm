module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#pragma endregion

export module Sturdy.RHI:Backend;

import :Error;
import :Device;  // BackendType, backend_type_name
import :Adapter; // RhiInstance, InstanceDesc

using std::optional;
using std::span;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

export namespace SFT::RHI {

    // ─── Backend availability ────────────────────────────────────────────────────
    //
    // The RHI names every graphics API in `BackendType` unconditionally, but which of them can
    // actually run is a property of the build and the machine, discovered at runtime. A backend
    // advertises itself with a `BackendRegistration`: its `BackendType`, a display name, and a factory
    // that mints its `RhiInstance`. The application collects the backends it was compiled with into a
    // `BackendRegistry` at startup — e.g. `registry.register_backend(vulkan_backend_registration())` —
    // and from then on can ask which APIs are available, pick one explicitly (the D3D12-vs-Vulkan
    // choice a future Windows build offers), or let the registry choose a platform-appropriate default.
    //
    // Registration is an application-driven list rather than global self-registration on purpose: it
    // keeps the RHI free of global mutable state and module static-init ordering hazards, and lets a
    // host (a test, a tool) present exactly the backends it wants.

    // A backend's instance factory. Returns a fresh `RhiInstance` for that API, or an error if the
    // driver/loader is missing at runtime even though the backend was compiled in.
    using InstanceFactory = RhiExpected<unique_ptr<RhiInstance>> (*)(const InstanceDesc &);

    struct BackendRegistration {
        BackendType backend = BackendType::Vulkan;
        string_view name; // display name, e.g. "Vulkan"; falls back to backend_type_name() if empty
        InstanceFactory create_instance = nullptr;
    };

    // The default order `preferred_backend()` walks when the caller doesn't state a priority. Listed
    // most-preferred first; a caller wanting native-API-first on a specific platform passes its own
    // priority span instead.
    inline constexpr BackendType default_backend_priority[] = {
        BackendType::Vulkan,
        BackendType::D3D12,
        BackendType::Metal,
        BackendType::WebGpu,
    };

    // An application-owned set of the backends available this run. Populate it at startup, then query
    // availability and mint instances from it.
    class BackendRegistry {
      public:
        // Adds (or replaces, if the same BackendType is already present) a backend.
        void register_backend(const BackendRegistration &registration) {
            for (BackendRegistration &existing : backends_) {
                if (existing.backend == registration.backend) {
                    existing = registration;
                    return;
                }
            }
            backends_.push_back(registration);
        }

        [[nodiscard]] span<const BackendRegistration> backends() const noexcept { return backends_; }
        [[nodiscard]] bool empty() const noexcept { return backends_.empty(); }
        [[nodiscard]] bool is_available(BackendType backend) const noexcept { return find(backend) != nullptr; }

        [[nodiscard]] const BackendRegistration *find(BackendType backend) const noexcept {
            for (const BackendRegistration &registration : backends_) {
                if (registration.backend == backend) {
                    return &registration;
                }
            }
            return nullptr;
        }

        // The first available backend in `priority`, else the first registered backend, else nullopt.
        [[nodiscard]] optional<BackendType> preferred_backend(
            span<const BackendType> priority = default_backend_priority) const noexcept {
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

        // Mints an instance for `backend`. Fails with `Unsupported` if that backend isn't registered.
        [[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> create_instance(
            BackendType backend, const InstanceDesc &desc) const {
            const BackendRegistration *registration = find(backend);
            if (registration == nullptr || registration->create_instance == nullptr) {
                return rhi_error(RhiErrorCode::Unsupported,
                                 string("No RHI backend registered for ") + backend_type_name(backend));
            }
            return registration->create_instance(desc);
        }

        // Mints an instance for the preferred available backend.
        [[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> create_preferred_instance(
            const InstanceDesc &desc) const {
            optional<BackendType> backend = preferred_backend();
            if (!backend.has_value()) {
                return rhi_error(RhiErrorCode::Unsupported, "No RHI graphics backend is registered.");
            }
            return create_instance(*backend, desc);
        }

      private:
        vector<BackendRegistration> backends_;
    };

    // A backend's display name, using its registration's `name` when set or `backend_type_name()`.
    [[nodiscard]] inline string_view backend_display_name(const BackendRegistration &registration) noexcept {
        return registration.name.empty() ? string_view{backend_type_name(registration.backend)}
                                         : registration.name;
    }

} // namespace SFT::RHI
