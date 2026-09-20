# Sample game/application logic kept in its own archive so Runtime and a future Editor can both
# reference the same explicit factory without linking either product host into the other.
sturdy_add_package(RuntimeDemoGameLogic
    PUBLIC_DEPS
        Sturdy::Engine
)

# Only the demo logic consumes the optional Khronos sample asset checkout. The reusable host and
# unrelated products never inherit this source-tree definition.
if(STURDY_GLTF_SAMPLE_ASSETS_DIR)
    target_compile_definitions(RuntimeDemoGameLogic PRIVATE
        "STURDY_GLTF_SAMPLE_ASSETS_DIR=\"${STURDY_GLTF_SAMPLE_ASSETS_DIR}\"")
endif()

sturdy_add_tests(RuntimeDemoGameLogic)
sturdy_add_static_unused_test(RuntimeDemoGameLogic)
