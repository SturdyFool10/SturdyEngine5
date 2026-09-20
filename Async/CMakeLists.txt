sturdy_add_package(Async
    ENGINE_OPTIMIZED

    PUBLIC_DEPS
        Sturdy::Foundation
        # Public, not private: Mutex.hpp (a public header, used by every consumer of Async::Mutex<T>
        # -- WindowManager, WindowManagerGLFW, and others that never otherwise touch Async/Tracy) includes
        # <tracy/Tracy.hpp> directly for its lock-visibility wrapping. Private visibility here only
        # covers Async's own .cpp files, not headers transitively included by other packages.
        Sturdy::Tracy

)

# Async deliberately depends on nothing but Foundation, so it gets its own copy of just the one
# platform bit it actually needs (STURDY_PLATFORM_WEB, to pick SynchronousRuntime as DefaultRuntime
# in :Runtime) rather than pulling in the whole WindowManager package for it — see WindowManager/CMakeLists.txt
# for the equivalent, fuller platform-definitions block.
if(STURDY_OS STREQUAL "Web")
    target_compile_definitions(Async PUBLIC STURDY_PLATFORM_WEB=1)
endif()

sturdy_add_tests(Async TIMEOUTS SpscRingBufferTest=30)
