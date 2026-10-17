#pragma once

#include <Foundation/RangesCompat.hpp>
#include <Async/ParIter.hpp>
#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <ranges>
#include <span>

/// Views over interleaved sample data, so code walks frames and channels instead of computing `frame * channels + channel`
/// by hand. Every access is through a span or an iterator, which keeps a wrong channel count from reading or writing past the
/// end of a buffer. The views are cheap enough for the real-time path; offline work can hand its frames to `for_each_frame`.
namespace SFT::Audio {

    /// One span per frame of `samples` (`channels` samples each). A trailing partial frame, if any, comes out shorter.
    template <class T>
    [[nodiscard]] auto frames_of(std::span<T> samples, usize channels) {
        // Not std::views::chunk: libc++ does not implement it. This is the same view (random access, sized, reversible).
        const usize width = std::max<usize>(channels, 1);
        const usize count = (samples.size() + width - 1) / width;
        return std::views::iota(usize{0}, count) | std::views::transform([samples, width](usize frame) {
                   const usize begin = frame * width;
                   return samples.subspan(begin, std::min(width, samples.size() - begin));
               });
    }

    /// `(frame index, frame)` pairs.
    template <class T>
    [[nodiscard]] auto indexed_frames(std::span<T> samples, usize channels) {
        auto frames = frames_of(samples, channels);
        return std::views::zip(std::views::iota(usize{0}, static_cast<usize>(std::ranges::size(frames))), frames);
    }

    /// Channel `channel` of every frame, as a strided view (empty when `channel` is past the end).
    template <class T>
    [[nodiscard]] auto channel_of(std::span<T> samples, usize channels, usize channel) {
        return Foundation::Compat::stride(samples | std::views::drop(channel), static_cast<std::ptrdiff_t>(std::max<usize>(channels, 1)));
    }

    /// Items below which `for_each_parallel` stays on the calling thread: handing out tasks costs more than the work. Sized
    /// for per-sample work; callers whose items are expensive (an FFT per column) pass a smaller number.
    inline constexpr usize parallel_frame_threshold = usize{1} << 16;

    /// Runs `fn(item)` for every item of a random-access range, spread across the scheduler's workers when there are at
    /// least `threshold` items. `fn` must only touch its own item (and read-only shared state).
    template <std::ranges::random_access_range R, class Fn>
    void for_each_parallel(R &&items, Fn fn, usize threshold = parallel_frame_threshold) {
        if (static_cast<usize>(std::ranges::size(items)) < threshold) {
            std::ranges::for_each(items, fn);
        } else {
            Async::par_iter(std::forward<R>(items)).for_each(std::move(fn));
        }
    }

    /// `for_each_parallel` over `frames_of`-style frame views.
    template <std::ranges::random_access_range R, class Fn>
    void for_each_frame(R &&frames, Fn fn) {
        for_each_parallel(std::forward<R>(frames), std::move(fn));
    }

} // namespace SFT::Audio
