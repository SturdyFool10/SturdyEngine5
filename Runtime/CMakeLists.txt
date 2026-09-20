# Reusable standalone-application host. Runtime owns no demo content and selects no GameLogic
# implementation; products pass an explicit factory to Runtime::run().
sturdy_add_package(Runtime
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Engine
        Sturdy::ApplicationHost
)

sturdy_add_static_unused_test(Runtime NAME_STEM RuntimeStaticUnused)
