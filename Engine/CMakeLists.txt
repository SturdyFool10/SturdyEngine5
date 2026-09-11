sturdy_add_package(Engine
    EXCLUDE_SOURCE_DIRS
        tests

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

    DEBUG_PUBLIC_DEPS
)


if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(EngineRenderGraphTest tests/RenderGraphTest.cpp)
    target_link_libraries(EngineRenderGraphTest PRIVATE Sturdy::Engine)
    sturdy_enable_warnings(EngineRenderGraphTest)
    add_test(NAME EngineRenderGraphTest COMMAND EngineRenderGraphTest)
    set_tests_properties(EngineRenderGraphTest PROPERTIES TIMEOUT 10)

    add_executable(EngineGameLogicContractTest tests/GameLogicContractTest.cpp)
    target_link_libraries(EngineGameLogicContractTest PRIVATE Sturdy::Engine)
    sturdy_enable_warnings(EngineGameLogicContractTest)
    add_test(NAME EngineGameLogicContractTest COMMAND EngineGameLogicContractTest)
    set_tests_properties(EngineGameLogicContractTest PROPERTIES TIMEOUT 10)

    add_executable(EngineEcsReflectionBridgeTest tests/EcsReflectionBridgeTest.cpp)
    target_link_libraries(EngineEcsReflectionBridgeTest PRIVATE Sturdy::Engine)
    sturdy_enable_warnings(EngineEcsReflectionBridgeTest)
    add_test(NAME EngineEcsReflectionBridgeTest COMMAND EngineEcsReflectionBridgeTest)
    set_tests_properties(EngineEcsReflectionBridgeTest PROPERTIES TIMEOUT 10)

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
    add_library(EngineHotReloadFixtureV1 MODULE tests/fixtures/HotReloadFixtureModule.cpp)
    target_link_libraries(EngineHotReloadFixtureV1 PRIVATE Sturdy::Reflection)
    target_include_directories(EngineHotReloadFixtureV1 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(EngineHotReloadFixtureV1 PRIVATE FIXTURE_VERSION=1)
    sturdy_enable_warnings(EngineHotReloadFixtureV1)

    add_library(EngineHotReloadFixtureV2 MODULE tests/fixtures/HotReloadFixtureModule.cpp)
    target_link_libraries(EngineHotReloadFixtureV2 PRIVATE Sturdy::Reflection)
    target_include_directories(EngineHotReloadFixtureV2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(EngineHotReloadFixtureV2 PRIVATE FIXTURE_VERSION=2)
    sturdy_enable_warnings(EngineHotReloadFixtureV2)

    add_executable(EngineHotReloadTest tests/HotReloadTest.cpp)
    target_link_libraries(EngineHotReloadTest PRIVATE Sturdy::Engine)
    add_dependencies(EngineHotReloadTest EngineHotReloadFixtureV1 EngineHotReloadFixtureV2)
    target_compile_definitions(EngineHotReloadTest PRIVATE
        "STURDY_HOT_RELOAD_FIXTURE_V1=\"$<TARGET_FILE:EngineHotReloadFixtureV1>\""
        "STURDY_HOT_RELOAD_FIXTURE_V2=\"$<TARGET_FILE:EngineHotReloadFixtureV2>\""
    )
    sturdy_enable_warnings(EngineHotReloadTest)
    add_test(NAME EngineHotReloadTest COMMAND EngineHotReloadTest)
    set_tests_properties(EngineHotReloadTest PROPERTIES TIMEOUT 20)

    add_executable(EngineWindowRequestTest tests/WindowRequestTest.cpp)
    target_link_libraries(EngineWindowRequestTest PRIVATE Sturdy::Engine)
    sturdy_enable_warnings(EngineWindowRequestTest)
    add_test(NAME EngineWindowRequestTest COMMAND EngineWindowRequestTest)
    set_tests_properties(EngineWindowRequestTest PROPERTIES TIMEOUT 10)

    add_executable(EngineInputStateTest tests/InputStateTest.cpp)
    target_link_libraries(EngineInputStateTest PRIVATE Sturdy::Engine)
    sturdy_enable_warnings(EngineInputStateTest)
    add_test(NAME EngineInputStateTest COMMAND EngineInputStateTest)
    set_tests_properties(EngineInputStateTest PROPERTIES TIMEOUT 10)

    add_executable(EngineDockWindowCoordinatorTest tests/DockWindowCoordinatorTest.cpp)
    target_link_libraries(EngineDockWindowCoordinatorTest PRIVATE Sturdy::Engine)
    sturdy_enable_warnings(EngineDockWindowCoordinatorTest)
    add_test(NAME EngineDockWindowCoordinatorTest COMMAND EngineDockWindowCoordinatorTest)
    set_tests_properties(EngineDockWindowCoordinatorTest PROPERTIES TIMEOUT 10)

    add_executable(EngineTextureStreamingCompressionTest tests/TextureStreamingCompressionTest.cpp)
    target_link_libraries(EngineTextureStreamingCompressionTest PRIVATE Sturdy::Engine)
    # See EngineImageDecodeTest below for why -UNDEBUG is required for an assert-based test.
    target_compile_options(EngineTextureStreamingCompressionTest PRIVATE -UNDEBUG)
    sturdy_enable_warnings(EngineTextureStreamingCompressionTest)
    add_test(NAME EngineTextureStreamingCompressionTest COMMAND EngineTextureStreamingCompressionTest)
    set_tests_properties(EngineTextureStreamingCompressionTest PROPERTIES TIMEOUT 10)

    # Exercises the colorimetry in ColorSpace.cpp: matrix derivation from chromaticities, Bradford
    # chromatic adaptation, and gamut mapping. Pure math with no I/O, so it is fast and hermetic.
    add_executable(EngineColorSpaceTest tests/ColorSpaceTest.cpp)
    target_link_libraries(EngineColorSpaceTest PRIVATE Sturdy::Engine)
    # See EngineImageDecodeTest below for why -UNDEBUG is required for an assert-based test.
    target_compile_options(EngineColorSpaceTest PRIVATE -UNDEBUG)
    sturdy_enable_warnings(EngineColorSpaceTest)
    add_test(NAME EngineColorSpaceTest COMMAND EngineColorSpaceTest)
    set_tests_properties(EngineColorSpaceTest PROPERTIES TIMEOUT 10)

    # Exercises Engine::Detail::decode_image_rgba8 (ImageDecode.cpp) directly against hand-built
    # encoded bytes, growing as image-format support (plans/image-format-support.md) is added.
    add_executable(EngineImageDecodeTest tests/ImageDecodeTest.cpp)
    target_link_libraries(EngineImageDecodeTest PRIVATE Sturdy::Engine)
    # This test states every expectation with assert(), which the Release and Dist configurations
    # compile to nothing via NDEBUG -- the test then "passes" without checking anything at all.
    # -UNDEBUG comes after the configuration's own flags, so the assertions stay live in every
    # configuration. (Every other assert-based test in this repo has the same hole.)
    target_compile_options(EngineImageDecodeTest PRIVATE -UNDEBUG)
    sturdy_enable_warnings(EngineImageDecodeTest)
    add_test(NAME EngineImageDecodeTest COMMAND EngineImageDecodeTest)
    set_tests_properties(EngineImageDecodeTest PROPERTIES TIMEOUT 10)

    if(NOT STURDY_BUILD_SHARED_LIBS)
        add_executable(
            EngineStaticAggregateUnusedProbe
            "${CMAKE_SOURCE_DIR}/cmake/tests/StaticAggregateUnused.cpp"
        )
        target_link_libraries(
            EngineStaticAggregateUnusedProbe
            PRIVATE Sturdy::Engine
        )
        sturdy_enable_warnings(EngineStaticAggregateUnusedProbe)

        set(
            _sturdy_static_aggregate_test
            "${CMAKE_SOURCE_DIR}/cmake/tests/AssertStaticAggregateUnused.cmake"
        )
        set(
            _sturdy_static_probe_path
            "$<TARGET_FILE:EngineStaticAggregateUnusedProbe>"
        )
        add_test(
            NAME EngineStaticAggregateUnusedTest
            COMMAND "${CMAKE_COMMAND}"
                "-DSTURDY_STATIC_PROBE=${_sturdy_static_probe_path}"
                -P "${_sturdy_static_aggregate_test}"
        )
        set_tests_properties(
            EngineStaticAggregateUnusedTest
            PROPERTIES TIMEOUT 60
        )
    endif()

    set(
        _sturdy_base_dependency_boundary_test
        "${CMAKE_SOURCE_DIR}/cmake/tests/AssertBaseDependencyBoundary.cmake"
    )
    add_test(
        NAME EngineBaseDependencyBoundaryTest
        COMMAND "${CMAKE_COMMAND}"
            "-DSTURDY_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${_sturdy_base_dependency_boundary_test}"
    )
    set_tests_properties(
        EngineBaseDependencyBoundaryTest
        PROPERTIES TIMEOUT 10
    )
endif()
