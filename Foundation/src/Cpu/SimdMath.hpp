#pragma once

#include <Foundation/src/Types.hpp>

namespace SFT::Foundation::Cpu {


    /// Adds the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param dst Destination value or resource.
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void add(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept;
    /// Adds the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param dst Destination value or resource.
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void add(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept;
    /// Computes the mul math operation over the supplied values or element range.
    ///
    /// @param dst Destination value or resource.
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void mul(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept;
    /// Computes the mul math operation over the supplied values or element range.
    ///
    /// @param dst Destination value or resource.
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void mul(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept;
    /// Computes or queries `fma` using the numeric semantics of `Cpu`.
    ///
    /// @param dst Destination value or resource.
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param c `c` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void fma(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept;
    /// Computes or queries `fma` using the numeric semantics of `Cpu`.
    ///
    /// @param dst Destination value or resource.
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param c `c` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void fma(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept;
    /// Computes the dot math operation over the supplied values or element range.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 dot(const f32 *a, const f32 *b, usize n) noexcept;
    /// Computes the dot math operation over the supplied values or element range.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f64 dot(const f64 *a, const f64 *b, usize n) noexcept;
    /// Computes or queries `sqrt` using the numeric semantics of `Cpu`.
    ///
    /// @param dst Destination value or resource.
    /// @param a `a` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void sqrt(f32 *dst, const f32 *a, usize n) noexcept;
    /// Computes or queries `sqrt` using the numeric semantics of `Cpu`.
    ///
    /// @param dst Destination value or resource.
    /// @param a `a` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void sqrt(f64 *dst, const f64 *a, usize n) noexcept;

} // namespace SFT::Foundation::Cpu
