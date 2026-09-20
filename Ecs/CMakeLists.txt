sturdy_add_package(Ecs
    ENGINE_OPTIMIZED

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::Tracy
)

sturdy_add_tests(Ecs TIMEOUT 20)
sturdy_add_script_test(EcsDependencyBoundaryTest AssertEcsDependencyBoundary
    DEFINES "STURDY_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
