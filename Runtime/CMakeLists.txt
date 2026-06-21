sturdy_add_package(Runtime
    EXECUTABLE

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Engine

    DEBUG_PUBLIC_DEPS
)

if(UNIX AND NOT APPLE)
    # GLFW and SDL3 each vendor the generated Wayland client-protocol code, so linking both
    # pulls two copies of identical symbols (e.g. wp_fractional_scale_manager_v1_interface).
    # The shared Platform/Linux/WindowEffects.cpp references both backends, so any binary that
    # creates a window pulls both. The duplicate definitions are the same upstream protocol, so
    # accept them. Clean long-term fix: compile-time window-backend selection that links only
    # the chosen backend library (then this flag can be removed).
    target_link_options(Runtime PRIVATE "LINKER:--allow-multiple-definition")
endif()
