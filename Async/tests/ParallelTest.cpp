// Fork-join + parallel iterators: results must equal the sequential ones, ordering must hold, short-circuits must
// really short-circuit, and nested/failing parallel code must stay correct.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace SFT;
using namespace SFT::Async;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("ParallelTest FAILED: {}", what);
            ++failures;
        }
    }

    template <typename F>
    bool throws(F &&fn) {
        try {
            fn();
        } catch (...) {
            return true;
        }
        return false;
    }

    void primitives() {
        auto [a, b] = join([] { return 20; }, [] { return 22; });
        check(a + b == 42, "join returns both results");
        std::atomic<int> ran{0};
        (void)join([&] { ran++; }, [&] { ran++; });
        check(ran == 2, "join with void halves");

        // Both halves finish even when one throws, and the exception arrives.
        std::atomic<bool> slow_finished{false};
        const bool threw = throws([&] {
            (void)join([]() -> int { throw std::runtime_error("a"); },
                       [&] {
                           std::this_thread::sleep_for(std::chrono::milliseconds(50));
                           slow_finished = true;
                           return 1;
                       });
        });
        check(threw && slow_finished, "join waits for the other half before propagating");

        // Recursive fork-join (fib) stays correct and does not deadlock a small pool.
        struct Fib {
            static long run(int n) {
                if (n < 2) return n;
                if (n < 14) return run(n - 1) + run(n - 2);
                auto [x, y] = join([n] { return run(n - 1); }, [n] { return run(n - 2); });
                return x + y;
            }
        };
        check(Fib::run(27) == 196418, "recursive join computes fib(27)");

        std::vector<int> v(1000, 0);
        scope([&](auto &s) {
            for (int i = 0; i < 10; ++i) {
                s.spawn([&v, i] {
                    for (int j = 0; j < 100; ++j) v[static_cast<std::size_t>(i * 100 + j)] = i;
                });
            }
        });
        bool filled = true;
        for (int i = 0; i < 1000; ++i) filled = filled && v[static_cast<std::size_t>(i)] == i / 100;
        check(filled, "scope tasks borrow the stack and finish before scope returns");
        check(throws([] { scope([](auto &s) { s.spawn([] { throw std::logic_error("task"); }); }); }), "scope rethrows task exceptions");
        std::atomic<int> nested{0};
        scope([&](auto &s) { s.spawn([&] { scope([&](auto &inner) { inner.spawn([&] { nested++; }); }); }); });
        check(nested == 1, "scopes nest");
    }

    void equivalence() {
        std::mt19937 rng(7);
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{17}, std::size_t{1000}, std::size_t{123457}}) {
            std::vector<long> v(n);
            for (auto &x : v) x = static_cast<long>(rng() % 1000) - 500;

            check(par_iter(v).sum() == std::accumulate(v.begin(), v.end(), 0L), "sum");
            check(par_iter(v).count() == n, "count");
            check(par_iter(v).map([](long x) { return x * 3; }).sum() == 3 * std::accumulate(v.begin(), v.end(), 0L), "map+sum");

            const auto evens = par_iter(v).filter([](long x) { return x % 2 == 0; }).collect();
            std::vector<long> expected_evens;
            std::copy_if(v.begin(), v.end(), std::back_inserter(expected_evens), [](long x) { return x % 2 == 0; });
            check(evens == expected_evens, "filter keeps order");

            const auto mapped = par_iter(v).map([](long x) { return x * x; }).collect();
            bool same = mapped.size() == n;
            for (std::size_t i = 0; i < n && same; ++i) same = mapped[i] == v[i] * v[i];
            check(same, "map collect keeps order");

            const auto fm = par_iter(v).filter_map([](long x) -> std::optional<long> { return x > 0 ? std::optional<long>(x * 2) : std::nullopt; }).collect();
            std::vector<long> expected_fm;
            for (long x : v) if (x > 0) expected_fm.push_back(x * 2);
            check(fm == expected_fm, "filter_map");

            const auto flat = par_iter(std::vector<int>(n % 50, 3)).flat_map([](int x) { return std::vector<int>{x, x + 1}; }).collect();
            check(flat.size() == 2 * (n % 50), "flat_map expands");

            if (n > 0) {
                check(*par_iter(v).min() == *std::min_element(v.begin(), v.end()), "min");
                check(*par_iter(v).max() == *std::max_element(v.begin(), v.end()), "max");
                const auto first_min = std::min_element(v.begin(), v.end()) - v.begin();
                const auto by_key = par_iter(par_range(n).enumerate().collect()).min_by_key([&](const auto &p) { return v[p.first]; });
                check(by_key && static_cast<long>(by_key->first) == first_min, "min_by_key returns the first minimum");
                std::ptrdiff_t last_max = 0;
                for (std::size_t i = 0; i < n; ++i) if (v[i] == *std::max_element(v.begin(), v.end())) last_max = static_cast<std::ptrdiff_t>(i);
                const auto max_key = par_iter(par_range(n).enumerate().collect()).max_by_key([&](const auto &p) { return v[p.first]; });
                check(max_key && static_cast<long>(max_key->first) == last_max, "max_by_key returns the last maximum");
            } else {
                check(!par_iter(v).min().has_value(), "min of empty");
            }

            check(par_iter(v).any([](long x) { return x == 499; }) == std::any_of(v.begin(), v.end(), [](long x) { return x == 499; }), "any");
            check(par_iter(v).all([](long x) { return x >= -500; }), "all");
            check(par_iter(v).reduce(0L, [](long a, long b) { return a ^ b; }) == std::accumulate(v.begin(), v.end(), 0L, [](long a, long b) { return a ^ b; }), "reduce xor");
            check(par_iter(v).fold(0L, [](long acc, long x) { return acc + x; }, [](long a, long b) { return a + b; }) == std::accumulate(v.begin(), v.end(), 0L), "fold with combine");

            auto [pos, neg] = par_iter(v).partition([](long x) { return x >= 0; });
            check(pos.size() + neg.size() == n, "partition covers everything");
            check(std::all_of(pos.begin(), pos.end(), [](long x) { return x >= 0; }) && std::all_of(neg.begin(), neg.end(), [](long x) { return x < 0; }), "partition sides");
        }
    }

    void indexed_adapters() {
        std::vector<int> v(10000);
        std::iota(v.begin(), v.end(), 0);

        const auto zipped = par_iter(v).zip(v).map([](auto p) { return p.first + p.second; }).collect();
        bool ok = zipped.size() == v.size();
        for (std::size_t i = 0; i < v.size() && ok; ++i) ok = zipped[i] == static_cast<int>(2 * i);
        check(ok, "zip pairs element i with element i (and is race-free)");

        const auto en = par_iter(v).enumerate().filter([](auto p) { return p.first % 1000 == 0; }).map([](auto p) { return p.second; }).collect();
        check(en.size() == 10 && en[3] == 3000, "enumerate gives true indices");

        const auto reversed = par_iter(v).rev().collect();
        check(reversed.front() == 9999 && reversed.back() == 0, "rev");
        check(par_iter(v).skip(9990).collect().size() == 10, "skip");
        check(par_iter(v).take(7).sum() == 21, "take");
        const auto stepped = par_iter(v).step_by(3).collect();
        check(stepped.size() == 3334 && stepped[2] == 6, "step_by");
        const auto chained = par_iter(v).take(3).chain(par_iter(v).skip(9997)).collect();
        check((chained == std::vector<int>{0, 1, 2, 9997, 9998, 9999}), "chain");
        const auto copied = par_iter(v).take(5).cloned().collect();
        check(copied.size() == 5, "cloned");

        std::vector<int> data(10007, 1);
        par_chunks(data, 100).for_each([](std::span<int> chunk) {
            for (int &x : chunk) x = static_cast<int>(chunk.size());
        });
        check(data.front() == 100 && data.back() == 7 && std::accumulate(data.begin(), data.end(), 0L) == 100L * 10000 + 7 * 7, "par_chunks mutates disjoint chunks (short tail)");
        check(par_chunks_exact(data, 100).count() == 100, "par_chunks_exact drops the tail");
        check(par_windows(data, 3).count() == data.size() - 2, "par_windows count");
        const auto window_sums = par_windows(std::vector<int>{1, 2, 3, 4, 5}, 3).map([](std::span<int> w) { return w[0] + w[1] + w[2]; }).collect();
        (void)window_sums;
        std::vector<int> five{1, 2, 3, 4, 5};
        check((par_windows(five, 3).map([](std::span<int> w) { return w[0] + w[1] + w[2]; }).collect() == std::vector<int>{6, 9, 12}), "windows overlap");

        std::vector<int> out(v.size());
        par_iter(v).map([](int x) { return x + 1; }).map_into(out, [](int x) { return x * 2; });
        check(out[10] == 22, "map_into");
        check((par_iter(v).map([](int x) { return std::pair<int, int>(x, x * 2); }).unzip().second[4] == 8), "unzip");
        check((par_iter(v).take(4).to<std::set<int>>() == std::set<int>{0, 1, 2, 3}), "to<container>");
        check(*par_iter(v).position_first([](int x) { return x >= 4321; }) == 4321, "position_first");
    }

    void short_circuit() {
        std::vector<int> v(2'000'000);
        std::iota(v.begin(), v.end(), 0);
        std::atomic<std::size_t> visited{0};
        const bool found = par_iter(v).any([&](int x) {
            visited.fetch_add(1, std::memory_order_relaxed);
            return x == 10;
        });
        check(found, "any finds the element");
        check(visited < v.size() / 2, "any stops early instead of scanning everything");

        visited = 0;
        const auto first = par_iter(v).find_first([&](int x) {
            visited.fetch_add(1, std::memory_order_relaxed);
            return x % 100000 == 77777 % 100000 && x > 50000;
        });
        check(first && *first == 77777, "find_first is the leftmost match even when later leaves also match");
        check(visited < v.size(), "find_first skips leaves after a hit");

        check(!par_iter(v).find_any([](int x) { return x < 0; }).has_value(), "find_any on no match");
        check(par_iter(v).find_any([](int x) { return x == 1999999; }) == 1999999, "find_any finds the last element too");
        check(!par_iter(v).for_each_until([](int x) { return x != 5; }), "for_each_until reports it stopped");
        check(par_iter(v).take(100).for_each_until([](int) { return true; }), "for_each_until reports completion");
    }

    void exceptions_and_state() {
        std::vector<int> v(100000, 1);
        check(throws([&] { par_iter(v).for_each([](int x) { if (x == 1) throw std::runtime_error("boom"); }); }), "exception from for_each propagates");
        check(throws([&] { (void)par_iter(v).map([](int x) -> int { if (x == 1) throw std::runtime_error("boom"); return x; }).sum(); }), "exception from map propagates");

        std::atomic<int> inits{0};
        std::atomic<long> total{0};
        par_iter(v).for_each_init([&] { inits++; return std::vector<int>(); }, [&](std::vector<int> &scratch, int x) {
            scratch.push_back(x);
            total += x;
        });
        check(total == 100000, "for_each_init visits all elements");
        check(inits > 0 && inits < 1000, "init runs once per leaf, not once per element");
    }

    void nesting() {
        // A parallel loop whose body runs parallel loops: must not deadlock and must stay correct.
        std::vector<long> rows(64, 0);
        par_for(rows.size(), [&](std::size_t r) {
            std::vector<long> cols(256);
            std::iota(cols.begin(), cols.end(), 0);
            rows[r] = par_iter(cols).map([r](long c) { return c * static_cast<long>(r); }).sum();
        });
        bool ok = true;
        for (std::size_t r = 0; r < rows.size(); ++r) ok = ok && rows[r] == static_cast<long>(r) * (255 * 256 / 2);
        check(ok, "nested parallel loops are correct");
    }

    void sorting() {
        std::mt19937_64 rng(99);
        std::vector<std::pair<int, int>> v(400000);
        for (std::size_t i = 0; i < v.size(); ++i) v[i] = {static_cast<int>(rng() % 1000), static_cast<int>(i)};
        auto expected = v;
        std::stable_sort(expected.begin(), expected.end(), [](auto &a, auto &b) { return a.first < b.first; });
        auto sorted = v;
        par_sort(sorted, [](auto &a, auto &b) { return a.first < b.first; });
        check(sorted == expected, "par_sort matches std::stable_sort exactly (stable)");

        auto unstable = v;
        par_sort_unstable(unstable, [](auto &a, auto &b) { return a.first < b.first; });
        check(std::is_sorted(unstable.begin(), unstable.end(), [](auto &a, auto &b) { return a.first < b.first; }), "par_sort_unstable sorts");

        std::vector<std::string> words;
        for (int i = 0; i < 20000; ++i) words.push_back(std::to_string(rng() % 100000));
        auto words_sorted = words;
        par_sort(words_sorted);
        std::sort(words.begin(), words.end());
        check(words_sorted == words, "par_sort on strings");
        std::vector<int> tiny{3, 1, 2};
        par_sort(tiny);
        check((tiny == std::vector<int>{1, 2, 3}), "tiny input");
        par_sort_by_key(v.begin(), v.end(), [](const auto &p) { return p.second; });
        check(v.front().second == 0 && v.back().second == 399999, "sort by key");
    }

    long fib_join(int n) {
        if (n < 2) return n;
        if (n < 12) return fib_join(n - 1) + fib_join(n - 2);
        auto [a, b] = join([n] { return fib_join(n - 1); }, [n] { return fib_join(n - 2); });
        return a + b;
    }

    void pools() {
        ThreadPool two({.threads = 2, .name = "test-two"});
        check(two.num_threads() == 2, "a pool reports its size");

        // Everything inside install() runs on the pool's own threads, and only on them.
        std::set<std::thread::id> used;
        std::mutex used_mutex;
        std::atomic<std::size_t> reported_threads{0};
        two.install([&] {
            reported_threads = current_num_threads();
            par_for(64, [&](std::size_t) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::lock_guard lock(used_mutex);
                used.insert(std::this_thread::get_id());
            });
        });
        check(reported_threads == 2, "current_num_threads() inside a pool is the pool's size");
        check(!used.empty() && used.size() <= 2, "a 2-thread pool uses at most 2 threads for a parallel loop");
        check(used.find(std::this_thread::get_id()) == used.end(), "...and none of them is the caller");
        check(current_num_threads() >= 1 && !two.on_pool_thread(), "outside the pool the global scheduler is in charge again");

        const std::vector<long> data = [] { std::vector<long> v(100000); std::iota(v.begin(), v.end(), 1L); return v; }();
        check(two.install([&] { return par_iter(data).map([](long x) { return x * 2; }).sum(); }) == 2 * (100000L * 100001L / 2), "par_iter inside install gives the right answer");
        check(two.install([&] { return two.install([] { return 7; }); }) == 7, "install from inside the pool runs inline");

        // A single-thread pool running recursive fork-join must not deadlock: waiting threads run queued work themselves.
        ThreadPool one({.threads = 1, .name = "test-one"});
        check(one.install([] { return fib_join(24); }) == 46368, "recursive join on a 1-thread pool completes (waiters help)");

        // Pools are isolated from each other and from the global scheduler.
        ThreadPool blocked({.threads = 1, .name = "test-blocked"});
        std::atomic<bool> release{false};
        auto hog = blocked.spawn([&] {
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        const auto start = std::chrono::steady_clock::now();
        check(two.install([] { return fib_join(20); }) == 6765, "another pool keeps working while one is fully busy");
        check(Scheduler::spawn([] { return 5; }).wait() == 5, "...and so does the global scheduler");
        check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "(without waiting for the busy pool)");
        release = true;
        hog.wait();

        check(throws([&] { two.install([]() -> int { throw std::runtime_error("from pool"); }); }), "exceptions propagate out of install");

        // Destruction finishes queued work first.
        std::atomic<int> finished{0};
        {
            ThreadPool brief({.threads = 1, .name = "test-brief"});
            for (int i = 0; i < 20; ++i) (void)brief.spawn([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ++finished;
            });
        }
        check(finished == 20, "destroying a pool runs everything already queued");

        // Sorting inside a pool uses the pool.
        std::vector<int> unsorted(200000);
        std::mt19937 rng(5);
        for (auto &x : unsorted) x = static_cast<int>(rng());
        auto expected = unsorted;
        std::sort(expected.begin(), expected.end());
        two.install([&] { par_sort(unsorted); });
        check(unsorted == expected, "par_sort works inside a custom pool");
    }

    void simd_kernels() {
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{15}, std::size_t{16}, std::size_t{17}, std::size_t{255}, std::size_t{100003}}) {
            std::vector<float> f(n);
            for (auto &x : f) x = dist(rng);
            std::vector<double> d(f.begin(), f.end());
            double reference = 0;
            for (double x : d) reference += x;
            const double tolerance = 1e-3 * (std::max)(1.0, std::fabs(reference)) + 1e-2;
            check(std::fabs(static_cast<double>(par_iter(f).sum()) - reference) <= tolerance, "float sum agrees with the sequential sum (up to rounding)");
            check(std::fabs(par_iter(d).sum() - reference) <= 1e-9 * (std::max)(1.0, std::fabs(reference)) * static_cast<double>(n + 1), "double sum agrees");
            if (n > 0) {
                check(*par_iter(f).min() == *std::min_element(f.begin(), f.end()), "float min via the vector kernel");
                check(*par_iter(f).max() == *std::max_element(f.begin(), f.end()), "float max via the vector kernel");
                check(*par_iter(d).min() == *std::min_element(d.begin(), d.end()) && *par_iter(d).max() == *std::max_element(d.begin(), d.end()), "double min/max");
            } else {
                check(!par_iter(f).min().has_value() && !par_iter(f).max().has_value() && par_iter(f).sum() == 0.0f, "empty float ranges");
            }
        }
        // Repeatable: the summation order depends only on the data size, so two runs give bit-identical results.
        std::vector<float> big(1'000'003);
        for (auto &x : big) x = dist(rng);
        check(par_iter(big).sum() == par_iter(big).sum(), "a float sum is bit-for-bit repeatable");
        // Slices stay on the fast path and stay correct.
        const float sliced = par_iter(big).skip(10).take(500'000).sum();
        double sliced_reference = 0;
        for (std::size_t i = 10; i < 500'010; ++i) sliced_reference += big[i];
        check(std::fabs(static_cast<double>(sliced) - sliced_reference) <= 1e-3 * std::fabs(sliced_reference) + 1.0, "take/skip slices sum correctly");
        // A mapped iterator is not contiguous and takes the generic path -- same answer.
        check(std::fabs(static_cast<double>(par_iter(big).map([](float x) { return x; }).sum()) - static_cast<double>(par_iter(big).sum())) < 1.0, "the generic path agrees with the vector path");

        std::vector<float> huge(16 << 20);
        for (std::size_t i = 0; i < huge.size(); ++i) huge[i] = static_cast<float>(i % 1000) * 0.001f;
        const auto t0 = std::chrono::steady_clock::now();
        float naive = 0;
        for (float x : huge) naive += x;
        const auto t1 = std::chrono::steady_clock::now();
        const float fast = par_iter(huge).sum();
        const auto t2 = std::chrono::steady_clock::now();
        Foundation::log_info("ParallelTest: sum of 16M floats: naive loop {:.2f} ms, par_iter().sum() {:.2f} ms", std::chrono::duration<double, std::milli>(t1 - t0).count(),
                             std::chrono::duration<double, std::milli>(t2 - t1).count());
        check(std::fabs(fast - naive) / naive < 1e-3, "the large float sum agrees with the naive one (the naive one is the less accurate)");
    }

    void bridge() {
        std::vector<int> source(5000);
        std::iota(source.begin(), source.end(), 1);
        // A single-pass generator range (not random access, not sized).
        auto generated = source | std::views::filter([](int x) { return x % 2 == 0; });
        std::atomic<long> sum{0};
        par_bridge_for_each(generated, [&](int x) { sum += x; }, 16);
        long expected = 0;
        for (int x : source) if (x % 2 == 0) expected += x;
        check(sum == expected, "par_bridge_for_each processes a lazy range");
        check(throws([&] { par_bridge_for_each(source, [](int x) { if (x == 77) throw std::runtime_error("x"); }, 8); }), "par_bridge propagates exceptions");
    }

    void speed() {
        // Not asserted (CI machines vary) but logged: heavy per-item work should scale with cores.
        std::vector<double> v(2'000'000);
        std::iota(v.begin(), v.end(), 1.0);
        auto work = [](double x) { double s = x; for (int i = 0; i < 40; ++i) s = std::sqrt(s + i); return s; };
        const auto t0 = std::chrono::steady_clock::now();
        double sequential = 0;
        for (double x : v) sequential += work(x);
        const auto t1 = std::chrono::steady_clock::now();
        const double parallel = par_iter(v).map(work).sum();
        const auto t2 = std::chrono::steady_clock::now();
        const double seq_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double par_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        Foundation::log_info("ParallelTest: heavy map+sum over 2M items: sequential {:.1f} ms, parallel {:.1f} ms ({:.1f}x on {} threads)", seq_ms,
                             par_ms, seq_ms / (std::max)(par_ms, 0.001), current_num_threads());
        check(std::fabs(parallel - sequential) / sequential < 1e-9, "parallel float sum agrees with sequential within rounding");
    }

} // namespace

int main() {
    Scheduler::initialize(4);
    primitives();
    equivalence();
    indexed_adapters();
    short_circuit();
    exceptions_and_state();
    nesting();
    sorting();
    bridge();
    pools();
    simd_kernels();
    speed();
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
