# Sturdy.D3D12 — Windows-only Direct3D 12 implementation of Sturdy.RHI.
# This target stays below RHI in the dependency graph: it consumes the neutral contract and exposes
# the backend factory, but never makes the RHI itself depend on Windows or DirectX headers.
sturdy_add_package(D3D12
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::RHI
        Sturdy::DirectX

    PRIVATE_DEPS
        Sturdy::Tracy
        Sturdy::D3D12MA
        dcomp

    DEBUG_PUBLIC_DEPS
)
