#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Device.hpp"
#include "Adapter.hpp"

using std::optional;
using std::span;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

namespace SFT::RHI {















    /// A backend's instance factory. Returns a fresh `RhiInstance` for that API, or an error if the
    /// driver/loader is missing at runtime even though the backend was compiled in.
    using InstanceFactory = RhiExpected<unique_ptr<RhiInstance>> (*)(const InstanceDesc &);

    struct BackendRegistration {
        BackendType backend = BackendType::Vulkan;
        string_view name;
        InstanceFactory create_instance = nullptr;
    };

    /// The default order `preferred_backend()` walks when the caller doesn't state a priority. Listed
    /// most-preferred first; a caller wanting native-API-first on a specific platform passes its own
    /// priority span instead.
    inline constexpr BackendType default_backend_priority[] = {
        BackendType::Vulkan,
        BackendType::D3D12,
        BackendType::Metal,
        BackendType::WebGpu,
    };

    /// An application-owned set of the backends available this run. Populate it at startup, then query
    /// availability and mint instances from it.
    class BackendRegistry {
      public:
        /// Adds (or replaces, if the same BackendType is already present) a backend.
        void register_backend(const BackendRegistration &registration);

        [[nodiscard]] span<const BackendRegistration> backends() const noexcept;
        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] bool is_available(BackendType backend) const noexcept;

        [[nodiscard]] const BackendRegistration *find(BackendType backend) const noexcept;

        /// The first available backend in `priority`, else the first registered backend, else nullopt.
        [[nodiscard]] optional<BackendType> preferred_backend(
            span<const BackendType> priority = default_backend_priority) const noexcept;

        /// Mints an instance for `backend`. Fails with `Unsupported` if that backend isn't registered.
        [[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> create_instance(
            BackendType backend, const InstanceDesc &desc) const;

        /// Mints an instance for the preferred available backend.
        [[nodiscard]] RhiExpected<unique_ptr<RhiInstance>> create_preferred_instance(
            const InstanceDesc &desc) const;

      private:
        vector<BackendRegistration> backends_;
    };

    /// A backend's display name, using its registration's `name` when set or `backend_type_name()`.
    [[nodiscard]] string_view backend_display_name(const BackendRegistration &registration) noexcept;

} // namespace SFT::RHI
