#include <Web/JsBridge.hpp>

#include <Foundation/Foundation.hpp>

using namespace SFT;

/// Manual, browser-only smoke test for the JS bridge: exercises calling an existing global JS
/// function, DOM element creation/manipulation, and an event-listener round trip. Not run under
/// ctest (there is no browser in CI) -- built only for the Web target and driven by hand against a
/// real browser, the same way the WASM/WebGPU work in [[project_web_wasm_support]] was verified.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
int main() {
    Foundation::log_info("[JsBridgeTest] available() = {}", Web::available());

    // Calling a global function that the hosting page is expected to define before loading this
    // script (see the manual test harness's injected `window.sturdyBridgeProbe`).
    if (Web::global_exists("sturdyBridgeProbe")) {
        const Web::JsValue result = Web::call_global("sturdyBridgeProbe", 21, 21);
        Foundation::log_info("[JsBridgeTest] sturdyBridgeProbe(21, 21) = {}", result.as<int>());
    } else {
        Foundation::log_warn("[JsBridgeTest] sturdyBridgeProbe not found on the page");
    }

    // DOM manipulation: create a real element, attach it, set its text/attribute/style, then read
    // the attribute back to confirm the round trip actually touched the live DOM, not a detached copy.
    Web::JsValue element = Web::Dom::create_element("div");
    Web::Dom::set_attribute(element, "id", "sturdy-bridge-probe-element");
    Web::Dom::set_text(element, "Hello from C++ via the Sturdy JS bridge");
    Web::Dom::set_style(element, "color", "rgb(0, 200, 0)");
    Web::Dom::append_child(Web::Dom::document()["body"], element);

    const Web::JsValue found = Web::Dom::get_element_by_id("sturdy-bridge-probe-element");
    Foundation::log_info("[JsBridgeTest] round-tripped attribute id = {}",
                         Web::Dom::get_attribute(found, "id"));

    // Event listener round trip: click the element from JS (simulated via .click()) and confirm the
    // C++ callback actually runs, then remove it and click again to confirm it does not fire twice.
    static int click_count = 0;
    const u64 handle =
        Web::Dom::add_event_listener(element, "click", [](Web::JsValue) { ++click_count; });
    element.call<void>("click");
    Web::Dom::remove_event_listener(handle);
    element.call<void>("click");
    Foundation::log_info("[JsBridgeTest] click_count after remove (expect 1) = {}", click_count);

    return 0;
}
