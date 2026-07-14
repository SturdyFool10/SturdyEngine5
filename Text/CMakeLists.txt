# Sturdy.Text — CPU-side text shaping (HarfBuzz), glyph outline extraction, and SDF/MSDF
# rasterization (msdfgen). Deliberately depends on nothing but Foundation, Async, and GLM — no
# RHI, no Core, no GPU dependency at all — so nothing outside Renderer needs to link HarfBuzz or
# msdfgen. Mirrors RHI/CMakeLists.txt's "stay a neutral, backend-independent contract" stance.
sturdy_add_package(Text
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::GLM

    PRIVATE_DEPS
        Sturdy::HarfBuzz
        Sturdy::MsdfGen
        Sturdy::StbImage

    DEBUG_PUBLIC_DEPS
)
