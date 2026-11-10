/// GeometryRangeSet hands out and takes back ranges of the mesh arenas. The renderer relies on it to reuse the space of destroyed
/// meshes, so it must never hand out overlapping ranges, must merge neighbours, and must stay fast with millions of ranges.

#include <Renderer/GeometryAllocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace {
    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    using SFT::Renderer::GeometryRangeSet;

    /// What the renderer does for one mesh: a freed range when one fits, else the end of the arena.
    std::uint64_t take(GeometryRangeSet &set, std::uint64_t &used, std::uint64_t bytes) {
        if (const auto reused = set.allocate(bytes)) {
            return *reused;
        }
        const std::uint64_t at = used;
        used += bytes;
        return at;
    }

    void reuse_and_merge() {
        GeometryRangeSet set;
        std::uint64_t used = 0;
        const std::uint64_t a = take(set, used, 100);
        const std::uint64_t b = take(set, used, 100);
        const std::uint64_t c = take(set, used, 100);
        const std::uint64_t d = take(set, used, 100);
        check(a == 0 && b == 100 && c == 200 && d == 300 && used == 400, "fresh allocations are consecutive");

        set.release(b, 100, used);
        set.release(c, 100, used);
        check(set.range_count() == 1 && set.free_bytes() == 200, "two neighbouring ranges merge into one");
        check(set.fits(200) && !set.fits(201), "the merged range holds exactly 200 bytes");

        const std::uint64_t e = take(set, used, 150);
        check(e == 100, "a merged range is reused");
        check(set.free_bytes() == 50 && set.range_count() == 1, "the rest of the range stays free");

        const std::uint64_t f = take(set, used, 50);
        check(f == 250 && set.free_bytes() == 0, "the remainder is handed out next");
        check(used == 400, "reuse does not grow the arena");

        set.release(d, 100, used);
        check(used == 300, "releasing the last range lowers the high-water mark");
        set.release(f, 50, used);
        set.release(e, 150, used);
        check(used == 100 && set.range_count() == 0, "a run of freed ranges reaching the end all collapse into the arena's end");
        set.release(a, 100, used);
        check(used == 0, "an empty arena has nothing in use");
    }

    void best_fit_picks_the_smallest_range() {
        GeometryRangeSet set;
        std::uint64_t used = 0;
        // Sizes laid out with live gaps between them: 300, 100, 200.
        const std::uint64_t big = take(set, used, 300);
        const std::uint64_t keep1 = take(set, used, 10);
        const std::uint64_t small = take(set, used, 100);
        const std::uint64_t keep2 = take(set, used, 10);
        const std::uint64_t mid = take(set, used, 200);
        const std::uint64_t keep3 = take(set, used, 10);
        (void)keep1;
        (void)keep2;
        (void)keep3;
        set.release(big, 300, used);
        set.release(small, 100, used);
        set.release(mid, 200, used);
        check(take(set, used, 90) == small, "the smallest range that fits is chosen");
        check(take(set, used, 150) == mid, "the next size up picks the next smallest range");
    }

    /// Random mesh lifetimes: no two live ranges may ever overlap, whatever the order of allocations and releases.
    void live_ranges_never_overlap() {
        std::mt19937_64 rng{12345};
        GeometryRangeSet set;
        std::uint64_t used = 0;
        struct Live {
            std::uint64_t offset;
            std::uint64_t bytes;
        };
        std::vector<Live> live;
        for (int step = 0; step < 20000; ++step) {
            if (live.empty() || rng() % 3 != 0) {
                const std::uint64_t bytes = 64 * (1 + rng() % 40);
                live.push_back({take(set, used, bytes), bytes});
            } else {
                const std::size_t index = rng() % live.size();
                set.release(live[index].offset, live[index].bytes, used);
                live[index] = live.back();
                live.pop_back();
            }
        }
        std::sort(live.begin(), live.end(), [](const Live &l, const Live &r) { return l.offset < r.offset; });
        bool overlap = false;
        for (std::size_t i = 1; i < live.size(); ++i) {
            overlap |= live[i - 1].offset + live[i - 1].bytes > live[i].offset;
        }
        check(!overlap, "no two live ranges overlap after 20000 random allocations and releases");
        check(live.empty() || live.back().offset + live.back().bytes <= used, "every live range lies inside the arena");
        std::uint64_t live_bytes = 0;
        for (const Live &range : live) {
            live_bytes += range.bytes;
        }
        check(live_bytes + set.free_bytes() == used, "live plus free bytes account for the whole arena");
    }

    /// A million meshes made and dropped: the arena must stay the size of the live set, and it must be quick.
    void a_million_meshes() {
        GeometryRangeSet set;
        std::uint64_t used = 0;
        constexpr int live_target = 50000;
        constexpr int total = 1'000'000;
        std::vector<std::uint64_t> live;
        live.reserve(live_target + 1);
        const std::uint64_t mesh_bytes = 256;
        std::uint64_t high_water = 0;
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < total; ++i) {
            live.push_back(take(set, used, mesh_bytes));
            high_water = std::max(high_water, used);
            if (live.size() > live_target) {
                const std::size_t victim = (static_cast<std::size_t>(i) * 2654435761u) % live.size();
                set.release(live[victim], mesh_bytes, used);
                live[victim] = live.back();
                live.pop_back();
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "1,000,000 meshes through a " << live_target << "-mesh live set: " << seconds << " s, arena high-water "
                  << high_water / 1024 << " KiB, " << set.range_count() << " free range(s)\n";
        check(high_water <= static_cast<std::uint64_t>(live_target + 2) * mesh_bytes,
              "the arena never grows beyond the live set when meshes are all the same size");
        check(seconds < 5.0, "a million allocations and releases take well under five seconds");

        // Millions of meshes live at once.
        GeometryRangeSet big;
        std::uint64_t big_used = 0;
        const auto big_started = std::chrono::steady_clock::now();
        for (int i = 0; i < 3'000'000; ++i) {
            take(big, big_used, mesh_bytes);
        }
        const double big_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - big_started).count();
        std::cout << "3,000,000 live meshes: " << big_seconds << " s, " << big_used / (1024 * 1024) << " MiB\n";
        check(big_used == 3'000'000ull * mesh_bytes, "three million live meshes are laid out back to back");
        check(big_seconds < 5.0, "allocating three million meshes takes well under five seconds");
    }
} // namespace

int main() {
    reuse_and_merge();
    best_fit_picks_the_smallest_range();
    live_ranges_never_overlap();
    a_million_meshes();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "GeometryAllocatorTest passed\n";
    return 0;
}
