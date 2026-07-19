#pragma once

#include <Foundation/src/Foundation.hpp>

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
    [[nodiscard]] Core::GraphicsBackendError graphics_error_from_rhi(const RHI::RhiError &error, const char *operation);

    // Clamps a desired tile edge length to the device's actual max 2D image dimension, so a tiled
    // resource (the glyph atlas — Renderer/TextAtlas.cppm; the large text canvas —
    // Renderer/TextCanvas.cppm) can never request a texture the GPU structurally cannot create.
    // `limits.max_texture_dimension_2d` unset (0, e.g. queried too early) leaves `desired` as-is.
    [[nodiscard]] u32 clamp_tile_size(u32 desired, const RHI::DeviceLimits &limits) noexcept;

    // A tile's position in an infinite logical grid of same-size square tiles. Signed so a grid
    // can extend in every direction from a (0, 0) origin (a canvas scrolled negative, say).
    struct TileCoord {
        i32 x = 0;
        i32 y = 0;
        [[nodiscard]] friend constexpr bool operator==(TileCoord, TileCoord) noexcept = default;
    };

    struct TileCoordHash {
        [[nodiscard]] usize operator()(TileCoord coord) const noexcept;
    };

    // Which tile a logical pixel coordinate falls in, and its offset within that tile. Floors
    // toward negative infinity (not toward zero), so tiling stays consistent across the origin.
    struct TileAddress {
        TileCoord tile{};
        u32 local_x = 0;
        u32 local_y = 0;
    };

    [[nodiscard]] TileAddress locate_in_grid(i32 logical_x, i32 logical_y, u32 tile_size) noexcept;

    // Every tile a logical rectangle [x, x+width) x [y, y+height) overlaps — used to split a draw
    // or upload that crosses tile boundaries (Renderer/TextCanvas.cppm's draw_run()).
    [[nodiscard]] vector<TileCoord> tiles_overlapping(i32 x, i32 y, u32 width, u32 height, u32 tile_size) noexcept;

    // A least-recently-used index over `Key`, shared by the glyph atlas (Renderer/TextAtlas.cpp,
    // evicting individual glyph rectangles) and the large text canvas (Renderer/TextCanvas.cpp,
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

        // Evicts the least-recently-used key accepted by `predicate`. This lets a caller pin the
        // resources referenced by the draw it is currently building while still reclaiming older
        // entries. Walking from the back preserves ordinary LRU order among eligible entries.
        template <typename Predicate>
        [[nodiscard]] optional<Key> evict_one_if(Predicate &&predicate) {
            for (auto reverse = order_.rbegin(); reverse != order_.rend(); ++reverse) {
                if (!predicate(*reverse)) {
                    continue;
                }
                Key key = *reverse;
                auto forward = reverse.base();
                --forward;
                order_.erase(forward);
                nodes_.erase(key);
                return key;
            }
            return std::nullopt;
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
