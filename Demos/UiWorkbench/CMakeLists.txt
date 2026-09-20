# Executable composition root for the interactive UI workbench.
sturdy_add_host_executable(UiWorkbench UiWorkbenchGameLogic)

if(STURDY_OS STREQUAL "Linux" OR STURDY_OS STREQUAL "FreeBSD")
    # CrashHandler.cpp's backtrace_symbols() can only name functions present in the dynamic symbol
    # table; without this, a non-PIE/static-ish executable exports almost nothing and every frame in a
    # crash log comes back as a bare address.
    target_link_options(UiWorkbench PRIVATE "LINKER:--export-dynamic")
endif()
