sturdy_add_package(Async
    PUBLIC_DEPS
        Sturdy::Foundation

    DEBUG_PUBLIC_DEPS
)

# Async deliberately depends on nothing but Foundation, so it gets its own copy of just the one
# platform bit it actually needs (STURDY_PLATFORM_WEB, to pick SynchronousRuntime as DefaultRuntime
# in :Runtime) rather than pulling in the whole Platform package for it — see Platform/CMakeLists.txt
# for the equivalent, fuller platform-definitions block.
if(STURDY_OS STREQUAL "Web")
    target_compile_definitions(Async PUBLIC STURDY_PLATFORM_WEB=1)
endif()
