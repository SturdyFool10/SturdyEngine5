#include <Web/JsBridge.hpp>

#include <iostream>

/// Verifies the non-Web stub in JsBridge.hpp/.cpp: every call must safely no-op (rather than fail to
/// compile or crash) so shared game/engine code can call this API unconditionally, only checking
/// `SFT::Web::available()` where it truly cannot proceed without a real JS engine.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
int main() {
    using namespace SFT::Web;

    if (available()) {
        std::cerr << "expected available() == false on a native build\n";
        return 1;
    }
    if (global_exists("anything")) {
        std::cerr << "expected global_exists() == false on a native build\n";
        return 1;
    }

    const JsValue result = call_global("doesNotExist", 1, "two", 3.0);
    if (!result.isUndefined()) {
        std::cerr << "expected call_global() to yield an undefined JsValue on a native build\n";
        return 1;
    }

    const JsValue element = Dom::get_element_by_id("missing");
    Dom::set_text(element, "hello");
    Dom::set_attribute(element, "data-test", "value");
    if (!Dom::get_attribute(element, "data-test").empty()) {
        std::cerr << "expected get_attribute() to yield an empty string on a native build\n";
        return 1;
    }

    const u64 handle = Dom::add_event_listener(element, "click", [](JsValue) {});
    if (handle != 0) {
        std::cerr << "expected add_event_listener() to yield handle 0 on a native build\n";
        return 1;
    }
    Dom::remove_event_listener(handle);

    return 0;
}
