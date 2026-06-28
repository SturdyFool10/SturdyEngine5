sturdy_add_package(Core
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Platform
        Sturdy::VMA
        Sturdy::Volk
        Sturdy::GLM
        Sturdy::SDL3
        Sturdy::Vulkan
        Sturdy::Slang

    # volk provides dynamically-loaded entry points; VK_NO_PROTOTYPES keeps the Vulkan headers
    # from also declaring statically-linked prototypes. Set now so volk includes are correct as
    # soon as the renderer starts including <volk.h>.
    PRIVATE_DEFINES
        VK_NO_PROTOTYPES

    DEBUG_PUBLIC_DEPS
)
