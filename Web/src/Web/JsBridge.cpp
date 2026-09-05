#include <Web/JsBridge.hpp>

#if defined(STURDY_PLATFORM_WEB)
#include <emscripten/bind.h>
#include <emscripten/emscripten.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#endif

namespace SFT::Web {

#if defined(STURDY_PLATFORM_WEB)

    namespace {

        /// Returns the process-wide `document` value, created on first use.
        ///
        /// @return Returns a read-only reference to the requested state.
        /// @note This function does not throw exceptions.
        const JsValue &document_value() {
            static const JsValue document = JsValue::global("document");
            return document;
        }

        /// Returns the process-wide `window` value, created on first use.
        ///
        /// @return Returns a read-only reference to the requested state.
        /// @note This function does not throw exceptions.
        const JsValue &window_value() {
            static const JsValue window = JsValue::global("window");
            return window;
        }

        /// Registry of live DOM event listeners, keyed by the opaque handle handed back to callers.
        /// A plain `std::mutex` (not `Async::Mutex<T>`) is deliberate: this package depends on
        /// nothing but Foundation (mirroring Async's/Foundation's own minimal-dependency pattern), and
        /// pulling in Async here just for this one lock would invert that. Web is single-threaded
        /// (see [[project_web_wasm_support]]), so contention is not a real concern regardless.
        std::mutex &listener_registry_mutex() {
            static std::mutex mutex;
            return mutex;
        }

        /// One installed listener: the C++ callback to run, plus everything `remove_event_listener`
        /// needs to actually detach the real JS-side listener (not just silence the dispatch) --
        /// `target.removeEventListener(event_name, listener)` requires the identical function
        /// reference and event name it was added with.
        struct ListenerEntry {
            Dom::EventCallback callback;
            JsValue target;
            std::string event_name;
            JsValue listener;
        };

        /// Returns the process-wide listener table.
        ///
        /// @return Returns a mutable reference to the requested state.
        /// @note This function does not throw exceptions.
        std::unordered_map<u64, ListenerEntry> &listener_registry() {
            static std::unordered_map<u64, ListenerEntry> registry;
            return registry;
        }

        /// Hands out the next opaque listener handle. Handles are never reused, so a stale handle
        /// passed to `remove_event_listener` after the real one was already removed is always a safe
        /// no-op rather than accidentally hitting an unrelated, newer listener.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        u64 next_listener_handle() noexcept {
            static std::atomic<u64> counter{1};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }

        /// Invoked from JavaScript (via the thunk installed in `Dom::add_event_listener`) whenever a
        /// registered listener fires. Looked up by `EMSCRIPTEN_BINDINGS` below.
        ///
        /// @param handle `handle` value used by the operation.
        /// @param event `event` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void dispatch_event_callback(double handle, emscripten::val event) {
            Dom::EventCallback callback;
            {
                std::scoped_lock lock{listener_registry_mutex()};
                auto it = listener_registry().find(static_cast<u64>(handle));
                if (it == listener_registry().end()) {
                    return;
                }
                // Copied out under the lock so the callback itself can freely add/remove other
                // listeners without deadlocking on this same mutex.
                callback = it->second.callback;
            }
            if (callback) {
                callback(std::move(event));
            }
        }

    } // namespace

    EMSCRIPTEN_BINDINGS(sturdy_web_js_bridge) {
        emscripten::function("__sturdyJsBridgeDispatch", &dispatch_event_callback);
    }

    /// Creates a JS closure that calls back into `dispatch_event_callback` with `handle`, without
    /// needing a bespoke `EM_JS` shim per event type. `Emval.toHandle`/`EM_VAL` are embind's own
    /// (documented) mechanism for handing a raw JS value back across the `EM_JS` boundary as a real
    /// `val` on the C++ side -- this is the standard way to construct a `val` from JS source that
    /// `emscripten/val.h` itself has no higher-level helper for.
    EM_JS(emscripten::EM_VAL, sturdy_make_event_listener_thunk, (double handle), {
        var fn = function(event) { Module.__sturdyJsBridgeDispatch(handle, event); };
        return Emval.toHandle(fn);
    });

    bool available() noexcept { return true; }

    bool global_exists(std::string_view name) noexcept {
        const std::string key{name};
        return !JsValue::global(key.c_str()).isUndefined();
    }

    namespace Dom {

        JsValue document() { return document_value(); }

        JsValue window() { return window_value(); }

        JsValue get_element_by_id(std::string_view id) {
            return document_value().call<JsValue>("getElementById", std::string{id});
        }

        JsValue query_selector(std::string_view selector) {
            return document_value().call<JsValue>("querySelector", std::string{selector});
        }

        JsValue query_selector_all(std::string_view selector) {
            return document_value().call<JsValue>("querySelectorAll", std::string{selector});
        }

        JsValue create_element(std::string_view tag_name) {
            return document_value().call<JsValue>("createElement", std::string{tag_name});
        }

        void append_child(const JsValue &parent, const JsValue &child) {
            parent.call<void>("appendChild", child);
        }

        void remove(const JsValue &element) { element.call<void>("remove"); }

        void set_text(const JsValue &element, std::string_view text) {
            // val::set() is non-const in embind even though it only ever mutates the JS object the
            // handle refers to, never the handle itself -- a const_cast here is correct, not a
            // workaround, since `element` genuinely isn't modified.
            const_cast<JsValue &>(element).set("textContent", std::string{text});
        }

        void set_html(const JsValue &element, std::string_view html) {
            const_cast<JsValue &>(element).set("innerHTML", std::string{html});
        }

        void set_attribute(const JsValue &element, std::string_view name, std::string_view value) {
            element.call<void>("setAttribute", std::string{name}, std::string{value});
        }

        std::string get_attribute(const JsValue &element, std::string_view name) {
            const JsValue result = element.call<JsValue>("getAttribute", std::string{name});
            if (result.isNull() || result.isUndefined()) {
                return {};
            }
            return result.as<std::string>();
        }

        void set_style(const JsValue &element, std::string_view property, std::string_view value) {
            element["style"].call<void>("setProperty", std::string{property}, std::string{value});
        }

        u64 add_event_listener(const JsValue &target, std::string_view event_name, EventCallback callback) {
            if (!callback) {
                return 0;
            }
            const u64 handle = next_listener_handle();
            JsValue listener =
                JsValue::take_ownership(sturdy_make_event_listener_thunk(static_cast<double>(handle)));
            target.call<void>("addEventListener", std::string{event_name}, listener);
            {
                std::scoped_lock lock{listener_registry_mutex()};
                listener_registry().emplace(
                    handle, ListenerEntry{std::move(callback), target, std::string{event_name},
                                          std::move(listener)});
            }
            return handle;
        }

        void remove_event_listener(u64 handle) {
            if (handle == 0) {
                return;
            }
            ListenerEntry entry;
            {
                std::scoped_lock lock{listener_registry_mutex()};
                auto it = listener_registry().find(handle);
                if (it == listener_registry().end()) {
                    return;
                }
                entry = std::move(it->second);
                listener_registry().erase(it);
            }
            // Detach for real -- removeEventListener() needs the identical function reference and
            // event name the listener was installed with, both kept in the registry entry.
            entry.target.call<void>("removeEventListener", entry.event_name, entry.listener);
        }

    } // namespace Dom

#else // !defined(STURDY_PLATFORM_WEB)

    bool available() noexcept { return false; }

    bool global_exists(std::string_view /*name*/) noexcept { return false; }

    namespace Dom {

        JsValue document() { return JsValue{}; }
        JsValue window() { return JsValue{}; }
        JsValue get_element_by_id(std::string_view /*id*/) { return JsValue{}; }
        JsValue query_selector(std::string_view /*selector*/) { return JsValue{}; }
        JsValue query_selector_all(std::string_view /*selector*/) { return JsValue{}; }
        JsValue create_element(std::string_view /*tag_name*/) { return JsValue{}; }
        void append_child(const JsValue & /*parent*/, const JsValue & /*child*/) {}
        void remove(const JsValue & /*element*/) {}
        void set_text(const JsValue & /*element*/, std::string_view /*text*/) {}
        void set_html(const JsValue & /*element*/, std::string_view /*html*/) {}
        void set_attribute(const JsValue & /*element*/, std::string_view /*name*/, std::string_view /*value*/) {}
        std::string get_attribute(const JsValue & /*element*/, std::string_view /*name*/) { return {}; }
        void set_style(const JsValue & /*element*/, std::string_view /*property*/, std::string_view /*value*/) {}
        u64 add_event_listener(const JsValue & /*target*/, std::string_view /*event_name*/,
                                EventCallback /*callback*/) {
            return 0;
        }
        void remove_event_listener(u64 /*handle*/) {}

    } // namespace Dom

#endif

} // namespace SFT::Web
