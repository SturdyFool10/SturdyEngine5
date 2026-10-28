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

# Optional pieces found through pkg-config. Switch one off to drop its dependency (the Rust build does, so its binaries depend on the system
# only for OpenSSL). Each quietly stays off when the library is not installed.
option(STURDY_ASYNC_WITH_HTTP2 "Build HTTP/2 (nghttp2) when found" ON)
option(STURDY_ASYNC_WITH_QUIC "Build QUIC and HTTP/3 (ngtcp2, nghttp3) when found" ON)
option(STURDY_ASYNC_WITH_PROCESS "Build child-process support (the compiled Boost.Process library) when found" ON)
option(STURDY_ASYNC_WITH_IO_URING "Use io_uring for positional file IO (liburing) when found" ON)

if(STURDY_OS STREQUAL "Web")
    target_compile_definitions(Async PUBLIC STURDY_PLATFORM_WEB=1)
else()
    find_package(Boost 1.74 CONFIG REQUIRED)
    find_package(OpenSSL 3.0 REQUIRED)
    target_link_libraries(Async PRIVATE Boost::headers OpenSSL::SSL OpenSSL::Crypto)
    # Optional: child processes (Boost.Process v2 is a compiled component in newer Boost).
    if(STURDY_ASYNC_WITH_PROCESS)
        find_package(Boost 1.74 CONFIG QUIET OPTIONAL_COMPONENTS process)
        if(TARGET Boost::process)
            target_link_libraries(Async PRIVATE Boost::process)
            target_compile_definitions(Async PRIVATE STURDY_ASYNC_PROCESS=1)
        endif()
    endif()
    # Optional: HTTP/2 (server and client) through nghttp2; without it everything stays HTTP/1.1.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        if(STURDY_ASYNC_WITH_HTTP2)
            pkg_check_modules(STURDY_NGHTTP2 IMPORTED_TARGET libnghttp2)
            if(STURDY_NGHTTP2_FOUND)
                target_link_libraries(Async PRIVATE PkgConfig::STURDY_NGHTTP2)
                target_compile_definitions(Async PRIVATE STURDY_ASYNC_HTTP2=1)
            endif()
        endif()
    endif()
    # Optional: QUIC (and HTTP/3 on top) through ngtcp2 with OpenSSL 3.5+'s QUIC TLS callbacks.
    if(STURDY_ASYNC_WITH_QUIC AND PkgConfig_FOUND AND OPENSSL_VERSION VERSION_GREATER_EQUAL "3.5")
        pkg_check_modules(STURDY_NGTCP2 IMPORTED_TARGET libngtcp2 libngtcp2_crypto_ossl)
        if(STURDY_NGTCP2_FOUND)
            target_link_libraries(Async PRIVATE PkgConfig::STURDY_NGTCP2)
            target_compile_definitions(Async PRIVATE STURDY_ASYNC_QUIC=1)
            pkg_check_modules(STURDY_NGHTTP3 IMPORTED_TARGET libnghttp3)
            if(STURDY_NGHTTP3_FOUND)
                target_link_libraries(Async PRIVATE PkgConfig::STURDY_NGHTTP3)
                target_compile_definitions(Async PRIVATE STURDY_ASYNC_HTTP3=1)
            endif()
        endif()
    endif()
    # Optional: io_uring for positional file IO on Linux (falls back to the IO lane at runtime if the kernel refuses a ring).
    if(STURDY_ASYNC_WITH_IO_URING AND PkgConfig_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        pkg_check_modules(STURDY_LIBURING IMPORTED_TARGET liburing)
        if(STURDY_LIBURING_FOUND)
            target_link_libraries(Async PRIVATE PkgConfig::STURDY_LIBURING)
            target_compile_definitions(Async PRIVATE STURDY_ASYNC_IO_URING=1)
        endif()
    endif()
    # Optional: transparent gzip/deflate decoding in HttpClient (the feature quietly turns off without zlib).
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_link_libraries(Async PRIVATE ZLIB::ZLIB)
        target_compile_definitions(Async PRIVATE STURDY_ASYNC_ZLIB=1)
    endif()
endif()

if(STURDY_OS STREQUAL "Web")
    sturdy_add_tests(Async TIMEOUTS SpscRingBufferTest=30 ParallelTest=60 EXCLUDE IoHandlerTest HttpServerTest NetServerTest IoStreamTest NetTlsTest WebSocketTest WebHostingTest P2pTest ProcessTest FileWatchTest IoTest NatTraversalTest RelayTest QuicTest FileUringTest Http2Test Http3Test BindingTest BindingServeTest BindingConcurrencyTest BindingIoTest BindingPrimitivesTest)
else()
    sturdy_add_tests(Async TIMEOUTS SpscRingBufferTest=30 ParallelTest=60)
endif()
