#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>

using std::list;
using std::optional;
using std::string;
using std::unordered_map;
using std::vector;

namespace SFT::Renderer {

    // Converts an RHI-level error into a Core::GraphicsBackendError, tagged with `operation` for
    // context. Free-standing (unlike the private Renderer::graphics_error_from_rhi member) so any
    // Renderer-side class outside the main Renderer object — the tiled glyph atlas
    // (Renderer/TextAtlas.cppm), the tiled text canvas (Renderer/TextCanvas.cppm) — can report RHI
    // failures the same way the rest of the engine does.
    [[nodiscard]] inline Core::GraphicsBackendError graphics_error_from_rhi(const RHI::RhiError &error, const char *operation) {
        Core::GraphicsBackendErrorCode code = Core::GraphicsBackendErrorCode::OperationFailed;
        switch (error.code) {
            case RHI::RhiErrorCode::Unsupported: code = Core::GraphicsBackendErrorCode::Unsupported; break;
            case RHI::RhiErrorCode::OutOfMemory: code = Core::GraphicsBackendErrorCode::OutOfMemory; break;
            case RHI::RhiErrorCode::DeviceLost: code = Core::GraphicsBackendErrorCode::DeviceLost; break;
            case RHI::RhiErrorCode::SurfaceLost: code = Core::GraphicsBackendErrorCode::SurfaceLost; break;
            case RHI::RhiErrorCode::InvalidArgument:
            case RHI::RhiErrorCode::NotReady:
            case RHI::RhiErrorCode::OperationFailed:
                code = Core::GraphicsBackendErrorCode::OperationFailed;
                break;
        }
        return Core::GraphicsBackendError{
            .code = code,
            .message = string(operation) + " failed through RHI: " + error.message,
        };
    }

    // Clamps a desired tile edge length to the device's actual max 2D image dimension, so a tiled
    // resource (the glyph atlas — Renderer/TextAtlas.cppm; the large text canvas —
    // Renderer/TextCanvas.cppm) can never request a texture the GPU structurally cannot create.
    // `limits.max_texture_dimension_2d` unset (0, e.g. queried too early) leaves `desired` as-is.
    [[nodiscard]] inline u32 clamp_tile_size(u32 desired, const RHI::DeviceLimits &limits) noexcept {
        const u32 device_max = limits.max_texture_dimension_2d;
        if (device_max == 0 || desired < device_max) {
            return desired;
        }
        return device_max;
    }

    // A tile's position in an infinite logical grid of same-size square tiles. Signed so a grid
    // can extend in every direction from a (0, 0) origin (a canvas scrolled negative, say).
    struct TileCoord {
        i32 x = 0;
        i32 y = 0;
        [[nodiscard]] friend constexpr bool operator==(TileCoord, TileCoord) noexcept = default;
    };

    struct TileCoordHash {
        [[nodiscard]] usize operator()(TileCoord coord) const noexcept {
            const u64 packed = (static_cast<u64>(static_cast<u32>(coord.x)) << 32) | static_cast<u32>(coord.y);
            u64 hashed = packed;
            hashed ^= hashed >> 33;
            hashed *= 0xff51afd7ed558ccdULL;
            hashed ^= hashed >> 33;
            hashed *= 0xc4ceb9fe1a85ec53ULL;
            hashed ^= hashed >> 33;
            return static_cast<usize>(hashed);
        }
    };

    // Which tile a logical pixel coordinate falls in, and its offset within that tile. Floors
    // toward negative infinity (not toward zero), so tiling stays consistent across the origin.
    struct TileAddress {
        TileCoord tile{};
        u32 local_x = 0;
        u32 local_y = 0;
    };

    [[nodiscard]] inline TileAddress locate_in_grid(i32 logical_x, i32 logical_y, u32 tile_size) noexcept {
        const i32 size = static_cast<i32>(tile_size);
        const i32 tile_x = logical_x >= 0 ? logical_x / size : -(((-logical_x) + size - 1) / size);
        const i32 tile_y = logical_y >= 0 ? logical_y / size : -(((-logical_y) + size - 1) / size);
        return TileAddress{
            .tile = TileCoord{tile_x, tile_y},
            .local_x = static_cast<u32>(logical_x - tile_x * size),
            .local_y = static_cast<u32>(logical_y - tile_y * size),
        };
    }

    // Every tile a logical rectangle [x, x+width) x [y, y+height) overlaps — used to split a draw
    // or upload that crosses tile boundaries (Renderer/TextCanvas.cppm's draw_run()).
    [[nodiscard]] inline vector<TileCoord> tiles_overlapping(i32 x, i32 y, u32 width, u32 height, u32 tile_size) noexcept {
        vector<TileCoord> result;
        if (width == 0 || height == 0) {
            return result;
        }
        const TileAddress min_addr = locate_in_grid(x, y, tile_size);
        const TileAddress max_addr = locate_in_grid(x + static_cast<i32>(width) - 1, y + static_cast<i32>(height) - 1, tile_size);
        result.reserve(static_cast<usize>(max_addr.tile.y - min_addr.tile.y + 1) *
                       static_cast<usize>(max_addr.tile.x - min_addr.tile.x + 1));
        for (i32 ty = min_addr.tile.y; ty <= max_addr.tile.y; ++ty) {
            for (i32 tx = min_addr.tile.x; tx <= max_addr.tile.x; ++tx) {
                result.push_back(TileCoord{tx, ty});
            }
        }
        return result;
    }

    // A least-recently-used index over `Key`, shared by the glyph atlas (Renderer/TextAtlas.cppm,
    // evicting individual glyph cells) and the large text canvas (Renderer/TextCanvas.cppm,
    // evicting whole tiles). Tracks residency order only — it does not own whatever `Key` maps to;
    // the caller looks up/destroys the associated resource when `evict_one()` returns a key.
    template <typename Key, typename Hash = std::hash<Key>>
    class LruIndex {
      public:
        // Marks `key` as most-recently-used, inserting it if not already tracked. Returns true if
        // this was a new insertion (the caller must still assign it storage), false if `key` was
        // already resident (its position in the LRU order is simply refreshed).
        bool touch(Key key) {
            auto it = nodes_.find(key);
            if (it != nodes_.end()) {
                order_.splice(order_.begin(), order_, it->second);
                it->second = order_.begin();
                return false;
            }
            order_.push_front(key);
            nodes_.emplace(std::move(key), order_.begin());
            return true;
        }

        // Evicts and returns the least-recently-used key, or nullopt if nothing is tracked.
        [[nodiscard]] optional<Key> evict_one() {
            if (order_.empty()) {
                return std::nullopt;
            }
            Key key = order_.back();
            order_.pop_back();
            nodes_.erase(key);
            return key;
        }

        void erase(const Key &key) {
            auto it = nodes_.find(key);
            if (it == nodes_.end()) {
                return;
            }
            order_.erase(it->second);
            nodes_.erase(it);
        }

        [[nodiscard]] bool contains(const Key &key) const { return nodes_.contains(key); }
        [[nodiscard]] usize size() const noexcept { return order_.size(); }
        [[nodiscard]] bool empty() const noexcept { return order_.empty(); }

      private:
        list<Key> order_; // front = most-recently-used, back = least-recently-used
        unordered_map<Key, typename list<Key>::iterator, Hash> nodes_;
    };

} // namespace SFT::Renderer
