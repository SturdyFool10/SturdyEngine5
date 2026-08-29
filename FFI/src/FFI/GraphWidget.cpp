/// C ABI implementation of the graph-widget data series handle.
///
/// `sturdy_ui_graph_draw` itself (Ui.cpp) is a one-shot, session-bound draw call, same shape as
/// `sturdy_ui_text` — it belongs there because it needs the open UI session's `Context`, exactly
/// like every other `sturdy_ui_*` drawing call. This file owns the separate, session-independent
/// concern of a `SturdyGraphSeries`: an owned ring buffer a caller pushes `(x, y)` samples into
/// across many frames (live/streaming telemetry), then reads back as flat arrays to bind into a
/// `SturdyUiGraphSeriesRef` each draw. Storage is a `map<u64, unique_ptr<GraphSeriesStorage>>`
/// keyed by the minted token, the same pattern `RhiResources.cpp` uses for command encoders.

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::u32;
    using SFT::u64;
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;

    /// One caller-owned ring-buffer data series. `flat_x`/`flat_y` are scratch arrays rebuilt on
    /// demand by `sturdy_ui_graph_series_data` — `push`/`clear` only touch the deques, so a series
    /// that's pushed to every frame but never read back doesn't pay for the rebuild.
    struct GraphSeriesStorage {
        std::deque<double> x;
        std::deque<double> y;
        u32 capacity = 0; // 0 = unbounded
        std::vector<double> flat_x;
        std::vector<double> flat_y;
    };

    std::mutex g_series_mutex;
    std::map<u64, std::unique_ptr<GraphSeriesStorage>> g_series;

    /// Resolves a series handle to its storage.
    ///
    /// @param series Handle supplied by the caller.
    /// @param out_storage Receives the borrowed storage on success.
    ///
    /// @return `STURDY_OK`, or the handle failure `resolve_handle` reported.
    /// @note This function does not throw exceptions. The error slot is populated on failure.
    [[nodiscard]] SturdyResult resolve_series(SturdyGraphSeries series, GraphSeriesStorage **out_storage) noexcept {
        void *pointer = nullptr;
        const SturdyResult resolved = resolve_handle(series.token, HandleKind::GraphSeries, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_storage = static_cast<GraphSeriesStorage *>(pointer);
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_ui_graph_series_create(SturdyEngine engine, uint32_t capacity,
                                                            SturdyGraphSeries *out_series) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (out_series == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_series must not be null");
        }
        auto owned = std::make_unique<GraphSeriesStorage>();
        owned->capacity = capacity;
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::GraphSeries, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_series_mutex};
            g_series.emplace(token, std::move(owned));
        }
        out_series->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_graph_series_push(SturdyGraphSeries series, double x, double y) {
    return guarded([&]() -> SturdyResult {
        GraphSeriesStorage *storage = nullptr;
        const SturdyResult resolved = resolve_series(series, &storage);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        storage->x.push_back(x);
        storage->y.push_back(y);
        if (storage->capacity != 0) {
            while (storage->x.size() > storage->capacity) {
                storage->x.pop_front();
                storage->y.pop_front();
            }
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_graph_series_clear(SturdyGraphSeries series) {
    return guarded([&]() -> SturdyResult {
        GraphSeriesStorage *storage = nullptr;
        const SturdyResult resolved = resolve_series(series, &storage);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        storage->x.clear();
        storage->y.clear();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_graph_series_data(SturdyGraphSeries series, const double **out_x,
                                                         const double **out_y, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_x == nullptr || out_y == nullptr || out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointers must not be null");
        }
        GraphSeriesStorage *storage = nullptr;
        const SturdyResult resolved = resolve_series(series, &storage);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        storage->flat_x.assign(storage->x.begin(), storage->x.end());
        storage->flat_y.assign(storage->y.begin(), storage->y.end());
        *out_x = storage->flat_x.data();
        *out_y = storage->flat_y.data();
        *out_count = static_cast<uint32_t>(storage->flat_x.size());
        return STURDY_OK;
    });
}

void STURDY_ABI_CALL sturdy_ui_graph_series_release(SturdyGraphSeries series) {
    (void)guarded([&]() -> SturdyResult {
        revoke_handle(series.token);
        const std::lock_guard<std::mutex> lock{g_series_mutex};
        g_series.erase(series.token);
        return STURDY_OK;
    });
}

} // extern "C"
