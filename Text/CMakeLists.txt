# Sturdy.Text — CPU-side text shaping (HarfBuzz), glyph outline extraction, and SDF/MSDF
# rasterization (msdfgen). Deliberately depends on nothing but Foundation, Async, and GLM beyond
# its own text-processing libraries — no RHI, no Core, no GPU dependency at all — so nothing
# outside Renderer needs to link against the GPU backend. Mirrors RHI/CMakeLists.txt's "stay a
# neutral, backend-independent contract" stance.
#
# HarfBuzz/MsdfGen/StbImage are PUBLIC (not PRIVATE): now that Text is plain headers rather than
# C++20 modules, public headers like Font.hpp/Raster.hpp/ColorGlyph.hpp #include their types
# directly (<hb-ot.h>, msdfgen.h, stb_image.h) - modules used to hide that behind the module
# boundary (a global-module-fragment #include never reached importers), but a header's #includes
# are always transitively visible to anyone who includes it, so every consumer needs these same
# include paths on its own compile line too.
sturdy_add_package(Text
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::GLM
        Sturdy::HarfBuzz
        Sturdy::MsdfGen
        Sturdy::StbImage

    DEBUG_PUBLIC_DEPS
)
