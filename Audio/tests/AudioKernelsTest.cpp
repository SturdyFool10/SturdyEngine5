#include <Audio/AudioBuffer.hpp>
#include <Audio/Kernels.hpp>
#include <Foundation/Cpu/CpuId.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string_view>
#include <vector>

using namespace SFT;
using namespace SFT::Audio::Kernels;

namespace {
    int failures = 0;

    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    bool near(float a, float b, float epsilon = 1.0e-5f) {
        return std::fabs(a - b) <= epsilon;
    }
}

int main() {
    constexpr usize count = 1031;
    std::vector<f32> src(count), dst(count), expected(count);
    for (usize i = 0; i < count; ++i) {
        src[i] = static_cast<f32>(static_cast<int>(i % 37) - 18) * 0.03125f;
        dst[i] = static_cast<f32>(static_cast<int>(i % 13) - 6) * 0.0625f;
    }

    add(dst.data(), src.data(), count);
    for (usize i = 0; i < count; ++i) {
        expected[i] = static_cast<f32>(static_cast<int>(i % 13) - 6) * 0.0625f + src[i];
        check(near(dst[i], expected[i]), "add matches scalar result");
    }

    add_gain(dst.data(), src.data(), 0.375f, count);
    for (usize i = 0; i < count; ++i) {
        expected[i] += src[i] * 0.375f;
        check(near(dst[i], expected[i]), "add_gain matches scalar result");
    }

    copy_gain(dst.data(), src.data(), -0.75f, count);
    for (usize i = 0; i < count; ++i) {
        expected[i] = src[i] * -0.75f;
        check(near(dst[i], expected[i]), "copy_gain matches scalar result");
    }

    scale_ramp(dst.data(), count, 0.25f, 1.25f);
    for (usize i = 0; i < count; ++i) {
        expected[i] *= 0.25f + (1.25f - 0.25f) * static_cast<f32>(i + 1) / static_cast<f32>(count);
        check(near(dst[i], expected[i]), "scale_ramp matches scalar result");
    }

    f32 expected_peak = 0.0f;
    f64 expected_squares = 0.0;
    for (f32 x : src) {
        expected_peak = std::max(expected_peak, std::fabs(x));
        expected_squares += static_cast<f64>(x) * x;
    }
    check(near(peak(src.data(), count), expected_peak), "peak matches scalar result");
    check(std::fabs(sum_squares(src.data(), count) - expected_squares) < 1.0e-4, "sum_squares matches scalar result");
    check(peak(nullptr, 0) == 0.0f, "empty peak is zero");
    check(sum_squares(nullptr, 0) == 0.0, "empty sum_squares is zero");

    SFT::Audio::AudioBuffer buffer(2, 5), source(2, 5);
    for (usize i = 0; i < 5; ++i) {
        buffer.data(0)[i] = static_cast<f32>(i + 1);
        buffer.data(1)[i] = static_cast<f32>(i + 6);
        source.data(0)[i] = 2.0f;
        source.data(1)[i] = -1.0f;
    }
    buffer.add(source, 0.5f, 4);
    check(near(buffer.data(0)[0], 2.0f) && near(buffer.data(0)[3], 5.0f) && near(buffer.data(0)[4], 5.0f),
          "AudioBuffer::add uses the runtime kernels and honors frame limits");
    buffer.add(buffer, 0.5f, 1);
    check(near(buffer.data(0)[0], 3.0f), "AudioBuffer self-add preserves aliasing semantics");
    buffer.add_channel(0, buffer, 1, 0.25f, 5);
    check(near(buffer.data(0)[0], 5.0625f), "AudioBuffer channel add supports disjoint self-channel ranges");
    buffer.add_channel(0, buffer, 0, 0.5f, 1);
    check(near(buffer.data(0)[0], 7.59375f), "AudioBuffer same-channel add preserves aliasing semantics");
    buffer.scale(0.5f);
    check(near(buffer.data(0)[0], 3.796875f), "AudioBuffer::scale uses the runtime kernels");
    const f32 expected_audio[2][5] = {{3.796875f, 2.3125f, 2.9375f, 3.5625f, 3.75f},
                                      {4.125f, 3.25f, 3.75f, 4.25f, 5.0f}};
    f64 expected_rms_squared = 0.0;
    for (usize c = 0; c < 2; ++c) {
        for (usize i = 0; i < 5; ++i) {
            expected_rms_squared += static_cast<f64>(expected_audio[c][i]) * expected_audio[c][i];
        }
    }
    check(near(buffer.peak(), 5.0f), "AudioBuffer::peak uses the runtime kernels");
    check(near(buffer.rms(), static_cast<f32>(std::sqrt(expected_rms_squared / 10.0))), "AudioBuffer::rms uses the runtime kernels");

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    const auto &cpu = Foundation::Cpu::features();
    const std::string_view selected = active_isa();
    if (cpu.avx512f && cpu.os_supports_avx512) {
        check(selected == "avx512f", "AVX-512F selected when CPU and OS support it");
    } else if (cpu.avx2 && cpu.fma3 && cpu.os_supports_avx) {
        check(selected == "avx2+fma", "AVX2 selected when AVX-512F is unavailable");
    }
#endif

    return failures == 0 ? 0 : 1;
}
