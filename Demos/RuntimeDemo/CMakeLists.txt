# Explicit composition root for the sample application. Runtime remains reusable and contains no
# dependency on this executable or the selected demo GameLogic archive.
sturdy_add_host_executable(RuntimeDemo RuntimeDemoGameLogic)

if(STURDY_OS STREQUAL "Web")
    # Building a bare .js/.wasm pair (Emscripten's default executable suffix) gives you nothing to
    # open in a browser. Overriding just this target's suffix to .html makes em++ additionally emit
    # its default HTML shell alongside the .js/.wasm -- that shell already declares
    # <canvas id="canvas">, which is exactly the element SDL3's Emscripten backend looks for by
    # default (see SDL_emscriptenvideo.c's Emscripten_CreateWindow, selector "#canvas") and what
    # WindowManager/Web/WindowEffectsImpl.cpp's native_window_handle_from_sdl() reads back as the
    # WebGPU surface's canvas selector. This is scoped to RuntimeDemo alone -- Slang's own
    # build-time codegen tools (see sturdy_fetch_slang()) must stay plain .js, since they're
    # invoked as command-line tools through CMAKE_CROSSCOMPILING_EMULATOR, not loaded as a page.
    set_target_properties(RuntimeDemo PROPERTIES SUFFIX ".html")
endif()
