sturdy_add_package(EngineComponents
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::Ecs
        Sturdy::Core
        Sturdy::Renderer
        Sturdy::Platform
        Sturdy::box3d

    PRIVATE_DEPS
        Sturdy::StbImage
        Sturdy::cgltf
        Sturdy::miniaudio

    DEBUG_PUBLIC_DEPS
)

add_library(Sturdy::Engine ALIAS EngineComponents)
