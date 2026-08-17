#include <Foundation/src/Foundation.hpp>

#if defined(__linux__)

#pragma region Imports
#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <wayland-client.h>
#include <wayland-ext-background-effect-v1-client-protocol.h>
#include <wayland-org-kde-kwin-blur-client-protocol.h>
#pragma endregion

#include <Platform/Linux/WaylandBackgroundEffects.hpp>
#include <Platform/Window/WindowLog.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Platform::Windowing::Detail {

    namespace {

        struct WaylandBackgroundEffectConnection {
            wl_display *display = nullptr;
            wl_event_queue *event_queue = nullptr;
            wl_registry *registry = nullptr;
            wl_compositor *compositor = nullptr;
            ext_background_effect_manager_v1 *ext_manager = nullptr;
            org_kde_kwin_blur_manager *kde_manager = nullptr;
            u32 ext_capabilities = 0;
            std::unordered_map<wl_surface *, ext_background_effect_surface_v1 *> ext_surfaces;
            std::unordered_map<wl_surface *, org_kde_kwin_blur *> kde_surfaces;
        };

        std::mutex &connection_mutex() noexcept {
            static std::mutex mutex;
            return mutex;
        }

        std::vector<std::unique_ptr<WaylandBackgroundEffectConnection>> &connections() noexcept {
            static std::vector<std::unique_ptr<WaylandBackgroundEffectConnection>> values;
            return values;
        }

        void assign_queue(void *proxy, wl_event_queue *queue) noexcept {
            if (proxy != nullptr) {
                wl_proxy_set_queue(static_cast<wl_proxy *>(proxy), queue);
            }
        }

        void ext_capabilities(void *data, ext_background_effect_manager_v1 *, u32 flags) noexcept {
            static_cast<WaylandBackgroundEffectConnection *>(data)->ext_capabilities = flags;
        }

        constexpr ext_background_effect_manager_v1_listener ext_manager_listener{
            .capabilities = ext_capabilities,
        };

        void registry_global(void *data, wl_registry *registry, u32 name,
                             const char *interface, u32 version) noexcept {
            auto &connection = *static_cast<WaylandBackgroundEffectConnection *>(data);
            if (std::strcmp(interface, wl_compositor_interface.name) == 0 && connection.compositor == nullptr) {
                connection.compositor = static_cast<wl_compositor *>(
                    wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
                assign_queue(connection.compositor, connection.event_queue);
                return;
            }
            if (std::strcmp(interface, ext_background_effect_manager_v1_interface.name) == 0 &&
                connection.ext_manager == nullptr) {
                connection.ext_manager = static_cast<ext_background_effect_manager_v1 *>(
                    wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1));
                assign_queue(connection.ext_manager, connection.event_queue);
                ext_background_effect_manager_v1_add_listener(
                    connection.ext_manager, &ext_manager_listener, &connection);
                return;
            }
            if (std::strcmp(interface, org_kde_kwin_blur_manager_interface.name) == 0 &&
                connection.kde_manager == nullptr) {
                connection.kde_manager = static_cast<org_kde_kwin_blur_manager *>(
                    wl_registry_bind(registry, name, &org_kde_kwin_blur_manager_interface, 1));
                assign_queue(connection.kde_manager, connection.event_queue);
            }
        }

        void registry_global_remove(void *, wl_registry *, u32) noexcept {}

        constexpr wl_registry_listener registry_listener{
            .global = registry_global,
            .global_remove = registry_global_remove,
        };

        WaylandBackgroundEffectConnection *find_connection(wl_display *display) noexcept {
            const auto found = std::ranges::find_if(connections(), [display](const auto &connection) {
                return connection->display == display;
            });
            return found != connections().end() ? found->get() : nullptr;
        }

        void remove_ext_effect(WaylandBackgroundEffectConnection &connection, wl_surface *surface,
                               bool commit_surface) noexcept {
            const auto found = connection.ext_surfaces.find(surface);
            if (found == connection.ext_surfaces.end()) {
                return;
            }
            ext_background_effect_surface_v1_destroy(found->second);
            connection.ext_surfaces.erase(found);
            if (commit_surface) {
                wl_surface_commit(surface);
            }
        }

        void remove_kde_effect(WaylandBackgroundEffectConnection &connection, wl_surface *surface) noexcept {
            const auto found = connection.kde_surfaces.find(surface);
            if (found == connection.kde_surfaces.end()) {
                return;
            }
            if (connection.kde_manager != nullptr) {
                org_kde_kwin_blur_manager_unset(connection.kde_manager, surface);
            }
            org_kde_kwin_blur_release(found->second);
            connection.kde_surfaces.erase(found);
        }

        void destroy_connection(WaylandBackgroundEffectConnection &connection) noexcept {
            for (auto &[surface, effect] : connection.ext_surfaces) {
                (void)surface;
                ext_background_effect_surface_v1_destroy(effect);
            }
            connection.ext_surfaces.clear();
            for (auto &[surface, effect] : connection.kde_surfaces) {
                if (connection.kde_manager != nullptr) {
                    org_kde_kwin_blur_manager_unset(connection.kde_manager, surface);
                }
                org_kde_kwin_blur_release(effect);
            }
            connection.kde_surfaces.clear();
            if (connection.ext_manager != nullptr) {
                ext_background_effect_manager_v1_destroy(connection.ext_manager);
            }
            if (connection.kde_manager != nullptr) {
                org_kde_kwin_blur_manager_destroy(connection.kde_manager);
            }
            if (connection.compositor != nullptr) {
                wl_compositor_destroy(connection.compositor);
            }
            if (connection.registry != nullptr) {
                wl_registry_destroy(connection.registry);
            }
            if (connection.display != nullptr) {
                (void)wl_display_flush(connection.display);
            }
            if (connection.event_queue != nullptr) {
                wl_event_queue_destroy(connection.event_queue);
            }
        }

        WaylandBackgroundEffectConnection *ensure_connection(wl_display *display) noexcept {
            if (WaylandBackgroundEffectConnection *existing = find_connection(display)) {
                return existing;
            }

            auto connection = std::make_unique<WaylandBackgroundEffectConnection>();
            connection->display = display;
            connection->event_queue = wl_display_create_queue(display);
            if (connection->event_queue == nullptr) {
                return nullptr;
            }
            connection->registry = wl_display_get_registry(display);
            if (connection->registry == nullptr) {
                destroy_connection(*connection);
                return nullptr;
            }
            assign_queue(connection->registry, connection->event_queue);
            wl_registry_add_listener(connection->registry, &registry_listener, connection.get());




            if (wl_display_roundtrip_queue(display, connection->event_queue) < 0 ||
                wl_display_roundtrip_queue(display, connection->event_queue) < 0) {
                destroy_connection(*connection);
                return nullptr;
            }

            WaylandBackgroundEffectConnection *result = connection.get();
            connections().push_back(std::move(connection));
            return result;
        }

        wl_region *full_surface_region(WaylandBackgroundEffectConnection &connection) noexcept {
            if (connection.compositor == nullptr) {
                return nullptr;
            }
            wl_region *region = wl_compositor_create_region(connection.compositor);
            if (region != nullptr) {


                wl_region_add(region, 0, 0, INT_MAX, INT_MAX);
            }
            return region;
        }

        WindowEffectResult set_ext_blur(WaylandBackgroundEffectConnection &connection,
                                        wl_surface *surface, bool enabled) noexcept {
            if (!enabled) {
                remove_ext_effect(connection, surface, true);
                (void)wl_display_flush(connection.display);
                return WindowEffectResult::success("ext-background-effect-v1 blur disabled.");
            }
            if (connection.ext_manager == nullptr) {
                return WindowEffectResult::failed("The Wayland compositor does not advertise ext-background-effect-v1.");
            }
            if ((connection.ext_capabilities & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR) == 0u) {
                return WindowEffectResult::failed("The Wayland compositor advertises ext-background-effect-v1 without blur capability.");
            }
            wl_region *region = full_surface_region(connection);
            if (region == nullptr) {
                return WindowEffectResult::failed("ext-background-effect-v1 blur requires wl_compositor region support.");
            }

            remove_kde_effect(connection, surface);
            ext_background_effect_surface_v1 *&effect = connection.ext_surfaces[surface];
            if (effect == nullptr) {
                effect = ext_background_effect_manager_v1_get_background_effect(
                    connection.ext_manager, surface);
                assign_queue(effect, connection.event_queue);
            }
            if (effect == nullptr) {
                connection.ext_surfaces.erase(surface);
                wl_region_destroy(region);
                return WindowEffectResult::failed("Failed to create an ext-background-effect-v1 surface object.");
            }
            ext_background_effect_surface_v1_set_blur_region(effect, region);
            wl_region_destroy(region);
            wl_surface_commit(surface);
            (void)wl_display_flush(connection.display);
            window_info("Wayland background blur enabled via ext-background-effect-v1.");
            return WindowEffectResult::success("ext-background-effect-v1 blur applied.");
        }

        WindowEffectResult set_kde_blur(WaylandBackgroundEffectConnection &connection,
                                        wl_surface *surface, bool enabled) noexcept {
            if (!enabled) {
                remove_kde_effect(connection, surface);
                (void)wl_display_flush(connection.display);
                return WindowEffectResult::success("org_kde_kwin_blur disabled.");
            }
            if (connection.kde_manager == nullptr) {
                return WindowEffectResult::failed("The Wayland compositor does not advertise org_kde_kwin_blur_manager.");
            }
            wl_region *region = full_surface_region(connection);
            if (region == nullptr) {
                return WindowEffectResult::failed("org_kde_kwin_blur requires wl_compositor region support.");
            }

            remove_ext_effect(connection, surface, true);
            org_kde_kwin_blur *&effect = connection.kde_surfaces[surface];
            if (effect == nullptr) {
                effect = org_kde_kwin_blur_manager_create(connection.kde_manager, surface);
                assign_queue(effect, connection.event_queue);
            }
            if (effect == nullptr) {
                connection.kde_surfaces.erase(surface);
                wl_region_destroy(region);
                return WindowEffectResult::failed("Failed to create an org_kde_kwin_blur surface object.");
            }
            org_kde_kwin_blur_set_region(effect, region);
            wl_region_destroy(region);
            org_kde_kwin_blur_commit(effect);
            (void)wl_display_flush(connection.display);
            window_info("Wayland background blur enabled via org_kde_kwin_blur compatibility route.");
            return WindowEffectResult::success("org_kde_kwin_blur applied.");
        }

    } // namespace

    WindowEffectResult set_wayland_background_blur(
        NativeWindowHandle handle, WindowEffect effect) noexcept {
        ZoneScopedN("Windowing::set_wayland_background_blur");
        if (handle.system != NativeWindowSystem::Wayland || handle.display == nullptr || handle.window == nullptr) {
            return WindowEffectResult::failed("Wayland background blur requires a wl_display and wl_surface.");
        }

        auto *display = static_cast<wl_display *>(handle.display);
        auto *surface = static_cast<wl_surface *>(handle.window);
        const std::lock_guard lock{connection_mutex()};
        WaylandBackgroundEffectConnection *connection = ensure_connection(display);
        if (connection == nullptr) {
            return WindowEffectResult::failed("Failed to discover Wayland background-effect globals.");
        }



        if (wl_display_dispatch_queue_pending(display, connection->event_queue) < 0) {
            return WindowEffectResult::failed("Failed to dispatch pending Wayland background-effect events.");
        }

        switch (effect.linux_blur_protocol) {
            case LinuxBlurProtocol::ExtBackgroundEffect:
                return set_ext_blur(*connection, surface, effect.enabled);
            case LinuxBlurProtocol::KdeBlur:
                return set_kde_blur(*connection, surface, effect.enabled);
            case LinuxBlurProtocol::Automatic:
                break;
        }

        if (!effect.enabled) {
            remove_ext_effect(*connection, surface, true);
            remove_kde_effect(*connection, surface);
            (void)wl_display_flush(display);
            window_info("Wayland background blur disabled for the surface.");
            return WindowEffectResult::success("Wayland background blur disabled.");
        }

        const WindowEffectResult ext = set_ext_blur(*connection, surface, true);
        if (ext.succeeded()) {
            return ext;
        }
        const WindowEffectResult kde = set_kde_blur(*connection, surface, true);
        if (kde.succeeded()) {
            window_warn("ext-background-effect-v1 unavailable ({}); using org_kde_kwin_blur compatibility path.",
                        ext.details);
            return WindowEffectResult::degraded(
                "ext-background-effect-v1 was unavailable; org_kde_kwin_blur compatibility blur applied.");
        }
        window_error("Wayland background blur failed: ext='{}' kde='{}'.", ext.details, kde.details);
        return WindowEffectResult::failed(
            "Neither ext-background-effect-v1 nor org_kde_kwin_blur is available on this compositor.");
    }

    void release_wayland_background_effects(
        NativeWindowHandle handle, bool release_display) noexcept {
        if (handle.system != NativeWindowSystem::Wayland || handle.display == nullptr) {
            return;
        }
        const std::lock_guard lock{connection_mutex()};
        auto *display = static_cast<wl_display *>(handle.display);
        WaylandBackgroundEffectConnection *connection = find_connection(display);
        if (connection == nullptr) {
            return;
        }
        if (handle.window != nullptr) {
            auto *surface = static_cast<wl_surface *>(handle.window);
            remove_ext_effect(*connection, surface, false);
            remove_kde_effect(*connection, surface);
        }
        if (!release_display) {
            (void)wl_display_flush(display);
            return;
        }
        const auto found = std::ranges::find_if(connections(), [connection](const auto &candidate) {
            return candidate.get() == connection;
        });
        if (found != connections().end()) {
            destroy_connection(**found);
            connections().erase(found);
        }
    }

} // namespace SFT::Platform::Windowing::Detail

#endif // defined(__linux__)
