# Sturdy.UI — Clay-based immediate-mode UI layout + GPU rendering.
#
# Built on top of Sturdy.Renderer's existing GPU text stack (TextAtlas/TextPipeline) instead of a
# second one, and on Clay (Sturdy::clay) for layout. Deliberately does NOT depend on Sturdy.Ecs —
# usable identically from a bare game loop, an ECS system, or editor tooling. See
# plans/clay-ui-renderer.md.
sturdy_add_package(UI
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Core
        Sturdy::RHI
        Sturdy::GLM
        Sturdy::Text
        Sturdy::Renderer
        Sturdy::clay

    DEBUG_PUBLIC_DEPS
)
