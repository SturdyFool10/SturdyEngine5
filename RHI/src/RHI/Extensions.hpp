#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string_view>
#pragma endregion

using std::span;
using std::string_view;

namespace SFT::RHI {

    // ─── Extension identity ───────────────────────────────────────────────────────
    //
    // `Feature` (:Features) is for stable, cross-backend optional capabilities the core RHI knows by
    // name. `ExtensionId` is the escape hatch for API/vendor-specific or still-experimental features
    // that should still be expressible without baking every vendor experiment into the core enum.
    //
    // An extension is identified by a namespace + name + interface version. Examples:
    //   - {"metal", "tile-shaders", 1}
    //   - {"apple", "programmable-blending", 1}
    //   - {"nvidia", "opacity-micromaps", 1}
    //
    // The safe pattern is the same as features: adapters report support, device creation enables the
    // required/optional subset, and use-sites branch on `RhiDevice::is_extension_enabled(...)`. A
    // mature extension can later graduate into the core `Feature` enum + first-class descriptors
    // without changing this negotiation shape.
    struct ExtensionId {
        string_view name_space;
        string_view name;
        u32 version = 1;

        friend constexpr bool operator==(const ExtensionId &, const ExtensionId &) noexcept = default;
    };

    [[nodiscard]] constexpr bool extension_matches(ExtensionId supported, ExtensionId requested) noexcept {
        return supported.name_space == requested.name_space && supported.name == requested.name &&
               supported.version >= requested.version;
    }

    [[nodiscard]] bool contains_extension(span<const ExtensionId> supported,
                                                 ExtensionId requested) noexcept;

    // Base class for extension-specific typed interfaces. A dedicated extension partition can derive
    // from this with first-class RHI descriptors/commands for one backend/vendor feature; the device
    // returns that interface only when the extension was enabled. Returning nullptr is the safe-fail
    // path for unsupported or non-enabled extensions.
    class RhiDeviceExtension {
      public:
        virtual ~RhiDeviceExtension() = default;
        [[nodiscard]] virtual ExtensionId extension_id() const noexcept = 0;
    };

} // namespace SFT::RHI
