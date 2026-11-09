#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slang {
    struct IGlobalSession;
}

namespace SFT::Core::Slang {

    /// Microsoft's DXC (`dxcompiler.dll`, and `dxil.dll` which signs what it produces) as embedded into the engine at build time. Empty on every
    /// platform that does not need it, and when the build could not fetch or embed it (see `sturdy_configure_bundled_dxc` in
    /// cmake/SturdyShaders.cmake), in which case `unavailable_reason` says why.
    struct BundledDxc {
        /// Names the pinned release; part of the directory the files are extracted to, so two releases never share one.
        std::string_view version;
        std::span<const std::byte> dxcompiler;
        std::span<const std::byte> dxil;
        /// Why nothing is embedded; empty when something is.
        std::string_view unavailable_reason;

        [[nodiscard]] bool available() const noexcept { return !dxcompiler.empty() && !dxil.empty(); }
    };

    /// The embedded DXC; defined by the generated `BundledDxcEmbed.cpp`.
    [[nodiscard]] BundledDxc bundled_dxc() noexcept;

    /// Where the DXC in use comes from.
    enum class DxcSource {
        /// Nothing is in use: DXC is not needed on this platform, or none could be set up (see `health`).
        None,
        /// `dxcompiler.dll` and `dxil.dll` placed beside the program, which win over the bundle.
        BesideProgram,
        /// The copy embedded in the engine, written to a per-user cache folder.
        Bundled,
        /// No bundle and nothing beside the program: whatever DXC Slang can find on this machine, if any.
        SystemSearch,
    };

    enum class DxcHealth {
        /// DXC plays no part here (not Windows).
        NotApplicable,
        /// Set up as intended.
        Ok,
        /// Usable, but not as intended: `summary` and `details` say what is different and what it costs.
        Degraded,
        /// Could not be set up; DXIL cannot be generated through it.
        Failed,
    };

    /// How setting DXC up went, with the specifics of everything that went wrong or was worked around. Reported up the chain (the DXIL probe,
    /// the backend inventory, shader compile errors) so no layer has to guess why D3D12 shaders do or do not build.
    struct DxcStatus {
        DxcHealth health = DxcHealth::NotApplicable;
        DxcSource source = DxcSource::None;
        /// One line: what the state is and, when it is not Ok, why.
        std::string summary;
        /// One entry per step that failed or was worked around, each naming the path, the operating-system error or the Slang result.
        std::vector<std::string> details;
        std::string bundle_version;
        std::string compiler_path;
        /// The folder holding `dxcompiler.dll`: what Slang is told, since for a shared-library back end it takes a directory, not a file.
        std::string compiler_directory;
        std::string signer_path;

        [[nodiscard]] bool usable() const noexcept { return health == DxcHealth::Ok || health == DxcHealth::Degraded; }
        /// `summary` followed by every detail, for a log line or an error message.
        [[nodiscard]] std::string describe() const;
    };

    /// Sets DXC up on first use (a one-time step shared by the whole process) and returns how it went; the result is logged when it is not
    /// Ok. Cheap afterwards. Safe from any thread.
    [[nodiscard]] DxcStatus bundled_dxc_status();

    /// `bundled_dxc_status()`, and makes `session` use the DXC it set up. A failure to do that is added to the status (health Degraded: Slang
    /// then searches the machine for a DXC itself) and logged; the returned status is the up-to-date one.
    DxcStatus prepare_bundled_dxc(slang::IGlobalSession *session);

    /// Whether `dxil` is a DXIL container carrying a validator signature. Unsigned DXIL (what DXC makes when `dxil.dll` cannot be found) is
    /// rejected by the D3D12 runtime, so a compile that "worked" but produced it is no use. Pure.
    [[nodiscard]] bool dxil_container_is_signed(std::span<const std::byte> dxil) noexcept;

} // namespace SFT::Core::Slang
