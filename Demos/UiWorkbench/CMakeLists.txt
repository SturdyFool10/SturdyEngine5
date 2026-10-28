# Executable composition root for the interactive UI workbench.
sturdy_add_host_executable(UiWorkbench UiWorkbenchGameLogic)
# CrashHandler.cpp's backtrace_symbols() can only name functions present in the dynamic symbol table; sturdy_add_host_executable now
# exports them (STURDY_EXPORT_SYMBOLS, on by default), so this target no longer needs its own flag.
