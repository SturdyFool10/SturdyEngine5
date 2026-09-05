#pragma once

#include <Foundation/Foundation.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#if defined(STURDY_PLATFORM_WEB)
#include <emscripten/val.h>
#endif

/// A small, ergonomic C++ <-> JavaScript bridge for WASM/Emscripten builds: call any JavaScript
/// function that exists by name, read/write properties, and manipulate the DOM, without hand-writing
/// `EM_ASM`/`EM_JS` glue per call site. Compiles (and safely no-ops) on every non-Web platform too, so
/// callers do not need to `#ifdef` every call site -- only code that truly cannot proceed without a
/// browser needs to check `SFT::Web::available()` itself.
namespace SFT::Web {

#if defined(STURDY_PLATFORM_WEB)
    /// A handle to any JavaScript value: an object, function, DOM node, string, number, ... On Web
    /// this *is* `emscripten::val` (aliased, not wrapped) -- its full API (`operator[]`, `operator()`,
    /// `call<T>(name, args...)`, `as<T>()`, `isNull()`/`isUndefined()`, ...) is already the ergonomic
    /// dynamic-call surface this bridge is built on, so there is no reason to re-invent it. Everything
    /// in this header beyond the alias is convenience built on top, plus the stub below that keeps the
    /// same call sites compiling on platforms with no JavaScript engine at all.
    using JsValue = emscripten::val;
#else
    /// Non-Web stand-in for `JsValue`: always "no such value," so cross-platform call sites using
    /// this header's free functions do not need to `#ifdef` themselves out on native builds.
    class JsValue {
    public:
        JsValue() noexcept = default;

        [[nodiscard]] static JsValue undefined() noexcept { return JsValue{}; }
        [[nodiscard]] static JsValue null_value() noexcept { return JsValue{}; }
        [[nodiscard]] static JsValue global(const char * /*name*/ = nullptr) noexcept { return JsValue{}; }

        [[nodiscard]] bool isUndefined() const noexcept { return true; }
        [[nodiscard]] bool isNull() const noexcept { return false; }
        [[nodiscard]] bool isString() const noexcept { return false; }
        [[nodiscard]] bool isNumber() const noexcept { return false; }

        [[nodiscard]] JsValue operator[](std::string_view /*key*/) const noexcept { return JsValue{}; }
        template <typename T> void set(std::string_view /*key*/, T && /*value*/) noexcept {}

        template <typename... Args>
        JsValue operator()(Args &&.../*args*/) const noexcept {
            return JsValue{};
        }

        template <typename Return = JsValue, typename... Args>
        Return call(const char * /*method*/, Args &&.../*args*/) const noexcept {
            if constexpr (std::is_same_v<Return, JsValue>) {
                return JsValue{};
            } else {
                return Return{};
            }
        }

        template <typename T>
        [[nodiscard]] T as() const noexcept {
            return T{};
        }
    };
#endif

    /// Reports whether the JavaScript bridge is actually backed by a real JS engine.
    ///
    /// @return Returns `true` only in a Web/Emscripten build; `false` everywhere else, including a
    /// build that targets Web but happens to run outside a browser.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool available() noexcept;

    /// Calls a global JavaScript function by name, e.g. `call_global("myFunction", 1, "hi")` for a
    /// JS-side `function myFunction(n, s) { ... }`. A no-op returning an empty value on non-Web
    /// platforms; safe to call unconditionally from shared game/engine code.
    ///
    /// @param function_name Name of a function reachable from the global scope (`window.<name>`).
    /// @param args Arguments forwarded to the JavaScript call, converted the same way `JsValue`'s own
    /// constructors/`operator()` would convert them (numbers, `bool`, `std::string`/`string_view`,
    /// other `JsValue`s, ...).
    ///
    /// @return Returns the JavaScript function's return value, or an undefined `JsValue` if the
    /// function does not exist or the bridge is unavailable on this platform.
    template <typename... Args>
    [[nodiscard]] JsValue call_global(std::string_view function_name, Args &&...args) {
        const std::string name{function_name};
        return JsValue::global(name.c_str())(std::forward<Args>(args)...);
    }

    /// Reports whether a global JavaScript function or value with the given name exists.
    ///
    /// @param name Name reachable from the global scope (`window.<name>`).
    ///
    /// @return Returns `true` when `name` resolves to something other than `undefined`; always
    /// `false` on non-Web platforms.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool global_exists(std::string_view name) noexcept;

    /// DOM manipulation helpers built on `JsValue`. Every function here is a thin, no-throw wrapper
    /// around the corresponding browser DOM call, and every one safely no-ops (returning an
    /// undefined/empty `JsValue` as appropriate) on non-Web platforms.
    namespace Dom {

        /// Returns the global `document` object.
        [[nodiscard]] JsValue document();

        /// Returns the global `window` object.
        [[nodiscard]] JsValue window();

        /// Looks up `document.getElementById(id)`.
        [[nodiscard]] JsValue get_element_by_id(std::string_view id);

        /// Looks up `document.querySelector(selector)`.
        [[nodiscard]] JsValue query_selector(std::string_view selector);

        /// Looks up every match of `document.querySelectorAll(selector)`.
        [[nodiscard]] JsValue query_selector_all(std::string_view selector);

        /// Creates a new, unattached element via `document.createElement(tag_name)`.
        [[nodiscard]] JsValue create_element(std::string_view tag_name);

        /// Appends `child` to `parent` (`parent.appendChild(child)`).
        void append_child(const JsValue &parent, const JsValue &child);

        /// Removes `element` from the DOM (`element.remove()`).
        void remove(const JsValue &element);

        /// Sets an element's text content (`element.textContent = text`), escaping any markup.
        void set_text(const JsValue &element, std::string_view text);

        /// Sets an element's inner HTML (`element.innerHTML = html`). Unlike `set_text`, `html` is
        /// parsed as markup -- never pass untrusted content through this.
        void set_html(const JsValue &element, std::string_view html);

        /// Sets an HTML attribute (`element.setAttribute(name, value)`).
        void set_attribute(const JsValue &element, std::string_view name, std::string_view value);

        /// Reads an HTML attribute (`element.getAttribute(name)`).
        ///
        /// @return Returns the attribute's value, or an empty string if it is not set.
        [[nodiscard]] std::string get_attribute(const JsValue &element, std::string_view name);

        /// Sets one inline CSS property (`element.style.setProperty(property, value)`).
        void set_style(const JsValue &element, std::string_view property, std::string_view value);

        /// A JavaScript-side event callback. Invoked with the DOM `Event` object.
        using EventCallback = std::function<void(JsValue event)>;

        /// Attaches `callback` to `target`'s `event_name` (`target.addEventListener(event_name, ...)`).
        /// `callback` is kept alive by the bridge until removed with `remove_event_listener`.
        ///
        /// @return Returns an opaque handle to pass to `remove_event_listener`; `0` on failure or on
        /// non-Web platforms.
        [[nodiscard]] u64 add_event_listener(const JsValue &target, std::string_view event_name,
                                              EventCallback callback);

        /// Detaches a listener previously installed by `add_event_listener`. The handle alone fully
        /// identifies the listener (target, event name, and function reference all travel with it),
        /// so nothing else needs to be passed back in. A no-op if `handle` is `0` or already removed.
        void remove_event_listener(u64 handle);

    } // namespace Dom

} // namespace SFT::Web
