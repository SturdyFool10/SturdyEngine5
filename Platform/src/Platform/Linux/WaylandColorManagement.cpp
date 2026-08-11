#include <Foundation/src/Foundation.hpp>

#if defined(__linux__)

#pragma region Imports
#include <algorithm>
#include <cstring>
#include <optional>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-color-management-v1-client-protocol.h>
#pragma endregion

#include <Platform/Linux/WaylandColorManagement.hpp>
#include <Platform/Window/WindowLog.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Platform::Windowing::Detail {

    namespace {

        struct ColorQuery {
            wl_display *display = nullptr;
            wl_event_queue *queue = nullptr;
            wl_registry *registry = nullptr;
            wp_color_manager_v1 *manager = nullptr;
            wp_color_management_surface_feedback_v1 *feedback = nullptr;
            wp_image_description_v1 *description = nullptr;
            wp_image_description_info_v1 *information = nullptr;
            std::optional<f32> reference_white_nits;
            bool description_ready = false;
            bool description_failed = false;
            bool information_done = false;
        };

        void assign_queue(void *proxy, wl_event_queue *queue) noexcept {
            if (proxy != nullptr) {
                wl_proxy_set_queue(static_cast<wl_proxy *>(proxy), queue);
            }
        }

        void manager_supported_intent(void *, wp_color_manager_v1 *, u32) noexcept {}
        void manager_supported_feature(void *, wp_color_manager_v1 *, u32) noexcept {}
        void manager_supported_tf(void *, wp_color_manager_v1 *, u32) noexcept {}
        void manager_supported_primaries(void *, wp_color_manager_v1 *, u32) noexcept {}
        void manager_done(void *, wp_color_manager_v1 *) noexcept {}

        constexpr wp_color_manager_v1_listener manager_listener{
            .supported_intent = manager_supported_intent,
            .supported_feature = manager_supported_feature,
            .supported_tf_named = manager_supported_tf,
            .supported_primaries_named = manager_supported_primaries,
            .done = manager_done,
        };

        void registry_global(void *data, wl_registry *registry, u32 name,
                             const char *interface, u32 version) noexcept {
            auto &query = *static_cast<ColorQuery *>(data);
            if (query.manager == nullptr &&
                std::strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
                query.manager = static_cast<wp_color_manager_v1 *>(
                    wl_registry_bind(registry, name, &wp_color_manager_v1_interface,
                                     std::min(version, 2u)));
                assign_queue(query.manager, query.queue);
                if (query.manager != nullptr) {
                    wp_color_manager_v1_add_listener(
                        query.manager, &manager_listener, &query);
                }
            }
        }

        void registry_global_remove(void *, wl_registry *, u32) noexcept {}

        constexpr wl_registry_listener registry_listener{
            .global = registry_global,
            .global_remove = registry_global_remove,
        };

        void preferred_changed(void *, wp_color_management_surface_feedback_v1 *, u32) noexcept {}
        void preferred_changed2(void *, wp_color_management_surface_feedback_v1 *, u32, u32) noexcept {}

        constexpr wp_color_management_surface_feedback_v1_listener feedback_listener{
            .preferred_changed = preferred_changed,
            .preferred_changed2 = preferred_changed2,
        };

        void description_failed(void *data, wp_image_description_v1 *, u32,
                                const char *) noexcept {
            static_cast<ColorQuery *>(data)->description_failed = true;
        }

        void description_ready(void *data, wp_image_description_v1 *, u32) noexcept {
            static_cast<ColorQuery *>(data)->description_ready = true;
        }

        void description_ready2(void *data, wp_image_description_v1 *, u32, u32) noexcept {
            static_cast<ColorQuery *>(data)->description_ready = true;
        }

        constexpr wp_image_description_v1_listener description_listener{
            .failed = description_failed,
            .ready = description_ready,
            .ready2 = description_ready2,
        };

        void information_done(void *data, wp_image_description_info_v1 *) noexcept {
            auto &query = *static_cast<ColorQuery *>(data);
            query.information_done = true;
            // The server object is gone after this destructor event, but wayland-client retains the
            // local proxy until wp_image_description_info_v1_destroy() calls wl_proxy_destroy().
        }

        void information_icc_file(void *, wp_image_description_info_v1 *, i32 fd, u32) noexcept {
            if (fd >= 0) {
                (void)::close(fd);
            }
        }

        void information_primaries(void *, wp_image_description_info_v1 *,
                                   i32, i32, i32, i32, i32, i32, i32, i32) noexcept {}
        void information_primaries_named(void *, wp_image_description_info_v1 *, u32) noexcept {}
        void information_tf_power(void *, wp_image_description_info_v1 *, u32) noexcept {}
        void information_tf_named(void *, wp_image_description_info_v1 *, u32) noexcept {}

        void information_luminances(void *data, wp_image_description_info_v1 *,
                                    u32, u32, u32 reference_luminance) noexcept {
            if (reference_luminance > 0) {
                static_cast<ColorQuery *>(data)->reference_white_nits =
                    static_cast<f32>(reference_luminance);
            }
        }

        void information_target_primaries(void *, wp_image_description_info_v1 *,
                                          i32, i32, i32, i32, i32, i32, i32, i32) noexcept {}
        void information_target_luminance(void *, wp_image_description_info_v1 *, u32, u32) noexcept {}
        void information_target_max_cll(void *, wp_image_description_info_v1 *, u32) noexcept {}
        void information_target_max_fall(void *, wp_image_description_info_v1 *, u32) noexcept {}

        constexpr wp_image_description_info_v1_listener information_listener{
            .done = information_done,
            .icc_file = information_icc_file,
            .primaries = information_primaries,
            .primaries_named = information_primaries_named,
            .tf_power = information_tf_power,
            .tf_named = information_tf_named,
            .luminances = information_luminances,
            .target_primaries = information_target_primaries,
            .target_luminance = information_target_luminance,
            .target_max_cll = information_target_max_cll,
            .target_max_fall = information_target_max_fall,
        };

        void destroy_query(ColorQuery &query) noexcept {
            if (query.information != nullptr) {
                wp_image_description_info_v1_destroy(query.information);
                query.information = nullptr;
            }
            if (query.description != nullptr) {
                wp_image_description_v1_destroy(query.description);
                query.description = nullptr;
            }
            if (query.feedback != nullptr) {
                wp_color_management_surface_feedback_v1_destroy(query.feedback);
                query.feedback = nullptr;
            }
            if (query.manager != nullptr) {
                wp_color_manager_v1_destroy(query.manager);
                query.manager = nullptr;
            }
            if (query.registry != nullptr) {
                wl_registry_destroy(query.registry);
                query.registry = nullptr;
            }
            if (query.display != nullptr) {
                (void)wl_display_flush(query.display);
            }
            if (query.queue != nullptr) {
                wl_event_queue_destroy(query.queue);
                query.queue = nullptr;
            }
        }

    } // namespace

    std::optional<f32> query_wayland_surface_reference_white(
        NativeWindowHandle handle) noexcept {
        ZoneScopedN("Windowing::query_wayland_surface_reference_white");
        if (handle.system != NativeWindowSystem::Wayland ||
            handle.display == nullptr || handle.window == nullptr) {
            return std::nullopt;
        }

        ColorQuery query{};
        query.display = static_cast<wl_display *>(handle.display);
        auto *surface = static_cast<wl_surface *>(handle.window);
        query.queue = wl_display_create_queue(query.display);
        if (query.queue == nullptr) {
            return std::nullopt;
        }
        // Queue assignment must happen before get_registry creates its child proxy. Creating through
        // a queue-bound wrapper makes inheritance atomic with respect to SDL's concurrent Wayland
        // reader; get_registry()+wl_proxy_set_queue() would leave a race window on the default queue.
        wl_proxy *display_wrapper = static_cast<wl_proxy *>(
            wl_proxy_create_wrapper(query.display));
        if (display_wrapper == nullptr) {
            destroy_query(query);
            return std::nullopt;
        }
        wl_proxy_set_queue(display_wrapper, query.queue);
        query.registry = wl_display_get_registry(
            reinterpret_cast<wl_display *>(display_wrapper));
        wl_proxy_wrapper_destroy(display_wrapper);
        if (query.registry == nullptr) {
            destroy_query(query);
            return std::nullopt;
        }
        wl_registry_add_listener(query.registry, &registry_listener, &query);

        // First roundtrip discovers/binds the manager; the second receives its initial capability
        // events. Both are isolated to this private queue and never dispatch SDL's Wayland objects.
        if (wl_display_roundtrip_queue(query.display, query.queue) < 0 ||
            wl_display_roundtrip_queue(query.display, query.queue) < 0 ||
            query.manager == nullptr) {
            destroy_query(query);
            return std::nullopt;
        }

        query.feedback = wp_color_manager_v1_get_surface_feedback(query.manager, surface);
        assign_queue(query.feedback, query.queue);
        if (query.feedback == nullptr ||
            wp_color_management_surface_feedback_v1_add_listener(
                query.feedback, &feedback_listener, &query) < 0) {
            destroy_query(query);
            return std::nullopt;
        }

        query.description =
            wp_color_management_surface_feedback_v1_get_preferred(query.feedback);
        assign_queue(query.description, query.queue);
        if (query.description == nullptr ||
            wp_image_description_v1_add_listener(
                query.description, &description_listener, &query) < 0 ||
            wl_display_roundtrip_queue(query.display, query.queue) < 0 ||
            query.description_failed || !query.description_ready) {
            destroy_query(query);
            return std::nullopt;
        }

        query.information = wp_image_description_v1_get_information(query.description);
        assign_queue(query.information, query.queue);
        if (query.information == nullptr ||
            wp_image_description_info_v1_add_listener(
                query.information, &information_listener, &query) < 0 ||
            wl_display_roundtrip_queue(query.display, query.queue) < 0 ||
            !query.information_done) {
            destroy_query(query);
            return std::nullopt;
        }

        const std::optional<f32> result = query.reference_white_nits;
        destroy_query(query);
        return result;
    }

} // namespace SFT::Platform::Windowing::Detail

#endif // defined(__linux__)
