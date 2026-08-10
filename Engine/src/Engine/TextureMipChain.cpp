#include "TextureMipChain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace SFT::Engine::Detail {

    namespace {

        [[nodiscard]] std::optional<usize> rgba8_mip_chain_bytes(u32 width, u32 height) noexcept {
            u64 total = 0;
            while (true) {
                const u64 texels = static_cast<u64>(width) * height;
                if (texels > std::numeric_limits<u64>::max() / 4u) {
                    return std::nullopt;
                }
                const u64 level_bytes = texels * 4u;
                if (total > std::numeric_limits<u64>::max() - level_bytes) {
                    return std::nullopt;
                }
                total += level_bytes;
                if (width == 1 && height == 1) {
                    break;
                }
                width = std::max(width / 2u, 1u);
                height = std::max(height / 2u, 1u);
            }
            if (total > std::numeric_limits<usize>::max()) {
                return std::nullopt;
            }
            return static_cast<usize>(total);
        }

        [[nodiscard]] f32 srgb_to_linear(u8 value) noexcept {
            const f32 encoded = static_cast<f32>(value) / 255.0f;
            return encoded <= 0.04045f ? encoded / 12.92f
                                       : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

        [[nodiscard]] u8 linear_to_srgb(f32 value) noexcept {
            const f32 linear = std::clamp(value, 0.0f, 1.0f);
            const f32 encoded = linear <= 0.0031308f ? linear * 12.92f
                                                     : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            return static_cast<u8>(std::clamp(std::lround(encoded * 255.0f), 0l, 255l));
        }

        [[nodiscard]] u8 byte_value(std::byte value) noexcept {
            return std::to_integer<u8>(value);
        }

    } // namespace

    u32 texture_mip_level_count(u32 width, u32 height) noexcept {
        if (width == 0 || height == 0) {
            return 0;
        }
        u32 levels = 1;
        while (width > 1 || height > 1) {
            width = std::max(width / 2u, 1u);
            height = std::max(height / 2u, 1u);
            ++levels;
        }
        return levels;
    }

    std::optional<TextureMipChain> generate_rgba8_mip_chain(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb) {
        if (width == 0 || height == 0) {
            return std::nullopt;
        }
        const u64 base_bytes = static_cast<u64>(width) * height * 4u;
        const std::optional<usize> total_bytes = rgba8_mip_chain_bytes(width, height);
        if (!total_bytes || base_bytes > std::numeric_limits<usize>::max() ||
            rgba8.size() != static_cast<usize>(base_bytes)) {
            return std::nullopt;
        }

        TextureMipChain chain{};
        chain.data.reserve(*total_bytes);
        chain.data.assign(rgba8.begin(), rgba8.end());
        chain.mip_levels = texture_mip_level_count(width, height);

        usize previous_offset = 0;
        u32 previous_width = width;
        u32 previous_height = height;
        while (previous_width > 1 || previous_height > 1) {
            const u32 next_width = std::max(previous_width / 2u, 1u);
            const u32 next_height = std::max(previous_height / 2u, 1u);
            const usize next_offset = chain.data.size();
            chain.data.resize(next_offset + static_cast<usize>(next_width) * next_height * 4u);

            for (u32 y = 0; y < next_height; ++y) {
                const f64 source_y_start = static_cast<f64>(y) * previous_height / next_height;
                const f64 source_y_finish = static_cast<f64>(y + 1u) * previous_height / next_height;
                const u32 source_y_begin = static_cast<u32>(std::floor(source_y_start));
                const u32 source_y_end = static_cast<u32>(std::ceil(source_y_finish));
                for (u32 x = 0; x < next_width; ++x) {
                    const f64 source_x_start = static_cast<f64>(x) * previous_width / next_width;
                    const f64 source_x_finish = static_cast<f64>(x + 1u) * previous_width / next_width;
                    const u32 source_x_begin = static_cast<u32>(std::floor(source_x_start));
                    const u32 source_x_end = static_cast<u32>(std::ceil(source_x_finish));
                    const f64 total_weight = (source_x_finish - source_x_start) *
                                             (source_y_finish - source_y_start);
                    const usize destination = next_offset + (static_cast<usize>(y) * next_width + x) * 4u;

                    if (srgb) {
                        f64 rgb_sum[3]{};
                        f64 alpha_sum = 0.0;
                        for (u32 source_y = source_y_begin; source_y < source_y_end; ++source_y) {
                            const f64 y_weight = std::min(source_y_finish, static_cast<f64>(source_y + 1u)) -
                                                 std::max(source_y_start, static_cast<f64>(source_y));
                            for (u32 source_x = source_x_begin; source_x < source_x_end; ++source_x) {
                                const f64 x_weight = std::min(source_x_finish, static_cast<f64>(source_x + 1u)) -
                                                     std::max(source_x_start, static_cast<f64>(source_x));
                                const f64 weight = x_weight * y_weight;
                                const usize source = previous_offset +
                                    (static_cast<usize>(source_y) * previous_width + source_x) * 4u;
                                for (u32 channel = 0; channel < 3; ++channel) {
                                    rgb_sum[channel] += srgb_to_linear(byte_value(chain.data[source + channel])) * weight;
                                }
                                alpha_sum += byte_value(chain.data[source + 3u]) * weight;
                            }
                        }
                        for (u32 channel = 0; channel < 3; ++channel) {
                            chain.data[destination + channel] = static_cast<std::byte>(
                                linear_to_srgb(static_cast<f32>(rgb_sum[channel] / total_weight)));
                        }
                        chain.data[destination + 3u] = static_cast<std::byte>(std::clamp(
                            std::lround(alpha_sum / total_weight), 0l, 255l));
                    } else {
                        f64 channel_sums[4]{};
                        for (u32 source_y = source_y_begin; source_y < source_y_end; ++source_y) {
                            const f64 y_weight = std::min(source_y_finish, static_cast<f64>(source_y + 1u)) -
                                                 std::max(source_y_start, static_cast<f64>(source_y));
                            for (u32 source_x = source_x_begin; source_x < source_x_end; ++source_x) {
                                const f64 x_weight = std::min(source_x_finish, static_cast<f64>(source_x + 1u)) -
                                                     std::max(source_x_start, static_cast<f64>(source_x));
                                const f64 weight = x_weight * y_weight;
                                const usize source = previous_offset +
                                    (static_cast<usize>(source_y) * previous_width + source_x) * 4u;
                                for (u32 channel = 0; channel < 4; ++channel) {
                                    channel_sums[channel] += byte_value(chain.data[source + channel]) * weight;
                                }
                            }
                        }
                        for (u32 channel = 0; channel < 4; ++channel) {
                            chain.data[destination + channel] = static_cast<std::byte>(std::clamp(
                                std::lround(channel_sums[channel] / total_weight), 0l, 255l));
                        }
                    }
                }
            }

            previous_offset = next_offset;
            previous_width = next_width;
            previous_height = next_height;
        }

        return chain;
    }

} // namespace SFT::Engine::Detail
