sturdy_add_package(Engine
    ENGINE_OPTIMIZED

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Reflection
        Sturdy::Async
        Sturdy::Ecs
        Sturdy::Core
        Sturdy::Renderer
        Sturdy::WindowManager

    PRIVATE_DEPS
        Sturdy::StbImage
        Sturdy::cgltf
        Sturdy::miniaudio
        Sturdy::bc7enc
        Sturdy::WebP
        Sturdy::WebPDemux
        Sturdy::libavif
        Sturdy::libjxl
        Sturdy::openjpeg
        Sturdy::libtiff
        Sturdy::OpenEXR
        Sturdy::Tracy
)

# Every one of these states its expectations with assert(), which Release and Dist compile to nothing
# via NDEBUG -- the test would "pass" without checking anything, hence KEEP_ASSERTS. (Every other
# assert-based test in this repo has the same hole.)
sturdy_add_tests(Engine
    KEEP_ASSERTS TextureStreamingCompressionTest ColorSpaceTest ImageDecodeTest
    EXCLUDE HotReloadTest)

if(STURDY_TESTS_ENABLED)
    # Two real, separately built shared libraries standing in for "the game logic got rebuilt" —
    # HotReloadTest.cpp loads/unloads/reloads them for real (real dlopen/LoadLibrary), not a
    # simulation. Deliberately built by hand with add_library(... MODULE ...) rather than through
    # sturdy_add_package(): that helper only offers one global STATIC/SHARED toggle
    # (STURDY_BUILD_SHARED_LIBS) applied uniformly to every package, with no per-package "always
    # build this one as a loadable module" option, and a hot-reload fixture must always be a real
    # shared library regardless of how the rest of the build is configured. Linked against
    # Sturdy::Reflection only (not Sturdy::Engine) — the fixture only needs TypeRegistry/TypeInfo,
    # and linking the whole Engine static archive into every loadable module would be enormously
    # wasteful and risks duplicate-symbol clashes with graphics/platform globals it never uses.
    foreach(_version 1 2)
        set(_fixture "EngineHotReloadFixtureV${_version}")
        add_library(${_fixture} MODULE tests/fixtures/HotReloadFixtureModule.cpp)
        target_link_libraries(${_fixture} PRIVATE Sturdy::Reflection)
        target_include_directories(${_fixture} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(${_fixture} PRIVATE FIXTURE_VERSION=${_version})
        sturdy_enable_warnings(${_fixture})
    endforeach()

    sturdy_add_test(Engine tests/HotReloadTest.cpp TIMEOUT 20
        DEPENDS EngineHotReloadFixtureV1 EngineHotReloadFixtureV2
        DEFINES
            "STURDY_HOT_RELOAD_FIXTURE_V1=\"$<TARGET_FILE:EngineHotReloadFixtureV1>\""
            "STURDY_HOT_RELOAD_FIXTURE_V2=\"$<TARGET_FILE:EngineHotReloadFixtureV2>\"")
endif()

sturdy_add_static_unused_test(Engine NAME_STEM EngineStaticAggregateUnused)
sturdy_add_script_test(EngineBaseDependencyBoundaryTest AssertBaseDependencyBoundary
    DEFINES "STURDY_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
