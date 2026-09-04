#pragma once

#include <Foundation/Foundation.hpp>

#include <RHI/Backend.hpp>

namespace SFT::Core::WebGpu {

    /// Returns this backend's registration entry, for `RHI::BackendRegistry`.
    ///
    /// @return Returns the current backend registration value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::BackendRegistration webgpu_backend_registration() noexcept;

} // namespace SFT::Core::WebGpu
