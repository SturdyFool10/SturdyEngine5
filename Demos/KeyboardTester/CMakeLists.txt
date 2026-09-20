# Explicit composition root for the keyboard-tester sample application. Runtime remains reusable
# and contains no dependency on this executable or the selected demo GameLogic archive.
sturdy_add_host_executable(KeyboardTester KeyboardTesterGameLogic)
