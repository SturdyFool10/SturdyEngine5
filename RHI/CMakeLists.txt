# Sturdy.RHI — a render-hardware-interface: an API-agnostic vocabulary (enums, descriptors, opaque
# handles, and abstract Device/command interfaces) for *describing* GPU work without naming a
# graphics API. It deliberately depends on Foundation, GLM, and graphicsPlatform (classic-C++
# OS/display capability queries such as HDR metadata) — no Vulkan, no high-level Platform, no Core —
# so it stays the neutral contract that a concrete backend (Core/Vulkan today; Metal/DX12/WebGPU
# later) implements, not the other way around.
sturdy_add_package(RHI
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::graphicsPlatform
        Sturdy::GLM

    DEBUG_PUBLIC_DEPS
)
