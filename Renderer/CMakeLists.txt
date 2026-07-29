sturdy_add_package(Renderer
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::RHI
        Sturdy::Core
        Sturdy::Platform
        Sturdy::Async
        Sturdy::GLM
        Sturdy::Text

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(RendererRenderGraphTest tests/RenderGraphTest.cpp)
    target_link_libraries(RendererRenderGraphTest PRIVATE Sturdy::Renderer)
    sturdy_enable_warnings(RendererRenderGraphTest)
    add_test(NAME RendererRenderGraphTest COMMAND RendererRenderGraphTest)
    set_tests_properties(RendererRenderGraphTest PROPERTIES TIMEOUT 10)
endif()

