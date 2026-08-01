# Explicit composition root for the keyboard-tester sample application. Runtime remains reusable
# and contains no dependency on this executable or the selected demo GameLogic archive.
sturdy_add_package(KeyboardTester
    EXECUTABLE

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Runtime
        Sturdy::KeyboardTesterGameLogic

    DEBUG_PUBLIC_DEPS
)

if(WIN32)
    # Foundation's WinMain command-line tokenizer currently uses CommandLineToArgvW.
    target_link_libraries(KeyboardTester PRIVATE Shell32)
    target_link_options(KeyboardTester PRIVATE
        "$<$<OR:$<CONFIG:Dist>,$<BOOL:${SFT_USE_WINMAIN}>>:LINKER:/subsystem:windows>")
endif()

if(UNIX AND NOT APPLE AND STURDY_BUILD_GLFW_WINDOW_PROVIDER)
    # SDL3 and GLFW both vendor generated Wayland protocol symbols when both providers are selected.
    target_link_options(KeyboardTester PRIVATE "LINKER:--allow-multiple-definition")
endif()
