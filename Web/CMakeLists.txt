# Sturdy.Web -- an ergonomic C++ <-> JavaScript bridge for WASM/Emscripten builds (calling any
# JavaScript function by name, DOM manipulation). Depends on nothing but Foundation, mirroring
# Async's own minimal-dependency pattern, so any package (Engine, Renderer, Demos, ...) can use it
# without pulling in a heavier chain. Every function here is safe to call unconditionally on native
# platforms too -- it simply no-ops there (see JsBridge.hpp).
sturdy_add_package(Web
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation

    DEBUG_PUBLIC_DEPS
)

if(STURDY_OS STREQUAL "Web")
    target_compile_definitions(Web PUBLIC STURDY_PLATFORM_WEB=1)
    # EMSCRIPTEN_BINDINGS()/emscripten::function() (used to expose the event-listener dispatch
    # trampoline to JavaScript) need embind's runtime support linked in; plain `emscripten::val`
    # usage elsewhere in this package does not, but the flag is harmless either way.
    target_link_options(Web PUBLIC "--bind")

    # Manual, browser-only smoke test -- there is no browser in CI, so this is not a ctest, just a
    # buildable target a developer loads by hand (see JsBridgeWebManualTest.cpp's doc comment).
    add_executable(WebJsBridgeWebManualTest tests/JsBridgeWebManualTest.cpp)
    target_link_libraries(WebJsBridgeWebManualTest PRIVATE Sturdy::Web Sturdy::Foundation)
    sturdy_enable_warnings(WebJsBridgeWebManualTest)
    # See Demos/RuntimeDemo/CMakeLists.txt's identical override for why: without this, em++ emits a
    # bare .js/.wasm pair with no HTML shell to actually load in a browser.
    set_target_properties(WebJsBridgeWebManualTest PROPERTIES SUFFIX ".html")
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(WebJsBridgeNativeStubTest tests/JsBridgeNativeStubTest.cpp)
    target_link_libraries(WebJsBridgeNativeStubTest PRIVATE Sturdy::Web)
    sturdy_enable_warnings(WebJsBridgeNativeStubTest)
    add_test(NAME WebJsBridgeNativeStubTest COMMAND WebJsBridgeNativeStubTest)
    set_tests_properties(WebJsBridgeNativeStubTest PROPERTIES TIMEOUT 10)
endif()
