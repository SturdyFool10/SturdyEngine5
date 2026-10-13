#include <WindowManager/WindowChrome.hpp>

#include <Async/Mutex.hpp>

#include <unordered_map>

namespace SFT::WindowManager {

    namespace {

        Async::Mutex<std::unordered_map<WindowId, std::vector<DroppedItem>>> &dropped_items() {
            static Async::Mutex<std::unordered_map<WindowId, std::vector<DroppedItem>>> items;
            return items;
        }

    } // namespace

    HitTestResult classify_hit(const HitTestRegions &regions, WindowExtent size, WindowPosition point,
                               bool allow_resize) noexcept {
        if (!regions.enabled) {
            return HitTestResult::Normal;
        }
        const i32 width = static_cast<i32>(size.x);
        const i32 height = static_cast<i32>(size.y);
        const i32 border = static_cast<i32>(regions.resize_border);
        if (allow_resize && border > 0 && width > 0 && height > 0) {
            const bool left = point.x < border;
            const bool right = point.x >= width - border;
            const bool top = point.y < border;
            const bool bottom = point.y >= height - border;
            if (top && left) return HitTestResult::ResizeTopLeft;
            if (top && right) return HitTestResult::ResizeTopRight;
            if (bottom && left) return HitTestResult::ResizeBottomLeft;
            if (bottom && right) return HitTestResult::ResizeBottomRight;
            if (top) return HitTestResult::ResizeTop;
            if (bottom) return HitTestResult::ResizeBottom;
            if (left) return HitTestResult::ResizeLeft;
            if (right) return HitTestResult::ResizeRight;
        }
        for (const WindowRect &rect : regions.no_drag) {
            if (rect.contains(point.x, point.y)) {
                return HitTestResult::Normal;
            }
        }
        for (const WindowRect &rect : regions.drag) {
            if (rect.contains(point.x, point.y)) {
                return HitTestResult::Draggable;
            }
        }
        return HitTestResult::Normal;
    }

    void push_dropped_item(WindowId window, DroppedItem item) {
        auto guard = dropped_items().lock();
        (*guard)[window].push_back(std::move(item));
    }

    std::vector<DroppedItem> take_dropped_items(WindowId window) {
        auto guard = dropped_items().lock();
        const auto found = guard->find(window);
        if (found == guard->end()) {
            return {};
        }
        std::vector<DroppedItem> out = std::move(found->second);
        guard->erase(found);
        return out;
    }

} // namespace SFT::WindowManager
