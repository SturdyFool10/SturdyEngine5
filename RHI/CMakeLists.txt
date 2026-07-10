# Sturdy.RHI — a render-hardware-interface: an API-agnostic vocabulary (enums, descriptors, opaque
# handles, and abstract Device/command interfaces) for *describing* GPU work without naming a
# graphics API. It deliberately depends on nothing but Foundation (types/logging) and GLM (vector
# math for colors/clears) — no Vulkan, no Platform, no Core — so it stays the neutral contract that
# a concrete backend (Core/Vulkan today; Metal/DX12/WebGPU later) implements, not the other way
# around.
sturdy_add_package(RHI
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::GLM

    DEBUG_PUBLIC_DEPS
)
