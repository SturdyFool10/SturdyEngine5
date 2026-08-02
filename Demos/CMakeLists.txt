# Demonstration products are intentionally outside Runtime and are only present when explicitly
# requested. Each demo is a separate package/target with an explicit dependency graph.
add_subdirectory(RuntimeDemoGameLogic)
add_subdirectory(RuntimeDemo)
add_subdirectory(KeyboardTesterGameLogic)
add_subdirectory(KeyboardTester)
add_subdirectory(UiWorkbenchGameLogic)
add_subdirectory(UiWorkbench)
