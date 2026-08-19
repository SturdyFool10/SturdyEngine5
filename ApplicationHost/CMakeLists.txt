# Sturdy.ApplicationHost — the reusable generic helper every delivered SturdyEngine product shares:
# constructs a product's Engine::ApplicationClient and drives it through Engine::Application's
# initialize()/run(). Deliberately depends on nothing but Foundation and Engine — no
# product/demo content lives here.
#
# CLI argument tokenizing (argv and, on Windows, the WinMain raw command line) lives in
# Sturdy::Foundation instead, not here (see Foundation/Cli.hpp) — it has no dependency on Engine
# or the client-hosting loop, so a consumer that never wants Engine at all can still reuse it.
#
# Runtime (the reusable standalone-app host) and, eventually, an Editor host both link this. Each
# delivered product owns its platform main()/WinMain() entry point and chooses its GameLogic factory
# explicitly; see Demos/RuntimeDemo/src/main.cpp for the standalone composition shape. This package
# exists so additional hosts never have to copy-paste the client/Application driving loop.
sturdy_add_package(ApplicationHost
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Engine

    DEBUG_PUBLIC_DEPS
)
