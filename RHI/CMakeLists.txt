# Sturdy.RHI — the API-neutral render-hardware-interface contract. It defines GPU vocabulary,
# descriptors, handles, capabilities, and abstract interfaces. Concrete APIs, window systems, and
# OS/display queries live below it in Core/WindowManager so RHI remains independently reusable.
sturdy_add_package(RHI
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::GLM

    DEBUG_PUBLIC_DEPS
)
