#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
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


    /// Performs the graphics error from RHI operation using the supplied arguments.
    ///
    /// @param error Error value describing the failure.
    /// @param operation `operation` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] Core::GraphicsBackendError graphics_error_from_rhi(const RHI::RhiError &error, const char *operation);


    /// Returns the requested clamp tile size.
    ///
    /// @param desired `desired` value used by the operation.
    /// @param limits `limits` value used by the operation.
    ///
    /// @return Returns the requested count or size.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 clamp_tile_size(u32 desired, const RHI::DeviceLimits &limits) noexcept;


    struct TileCoord {
        i32 x = 0;
        i32 y = 0;
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(TileCoord, TileCoord) noexcept = default;
    };

    struct TileCoordHash {
        /// Invokes the callable behavior provided by `TileCoordHash`.
        ///
        /// @param coord `coord` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize operator()(TileCoord coord) const noexcept;
    };


    struct TileAddress {
        TileCoord tile{};
        u32 local_x = 0;
        u32 local_y = 0;
    };

    /// Performs the locate in grid operation using the supplied arguments.
    ///
    /// @param logical_x `logical_x` value used by the operation.
    /// @param logical_y `logical_y` value used by the operation.
    /// @param tile_size Requested or available size for the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] TileAddress locate_in_grid(i32 logical_x, i32 logical_y, u32 tile_size) noexcept;


    /// Performs the tiles overlapping operation using the supplied arguments.
    ///
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param tile_size Requested or available size for the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] vector<TileCoord> tiles_overlapping(i32 x, i32 y, u32 width, u32 height, u32 tile_size) noexcept;


    template <typename Key, typename Hash = std::hash<Key>>
    class LruIndex {
      public:
        /// Constructs a `LruIndex` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        LruIndex() = default;


        /// Constructs a `LruIndex` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        LruIndex(const LruIndex &other) : order_(other.order_) { rebuild_nodes(); }

        /// Assigns a new value to this `LruIndex`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        LruIndex &operator=(const LruIndex &other) {
            if (this != &other) {
                order_ = other.order_;
                rebuild_nodes();
            }
            return *this;
        }

        /// Constructs a `LruIndex` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        LruIndex(LruIndex &&other) : order_(std::move(other.order_)) {
            rebuild_nodes();
            other.nodes_.clear();
        }

        /// Assigns a new value to this `LruIndex`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        LruIndex &operator=(LruIndex &&other) {
            if (this != &other) {
                order_ = std::move(other.order_);
                rebuild_nodes();
                other.nodes_.clear();
            }
            return *this;
        }


        /// Performs the touch operation for `LruIndex` using the supplied arguments.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool touch(Key key) {


            const auto node = std::ranges::find(order_, key);
            if (node != order_.end()) {


                order_.erase(node);
                order_.push_front(key);
                nodes_[key] = order_.begin();
                return false;
            }
            order_.push_front(key);
            nodes_[key] = order_.begin();
            return true;
        }


        /// Returns the current or globally available evict one value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<Key> evict_one() {
            if (order_.empty()) {
                return std::nullopt;
            }
            Key key = order_.back();
            order_.pop_back();
            nodes_.erase(key);
            return key;
        }


        /// Returns the current or globally available evict one if value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
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

        /// Erases the selected element or range from the container.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void erase(const Key &key) {
            const auto node = std::ranges::find(order_, key);
            if (node != order_.end()) {
                order_.erase(node);
            }
            nodes_.erase(key);
        }

        /// Reports whether contains holds for this `LruIndex`.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool contains(const Key &key) const { return nodes_.contains(key); }
        /// Returns the size for this `LruIndex`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `LruIndex`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept { return order_.size(); }
        /// Reports whether this `LruIndex` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept { return order_.empty(); }

      private:
        /// Performs the rebuild nodes operation for `LruIndex` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void rebuild_nodes() {
            nodes_.clear();
            nodes_.reserve(order_.size());
            for (auto it = order_.begin(); it != order_.end(); ++it) {
                nodes_.emplace(*it, it);
            }
        }

        list<Key> order_;
        unordered_map<Key, typename list<Key>::iterator, Hash> nodes_;
    };

} // namespace SFT::Renderer
