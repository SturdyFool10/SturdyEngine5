sturdy_add_package(Engine
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation
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
    sturdy_enable_warnings(EngineTextureStreamingCompressionTest)
    add_test(NAME EngineTextureStreamingCompressionTest COMMAND EngineTextureStreamingCompressionTest)
    set_tests_properties(EngineTextureStreamingCompressionTest PROPERTIES TIMEOUT 10)

    # Exercises Engine::Detail::decode_image_rgba8 (ImageDecode.cpp) directly against hand-built
    # encoded bytes, growing as image-format support (plans/image-format-support.md) is added.
    add_executable(EngineImageDecodeTest tests/ImageDecodeTest.cpp)
    target_link_libraries(EngineImageDecodeTest PRIVATE Sturdy::Engine)
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
