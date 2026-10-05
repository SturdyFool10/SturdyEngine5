# Sturdy.RenderSettings — the single definition of every per-frame rendering setting (shadows, AO, GI, fog, post...).
# Header-only and dependency-free: the renderer reads these structs, Engine::RenderGraph stores them, the C ABI copies
# them in one place and the CxxApi hands them to Rust through bindgen. See RenderSettings.hpp for the plain-data rules.
sturdy_add_package(RenderSettings)

sturdy_add_tests(RenderSettings TIMEOUT 10)
