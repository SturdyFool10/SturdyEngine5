#pragma once

#include <Foundation/Foundation.hpp>

#include <iterator>

#include <map>
#include <optional>

namespace SFT::Renderer {

    /// A byte range of the vertex arena (`index == false`) or the index arena, waiting to be freed.
    struct GeometryRange {
        bool index = false;
        u64 offset = 0;
        u64 bytes = 0;
        /// The allocator generation the range belongs to; a range from before the arena was reset or compacted is ignored.
        u64 epoch = 0;
    };

    /// Hands out byte ranges of a geometry arena and takes them back, so the space of a destroyed mesh is used again.
    ///
    /// The arena is a bump region (`used`, the high-water mark) with the ranges below it that were released kept in two ordered
    /// indexes: by offset (to merge a released range with its neighbours) and by size (to find the smallest range that fits). Every
    /// operation is logarithmic in the number of free ranges, so millions of meshes coming and going cost no more per mesh than a few.
    ///
    /// Not thread safe: the renderer guards it.
    class GeometryRangeSet {
    public:
        /// Forgets every free range (the arena was recreated, or is about to be filled again from scratch).
        void reset() noexcept {
            by_offset_.clear();
            by_size_.clear();
            free_bytes_ = 0;
        }

        /// Whether a free range holds `bytes`.
        [[nodiscard]] bool fits(u64 bytes) const { return bytes == 0 || by_size_.lower_bound(bytes) != by_size_.end(); }

        /// Takes `bytes` from the smallest free range that holds them. `nullopt` when none does; the caller then bumps `used`.
        [[nodiscard]] std::optional<u64> allocate(u64 bytes) {
            if (bytes == 0) {
                return 0;
            }
            const auto fit = by_size_.lower_bound(bytes);
            if (fit == by_size_.end()) {
                return std::nullopt;
            }
            const u64 size = fit->first;
            const u64 offset = fit->second;
            by_size_.erase(fit);
            by_offset_.erase(offset);
            free_bytes_ -= size;
            if (size > bytes) {
                insert(offset + bytes, size - bytes);
            }
            return offset;
        }

        /// Gives a range back. A range that touches a free neighbour is merged with it; one that reaches the high-water mark `used`
        /// lowers it instead (and swallows a free range that then touches it), so a mostly empty arena is not left looking full.
        void release(u64 offset, u64 bytes, u64 &used) {
            if (bytes == 0) {
                return;
            }
            // Merge with the free range just before.
            if (auto next = by_offset_.lower_bound(offset); next != by_offset_.begin()) {
                const auto before = std::prev(next);
                if (before->first + before->second == offset) {
                    offset = before->first;
                    bytes += before->second;
                    erase(before->first);
                }
            }
            // Merge with the free range just after.
            if (const auto after = by_offset_.find(offset + bytes); after != by_offset_.end()) {
                bytes += after->second;
                erase(after->first);
            }
            if (offset + bytes >= used) {
                used = offset;
                return;
            }
            insert(offset, bytes);
        }

        [[nodiscard]] u64 free_bytes() const noexcept { return free_bytes_; }
        [[nodiscard]] usize range_count() const noexcept { return by_offset_.size(); }

    private:
        void insert(u64 offset, u64 bytes) {
            by_offset_.emplace(offset, bytes);
            by_size_.emplace(bytes, offset);
            free_bytes_ += bytes;
        }

        void erase(u64 offset) {
            const auto it = by_offset_.find(offset);
            if (it == by_offset_.end()) {
                return;
            }
            const u64 size = it->second;
            for (auto range = by_size_.equal_range(size); range.first != range.second; ++range.first) {
                if (range.first->second == offset) {
                    by_size_.erase(range.first);
                    break;
                }
            }
            free_bytes_ -= size;
            by_offset_.erase(it);
        }

        std::map<u64, u64> by_offset_;
        std::multimap<u64, u64> by_size_;
        u64 free_bytes_ = 0;
    };

} // namespace SFT::Renderer
