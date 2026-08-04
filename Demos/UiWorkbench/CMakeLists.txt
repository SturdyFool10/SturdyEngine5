# Executable composition root for the interactive UI workbench.
sturdy_add_package(UiWorkbench
    EXECUTABLE

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Runtime
        Sturdy::UiWorkbenchGameLogic

    DEBUG_PUBLIC_DEPS
)

if(WIN32)
    target_link_libraries(UiWorkbench PRIVATE Shell32)
    target_link_options(UiWorkbench PRIVATE
        "$<$<OR:$<CONFIG:Dist>,$<BOOL:${SFT_USE_WINMAIN}>>:LINKER:/subsystem:windows>")
endif()

if(UNIX AND NOT APPLE AND STURDY_BUILD_GLFW_WINDOW_PROVIDER)
    target_link_options(UiWorkbench PRIVATE "LINKER:--allow-multiple-definition")
endif()

if(STURDY_OS STREQUAL "Linux" OR STURDY_OS STREQUAL "FreeBSD")
    # CrashHandler.cpp's backtrace_symbols() can only name functions present in the dynamic symbol
    # table; without this, a non-PIE/static-ish executable exports almost nothing and every frame in a
    # crash log comes back as a bare address.
    target_link_options(UiWorkbench PRIVATE "LINKER:--export-dynamic")
endif()
