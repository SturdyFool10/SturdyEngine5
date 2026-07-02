sturdy_add_package(EngineComponents
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Core
        Sturdy::Platform

    DEBUG_PUBLIC_DEPS
)

add_library(Sturdy::Engine ALIAS EngineComponents)
