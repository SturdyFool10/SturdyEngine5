sturdy_add_package(EngineComponents
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::Ecs
        Sturdy::Core
        Sturdy::Renderer
        Sturdy::Platform
        Sturdy::box3d
        Sturdy::UI

    PRIVATE_DEPS
        Sturdy::StbImage
        Sturdy::cgltf
        Sturdy::miniaudio
        Sturdy::bc7enc

    DEBUG_PUBLIC_DEPS
)

add_library(Sturdy::Engine ALIAS EngineComponents)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(EngineRenderGraphTest tests/RenderGraphTest.cpp)
    target_link_libraries(EngineRenderGraphTest PRIVATE Sturdy::Engine)
    sturdy_enable_warnings(EngineRenderGraphTest)
    add_test(NAME EngineRenderGraphTest COMMAND EngineRenderGraphTest)
    set_tests_properties(EngineRenderGraphTest PROPERTIES TIMEOUT 10)
endif()
