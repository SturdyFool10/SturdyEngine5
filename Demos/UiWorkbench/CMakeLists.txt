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
