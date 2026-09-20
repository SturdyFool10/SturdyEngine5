# Sample game/application logic kept in its own archive so Runtime and a future Editor can both
# reference the same explicit factory without linking either product host into the other — same
# reasoning as Demos/RuntimeDemoGameLogic's own CMakeLists.txt.
sturdy_add_package(KeyboardTesterGameLogic
    PUBLIC_DEPS
        Sturdy::Engine
)

sturdy_add_tests(KeyboardTesterGameLogic)
sturdy_add_static_unused_test(KeyboardTesterGameLogic)
