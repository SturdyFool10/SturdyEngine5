# Sturdy.AudioGpu — the GPU implementation of Audio's compute-mixing backend: samples live in a pool in video memory and a
# block of voices is mixed by three compute dispatches (Shaders/audio_mix.slang). It takes the RHI device of whoever owns one
# (the renderer's, or a standalone one in tests), so Audio itself stays free of any graphics dependency.
sturdy_add_package(AudioGpu
    ENGINE_OPTIMIZED

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Audio
        Sturdy::RHI
        Sturdy::Core
        Sturdy::Renderer
)

sturdy_add_tests(AudioGpu TIMEOUT 60)
