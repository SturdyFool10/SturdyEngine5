sturdy_add_package(Runtime
    EXECUTABLE

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::EngineComponents

    DEBUG_PUBLIC_DEPS
)

if(WIN32)
    # Shell32 provides CommandLineToArgvW, used by Sturdy.Runtime.Cli to tokenize the raw command
    # line for the WinMain entry point. Kernel32 (GetCommandLineW / WideCharToMultiByte / LocalFree)
    # is linked by default.
    target_link_libraries(Runtime PRIVATE Shell32)

    # When the WinMain entry point is in play — Dist builds, or any build with SFT_USE_WINMAIN — link
    # as a GUI (WINDOWS subsystem) executable so launching it never spawns a console window. The
    # clang driver forwards /subsystem to the MSVC-style linker, and the CRT's WinMainCRTStartup
    # then calls WinMain. This condition must stay in sync with the entry-point guard in main.cpp.
    target_link_options(Runtime PRIVATE
        "$<$<OR:$<CONFIG:Dist>,$<BOOL:${SFT_USE_WINMAIN}>>:LINKER:/subsystem:windows>")
endif()

if(UNIX AND NOT APPLE AND NOT STURDY_OS STREQUAL "Web")
    # GLFW and SDL3 each vendor the generated Wayland client-protocol code, so linking both
    # pulls two copies of identical symbols (e.g. wp_fractional_scale_manager_v1_interface).
    # The shared Platform/Linux/WindowEffects.cpp references both backends, so any binary that
    # creates a window pulls both. The duplicate definitions are the same upstream protocol, so
    # accept them. Clean long-term fix: compile-time window-backend selection that links only
    # the chosen backend library (then this flag can be removed).
    #
    # Excluded on Web: the Emscripten toolchain reports UNIX=TRUE/APPLE=FALSE too, but GLFW is
    # never linked there (see Platform/CMakeLists.txt) so the duplicate-symbol problem does not
    # exist, and wasm-ld does not accept GNU ld's --allow-multiple-definition anyway.
    target_link_options(Runtime PRIVATE "LINKER:--allow-multiple-definition")
endif()
