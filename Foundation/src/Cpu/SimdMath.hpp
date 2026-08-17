#pragma once

#include <Foundation/src/Types.hpp>

namespace SFT::Foundation::Cpu {












    void add(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept;
    void add(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept;
    void mul(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept;
    void mul(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept;
    void fma(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept;
    void fma(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept;
    [[nodiscard]] f32 dot(const f32 *a, const f32 *b, usize n) noexcept;
    [[nodiscard]] f64 dot(const f64 *a, const f64 *b, usize n) noexcept;
    void sqrt(f32 *dst, const f32 *a, usize n) noexcept;
    void sqrt(f64 *dst, const f64 *a, usize n) noexcept;

} // namespace SFT::Foundation::Cpu
