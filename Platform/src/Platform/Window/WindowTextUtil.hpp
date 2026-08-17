#pragma once

#include <Foundation/src/Foundation.hpp>

namespace SFT::Platform::Windowing {


    /// Copies UTF-8 truncated to its destination.
    ///
    /// @param dest Destination value or resource.
    /// @param dest_capacity `dest_capacity` value used by the operation.
    /// @param src Source value or resource.
    ///
    /// @note This function does not throw exceptions.
    void copy_utf8_truncated(char *dest, usize dest_capacity, const char *src) noexcept;

} // namespace SFT::Platform::Windowing
