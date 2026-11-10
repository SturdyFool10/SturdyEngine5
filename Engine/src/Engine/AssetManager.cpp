#include <Engine/AssetManager.hpp>
#include <Foundation/Trace.hpp>
#include <Engine/ImageDecode.hpp>
#include <Engine/TextureCompression.hpp>
#include <Engine/TextureMipChain.hpp>

#include <Core/Core.hpp>
#include <Core/Slang/EmbeddedShaders.hpp>
#include <Renderer/Renderer.hpp>
#include <RHI/RHI.hpp>

#include <Audio/Decoder.hpp>
#include <Async/Affinity.hpp>
#include <Renderer/UI/Svg/SvgIcon.hpp>


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <Foundation/FileIo.hpp>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace SFT::Engine {

    namespace {

        /// Returns the decode options that produce what `dynamic_range` asks for: an ordinary
        /// 8-bit sRGB image, or a scene-linear half-float one with the source's highlights and
        /// wide-gamut color preserved and color-managed into the working primaries.
        ///
        /// @param dynamic_range `dynamic_range` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Detail::DecodeOptions decode_options_for(TextureDynamicRange dynamic_range) noexcept {
            Detail::DecodeOptions options;
            options.precision = dynamic_range == TextureDynamicRange::Hdr
                                    ? Detail::DecodePrecision::SceneLinear
                                    : Detail::DecodePrecision::Rgba8Srgb;
            return options;
        }

        /// Returns the texture sample layout `dynamic_range` decodes into.
        ///
        /// @param dynamic_range `dynamic_range` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TexturePixelFormat texture_pixel_format_for(TextureDynamicRange dynamic_range) noexcept {
            return dynamic_range == TextureDynamicRange::Hdr ? TexturePixelFormat::Rgba16Float
                                                             : TexturePixelFormat::Rgba8;
        }

        /// Returns the color space a texture decoded for `dynamic_range` must be tagged with.
        ///
        /// An HDR decode always lands in scene-linear light, so `TextureColorSpace::Srgb` is not a
        /// meaningful request for it -- a caller passing the default Srgb alongside
        /// `TextureDynamicRange::Hdr` means "an sRGB-ish source file", not "apply the sRGB decode
        /// again on the GPU", and honouring it literally would double-apply the transfer function.
        ///
        /// @param dynamic_range `dynamic_range` value used by the operation.
        /// @param requested The color space the caller asked for.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TextureColorSpace texture_color_space_for(TextureDynamicRange dynamic_range,
                                                                TextureColorSpace requested) noexcept {
            return dynamic_range == TextureDynamicRange::Hdr ? TextureColorSpace::Linear : requested;
        }



        std::atomic<u64> next_asset_manager_id{1};

        /// Performs the path label operation for `Engine` using the supplied arguments.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString path_label(const std::filesystem::path &path, std::string_view fallback) {
            const std::string name = path.filename().string();
            if (auto utf8 = UString::try_from_utf8(name)) {
                return std::move(*utf8);
            }
            return UString{fallback};
        }

        /// Rewrites text that is not valid UTF-8 into text that is, preserving what it can.
        ///
        /// Bytes that cannot begin or continue a valid sequence become `?`. Used only on failure
        /// paths, where a diagnostic from a backend or shader compiler may carry raw bytes from an
        /// OS or driver string, and where dropping the message entirely would be worse than
        /// showing it with a few characters replaced.
        ///
        /// @param message Possibly-invalid UTF-8.
        ///
        /// @return A valid UTF-8 rendering of `message`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString sanitize_to_utf8(std::string_view message) {
            std::string sanitized;
            sanitized.reserve(message.size());
            for (usize index = 0; index < message.size();) {
                // Longest valid sequence starting here wins; anything else is one replaced byte.
                usize length = 0;
                const auto lead = static_cast<unsigned char>(message[index]);
                if (lead < 0x80u) {
                    length = 1;
                } else if ((lead & 0xE0u) == 0xC0u) {
                    length = 2;
                } else if ((lead & 0xF0u) == 0xE0u) {
                    length = 3;
                } else if ((lead & 0xF8u) == 0xF0u) {
                    length = 4;
                }

                bool valid = length != 0 && index + length <= message.size();
                for (usize offset = 1; valid && offset < length; ++offset) {
                    valid = (static_cast<unsigned char>(message[index + offset]) & 0xC0u) == 0x80u;
                }
                if (valid && UString::try_from_utf8(message.substr(index, length))) {
                    sanitized.append(message.substr(index, length));
                    index += length;
                } else {
                    sanitized.push_back('?');
                    ++index;
                }
            }

            auto text = UString::try_from_utf8(sanitized);
            return text ? std::move(*text) : UString{"(unprintable error message)"};
        }

        [[nodiscard]] AssetError error(AssetErrorCode code,
                                       std::string message,
                                       std::filesystem::path source = {}) {
            // try_from_utf8 rather than UString{message}: the validating constructor throws on
            // invalid UTF-8, and this runs on the failure path, where a throw has nowhere to go —
            // a backend message carrying a stray byte would terminate the process instead of
            // reporting the failure it was describing. Backend and shader-compiler diagnostics are
            // exactly the strings most likely to contain one.
            //
            // Sanitized rather than discarded when that happens: the message is the whole reason
            // this error exists, so losing it to one bad byte would trade a crash for a mystery.
            auto text = UString::try_from_utf8(message);
            return AssetError{
                .code = code,
                .message = text ? std::move(*text) : sanitize_to_utf8(message),
                .source = std::move(source),
            };
        }

        [[nodiscard]] AssetError backend_error(const Core::GraphicsBackendError &failure,
                                               const std::filesystem::path &source = {}) {
            return error(AssetErrorCode::BackendFailure, failure.message, source);
        }

        /// Reads binary file from the associated source.
        ///
        /// @param source Source value or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::IoFailure`, `AssetErrorCode::NotFound`.
        [[nodiscard]] AssetExpected<std::vector<std::byte>> read_binary_file(
            const std::filesystem::path &source) {
            auto bytes = Foundation::Io::read_file(source, Foundation::Io::AccessHint::OneShot);
            if (!bytes) {
                const AssetErrorCode code = std::filesystem::exists(source) ? AssetErrorCode::IoFailure : AssetErrorCode::NotFound;
                return std::unexpected(error(code, bytes.error(), source));
            }
            return std::move(*bytes);
        }

        /// A texture ready to hand to the renderer: the format it ends up in (block-compressed when that applies), its mip
        /// count, and the complete mip chain's bytes.
        struct PreparedTexture {
            u32 width = 0;
            u32 height = 0;
            RHI::Format format = RHI::Format::Undefined;
            u32 mip_levels = 1;
            std::vector<std::byte> bytes;
        };

        /// Everything `create_texture` does to pixels before the GPU sees them: validation, mip chain, block compression.
        /// CPU only (no renderer calls), so it can run on a worker thread; `allow_bc` is whether the device takes
        /// block-compressed textures, asked of the renderer by the caller on the render thread.
        [[nodiscard]] AssetExpected<PreparedTexture> prepare_texture(TextureAssetDesc &desc, bool allow_bc) {
            if (desc.width == 0 || desc.height == 0) {
                return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                             "A texture asset requires non-zero dimensions."));
            }
            const bool half_float = desc.format == TexturePixelFormat::Rgba16Float;
            const u64 bytes_per_pixel = half_float ? 8u : 4u;
            const u64 required_bytes = static_cast<u64>(desc.width) * desc.height * bytes_per_pixel;
            if (required_bytes > std::numeric_limits<usize>::max() || desc.pixels.size() != required_bytes) {
                return std::unexpected(error(
                    AssetErrorCode::InvalidDescription,
                    "Texture byte count does not match width * height * bytes-per-pixel for its format."));
            }
            if (half_float && desc.color_space == TextureColorSpace::Srgb) {
                // There is no half-float sRGB texture format, and there should not be: the whole point
                // of decoding to half-float is to hold scene-linear light. Rejected rather than
                // silently re-tagged, because a caller asking for both has a mistaken model of one of
                // them.
                return std::unexpected(error(
                    AssetErrorCode::InvalidDescription,
                    "An Rgba16Float texture is scene-linear and cannot use TextureColorSpace::Srgb."));
            }
            const bool srgb = desc.color_space == TextureColorSpace::Srgb;
            RHI::Format format = half_float ? RHI::Format::RGBA16Float
                                 : srgb     ? RHI::Format::RGBA8UnormSrgb
                                            : RHI::Format::RGBA8Unorm;

            Detail::TextureMipChain mip_chain{};
            if (desc.generate_mipmaps) {
                auto generated = half_float
                                     ? Detail::generate_rgba16f_mip_chain(desc.pixels, desc.width, desc.height)
                                     : Detail::generate_rgba8_mip_chain(desc.pixels, desc.width, desc.height, srgb);
                if (!generated) {
                    return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                                 "Could not generate the texture mip chain."));
                }
                mip_chain = std::move(*generated);
            } else {
                mip_chain.data = std::move(desc.pixels);
                mip_chain.mip_levels = 1;
            }
            std::vector<std::byte>{}.swap(desc.pixels);

            PreparedTexture prepared{.width = desc.width, .height = desc.height, .format = format,
                                     .mip_levels = mip_chain.mip_levels};
            const std::span<const std::byte> upload_bytes{mip_chain.data.data(), mip_chain.data.size()};
            std::optional<std::vector<std::byte>> compressed;
            // Every BC codec bundled here (BC1/3/4/5/7) takes 8-bit input and produces an 8-bit-range
            // result, so a half-float texture is uploaded uncompressed. Compressing it would need
            // BC6H, which is the one BC format bc7enc does not implement.
            // WebGPU requires a block-compressed texture's base extent to be a whole number of 4x4 blocks.
            if (!half_float && desc.allow_compression && desc.width >= 4 && desc.height >= 4 &&
                desc.width % 4 == 0 && desc.height % 4 == 0 && allow_bc) {
                switch (desc.kind) {
                    case TextureKind::ColorOpaque:
                        compressed = Detail::compress_bc1_mip_chain(
                            upload_bytes, desc.width, desc.height, mip_chain.mip_levels, srgb);
                        break;
                    case TextureKind::Mask:
                        compressed = Detail::compress_bc4_mip_chain(
                            upload_bytes, desc.width, desc.height, mip_chain.mip_levels);
                        break;
                    case TextureKind::NormalMap:
                    case TextureKind::MetallicRoughness:
                        compressed = Detail::compress_bc5_mip_chain(
                            upload_bytes, desc.width, desc.height, mip_chain.mip_levels);
                        break;
                    case TextureKind::ColorAlpha:
                    default:
                        compressed = Detail::compress_bc7_mip_chain(
                            upload_bytes, desc.width, desc.height, mip_chain.mip_levels, srgb);
                        break;
                }
            }
            if (compressed) {
                prepared.format = Detail::choose_bc_format(desc.kind, srgb);
                prepared.bytes = std::move(*compressed);
            } else {
                prepared.bytes = std::move(mip_chain.data);
            }
            return prepared;
        }

        /// Whether the device takes block-compressed textures. Read on the render thread.
        [[nodiscard]] bool device_allows_bc(SFT::Renderer::Renderer &renderer) {
            RHI::RhiDevice *device = renderer.rhi_device();
            return device != nullptr && device->limits().supports_bc_texture_compression;
        }


        /// What a progressive load's worker hands the render thread, in order.
        struct ProgressiveUpdate {
            enum class Kind : u8 {
                /// The first image: full size, low detail. Replaces the 1x1 stand-in.
                First,
                /// Rectangles that reached full detail, rows packed back to back.
                Regions,
                /// The finished texture: mip chain, block compression.
                Final,
                /// The decode failed.
                Failed,
            };
            Kind kind = Kind::Failed;
            u32 width = 0;
            u32 height = 0;
            RHI::Format format = RHI::Format::Undefined;
            u32 mip_levels = 1;
            std::vector<std::byte> pixels;
            std::vector<SFT::Renderer::Renderer::TextureRegion> regions;
            f32 refined_fraction = 0.0f;
            std::string message;
        };

        /// State shared between a progressive load's worker thread and the asset manager.
        struct ProgressiveLoad {
            std::mutex mutex;
            std::deque<ProgressiveUpdate> updates;
            std::atomic<bool> cancel{false};
        };

        struct SvgSource {
            /// The file the SVG came from, or the caller's name for it when it was given as text.
            std::filesystem::path path;
            /// The SVG text when it was given in memory (nothing is read from `path`); null for a file.
            std::shared_ptr<const std::string> data;
            u32 raster_px = 0;
            std::filesystem::file_time_type loaded_time{};
            std::uintmax_t loaded_size = 0;
            std::filesystem::file_time_type seen_time{};
            std::uintmax_t seen_size = 0;
        };

        /// Edge of the squares a progressive load compares and re-uploads.
        constexpr u32 kProgressiveTile = 64;

        /// Rounds an SVG raster size up to a multiple of 4 (so block compression applies) within sane bounds.
        [[nodiscard]] u32 svg_raster_px(f32 target_px) noexcept {
            const f32 clamped = std::clamp(std::isfinite(target_px) ? target_px : 16.0f, 4.0f, 4096.0f);
            return static_cast<u32>(std::ceil(clamped / 4.0f)) * 4u;
        }

        /// Shrinks 8-bit RGBA so its longest edge is at most `limit`, averaging boxes of source pixels (no ringing, and alpha
        /// is averaged premultiplied so transparent edges do not darken).
        void downscale_rgba8(std::vector<std::byte> &pixels, u32 &width, u32 &height, u32 limit) {
            const u32 longest = std::max(width, height);
            if (limit == 0 || longest <= limit) {
                return;
            }
            const f64 scale = static_cast<f64>(limit) / longest;
            const u32 out_w = std::max<u32>(1, static_cast<u32>(std::lround(width * scale)));
            const u32 out_h = std::max<u32>(1, static_cast<u32>(std::lround(height * scale)));
            std::vector<std::byte> out(static_cast<usize>(out_w) * out_h * 4);
            const auto *in = reinterpret_cast<const u8 *>(pixels.data());
            for (u32 y = 0; y < out_h; ++y) {
                const u32 y0 = static_cast<u32>(static_cast<u64>(y) * height / out_h);
                const u32 y1 = std::max(y0 + 1, static_cast<u32>(static_cast<u64>(y + 1) * height / out_h));
                for (u32 x = 0; x < out_w; ++x) {
                    const u32 x0 = static_cast<u32>(static_cast<u64>(x) * width / out_w);
                    const u32 x1 = std::max(x0 + 1, static_cast<u32>(static_cast<u64>(x + 1) * width / out_w));
                    f64 r = 0, g = 0, b = 0, a = 0;
                    for (u32 sy = y0; sy < y1; ++sy) {
                        for (u32 sx = x0; sx < x1; ++sx) {
                            const u8 *px = in + (static_cast<usize>(sy) * width + sx) * 4;
                            const f64 alpha = px[3];
                            r += px[0] * alpha;
                            g += px[1] * alpha;
                            b += px[2] * alpha;
                            a += alpha;
                        }
                    }
                    const f64 count = static_cast<f64>(y1 - y0) * (x1 - x0);
                    auto *dst = reinterpret_cast<u8 *>(out.data()) + (static_cast<usize>(y) * out_w + x) * 4;
                    if (a > 0) {
                        dst[0] = static_cast<u8>(std::lround(r / a));
                        dst[1] = static_cast<u8>(std::lround(g / a));
                        dst[2] = static_cast<u8>(std::lround(b / a));
                    } else {
                        dst[0] = dst[1] = dst[2] = 0;
                    }
                    dst[3] = static_cast<u8>(std::lround(a / count));
                }
            }
            pixels = std::move(out);
            width = out_w;
            height = out_h;
        }

        /// The body of a progressive load, run on the asset manager's decode thread. `max_dimension` (0 = none) shrinks the
        /// finished image to that longest edge before it is uploaded; the low-detail intermediate passes are skipped then.
        void run_progressive_load(const std::shared_ptr<ProgressiveLoad> &load, const std::filesystem::path &source,
                                  std::vector<std::byte> encoded_bytes, TextureColorSpace color_space, TextureKind kind,
                                  bool allow_bc, u32 max_dimension = 0) {
            const auto push = [&](ProgressiveUpdate update) {
                std::lock_guard lock{load->mutex};
                load->updates.push_back(std::move(update));
            };
            const auto fail = [&](std::string message) {
                ProgressiveUpdate update;
                update.kind = ProgressiveUpdate::Kind::Failed;
                update.message = std::move(message);
                push(std::move(update));
            };
            if (load->cancel.load(std::memory_order_acquire)) {
                return;
            }
            // A path is read here, on the worker; bytes the caller already holds (a download, an embedded picture) are used as is.
            AssetExpected<std::vector<std::byte>> encoded =
                encoded_bytes.empty() ? read_binary_file(source) : AssetExpected<std::vector<std::byte>>{std::move(encoded_bytes)};
            if (!encoded) {
                fail(std::string{encoded.error().message.c_str()});
                return;
            }

            const bool srgb = color_space == TextureColorSpace::Srgb;
            std::vector<std::byte> shown;
            std::vector<u8> refined;
            u32 tiles_x = 0;
            u32 tiles_y = 0;
            u32 refined_count = 0;
            bool sent_first = false;

            const Detail::ProgressiveSink sink = [&](u32 width, u32 height, std::span<const std::byte> rgba8,
                                                     bool is_final) {
                if (load->cancel.load(std::memory_order_acquire)) {
                    return false;
                }
                if (is_final) {
                    return true; // the finished image replaces everything below
                }
                if (!sent_first) {
                    sent_first = true;
                    shown.assign(rgba8.begin(), rgba8.end());
                    tiles_x = (width + kProgressiveTile - 1) / kProgressiveTile;
                    tiles_y = (height + kProgressiveTile - 1) / kProgressiveTile;
                    refined.assign(static_cast<usize>(tiles_x) * tiles_y, 0);
                    ProgressiveUpdate update;
                    update.kind = ProgressiveUpdate::Kind::First;
                    update.width = width;
                    update.height = height;
                    update.format = srgb ? RHI::Format::RGBA8UnormSrgb : RHI::Format::RGBA8Unorm;
                    update.pixels.assign(rgba8.begin(), rgba8.end());
                    push(std::move(update));
                    return true;
                }

                // Which tiles differ from what the render thread was last given?
                const auto tile_rect = [&](u32 tx, u32 ty) {
                    SFT::Renderer::Renderer::TextureRegion region;
                    region.x = tx * kProgressiveTile;
                    region.y = ty * kProgressiveTile;
                    region.width = std::min(kProgressiveTile, width - region.x);
                    region.height = std::min(kProgressiveTile, height - region.y);
                    return region;
                };
                std::vector<SFT::Renderer::Renderer::TextureRegion> regions;
                for (u32 ty = 0; ty < tiles_y; ++ty) {
                    u32 run_start = 0;
                    bool in_run = false;
                    const auto close_run = [&](u32 run_end_exclusive) {
                        SFT::Renderer::Renderer::TextureRegion first = tile_rect(run_start, ty);
                        const SFT::Renderer::Renderer::TextureRegion last = tile_rect(run_end_exclusive - 1, ty);
                        first.width = last.x + last.width - first.x;
                        // Same columns as the region just above it: one taller region instead of two.
                        if (!regions.empty() && regions.back().x == first.x && regions.back().width == first.width &&
                            regions.back().y + regions.back().height == first.y) {
                            regions.back().height += first.height;
                        } else {
                            regions.push_back(first);
                        }
                    };
                    for (u32 tx = 0; tx < tiles_x; ++tx) {
                        const SFT::Renderer::Renderer::TextureRegion tile = tile_rect(tx, ty);
                        bool dirty = false;
                        for (u32 row = 0; row < tile.height && !dirty; ++row) {
                            const usize offset = (static_cast<usize>(tile.y + row) * width + tile.x) * 4u;
                            dirty = std::memcmp(shown.data() + offset, rgba8.data() + offset,
                                                static_cast<usize>(tile.width) * 4u) != 0;
                        }
                        if (dirty) {
                            u8 &flag = refined[static_cast<usize>(ty) * tiles_x + tx];
                            if (flag == 0) {
                                flag = 1;
                                ++refined_count;
                            }
                            if (!in_run) {
                                in_run = true;
                                run_start = tx;
                            }
                        } else if (in_run) {
                            in_run = false;
                            close_run(tx);
                        }
                    }
                    if (in_run) {
                        close_run(tiles_x);
                    }
                }
                if (regions.empty()) {
                    return true;
                }
                ProgressiveUpdate update;
                update.kind = ProgressiveUpdate::Kind::Regions;
                update.refined_fraction =
                    static_cast<f32>(refined_count) / static_cast<f32>(std::max<usize>(refined.size(), 1));
                for (const SFT::Renderer::Renderer::TextureRegion &region : regions) {
                    for (u32 row = 0; row < region.height; ++row) {
                        const usize offset = (static_cast<usize>(region.y + row) * width + region.x) * 4u;
                        const usize bytes = static_cast<usize>(region.width) * 4u;
                        update.pixels.insert(update.pixels.end(), rgba8.begin() + offset, rgba8.begin() + offset + bytes);
                        std::memcpy(shown.data() + offset, rgba8.data() + offset, bytes);
                    }
                }
                update.regions = std::move(regions);
                push(std::move(update));
                return true;
            };

            Detail::DecodeOptions decode_options;
            decode_options.preview_min_dimension = max_dimension;
            auto image = max_dimension != 0 ? Detail::decode_image(*encoded, decode_options, source)
                                            : Detail::decode_image_progressive(*encoded, decode_options, sink, source);
            if (image && max_dimension != 0 && !image->is_preview) {
                downscale_rgba8(image->pixels(), image->width, image->height, max_dimension);
            }
            if (!image) {
                if (!load->cancel.load(std::memory_order_acquire)) {
                    fail(std::string{image.error().message.c_str()});
                }
                return;
            }
            std::vector<std::byte>{}.swap(*encoded);
            TextureAssetDesc desc{
                .width = image->width,
                .height = image->height,
                .color_space = color_space,
                .pixels = std::move(image->pixels()),
                .kind = kind,
            };
            auto prepared = prepare_texture(desc, allow_bc);
            if (!prepared) {
                fail(std::string{prepared.error().message.c_str()});
                return;
            }
            ProgressiveUpdate update;
            update.kind = ProgressiveUpdate::Kind::Final;
            update.width = prepared->width;
            update.height = prepared->height;
            update.format = prepared->format;
            update.mip_levels = prepared->mip_levels;
            update.pixels = std::move(prepared->bytes);
            push(std::move(update));
        }


        /// Performs the combine stable ID operation for `Engine` using the supplied arguments.
        ///
        /// @param object `object` value used by the operation.
        /// @param primitive `primitive` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 combine_stable_id(u64 object, usize primitive) noexcept {


            const u64 value = static_cast<u64>(primitive) + 0x9e3779b97f4a7c15ULL;
            return object ^ (value + (object << 6u) + (object >> 2u));
        }

    } // namespace

    struct AssetManager::Impl {
        struct ShaderData {
            SFT::Renderer::MaterialTemplateHandle material_template{};
        };

        /// Playback state of an animated image (GIF, APNG, animated WebP/AVIF/JXL): every frame decoded, the clock decides
        /// which one the texture currently shows.
        struct TextureAnimation {
            std::vector<std::vector<std::byte>> frames; // Rgba8
            std::vector<u32> duration_ms;
            u64 total_ms = 0;
            u32 loop_count = 0; // 0 = forever
            std::chrono::steady_clock::time_point start{};
            usize shown = 0;
            bool playing = true;
            /// Time already played when paused, so resuming continues where it stopped.
            u64 paused_at_ms = 0;
        };

        /// Where a managed texture's image comes from, and what the manager does about it: reload when the file changes,
        /// reload after being evicted, shrink while decoding.
        struct ManagedSource {
            std::string key;
            std::filesystem::path path;
            std::shared_ptr<const std::vector<std::byte>> bytes;
            TextureColorSpace color_space = TextureColorSpace::Srgb;
            TextureKind kind = TextureKind::ColorAlpha;
            u32 max_dimension = 0;
            bool watch = true;
            bool evictable = true;
            /// The file as it was when the current load started, and as it was at the last look (a change counts once it
            /// has stopped moving: a screenshot still being written is not reloaded half-way).
            std::filesystem::file_time_type loaded_time{};
            std::uintmax_t loaded_size = 0;
            std::filesystem::file_time_type seen_time{};
            std::uintmax_t seen_size = 0;
            bool evicted = false;
            std::chrono::steady_clock::time_point last_drawn = std::chrono::steady_clock::now();
        };

        struct TextureData {
            SFT::Renderer::TextureHandle texture{};
            std::optional<ManagedSource> managed;
            std::optional<TextureAnimation> animation;
            TextureAssetInfo info{};
            bool nearest_sampling = false;
            TextureKind kind = TextureKind::ColorAlpha;
            /// Set for textures made by `load_svg`: the vector source the raster comes from.
            std::optional<SvgSource> svg;
            /// Set while a progressive load is still delivering.
            std::shared_ptr<ProgressiveLoad> progressive;
            TextureDetail detail = TextureDetail::Complete;
            f32 refined_fraction = 1.0f;
            u64 version = 1;
        };

        struct SoundData {
            /// Decoded audio with the cue points and loop region the file carried, shared with the mixer (never copied).
            std::shared_ptr<const Audio::SampleBuffer> buffer;
            SoundAssetInfo info{};
        };

        struct FileData {
            std::shared_ptr<std::vector<std::byte>> bytes;
        };

        struct ModelPrimitiveData {
            SFT::Renderer::MeshHandle mesh{};
            SFT::Renderer::MaterialInstanceHandle material{};
            Asset shader{};
            std::vector<Asset> textures;
            bool double_sided = false;
            /// Skinning source: bind-pose vertices (the renderer's copy is overwritten by posing) and weights.
            std::shared_ptr<const Animation::SkinWeights> skin;
            std::shared_ptr<const Animation::MorphTargetSet> morph;
            /// The skin was invented (all vertices on joint 0) so a morph-only primitive can use the skinning pass.
            bool synthetic_skin = false;
            std::shared_ptr<const std::vector<SFT::Renderer::GeometryVertex>> bind_vertices;
            /// Cloned primitives reference the source model's material instance instead of owning one.
            bool shares_material = false;
            /// Posed by the renderer's compute pass (`Renderer::attach_skin`) instead of on the CPU.
            bool gpu_skinned = false;
        };

        struct ModelData {
            std::vector<ModelPrimitiveData> primitives;
            ModelAssetInfo info{};
        };

        using Data = std::variant<std::monostate, ModelData, ShaderData, SoundData, TextureData, FileData>;

        struct Record {
            AssetType type = AssetType::Invalid;
            u32 generation = 1;
            bool loaded = false;
            UString label;
            std::filesystem::path source;
            usize memory_bytes = 0;
            Data data;
        };

        /// Performs the impl operation for `AssetManager` using the supplied arguments.
        ///
        /// @param renderer_ref Renderer used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit Impl(SFT::Renderer::Renderer &renderer_ref)
            : renderer(renderer_ref), owner(next_asset_manager_id.fetch_add(1, std::memory_order_relaxed)) {
            if (owner == 0) {
                owner = next_asset_manager_id.fetch_add(1, std::memory_order_relaxed);
            }
        }

        /// Resolves the asset associated with the supplied key, handle, or resource.
        ///
        /// @param index Zero-based index of the target element or entry.
        /// @param record `record` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Asset asset_for(usize index, const Record &record) const noexcept {
            return Asset{owner, static_cast<u64>(index + 1), record.generation, record.type};
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Record *find(Asset asset) noexcept {
            if (!asset || asset.owner_ != owner || asset.id_ > records.size()) {
                return nullptr;
            }
            Record &record = records[static_cast<usize>(asset.id_ - 1)];
            if (!record.loaded || record.generation != asset.generation_ || record.type != asset.type_) {
                return nullptr;
            }
            return &record;
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Record *find(Asset asset) const noexcept {
            if (!asset || asset.owner_ != owner || asset.id_ > records.size()) {
                return nullptr;
            }
            const Record &record = records[static_cast<usize>(asset.id_ - 1)];
            if (!record.loaded || record.generation != asset.generation_ || record.type != asset.type_) {
                return nullptr;
            }
            return &record;
        }

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Asset insert(Record record) {
            record.loaded = true;
            note_model_references(record, +1);
            usize index = 0;
            if (!free_records.empty()) {
                // The slot of a destroyed model, shader, sound or file: it comes back under a newer generation, so handles to what it
                // held are stale. (Texture slots are never reused: the streaming lists below hold raw indices of those.)
                index = free_records.back();
                free_records.pop_back();
                record.generation = records[index].generation;
                records[index] = std::move(record);
            } else {
                records.push_back(std::move(record));
                index = records.size() - 1;
            }
            ++loaded_count;
            return asset_for(index, records[index]);
        }

        /// What a model asset needs to stay loaded: its shaders and textures. Counted exactly, so unloading a shader or texture asks one
        /// table instead of scanning every asset for a model that uses it.
        [[nodiscard]] static u64 reference_key(const Asset &asset) noexcept {
            return (static_cast<u64>(asset.generation_) << 40) | asset.id_;
        }

        void note_model_references(const Record &record, int direction) {
            const auto *model = std::get_if<ModelData>(&record.data);
            if (model == nullptr) {
                return;
            }
            const auto note = [&](const Asset &asset) {
                if (!asset) {
                    return;
                }
                if (direction > 0) {
                    ++model_references[reference_key(asset)];
                    return;
                }
                // Dropping a reference never allocates (`destroy` must not throw).
                const auto found = model_references.find(reference_key(asset));
                if (found == model_references.end()) {
                    return;
                }
                if (found->second <= 1) {
                    model_references.erase(found);
                } else {
                    --found->second;
                }
            };
            for (const ModelPrimitiveData &primitive : model->primitives) {
                note(primitive.shader);
                for (const Asset &texture : primitive.textures) {
                    note(texture);
                }
            }
        }

        [[nodiscard]] bool referenced_by_a_model(const Asset &asset) const {
            return model_references.contains(reference_key(asset));
        }

        /// Destroys or releases the `AssetManager` resource represented by the supplied parameters.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(Record &record) noexcept {
            if (!record.loaded) {
                return;
            }
            note_model_references(record, -1);
            if (auto *model = std::get_if<ModelData>(&record.data)) {
                for (ModelPrimitiveData &primitive : model->primitives) {
                    if (primitive.material && !primitive.shares_material) {
                        renderer.destroy_material_instance(primitive.material);
                    }
                    if (primitive.mesh) {
                        renderer.destroy_mesh(primitive.mesh);
                    }
                }
            } else if (auto *texture = std::get_if<TextureData>(&record.data)) {
                if (texture->progressive) {
                    texture->progressive->cancel.store(true, std::memory_order_release);
                }
                if (texture->managed) {
                    managed_by_handle.erase(texture->texture.value);
                    path_cache.erase(texture->managed->key);
                }
                if (texture->svg) {
                    svg_records.erase(texture->texture.value);
                    if (texture->svg->data) {
                        path_cache.erase("svg-data|" + texture->svg->path.string());
                    }
                }
                if (texture->texture) {
                    renderer.destroy_texture(texture->texture);
                }
            } else if (auto *shader = std::get_if<ShaderData>(&record.data)) {
                if (shader->material_template) {
                    renderer.destroy_material_template(shader->material_template);
                }
            }


            if (!record.source.empty()) {
                const std::string source_string = record.source.string();
                path_cache.erase(source_string);
                path_cache.erase(source_string + "|srgb");
                path_cache.erase(source_string + "|linear");
                path_cache.erase("svg|" + source_string);
                path_cache.erase("progressive|" + source_string + "|srgb");
                path_cache.erase("progressive|" + source_string + "|linear");
            }

            const AssetType destroyed_type = record.type;
            record.data = std::monostate{};
            record.loaded = false;
            record.memory_bytes = 0;
            record.label.clear();
            record.source.clear();
            ++record.generation;
            if (record.generation == 0) {
                record.generation = 1;
            }
            --loaded_count;
            if (destroyed_type != AssetType::Texture) {
                free_records.push_back(static_cast<usize>(&record - records.data()));
            }
        }

        /// Applies what the progressive loads' workers have produced; see `pump_texture_streaming`.
        void pump_progressive();
        /// Moves animated images on to the frame the clock says they are at.
        void advance_animations();
        /// Starts (or restarts) the decode of a managed texture's source; the old pixels keep showing until the new ones arrive.
        void start_load_locked(usize index);
        /// Reloads managed textures and SVG files whose files changed on disk.
        void watch_sources();
        /// Evicts the least recently drawn managed textures beyond `residency_limit`.
        void enforce_residency();
        /// Grows SVG rasters to the size the UI actually drew them at.
        void apply_texture_demand();
        AssetResult replace_locked(Record &record, TextureData &data, TextureAssetDesc desc);
        AssetResult rasterize_svg_locked(Record &record, TextureData &data, u32 px);

        SFT::Renderer::Renderer &renderer;
        u64 owner = 0;
        mutable std::shared_mutex mutex;
        std::vector<Record> records;
        /// Slots of destroyed non-texture records, to be reused by `insert`.
        std::vector<usize> free_records;
        usize loaded_count = 0;
        /// How many loaded models use each shader and texture (see `note_model_references`).
        std::unordered_map<u64, u32> model_references;


        std::unordered_map<std::string, Asset> path_cache;
        /// Records with a progressive load in flight, so the per-frame pump does not scan every asset.
        std::vector<usize> active_progressive;
        /// Records holding an animated image, so the per-frame advance does not scan every asset.
        std::vector<usize> animated;
        /// Records holding a managed texture, and the renderer handle -> record index for them (UI draw demand finds them).
        std::vector<usize> managed_list;
        std::unordered_map<u64, usize> managed_by_handle;
        usize residency_limit = 0;
        u64 frame = 0;
        std::chrono::steady_clock::time_point last_watch{};
        /// Renderer texture handle -> record index, for the SVG textures (so the UI's drawn sizes can find them).
        std::unordered_map<u64, usize> svg_records;
        /// Declared last: destroyed first, so its worker has stopped before anything the worker's jobs touch goes away.
        Async::DedicatedThread decode_thread{"AssetDecode"};
    };

    /// Performs the asset manager operation for `Engine` using the supplied arguments.
    ///
    /// @param renderer Renderer used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    AssetManager::AssetManager(SFT::Renderer::Renderer &renderer)
        : impl_(std::make_unique<Impl>(renderer)) {}

    /// Destroys the `Engine` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    AssetManager::~AssetManager() {
        clear();
    }

    /// Loads shader.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`, `AssetErrorCode::NotFound`.
    AssetExpected<Asset> AssetManager::load_shader(ShaderAssetDesc desc) {
        SFT_TRACE_SCOPE("AssetManager::load_shader");
        const Foundation::Stopwatch stopwatch;
        const std::string source_for_log = desc.source.string();
        if (desc.source.empty()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "A shader asset requires a source path."));
        }


        if (!std::filesystem::exists(desc.source) && Core::Slang::find_embedded_shader(desc.source.string()) == nullptr) {
            return std::unexpected(error(AssetErrorCode::NotFound,
                                         "Shader source does not exist on disk and has no embedded fallback: '" + desc.source.string() + "'.",
                                         desc.source));
        }
        if (desc.label.empty()) {
            desc.label = path_label(desc.source, "shader");
        }
        if (desc.module_name.empty()) {
            desc.module_name = path_label(desc.source.stem(), "shader_module");
        }
        if (desc.vertex_entry_point.empty()) {
            desc.vertex_entry_point = UString{"vertexMain"_ustr};
        }
        if (desc.fragment_entry_point.empty()) {
            desc.fragment_entry_point = UString{"fragmentMain"_ustr};
        }

        Core::Slang::ShaderCompileOptions options{};
        options.entry_points = {
            Core::Slang::ShaderEntryPointRequest{
                .name = desc.vertex_entry_point.cpp_string(),
                .stage = Core::Slang::ShaderStage::Vertex,
            },
            Core::Slang::ShaderEntryPointRequest{
                .name = desc.fragment_entry_point.cpp_string(),
                .stage = Core::Slang::ShaderStage::Fragment,
            },
        };
        if (!desc.depth_only_fragment_entry_point.empty()) {


            options.entry_points.push_back(Core::Slang::ShaderEntryPointRequest{
                .name = desc.depth_only_fragment_entry_point.cpp_string(),
                .stage = Core::Slang::ShaderStage::Fragment,
            });
        }
        options.macros.reserve(desc.defines.size());
        for (const ShaderDefine &define : desc.defines) {
            options.macros.push_back(Core::Slang::ShaderMacro{
                .name = define.name.cpp_string(),
                .value = define.value.cpp_string(),
            });
        }

        const std::string source_path = desc.source.string();
        auto material_template = impl_->renderer.create_material_template_from_source(
            Core::Slang::ShaderSource::from_file(source_path, desc.module_name.cpp_string()),
            options,
            desc.label.c_str());
        if (!material_template) {
            return std::unexpected(backend_error(material_template.error(), desc.source));
        }

        usize source_bytes = 0;
        std::error_code size_error;
        const auto size = std::filesystem::file_size(desc.source, size_error);
        if (!size_error && size <= std::numeric_limits<usize>::max()) {
            source_bytes = static_cast<usize>(size);
        }

        Foundation::log_info("AssetManager: loaded shader '{}' ({} bytes) in {}", source_for_log, source_bytes, stopwatch.elapsed_human());
        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
            .type = AssetType::Shader,
            .label = std::move(desc.label),
            .source = std::move(desc.source),
            .memory_bytes = source_bytes,
            .data = Impl::ShaderData{.material_template = *material_template},
        });
    }

    /// Loads shader.
    ///
    /// @param source Source value or resource.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::load_shader(const std::filesystem::path &source, UString label) {
        return load_shader(ShaderAssetDesc{.source = source, .label = std::move(label)});
    }

    /// Creates a texture from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`.
    AssetExpected<Asset> AssetManager::create_texture(TextureAssetDesc desc) {
        if (desc.label.empty()) {
            desc.label = UString{"texture"_ustr};
        }
        const u32 width = desc.width;
        const u32 height = desc.height;
        const TextureColorSpace color_space = desc.color_space;
        const bool nearest_sampling = desc.nearest_sampling;
        const TextureKind kind = desc.kind;
        UString label = desc.label;
        auto prepared = prepare_texture(desc, device_allows_bc(impl_->renderer));
        if (!prepared) {
            return std::unexpected(prepared.error());
        }

        auto texture = impl_->renderer.create_texture(
            prepared->width,
            prepared->height,
            prepared->format,
            std::span<const std::byte>{prepared->bytes},
            label.c_str(),
            {},
            prepared->mip_levels,
            nearest_sampling);
        if (!texture) {
            return std::unexpected(backend_error(texture.error()));
        }

        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
            .type = AssetType::Texture,
            .label = std::move(label),
            .memory_bytes = prepared->bytes.size(),
            .data = Impl::TextureData{
                .texture = *texture,
                .info = TextureAssetInfo{
                    .width = width,
                    .height = height,
                    .color_space = color_space,
                },
                .nearest_sampling = nearest_sampling,
                .kind = kind,
            },
        });
    }

    /// Creates a orm texture from the supplied parameters.
    ///
    /// @param occlusion_rgba8 `occlusion_rgba8` value used by the operation.
    /// @param metallic_roughness_rgba8 `metallic_roughness_rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`.
    AssetExpected<Asset> AssetManager::create_orm_texture(
        std::span<const std::byte> occlusion_rgba8, std::span<const std::byte> metallic_roughness_rgba8,
        u32 width, u32 height, UString label) {
        auto packed = Detail::pack_orm_rgba8(occlusion_rgba8, metallic_roughness_rgba8, width, height);
        if (!packed) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "Could not pack occlusion + metallic-roughness textures into one "
                                         "ORM texture (dimensions must match exactly)."));
        }
        if (label.empty()) {
            label = UString{"orm_texture"_ustr};
        }


        return create_texture(TextureAssetDesc{
            .width = width,
            .height = height,
            .color_space = TextureColorSpace::Linear,
            .pixels = std::move(*packed),
            .label = std::move(label),
            .kind = TextureKind::ColorAlpha,
        });
    }

    /// Loads texture.
    ///
    /// @param source Source value or resource.
    /// @param color_space `color_space` value used by the operation.
    /// @param kind `kind` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::load_texture(const std::filesystem::path &source,
                                                    TextureColorSpace color_space,
                                                    TextureKind kind,
                                                    UString label,
                                                    TextureDynamicRange dynamic_range) {
        SFT_TRACE_SCOPE("AssetManager::load_texture");
        // The dynamic range is part of the key: the same file loaded as SDR and as HDR produces
        // genuinely different textures (different format, different VRAM cost), so sharing one
        // cache entry between them would hand the second caller the first one's result.
        const std::string dedup_key = source.string() +
            (color_space == TextureColorSpace::Srgb ? "|srgb" : "|linear") +
            (dynamic_range == TextureDynamicRange::Hdr ? "|hdr" : "|sdr") +
            "|kind" + std::to_string(static_cast<int>(kind));
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(dedup_key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }

        const Foundation::Stopwatch stopwatch;
        auto encoded = read_binary_file(source);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto decoded = Detail::decode_image(*encoded, decode_options_for(dynamic_range), source);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }

        if (label.empty()) {
            label = path_label(source, "texture");
        }
        const u32 decoded_width = decoded->width;
        const u32 decoded_height = decoded->height;
        auto texture = create_texture(TextureAssetDesc{
            .width = decoded->width,
            .height = decoded->height,
            .color_space = texture_color_space_for(dynamic_range, color_space),
            .format = texture_pixel_format_for(dynamic_range),
            .pixels = std::move(decoded->pixels()),
            .label = std::move(label),
            .kind = kind,
        });
        if (!texture) {
            return texture;
        }
        Foundation::log_info("AssetManager: loaded texture '{}' ({}x{}) in {}",
                             source.string(),
                             decoded_width,
                             decoded_height,
                             stopwatch.elapsed_human());

        std::unique_lock lock{impl_->mutex};
        if (Impl::Record *record = impl_->find(*texture)) {
            record->source = source;
        }
        impl_->path_cache.emplace(dedup_key, *texture);
        return texture;
    }

    /// Loads texture streamed.
    ///
    /// @param source Source value or resource.
    /// @param color_space `color_space` value used by the operation.
    /// @param kind `kind` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::IoFailure`.
    AssetExpected<Asset> AssetManager::load_texture_streamed(const std::filesystem::path &source,
                                                              TextureColorSpace color_space, TextureKind kind,
                                                              UString label) {
        SFT_TRACE_SCOPE("AssetManager::load_texture_streamed");
        // One path for everything shown on screen: the progressive pipeline gives the same contract (a usable asset at
        // once, block-compressed with its mips when the worker is done) and also shows the image sooner.
        return load_texture_progressive(source, color_space, std::move(label), false, kind);
    }

    /// Pumps texture streaming using the supplied arguments and current state.
    ///
    /// @return Returns the current pump texture streaming value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void AssetManager::pump_texture_streaming() {
        impl_->apply_texture_demand();
        impl_->pump_progressive();
        impl_->advance_animations();
        impl_->watch_sources();
        impl_->enforce_residency();
    }

    void AssetManager::Impl::advance_animations() {
        if (animated.empty()) {
            return;
        }
        std::unique_lock lock{mutex};
        const auto now = std::chrono::steady_clock::now();
        for (usize slot = 0; slot < animated.size();) {
            Record &record = records[animated[slot]];
            auto *data = record.loaded ? std::get_if<TextureData>(&record.data) : nullptr;
            if (data == nullptr || !data->animation) {
                animated[slot] = animated.back(); // destroyed or replaced by a still
                animated.pop_back();
                continue;
            }
            ++slot;
            TextureAnimation &animation = *data->animation;
            if (!animation.playing || animation.total_ms == 0) {
                continue;
            }
            const u64 elapsed = animation.paused_at_ms + static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                              now - animation.start).count());
            const u64 cycle = elapsed / animation.total_ms;
            usize target = animation.frames.size() - 1;
            if (animation.loop_count == 0 || cycle < animation.loop_count) {
                u64 into = elapsed % animation.total_ms;
                for (usize frame = 0; frame < animation.frames.size(); ++frame) {
                    if (into < animation.duration_ms[frame]) {
                        target = frame;
                        break;
                    }
                    into -= animation.duration_ms[frame];
                }
            } else {
                animation.playing = false; // played its last loop: rests on the final frame
            }
            if (target == animation.shown) {
                continue;
            }
            auto replaced = replace_locked(record, *data,
                                           TextureAssetDesc{.width = data->info.width,
                                                            .height = data->info.height,
                                                            .color_space = data->info.color_space,
                                                            .pixels = animation.frames[target],
                                                            .allow_compression = false,
                                                            .generate_mipmaps = false});
            if (!replaced) {
                Foundation::log_warn("AssetManager: could not show animation frame {}: {}", target, replaced.error().message);
                continue;
            }
            animation.shown = target;
        }
    }

    AssetExpected<Asset> AssetManager::create_texture_animated_from_bytes(std::string_view name, std::vector<std::byte> encoded,
                                                                          TextureColorSpace color_space, UString label) {
        const std::string key = "anim|" + std::string{name} + (color_space == TextureColorSpace::Srgb ? "|srgb" : "|linear");
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }
        Detail::DecodeOptions options;
        options.decode_all_frames = true;
        auto decoded = Detail::decode_image(encoded, options, std::filesystem::path{std::string{name}});
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        const usize frame_count = decoded->frames.size();
        if (label.empty()) {
            label = UString{std::string{name}};
        }
        auto first = std::move(decoded->frames.front().pixels);
        auto texture = create_texture(TextureAssetDesc{.width = decoded->width,
                                                       .height = decoded->height,
                                                       .color_space = color_space,
                                                       .pixels = frame_count > 1 ? first : std::move(first),
                                                       .label = std::move(label),
                                                       .allow_compression = frame_count == 1,
                                                       .generate_mipmaps = frame_count == 1});
        if (!texture) {
            return texture;
        }
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(*texture);
        auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data != nullptr && frame_count > 1) {
            Impl::TextureAnimation animation;
            animation.loop_count = decoded->loop_count;
            animation.frames.push_back(std::move(first));
            for (usize i = 1; i < frame_count; ++i) {
                animation.frames.push_back(std::move(decoded->frames[i].pixels));
            }
            for (const Detail::ImageFrame &frame : decoded->frames) {
                // A frame with no delay would spin the animation; browsers treat it as 100 ms.
                const u32 delay = frame.duration_ms == 0 ? 100u : frame.duration_ms;
                animation.duration_ms.push_back(delay);
                animation.total_ms += delay;
            }
            animation.start = std::chrono::steady_clock::now();
            data->animation = std::move(animation);
            impl_->animated.push_back(static_cast<usize>(texture->id_ - 1));
        }
        if (record != nullptr) {
            record->source = key;
        }
        impl_->path_cache.emplace(key, *texture);
        return texture;
    }

    AssetExpected<Asset> AssetManager::load_texture_animated(const std::filesystem::path &source,
                                                             TextureColorSpace color_space, UString label) {
        auto bytes = read_binary_file(source);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return create_texture_animated_from_bytes(source.string(), std::move(*bytes), color_space, std::move(label));
    }

    AssetResult AssetManager::set_animation_playing(Asset asset, bool playing) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(asset);
        auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "set_animation_playing requires a live texture asset."));
        }
        if (!data->animation || data->animation->playing == playing) {
            return {};
        }
        Impl::TextureAnimation &animation = *data->animation;
        const auto now = std::chrono::steady_clock::now();
        if (playing) {
            animation.start = now;
        } else {
            animation.paused_at_ms += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - animation.start).count());
        }
        animation.playing = playing;
        return {};
    }

    bool AssetManager::is_animated(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        return data != nullptr && data->animation.has_value();
    }

    void AssetManager::Impl::apply_texture_demand() {
        const auto demand = renderer.take_texture_demand();
        ++frame;
        if (demand.empty()) {
            return;
        }
        std::unique_lock lock{mutex};
        // Drawn textures are in use: note when, and bring back one that was evicted.
        if (!managed_by_handle.empty()) {
            const auto now = std::chrono::steady_clock::now();
            for (const auto &[handle, pixels] : demand) {
                const auto found = managed_by_handle.find(handle.value);
                if (found == managed_by_handle.end()) {
                    continue;
                }
                auto *data = records[found->second].loaded ? std::get_if<TextureData>(&records[found->second].data) : nullptr;
                if (data == nullptr || !data->managed) {
                    continue;
                }
                data->managed->last_drawn = now;
                if (data->managed->evicted) {
                    start_load_locked(found->second);
                }
            }
        }
        if (svg_records.empty()) {
            return;
        }
        // A few re-rasterisations a frame at most: a screen of icons that all need growing spreads over a few frames.
        u32 budget = 4;
        for (const auto &[handle, pixels] : demand) {
            const auto found = svg_records.find(handle.value);
            if (found == svg_records.end() || budget == 0) {
                continue;
            }
            Record &record = records[found->second];
            auto *data = record.loaded ? std::get_if<TextureData>(&record.data) : nullptr;
            if (data == nullptr || !data->svg) {
                continue;
            }
            // Only ever bigger, and only when the raster is actually too small for what was drawn.
            const u32 wanted = svg_raster_px(pixels);
            if (wanted <= data->svg->raster_px) {
                continue;
            }
            --budget;
            if (auto grown = rasterize_svg_locked(record, *data, wanted); !grown) {
                Foundation::log_warn("AssetManager: could not re-rasterise '{}': {}", data->svg->path.string(),
                                     grown.error().message);
            }
        }
    }

    void AssetManager::Impl::start_load_locked(usize index) {
        Record &record = records[index];
        auto *data = record.loaded ? std::get_if<TextureData>(&record.data) : nullptr;
        if (data == nullptr || !data->managed) {
            return;
        }
        ManagedSource &managed = *data->managed;
        if (data->progressive) {
            data->progressive->cancel.store(true, std::memory_order_release);
        }
        auto load = std::make_shared<ProgressiveLoad>();
        data->progressive = load;
        if (std::ranges::find(active_progressive, index) == active_progressive.end()) {
            active_progressive.push_back(index);
        }
        if (!managed.path.empty()) {
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(managed.path, ec);
            const auto size = ec ? 0 : std::filesystem::file_size(managed.path, ec);
            managed.loaded_time = ec ? std::filesystem::file_time_type{} : time;
            managed.loaded_size = ec ? 0 : size;
            managed.seen_time = managed.loaded_time;
            managed.seen_size = managed.loaded_size;
        }
        managed.evicted = false;
        managed.last_drawn = std::chrono::steady_clock::now();
        std::vector<std::byte> bytes;
        if (managed.bytes) {
            bytes = *managed.bytes;
        }
        const bool allow_bc = device_allows_bc(renderer);
        (void)decode_thread.run([load, path = managed.path, bytes = std::move(bytes), cs = managed.color_space,
                                 kind = managed.kind, allow_bc, max = managed.max_dimension]() mutable {
            run_progressive_load(load, path, std::move(bytes), cs, kind, allow_bc, max);
        });
    }

    void AssetManager::Impl::watch_sources() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_watch < std::chrono::milliseconds(500)) {
            return;
        }
        last_watch = now;
        std::unique_lock lock{mutex};
        for (usize slot = 0; slot < managed_list.size();) {
            const usize index = managed_list[slot];
            Record &record = records[index];
            auto *data = record.loaded ? std::get_if<TextureData>(&record.data) : nullptr;
            if (data == nullptr || !data->managed) {
                managed_list[slot] = managed_list.back();
                managed_list.pop_back();
                continue;
            }
            ++slot;
            ManagedSource &managed = *data->managed;
            if (!managed.watch || managed.path.empty() || managed.evicted) {
                continue;
            }
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(managed.path, ec);
            if (ec) {
                continue;
            }
            const auto size = std::filesystem::file_size(managed.path, ec);
            if (ec) {
                continue;
            }
            const bool differs = time != managed.loaded_time || size != managed.loaded_size;
            const bool still = time == managed.seen_time && size == managed.seen_size;
            managed.seen_time = time;
            managed.seen_size = size;
            if (differs && still) {
                Foundation::log_info("AssetManager: '{}' changed on disk; reloading.", managed.path.string());
                start_load_locked(index);
            }
        }
        for (const auto &[handle, index] : svg_records) {
            Record &record = records[index];
            auto *data = record.loaded ? std::get_if<TextureData>(&record.data) : nullptr;
            if (data == nullptr || !data->svg || data->svg->data) {
                continue;
            }
            SvgSource &svg = *data->svg;
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(svg.path, ec);
            if (ec) {
                continue;
            }
            const auto size = std::filesystem::file_size(svg.path, ec);
            if (ec) {
                continue;
            }
            const bool differs = time != svg.loaded_time || size != svg.loaded_size;
            const bool still = time == svg.seen_time && size == svg.seen_size;
            svg.seen_time = time;
            svg.seen_size = size;
            if (differs && still) {
                svg.loaded_time = time;
                svg.loaded_size = size;
                const u32 px = svg.raster_px;
                svg.raster_px = 0; // forces the re-rasterisation
                if (auto again = rasterize_svg_locked(record, *data, px); !again) {
                    svg.raster_px = px;
                    Foundation::log_warn("AssetManager: could not reload '{}': {}", svg.path.string(), again.error().message);
                }
            }
        }
    }

    void AssetManager::Impl::enforce_residency() {
        if (residency_limit == 0 || frame % 30 != 0) {
            return;
        }
        std::unique_lock lock{mutex};
        struct Candidate {
            std::chrono::steady_clock::time_point last_drawn;
            usize index;
        };
        std::vector<Candidate> candidates;
        usize resident = 0;
        for (const usize index : managed_list) {
            const Record &record = records[index];
            const auto *data = record.loaded ? std::get_if<TextureData>(&record.data) : nullptr;
            if (data == nullptr || !data->managed || data->managed->evicted) {
                continue;
            }
            ++resident;
            if (data->managed->evictable && data->progressive == nullptr && data->detail != TextureDetail::Placeholder) {
                candidates.push_back({data->managed->last_drawn, index});
            }
        }
        if (resident <= residency_limit || candidates.empty()) {
            return;
        }
        // A texture a model uses is not drawn through the UI, so its draw time says nothing: never evict those.
        std::unordered_set<u64> in_models;
        for (const Record &record : records) {
            const auto *model = record.loaded ? std::get_if<ModelData>(&record.data) : nullptr;
            if (model == nullptr) {
                continue;
            }
            for (const ModelPrimitiveData &primitive : model->primitives) {
                for (const Asset &texture : primitive.textures) {
                    in_models.insert(texture.id_);
                }
            }
        }
        std::ranges::sort(candidates, {}, &Candidate::last_drawn);
        const auto now = std::chrono::steady_clock::now();
        for (const Candidate &candidate : candidates) {
            if (resident <= residency_limit || now - candidate.last_drawn < std::chrono::seconds(2)) {
                break;
            }
            Record &record = records[candidate.index];
            if (in_models.contains(static_cast<u64>(candidate.index + 1))) {
                continue;
            }
            auto &data = std::get<TextureData>(record.data);
            const std::vector<std::byte> clear(4, std::byte{0});
            if (auto replaced = renderer.replace_texture(data.texture, 1, 1, RHI::Format::RGBA8Unorm, clear, 1); !replaced) {
                continue;
            }
            // The texture keeps its logical size (`info`), so layouts that depend on a picture's aspect do not jump.
            data.managed->evicted = true;
            data.detail = TextureDetail::Placeholder;
            ++data.version;
            record.memory_bytes = 4;
            --resident;
        }
    }

    /// Applies what progressive loads have decoded since the last call, spending a few milliseconds at most per call
    /// (but always finishing the update it started): a big image's refinement spreads over frames instead of stalling one.
    void AssetManager::Impl::pump_progressive() {
        if (active_progressive.empty()) {
            return;
        }
        constexpr auto kBudget = std::chrono::milliseconds(4);
        const auto started = std::chrono::steady_clock::now();
        std::unique_lock lock{mutex};
        for (usize slot = 0; slot < active_progressive.size();) {
            Record &record = records[active_progressive[slot]];
            auto *data = record.loaded ? std::get_if<TextureData>(&record.data) : nullptr;
            if (data == nullptr || data->progressive == nullptr) {
                active_progressive.erase(active_progressive.begin() + static_cast<std::ptrdiff_t>(slot));
                continue;
            }
            bool finished = false;
            while (!finished && std::chrono::steady_clock::now() - started < kBudget) {
                ProgressiveUpdate update;
                {
                    std::lock_guard queue_lock{data->progressive->mutex};
                    if (data->progressive->updates.empty()) {
                        break;
                    }
                    update = std::move(data->progressive->updates.front());
                    data->progressive->updates.pop_front();
                }
                switch (update.kind) {
                    case ProgressiveUpdate::Kind::First: {
                        if (auto replaced = renderer.replace_texture(data->texture, update.width, update.height,
                                                                     update.format, update.pixels, 1);
                            !replaced) {
                            Foundation::log_warn("AssetManager: could not show the first pass of '{}': {}",
                                                 record.source.string(), replaced.error().message);
                            break;
                        }
                        data->info.width = update.width;
                        data->info.height = update.height;
                        data->detail = TextureDetail::Partial;
                        data->refined_fraction = 0.0f;
                        ++data->version;
                        record.memory_bytes = update.pixels.size();
                        break;
                    }
                    case ProgressiveUpdate::Kind::Regions: {
                        if (data->detail != TextureDetail::Partial) {
                            break;
                        }
                        if (auto updated = renderer.update_texture_regions(data->texture, update.regions, update.pixels);
                            !updated) {
                            Foundation::log_warn("AssetManager: could not refine '{}': {}", record.source.string(),
                                                 updated.error().message);
                            break;
                        }
                        data->refined_fraction = update.refined_fraction;
                        ++data->version;
                        break;
                    }
                    case ProgressiveUpdate::Kind::Final: {
                        if (auto replaced = renderer.replace_texture(data->texture, update.width, update.height,
                                                                     update.format, update.pixels, update.mip_levels);
                            !replaced) {
                            Foundation::log_warn("AssetManager: could not finish '{}': {}", record.source.string(),
                                                 replaced.error().message);
                            data->detail = TextureDetail::Failed;
                        } else {
                            data->info.width = update.width;
                            data->info.height = update.height;
                            data->detail = TextureDetail::Complete;
                            data->refined_fraction = 1.0f;
                            record.memory_bytes = update.pixels.size();
                            ++data->version;
                        }
                        data->progressive.reset();
                        finished = true;
                        break;
                    }
                    case ProgressiveUpdate::Kind::Failed: {
                        Foundation::log_warn("AssetManager: could not load texture '{}': {}", record.source.string(),
                                             update.message);
                        data->detail = TextureDetail::Failed;
                        ++data->version;
                        data->progressive.reset();
                        finished = true;
                        break;
                    }
                }
            }
            if (finished) {
                active_progressive.erase(active_progressive.begin() + static_cast<std::ptrdiff_t>(slot));
            } else {
                ++slot;
            }
            if (std::chrono::steady_clock::now() - started >= kBudget) {
                break;
            }
        }
    }

    AssetExpected<Asset> AssetManager::load_texture_progressive(const std::filesystem::path &source,
                                                                TextureColorSpace color_space, UString label,
                                                                bool nearest_sampling, TextureKind kind) {
        SFT_TRACE_SCOPE("AssetManager::load_texture_progressive");
        const std::string key =
            "progressive|" + source.string() + (color_space == TextureColorSpace::Srgb ? "|srgb" : "|linear") +
            (nearest_sampling ? "|nearest" : "") + "|kind" + std::to_string(static_cast<int>(kind));
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }
        if (label.empty()) {
            label = path_label(source, "texture");
        }
        // A transparent 1x1 until the first pass is decoded: whatever shows the asset draws nothing in the meantime.
        auto placeholder = create_texture(TextureAssetDesc{
            .width = 1,
            .height = 1,
            .color_space = color_space,
            .pixels = std::vector<std::byte>(4, std::byte{0}),
            .label = label,
            .allow_compression = false,
            .generate_mipmaps = false,
            .nearest_sampling = nearest_sampling,
        });
        if (!placeholder) {
            return placeholder;
        }

        auto load = std::make_shared<ProgressiveLoad>();
        const bool allow_bc = device_allows_bc(impl_->renderer);
        {
            std::unique_lock lock{impl_->mutex};
            if (Impl::Record *record = impl_->find(*placeholder)) {
                record->source = source;
                if (auto *data = std::get_if<Impl::TextureData>(&record->data)) {
                    data->progressive = load;
                    data->detail = TextureDetail::Placeholder;
                    data->refined_fraction = 0.0f;
                }
                impl_->active_progressive.push_back(static_cast<usize>(placeholder->id_ - 1));
            }
            impl_->path_cache.emplace(key, *placeholder);
        }
        (void)impl_->decode_thread.run([load, source, color_space, kind, allow_bc] {
            run_progressive_load(load, source, {}, color_space, kind, allow_bc);
        });
        return placeholder;
    }

    AssetExpected<Asset> AssetManager::create_texture_progressive_from_bytes(std::vector<std::byte> encoded,
                                                                             TextureColorSpace color_space,
                                                                             UString label, bool nearest_sampling,
                                                                             TextureKind kind) {
        if (encoded.empty()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription, "No image data was given."));
        }
        if (label.empty()) {
            label = UString{"texture"_ustr};
        }
        auto placeholder = create_texture(TextureAssetDesc{
            .width = 1,
            .height = 1,
            .color_space = color_space,
            .pixels = std::vector<std::byte>(4, std::byte{0}),
            .label = label,
            .allow_compression = false,
            .generate_mipmaps = false,
            .nearest_sampling = nearest_sampling,
        });
        if (!placeholder) {
            return placeholder;
        }
        auto load = std::make_shared<ProgressiveLoad>();
        const bool allow_bc = device_allows_bc(impl_->renderer);
        {
            std::unique_lock lock{impl_->mutex};
            if (Impl::Record *record = impl_->find(*placeholder)) {
                if (auto *data = std::get_if<Impl::TextureData>(&record->data)) {
                    data->progressive = load;
                    data->detail = TextureDetail::Placeholder;
                    data->refined_fraction = 0.0f;
                }
                impl_->active_progressive.push_back(static_cast<usize>(placeholder->id_ - 1));
            }
        }
        (void)impl_->decode_thread.run([load, bytes = std::move(encoded), color_space, kind, allow_bc]() mutable {
            run_progressive_load(load, {}, std::move(bytes), color_space, kind, allow_bc);
        });
        return placeholder;
    }

    namespace {
        std::string managed_key(std::string_view origin, const TextureLoadOptions &options) {
            return "managed|" + std::string{origin} + (options.color_space == TextureColorSpace::Srgb ? "|srgb" : "|linear") +
                   "|kind" + std::to_string(static_cast<int>(options.kind)) + "|max" + std::to_string(options.max_dimension) +
                   (options.nearest_sampling ? "|nearest" : "");
        }
    } // namespace

    AssetExpected<Asset> AssetManager::load_texture_managed(const std::filesystem::path &source, TextureLoadOptions options,
                                                            UString label) {
        const std::string key = managed_key(source.string(), options);
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }
        if (label.empty()) {
            label = path_label(source, "texture");
        }
        auto placeholder = create_texture(TextureAssetDesc{
            .width = 1, .height = 1, .color_space = options.color_space, .pixels = std::vector<std::byte>(4, std::byte{0}),
            .label = label, .kind = options.kind, .allow_compression = false, .generate_mipmaps = false,
            .nearest_sampling = options.nearest_sampling});
        if (!placeholder) {
            return placeholder;
        }
        std::unique_lock lock{impl_->mutex};
        const usize index = static_cast<usize>(placeholder->id_ - 1);
        if (Impl::Record *record = impl_->find(*placeholder)) {
            record->source = source;
            if (auto *data = std::get_if<Impl::TextureData>(&record->data)) {
                data->detail = TextureDetail::Placeholder;
                data->refined_fraction = 0.0f;
                data->managed = Impl::ManagedSource{.key = key, .path = source, .color_space = options.color_space,
                                                    .kind = options.kind, .max_dimension = options.max_dimension,
                                                    .watch = options.watch, .evictable = options.evictable};
                impl_->managed_list.push_back(index);
                impl_->managed_by_handle[data->texture.value] = index;
                impl_->start_load_locked(index);
            }
        }
        impl_->path_cache.emplace(key, *placeholder);
        return placeholder;
    }

    AssetExpected<Asset> AssetManager::create_texture_managed_from_bytes(std::string_view name, std::vector<std::byte> encoded,
                                                                         TextureLoadOptions options, UString label) {
        if (encoded.empty()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription, "No image data was given."));
        }
        const std::string key = managed_key("bytes:" + std::string{name}, options);
        auto shared = std::make_shared<const std::vector<std::byte>>(std::move(encoded));
        {
            std::unique_lock lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
                // Same name: the same asset. Different bytes under it mean the picture changed (a new avatar), so it reloads.
                Impl::Record *record = impl_->find(it->second);
                auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
                if (data != nullptr && data->managed && (!data->managed->bytes || *data->managed->bytes != *shared)) {
                    data->managed->bytes = shared;
                    impl_->start_load_locked(static_cast<usize>(it->second.id_ - 1));
                }
                return it->second;
            }
        }
        if (label.empty()) {
            label = UString{std::string{name}};
        }
        auto placeholder = create_texture(TextureAssetDesc{
            .width = 1, .height = 1, .color_space = options.color_space, .pixels = std::vector<std::byte>(4, std::byte{0}),
            .label = label, .kind = options.kind, .allow_compression = false, .generate_mipmaps = false,
            .nearest_sampling = options.nearest_sampling});
        if (!placeholder) {
            return placeholder;
        }
        std::unique_lock lock{impl_->mutex};
        const usize index = static_cast<usize>(placeholder->id_ - 1);
        if (Impl::Record *record = impl_->find(*placeholder)) {
            if (auto *data = std::get_if<Impl::TextureData>(&record->data)) {
                data->detail = TextureDetail::Placeholder;
                data->refined_fraction = 0.0f;
                data->managed = Impl::ManagedSource{.key = key, .bytes = shared, .color_space = options.color_space,
                                                    .kind = options.kind, .max_dimension = options.max_dimension,
                                                    .watch = false, .evictable = options.evictable};
                impl_->managed_list.push_back(index);
                impl_->managed_by_handle[data->texture.value] = index;
                impl_->start_load_locked(index);
            }
        }
        impl_->path_cache.emplace(key, *placeholder);
        return placeholder;
    }

    AssetResult AssetManager::reload(Asset asset) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(asset);
        auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::Unsupported : AssetErrorCode::InvalidAsset,
                                         "reload supports texture assets loaded from a file, bytes or SVG."));
        }
        if (data->managed) {
            impl_->start_load_locked(static_cast<usize>(asset.id_ - 1));
            return {};
        }
        if (data->svg) {
            const u32 px = data->svg->raster_px;
            data->svg->raster_px = 0;
            auto again = impl_->rasterize_svg_locked(*record, *data, px);
            if (!again) {
                data->svg->raster_px = px;
            }
            return again;
        }
        return std::unexpected(error(AssetErrorCode::Unsupported, "This texture has no source to reload from."));
    }

    void AssetManager::set_texture_residency_limit(usize max_resident) {
        std::unique_lock lock{impl_->mutex};
        impl_->residency_limit = max_resident;
    }

    AssetExpected<TextureProgress> AssetManager::texture_progress(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "texture_progress requires a live texture asset."));
        }
        return TextureProgress{.detail = data->detail,
                               .refined_fraction = data->refined_fraction,
                               .width = data->info.width,
                               .height = data->info.height,
                               .version = data->version};
    }

    /// Swaps a texture record's pixels for `desc`'s through the renderer. The caller holds the manager's lock.
    AssetResult AssetManager::Impl::replace_locked(Record &record, TextureData &data, TextureAssetDesc desc) {
        const u32 width = desc.width;
        const u32 height = desc.height;
        auto prepared = prepare_texture(desc, device_allows_bc(renderer));
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        if (auto replaced = renderer.replace_texture(data.texture, prepared->width, prepared->height, prepared->format,
                                                     std::span<const std::byte>{prepared->bytes}, prepared->mip_levels);
            !replaced) {
            return std::unexpected(backend_error(replaced.error()));
        }
        data.info.width = width;
        data.info.height = height;
        record.memory_bytes = prepared->bytes.size();
        ++data.version;
        return {};
    }

    AssetResult AssetManager::replace_texture(Asset asset, TextureAssetDesc desc) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(asset);
        auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "replace_texture requires a live texture asset."));
        }
        data->info.color_space = desc.color_space;
        if (auto result = impl_->replace_locked(*record, *data, std::move(desc)); !result) {
            return result;
        }
        if (data->progressive) {
            data->progressive->cancel.store(true, std::memory_order_release);
            data->progressive.reset();
        }
        data->svg.reset();
        data->animation.reset();
        if (data->managed) {
            impl_->managed_by_handle.erase(data->texture.value);
            impl_->path_cache.erase(data->managed->key);
            data->managed.reset();
        }
        data->detail = TextureDetail::Complete;
        data->refined_fraction = 1.0f;
        return {};
    }

    /// Rasterises the SVG `data` was made from at `px` and swaps it in. The caller holds the manager's lock.
    AssetResult AssetManager::Impl::rasterize_svg_locked(Record &record, TextureData &data, u32 px) {
        if (!data.svg) {
            return std::unexpected(error(AssetErrorCode::WrongType, "The texture asset was not made from an SVG."));
        }
        if (data.svg->raster_px == px) {
            return {};
        }
        std::optional<UI::Svg::RasterizedSvg> rasterized =
            data.svg->data ? UI::Svg::rasterize_svg_data(*data.svg->data, static_cast<f32>(px))
                           : UI::Svg::rasterize_svg_file(data.svg->path, static_cast<f32>(px));
        if (!rasterized) {
            return std::unexpected(error(AssetErrorCode::DecodeFailure, "Failed to load/rasterize SVG.",
                                         data.svg->path));
        }
        if (auto result = replace_locked(record, data,
                                         TextureAssetDesc{.width = rasterized->width,
                                                          .height = rasterized->height,
                                                          .color_space = data.info.color_space,
                                                          .pixels = std::move(rasterized->rgba),
                                                          .label = record.label});
            !result) {
            return result;
        }
        data.svg->raster_px = px;
        return {};
    }

    AssetExpected<Asset> AssetManager::load_svg(const std::filesystem::path &source, f32 target_px, UString label) {
        SFT_TRACE_SCOPE("AssetManager::load_svg");
        const u32 px = svg_raster_px(target_px);
        const std::string key = "svg|" + source.string();
        {
            std::unique_lock lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
                Impl::Record *record = impl_->find(it->second);
                auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
                if (data != nullptr && data->svg) {
                    // Only ever bigger: the same icon drawn at two sizes shares one raster, which the mip chain
                    // keeps clean at the smaller one.
                    if (data->svg->raster_px < px) {
                        if (auto grown = impl_->rasterize_svg_locked(*record, *data, px); !grown) {
                            return std::unexpected(grown.error());
                        }
                    }
                    return it->second;
                }
            }
        }

        std::optional<UI::Svg::RasterizedSvg> rasterized = UI::Svg::rasterize_svg_file(source, static_cast<f32>(px));
        if (!rasterized) {
            return std::unexpected(error(AssetErrorCode::DecodeFailure, "Failed to load/rasterize SVG.", source));
        }
        if (label.empty()) {
            label = path_label(source, "svg");
        }
        auto asset = create_texture(TextureAssetDesc{
            .width = rasterized->width,
            .height = rasterized->height,
            .color_space = TextureColorSpace::Srgb,
            .pixels = std::move(rasterized->rgba),
            .label = std::move(label),
        });
        if (!asset) {
            return asset;
        }
        std::unique_lock lock{impl_->mutex};
        if (Impl::Record *record = impl_->find(*asset)) {
            record->source = source;
            if (auto *data = std::get_if<Impl::TextureData>(&record->data)) {
                data->svg = SvgSource{.path = source, .raster_px = px};
                std::error_code ec;
                const auto time = std::filesystem::last_write_time(source, ec);
                const auto size = ec ? 0 : std::filesystem::file_size(source, ec);
                if (!ec) {
                    data->svg->loaded_time = data->svg->seen_time = time;
                    data->svg->loaded_size = data->svg->seen_size = size;
                }
                impl_->svg_records[data->texture.value] = static_cast<usize>(asset->id_ - 1);
            }
        }
        impl_->path_cache[key] = *asset;
        return asset;
    }

    AssetExpected<Asset> AssetManager::load_svg_data(std::string_view name, std::string_view svg, f32 target_px,
                                                     UString label) {
        SFT_TRACE_SCOPE("AssetManager::load_svg_data");
        const u32 px = svg_raster_px(target_px);
        const std::string key = "svg-data|" + std::string{name};
        std::shared_ptr<const std::string> text;
        {
            std::unique_lock lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
                Impl::Record *record = impl_->find(it->second);
                auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
                if (data != nullptr && data->svg) {
                    // The same name is the same icon; the source is kept from the first time. Only ever bigger.
                    if (data->svg->raster_px < px) {
                        if (auto grown = impl_->rasterize_svg_locked(*record, *data, px); !grown) {
                            return std::unexpected(grown.error());
                        }
                    }
                    return it->second;
                }
            }
        }
        text = std::make_shared<const std::string>(svg);
        std::optional<UI::Svg::RasterizedSvg> rasterized = UI::Svg::rasterize_svg_data(*text, static_cast<f32>(px));
        if (!rasterized) {
            return std::unexpected(error(AssetErrorCode::DecodeFailure, "Failed to parse/rasterize SVG data.",
                                         std::filesystem::path{std::string{name}}));
        }
        if (label.empty()) {
            label = UString{std::string_view{name}.empty() ? std::string_view{"svg"} : name};
        }
        auto asset = create_texture(TextureAssetDesc{
            .width = rasterized->width,
            .height = rasterized->height,
            .color_space = TextureColorSpace::Srgb,
            .pixels = std::move(rasterized->rgba),
            .label = std::move(label),
        });
        if (!asset) {
            return asset;
        }
        std::unique_lock lock{impl_->mutex};
        if (Impl::Record *record = impl_->find(*asset)) {
            // No `source`: there is no file, and the path cache key below is what finds this asset again.
            if (auto *data = std::get_if<Impl::TextureData>(&record->data)) {
                data->svg = SvgSource{.path = std::filesystem::path{std::string{name}}, .data = std::move(text), .raster_px = px};
                impl_->svg_records[data->texture.value] = static_cast<usize>(asset->id_ - 1);
            }
        }
        impl_->path_cache[key] = *asset;
        return asset;
    }

    AssetResult AssetManager::set_svg_size(Asset asset, f32 target_px) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(asset);
        auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "set_svg_size requires a live texture asset."));
        }
        return impl_->rasterize_svg_locked(*record, *data, svg_raster_px(target_px));
    }

    /// Computes the create texture from encoded bytes required by the supplied values.
    ///
    /// @param encoded `encoded` value used by the operation.
    /// @param color_space `color_space` value used by the operation.
    /// @param kind `kind` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::create_texture_from_encoded_bytes(
        std::span<const std::byte> encoded,
        TextureColorSpace color_space,
        TextureKind kind,
        UString label,
        TextureDynamicRange dynamic_range) {
        auto decoded = Detail::decode_image(encoded, decode_options_for(dynamic_range), {});
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (label.empty()) {
            label = UString{"texture"_ustr};
        }
        return create_texture(TextureAssetDesc{
            .width = decoded->width,
            .height = decoded->height,
            .color_space = texture_color_space_for(dynamic_range, color_space),
            .format = texture_pixel_format_for(dynamic_range),
            .pixels = std::move(decoded->pixels()),
            .label = std::move(label),
            .kind = kind,
        });
    }

    /// Loads sound.
    ///
    /// @param source Source value or resource.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`.
    AssetExpected<Asset> AssetManager::load_sound(const std::filesystem::path &source, UString label) {
        SFT_TRACE_SCOPE("AssetManager::load_sound");
        const std::string dedup_key = source.string();
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(dedup_key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }

        const Foundation::Stopwatch stopwatch;
        // Every codec the audio decoder registry knows (WAV, FLAC, MP3, Ogg Vorbis, AIFF, Opus, and AAC/M4A/WMA where the OS provides them).
        auto decoded = Audio::load_sound_file(source);
        if (!decoded) {
            return std::unexpected(error(AssetErrorCode::DecodeFailure,
                                         "Could not decode sound '" + source.string() + "': " + decoded.error().cpp_string(), source));
        }
        std::shared_ptr<const Audio::SampleBuffer> buffer = *decoded;
        const u32 channels = buffer->channels;
        const u32 sample_rate = buffer->sample_rate;
        const u64 frames_read = buffer->frames();

        if (label.empty()) {
            label = path_label(source, "sound");
        }
        Foundation::log_info("AssetManager: loaded sound '{}' ({} frames, {}ch @ {}Hz) in {}",
                             source.string(),
                             frames_read,
                             channels,
                             sample_rate,
                             stopwatch.elapsed_human());
        std::unique_lock lock{impl_->mutex};
        const Asset asset = impl_->insert(Impl::Record{
            .type = AssetType::Sound,
            .label = std::move(label),
            .source = source,
            .memory_bytes = buffer->samples->size() * sizeof(f32),
            .data = Impl::SoundData{
                .buffer = std::move(buffer),
                .info = SoundAssetInfo{
                    .channels = channels,
                    .sample_rate = sample_rate,
                    .frame_count = frames_read,
                    .duration_seconds = sample_rate != 0
                                            ? static_cast<f64>(frames_read) / sample_rate
                                            : 0.0,
                },
            },
        });
        impl_->path_cache.emplace(dedup_key, asset);
        return asset;
    }

    /// Loads file.
    ///
    /// @param source Source value or resource.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::load_file(const std::filesystem::path &source, UString label) {
        SFT_TRACE_SCOPE("AssetManager::load_file");
        const std::string dedup_key = source.string();
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(dedup_key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }

        const Foundation::Stopwatch stopwatch;
        auto bytes = read_binary_file(source);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (label.empty()) {
            label = path_label(source, "file");
        }
        auto shared = std::make_shared<std::vector<std::byte>>(std::move(*bytes));
        Foundation::log_info("AssetManager: loaded file '{}' ({} bytes) in {}", source.string(), shared->size(), stopwatch.elapsed_human());
        std::unique_lock lock{impl_->mutex};
        const Asset asset = impl_->insert(Impl::Record{
            .type = AssetType::File,
            .label = std::move(label),
            .source = source,
            .memory_bytes = shared->size(),
            .data = Impl::FileData{.bytes = std::move(shared)},
        });
        impl_->path_cache.emplace(dedup_key, asset);
        return asset;
    }

    /// Creates a model from the supplied parameters.
    ///
    /// @param mesh `mesh` value used by the operation.
    /// @param shader Shader used or affected by the operation.
    /// @param vertex_color `vertex_color` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::create_model(SFT::Renderer::Mesh mesh,
                                                    Asset shader,
                                                    std::optional<glm::vec4> vertex_color,
                                                    UString label) {
        if (label.empty() && !mesh.label().empty()) {
            if (auto mesh_label = UString::try_from_utf8(mesh.label())) {
                label = std::move(*mesh_label);
            }
        }
        return create_model(ModelAssetDesc{
            .label = std::move(label),
            .primitives = {
                ModelPrimitiveDesc{
                    .mesh = std::move(mesh),
                    .shader = shader,
                    .vertex_color = vertex_color,
                },
            },
        });
    }

    /// Creates a model from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`, `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<Asset> AssetManager::create_model(ModelAssetDesc desc) {
        const Foundation::Stopwatch stopwatch;
        if (desc.primitives.empty()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "A model asset requires at least one primitive."));
        }
        if (desc.label.empty()) {
            desc.label = UString{"model"_ustr};
        }

        std::unique_lock lock{impl_->mutex};
        Impl::ModelData model{};
        model.primitives.reserve(desc.primitives.size());

        const auto rollback = [this, &model]() noexcept {
            for (Impl::ModelPrimitiveData &created : model.primitives) {
                impl_->renderer.destroy_material_instance(created.material);
                impl_->renderer.destroy_mesh(created.mesh);
            }
            model.primitives.clear();
        };

        for (usize primitive_index = 0; primitive_index < desc.primitives.size(); ++primitive_index) {
            ModelPrimitiveDesc &primitive = desc.primitives[primitive_index];
            const Impl::Record *shader_record = impl_->find(primitive.shader);
            const auto *shader = shader_record ? std::get_if<Impl::ShaderData>(&shader_record->data) : nullptr;
            if (shader == nullptr) {
                rollback();
                return std::unexpected(error(
                    primitive.shader && primitive.shader.type() != AssetType::Shader
                        ? AssetErrorCode::WrongType
                        : AssetErrorCode::InvalidAsset,
                    "Model primitive references an invalid shader asset."));
            }

            if (primitive.vertex_color) {
                primitive.mesh.set_vertex_color(*primitive.vertex_color);
            }
            model.info.vertex_count += primitive.mesh.vertices().size();
            model.info.index_count += primitive.mesh.indices().size();

            auto mesh = impl_->renderer.upload(primitive.mesh);
            if (!mesh) {
                rollback();
                return std::unexpected(backend_error(mesh.error()));
            }

            const std::string material_label = desc.label.cpp_string() + " material " +
                                               std::to_string(primitive_index);
            auto material = impl_->renderer.create_material_instance(
                shader->material_template,
                material_label.c_str());
            if (!material) {
                impl_->renderer.destroy_mesh(*mesh);
                rollback();
                return std::unexpected(backend_error(material.error()));
            }

            Impl::ModelPrimitiveData created{
                .mesh = *mesh,
                .material = *material,
                .shader = primitive.shader,
                .double_sided = primitive.double_sided,
            };
            const bool morph_ok = primitive.morph && !primitive.morph->empty() &&
                                  primitive.morph->vertex_count() == primitive.mesh.vertices().size();
            if (morph_ok) {
                created.morph = primitive.morph;
            }
            std::shared_ptr<const Animation::SkinWeights> effective_skin = primitive.skin;
            if (!(effective_skin && effective_skin->vertex_count() == primitive.mesh.vertices().size()) && morph_ok) {
                auto synthetic = std::make_shared<Animation::SkinWeights>();
                synthetic->joints.assign(primitive.mesh.vertices().size(), glm::uvec4{0});
                synthetic->weights.assign(primitive.mesh.vertices().size(), glm::vec4{1.0f, 0.0f, 0.0f, 0.0f});
                effective_skin = std::move(synthetic);
                created.synthetic_skin = true;
            }
            if (effective_skin && effective_skin->vertex_count() == primitive.mesh.vertices().size()) {
                created.skin = effective_skin;
                created.bind_vertices = std::make_shared<const std::vector<SFT::Renderer::GeometryVertex>>(
                    primitive.mesh.vertices().begin(), primitive.mesh.vertices().end());
            }
            created.textures.reserve(primitive.textures.size());
            for (const ModelTextureBinding &binding : primitive.textures) {
                const Impl::Record *texture_record = impl_->find(binding.texture);
                const auto *texture = texture_record ? std::get_if<Impl::TextureData>(&texture_record->data) : nullptr;
                if (texture == nullptr) {
                    impl_->renderer.destroy_material_instance(created.material);
                    impl_->renderer.destroy_mesh(created.mesh);
                    rollback();
                    return std::unexpected(error(
                        binding.texture && binding.texture.type() != AssetType::Texture
                            ? AssetErrorCode::WrongType
                            : AssetErrorCode::InvalidAsset,
                        "Model primitive references an invalid texture asset."));
                }
                if (Core::RendererResult bound = impl_->renderer.set_material_texture(
                        created.material, binding.slot.cpp_string_view(), texture->texture);
                    !bound) {
                    impl_->renderer.destroy_material_instance(created.material);
                    impl_->renderer.destroy_mesh(created.mesh);
                    rollback();
                    return std::unexpected(backend_error(bound.error()));
                }
                created.textures.push_back(binding.texture);
            }
            model.primitives.push_back(std::move(created));
        }
        model.info.primitive_count = model.primitives.size();
        model.info.triangle_count = model.info.index_count / 3;

        const usize approximate_bytes =
            model.info.vertex_count * sizeof(SFT::Renderer::GeometryVertex) +
            model.info.index_count * sizeof(u32);
        Foundation::log_info("AssetManager: created model '{}' ({} primitive(s), {} tri) in {}",
                             desc.label.cpp_string(),
                             model.info.primitive_count,
                             model.info.triangle_count,
                             stopwatch.elapsed_human());
        return impl_->insert(Impl::Record{
            .type = AssetType::Model,
            .label = std::move(desc.label),
            .memory_bytes = approximate_bytes,
            .data = std::move(model),
        });
    }

    bool AssetManager::is_skinnable(Asset model) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(model);
        const auto *data = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (data == nullptr) {
            return false;
        }
        return std::any_of(data->primitives.begin(), data->primitives.end(),
                           [](const Impl::ModelPrimitiveData &p) { return p.skin != nullptr; });
    }

    AssetExpected<Asset> AssetManager::create_skinned_instance(Asset model, UString label) {
        std::unique_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(model);
        const auto *source = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (source == nullptr) {
            return std::unexpected(error(model && model.type() != AssetType::Model ? AssetErrorCode::WrongType
                                                                                   : AssetErrorCode::InvalidAsset,
                                         "create_skinned_instance requires a live model asset."));
        }
        if (label.empty()) {
            label = record->label;
        }
        Impl::ModelData clone{};
        clone.info = source->info;
        const auto rollback = [this, &clone]() noexcept {
            for (Impl::ModelPrimitiveData &created : clone.primitives) {
                impl_->renderer.destroy_mesh(created.mesh);
            }
        };
        bool any_skinned = false;
        for (const Impl::ModelPrimitiveData &primitive : source->primitives) {
            Impl::ModelPrimitiveData copy = primitive;
            copy.shares_material = true;
            if (primitive.skin && primitive.bind_vertices) {
                const SFT::Renderer::MeshResource *resource = impl_->renderer.mesh(primitive.mesh);
                if (resource == nullptr) {
                    rollback();
                    return std::unexpected(error(AssetErrorCode::InvalidAsset, "Source model mesh is no longer alive."));
                }
                auto mesh = impl_->renderer.create_mesh(*primitive.bind_vertices, resource->indices, label.c_str());
                if (!mesh) {
                    rollback();
                    return std::unexpected(backend_error(mesh.error()));
                }
                copy.mesh = *mesh;
                std::vector<SFT::Renderer::SkinInfluence> influences(primitive.skin->vertex_count());
                for (usize v = 0; v < influences.size(); ++v) {
                    influences[v] = SFT::Renderer::SkinInfluence{.joints = primitive.skin->joints[v],
                                                                .weights = primitive.skin->weights[v]};
                }
                SFT::Renderer::SkinAttachDesc attach{.influences = influences};
                std::vector<SFT::Renderer::MorphDelta> deltas;
                if (primitive.morph) {
                    deltas.resize(primitive.morph->entries.size());
                    for (usize i = 0; i < deltas.size(); ++i) {
                        const Animation::MorphEntry &e = primitive.morph->entries[i];
                        deltas[i] = SFT::Renderer::MorphDelta{.position_delta = e.position_delta, .target = e.target,
                                                              .normal_delta = e.normal_delta};
                    }
                    attach.morph = SFT::Renderer::SkinMorphDesc{.target_count = primitive.morph->target_count(),
                                                                .vertex_offsets = primitive.morph->vertex_offsets,
                                                                .entries = deltas};
                }
                copy.gpu_skinned = impl_->renderer.attach_skin(*mesh, attach).has_value();
                any_skinned = true;
            } else {
                // Rigid primitive: nothing deforms, so it keeps the source mesh. Destroying the clone would
                // destroy that shared mesh, so mark it as not owned by dropping the handle's ownership: the
                // simplest correct option is a private copy of the geometry.
                const SFT::Renderer::MeshResource *resource = impl_->renderer.mesh(primitive.mesh);
                if (resource == nullptr) {
                    rollback();
                    return std::unexpected(error(AssetErrorCode::InvalidAsset, "Source model mesh is no longer alive."));
                }
                auto mesh = impl_->renderer.create_mesh(resource->vertices, resource->indices, label.c_str());
                if (!mesh) {
                    rollback();
                    return std::unexpected(backend_error(mesh.error()));
                }
                copy.mesh = *mesh;
            }
            clone.primitives.push_back(std::move(copy));
        }
        if (!any_skinned) {
            rollback();
            return std::unexpected(error(AssetErrorCode::InvalidDescription, "Model has no skinned primitives."));
        }
        return impl_->insert(Impl::Record{
            .type = AssetType::Model,
            .label = std::move(label),
            .memory_bytes = record->memory_bytes,
            .data = std::move(clone),
        });
    }

    AssetResult AssetManager::set_skinned_pose(Asset model, std::span<const glm::mat4> skin_matrices,
                                               std::span<const f32> morph_weights) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(model);
        auto *data = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(model && model.type() != AssetType::Model ? AssetErrorCode::WrongType
                                                                                   : AssetErrorCode::InvalidAsset,
                                         "set_skinned_pose requires a live model asset."));
        }
        std::vector<SFT::Renderer::GeometryVertex> posed;
        for (Impl::ModelPrimitiveData &primitive : data->primitives) {
            if (!primitive.skin || !primitive.bind_vertices) {
                continue;
            }
            static const glm::mat4 identity_matrix{1.0f};
            const std::span<const glm::mat4> matrices =
                primitive.synthetic_skin ? std::span<const glm::mat4>{&identity_matrix, 1} : skin_matrices;
            if (primitive.gpu_skinned) {
                if (Core::RendererResult result = impl_->renderer.set_skin_pose(primitive.mesh, matrices, morph_weights); !result) {
                    return std::unexpected(backend_error(result.error()));
                }
                continue;
            }
            const auto &bind = *primitive.bind_vertices;
            posed.assign(bind.begin(), bind.end());
            for (usize v = 0; v < posed.size(); ++v) {
                glm::vec3 position = bind[v].position;
                glm::vec3 normal = bind[v].normal;
                if (primitive.morph) {
                    Animation::apply_morph(*primitive.morph, v, morph_weights, position, normal);
                    normal = glm::normalize(normal);
                }
                const Animation::SkinnedVertex out = Animation::skin_vertex(
                    matrices, primitive.skin->joints[v], primitive.skin->weights[v], position,
                    normal, glm::vec3(bind[v].tangent));
                posed[v].position = out.position;
                posed[v].normal = out.normal;
                posed[v].tangent = glm::vec4(out.tangent, bind[v].tangent.w);
            }
            if (Core::RendererResult result = impl_->renderer.update_mesh_vertices(primitive.mesh, posed); !result) {
                return std::unexpected(backend_error(result.error()));
            }
        }
        return {};
    }

    /// Sets the model float for this `Engine`.
    ///
    /// @param model `model` value used by the operation.
    /// @param primitive `primitive` value used by the operation.
    /// @param name Name used to identify or label the target.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
    AssetResult AssetManager::set_model_float(Asset model, usize primitive, std::string_view name, f32 value) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(model);
        auto *data = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(model && model.type() != AssetType::Model
                                             ? AssetErrorCode::WrongType
                                             : AssetErrorCode::InvalidAsset,
                                         "set_model_float requires a live model asset."));
        }
        if (primitive >= data->primitives.size()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "Model primitive index is out of range."));
        }
        Core::RendererResult result = impl_->renderer.set_material_float(
            data->primitives[primitive].material, name, value);
        if (!result) {
            return std::unexpected(backend_error(result.error()));
        }
        return {};
    }

    /// Sets the model vec4 for this `Engine`.
    ///
    /// @param model `model` value used by the operation.
    /// @param primitive `primitive` value used by the operation.
    /// @param name Name used to identify or label the target.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
    AssetResult AssetManager::set_model_vec4(Asset model, usize primitive, std::string_view name,
                                             const glm::vec4 &value) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(model);
        auto *data = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(model && model.type() != AssetType::Model
                                             ? AssetErrorCode::WrongType
                                             : AssetErrorCode::InvalidAsset,
                                         "set_model_vec4 requires a live model asset."));
        }
        if (primitive >= data->primitives.size()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "Model primitive index is out of range."));
        }
        Core::RendererResult result = impl_->renderer.set_material_vec4(
            data->primitives[primitive].material, name, value.x, value.y, value.z, value.w);
        if (!result) {
            return std::unexpected(backend_error(result.error()));
        }
        return {};
    }

    /// Sets the model texture for this `Engine`.
    ///
    /// @param model `model` value used by the operation.
    /// @param primitive `primitive` value used by the operation.
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param texture_asset Texture used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
    AssetResult AssetManager::set_model_texture(Asset model, usize primitive, std::string_view slot,
                                                Asset texture_asset) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *model_record = impl_->find(model);
        auto *model_data = model_record ? std::get_if<Impl::ModelData>(&model_record->data) : nullptr;
        Impl::Record *texture_record = impl_->find(texture_asset);
        auto *texture = texture_record ? std::get_if<Impl::TextureData>(&texture_record->data) : nullptr;
        if (model_data == nullptr || texture == nullptr) {
            return std::unexpected(error(AssetErrorCode::InvalidAsset,
                                         "set_model_texture requires live model and texture assets from this manager."));
        }
        if (primitive >= model_data->primitives.size()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "Model primitive index is out of range."));
        }
        Impl::ModelPrimitiveData &target = model_data->primitives[primitive];
        Core::RendererResult result = impl_->renderer.set_material_texture(target.material, slot, texture->texture);
        if (!result) {
            return std::unexpected(backend_error(result.error()));
        }
        if (std::ranges::find(target.textures, texture_asset) == target.textures.end()) {
            target.textures.push_back(texture_asset);
            ++impl_->model_references[Impl::reference_key(texture_asset)];
        }
        return {};
    }

    /// Reports whether contains holds for this `Engine`.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool AssetManager::contains(Asset asset) const noexcept {
        std::shared_lock lock{impl_->mutex};
        return impl_->find(asset) != nullptr;
    }

    /// Returns the size for this `Engine`.
    ///
    /// @return Returns the current size value.
    /// @note This function does not throw exceptions.
    usize AssetManager::size() const noexcept {
        std::shared_lock lock{impl_->mutex};
        return impl_->loaded_count;
    }

    /// Performs the info operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`.
    AssetExpected<AssetInfo> AssetManager::info(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        if (record == nullptr) {
            return std::unexpected(error(AssetErrorCode::InvalidAsset,
                                         "Asset is stale, unloaded, or belongs to another AssetManager."));
        }
        return AssetInfo{
            .asset = asset,
            .label = record->label,
            .source = record->source,
            .memory_bytes = record->memory_bytes,
            .loaded = record->loaded,
        };
    }

    /// Performs the model info operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<ModelAssetInfo> AssetManager::model_info(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "model_info requires a live model asset."));
        }
        return data->info;
    }

    /// Performs the texture info operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<TextureAssetInfo> AssetManager::texture_info(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "texture_info requires a live texture asset."));
        }
        return data->info;
    }

    /// Returns the texture handle associated with this `Engine`.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<SFT::Renderer::TextureHandle> AssetManager::texture_handle(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "texture_handle requires a live texture asset."));
        }
        return data->texture;
    }

    /// Performs the sound info operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<SoundAssetInfo> AssetManager::sound_info(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::SoundData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "sound_info requires a live sound asset."));
        }
        return data->info;
    }

    /// Computes the file bytes required by the supplied values.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<std::shared_ptr<const std::vector<std::byte>>> AssetManager::file_bytes(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::FileData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "file_bytes requires a live file asset."));
        }
        return std::shared_ptr<const std::vector<std::byte>>{data->bytes};
    }

    /// Performs the sound samples operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<std::shared_ptr<const Audio::SampleBuffer>> AssetManager::sound_buffer(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::SoundData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "sound_buffer requires a live sound asset."));
        }
        return data->buffer;
    }

    AssetExpected<std::shared_ptr<const std::vector<f32>>> AssetManager::sound_samples(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::SoundData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "sound_samples requires a live sound asset."));
        }
        return std::shared_ptr<const std::vector<f32>>{data->buffer->samples};
    }

    /// Performs the unload operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InUse`.
    AssetResult AssetManager::unload(Asset asset) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(asset);
        if (record == nullptr) {
            return std::unexpected(error(AssetErrorCode::InvalidAsset,
                                         "Asset is stale, unloaded, or belongs to another AssetManager."));
        }

        if ((record->type == AssetType::Shader || record->type == AssetType::Texture) && impl_->referenced_by_a_model(asset)) {
            return std::unexpected(error(AssetErrorCode::InUse,
                                         "Cannot unload a " + std::string{to_string(record->type)} +
                                             " asset while a loaded model depends on it."));
        }

        impl_->destroy(*record);
        return {};
    }

    namespace {
        std::string memory_key(std::string_view kind, std::string_view name) {
            return "mem|" + std::string{kind} + "|" + std::string{name};
        }
    } // namespace

    AssetExpected<Asset> AssetManager::create_file_from_bytes(std::string_view name, std::vector<std::byte> bytes,
                                                              UString label) {
        const std::string key = memory_key("file", name);
        std::unique_lock lock{impl_->mutex};
        if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
            return it->second;
        }
        if (label.empty()) {
            label = UString{std::string{name}};
        }
        const usize size = bytes.size();
        const Asset asset = impl_->insert(Impl::Record{
            .type = AssetType::File,
            .label = std::move(label),
            .source = key,
            .memory_bytes = size,
            .data = Impl::FileData{.bytes = std::make_shared<std::vector<std::byte>>(std::move(bytes))},
        });
        impl_->path_cache.emplace(key, asset);
        return asset;
    }

    AssetResult AssetManager::replace_file(Asset asset, std::vector<std::byte> bytes) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(asset);
        auto *data = record ? std::get_if<Impl::FileData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "replace_file requires a live file asset."));
        }
        record->memory_bytes = bytes.size();
        // Readers holding the old shared bytes keep them; new reads see the replacement.
        data->bytes = std::make_shared<std::vector<std::byte>>(std::move(bytes));
        return {};
    }

    namespace {
        struct DecodedSound {
            std::shared_ptr<const Audio::SampleBuffer> buffer;
            SoundAssetInfo info;
        };

        AssetExpected<DecodedSound> decode_sound_bytes(std::vector<std::byte> encoded, std::string_view extension_hint) {
            auto decoded = Audio::load_sound_memory(Foundation::Io::ByteBlob::from_vector(std::move(encoded)),
                                                    UString{std::string{extension_hint}});
            if (!decoded) {
                return std::unexpected(AssetError{AssetErrorCode::DecodeFailure,
                                                  UString{"Could not decode sound data: " + decoded.error().cpp_string()}, {}});
            }
            DecodedSound out;
            out.buffer = *decoded;
            out.info = SoundAssetInfo{
                .channels = out.buffer->channels,
                .sample_rate = out.buffer->sample_rate,
                .frame_count = out.buffer->frames(),
                .duration_seconds = out.buffer->sample_rate != 0
                                        ? static_cast<f64>(out.buffer->frames()) / out.buffer->sample_rate
                                        : 0.0,
            };
            return out;
        }
    } // namespace

    AssetExpected<Asset> AssetManager::create_sound_from_bytes(std::string_view name, std::vector<std::byte> encoded,
                                                               std::string_view extension_hint, UString label) {
        const std::string key = memory_key("sound", name);
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }
        auto decoded = decode_sound_bytes(std::move(encoded), extension_hint);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (label.empty()) {
            label = UString{std::string{name}};
        }
        std::unique_lock lock{impl_->mutex};
        if (const auto it = impl_->path_cache.find(key); it != impl_->path_cache.end()) {
            return it->second;
        }
        const usize bytes = decoded->buffer->samples->size() * sizeof(f32);
        const Asset asset = impl_->insert(Impl::Record{
            .type = AssetType::Sound,
            .label = std::move(label),
            .source = key,
            .memory_bytes = bytes,
            .data = Impl::SoundData{.buffer = std::move(decoded->buffer), .info = decoded->info},
        });
        impl_->path_cache.emplace(key, asset);
        return asset;
    }

    AssetResult AssetManager::replace_sound(Asset asset, std::vector<std::byte> encoded,
                                            std::string_view extension_hint) {
        {
            std::shared_lock read_lock{impl_->mutex};
            const Impl::Record *record = impl_->find(asset);
            if (record == nullptr || !std::holds_alternative<Impl::SoundData>(record->data)) {
                return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                             "replace_sound requires a live sound asset."));
            }
        }
        auto decoded = decode_sound_bytes(std::move(encoded), extension_hint);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(asset);
        auto *data = record ? std::get_if<Impl::SoundData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(AssetErrorCode::InvalidAsset, "The sound asset was unloaded while replacing it."));
        }
        record->memory_bytes = decoded->buffer->samples->size() * sizeof(f32);
        // The mixer keeps playing voices on the buffer they started with; new plays use the replacement.
        data->buffer = std::move(decoded->buffer);
        data->info = decoded->info;
        return {};
    }

    AssetResult AssetManager::replace_model(Asset asset, ModelAssetDesc desc) {
        {
            std::shared_lock read_lock{impl_->mutex};
            const Impl::Record *record = impl_->find(asset);
            if (record == nullptr || !std::holds_alternative<Impl::ModelData>(record->data)) {
                return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                             "replace_model requires a live model asset."));
            }
        }
        // Build the replacement first so a failure leaves the old model untouched.
        auto built = create_model(std::move(desc));
        if (!built) {
            return std::unexpected(built.error());
        }
        std::unique_lock lock{impl_->mutex};
        Impl::Record *target = impl_->find(asset);
        Impl::Record *fresh = impl_->find(*built);
        if (target == nullptr || fresh == nullptr) {
            if (fresh != nullptr) {
                impl_->destroy(*fresh);
            }
            return std::unexpected(error(AssetErrorCode::InvalidAsset, "The model asset was unloaded while replacing it."));
        }
        // Free the old GPU resources, move the new ones under the old handle, and retire the temporary record without
        // destroying what it handed over.
        Impl::Data replacement = std::move(fresh->data);
        const usize bytes = fresh->memory_bytes;
        UString label = std::move(fresh->label);
        fresh->data = std::monostate{};
        fresh->loaded = false;
        fresh->memory_bytes = 0;
        const std::filesystem::path source = target->source;
        const u32 generation = target->generation;
        impl_->destroy(*target);
        // `destroy` retires the generation; the whole point here is that the handle stays valid.
        target->generation = generation;
        target->data = std::move(replacement);
        target->loaded = true;
        target->memory_bytes = bytes;
        target->label = std::move(label);
        target->source = source;
        return {};
    }

    /// Clears the stored state or contents.
    ///
    /// @return Returns the current clear value.
    /// @note This function does not throw exceptions.
    void AssetManager::clear() noexcept {
        if (!impl_) {
            return;
        }
        std::unique_lock lock{impl_->mutex};

        for (Impl::Record &record : impl_->records) {
            if (record.type == AssetType::Model) {
                impl_->destroy(record);
            }
        }
        for (Impl::Record &record : impl_->records) {
            if (record.type == AssetType::Texture) {
                impl_->destroy(record);
            }
        }
        for (Impl::Record &record : impl_->records) {
            if (record.type == AssetType::Shader) {
                impl_->destroy(record);
            }
        }
        for (Impl::Record &record : impl_->records) {
            impl_->destroy(record);
        }
        impl_->records.clear();
        impl_->free_records.clear();
        impl_->model_references.clear();
        impl_->loaded_count = 0;
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param model_asset `model_asset` value used by the operation.
    /// @param world_transform World used or affected by the operation.
    /// @param stable_id Identifier of the target object or resource.
    /// @param visibility_mask `visibility_mask` value used by the operation.
    /// @param sort_key Key used to identify the requested entry.
    /// @param destination Destination value or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool AssetManager::append_model_renderables(
        Asset model_asset,
        const glm::mat4 &world_transform,
        u64 stable_id,
        u32 visibility_mask,
        u32 sort_key,
        std::vector<SFT::Renderer::SceneRenderable> &destination) const noexcept {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(model_asset);
        const auto *model = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (model == nullptr) {
            return false;
        }
        destination.reserve(destination.size() + model->primitives.size());
        for (usize primitive_index = 0; primitive_index < model->primitives.size(); ++primitive_index) {
            const Impl::ModelPrimitiveData &primitive = model->primitives[primitive_index];
            destination.push_back(SFT::Renderer::SceneRenderable{
                .mesh = primitive.mesh,
                .material = primitive.material,
                .world_transform = world_transform,
                .stable_id = combine_stable_id(stable_id, primitive_index),
                .visibility_mask = visibility_mask,
                .sort_key = sort_key + static_cast<u32>(primitive_index),
                .cull_mode = primitive.double_sided ? RHI::CullMode::None : RHI::CullMode::Back,
            });
        }
        return true;
    }

} // namespace SFT::Engine
