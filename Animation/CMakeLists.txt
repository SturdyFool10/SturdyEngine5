# Sturdy.Animation — CPU-side skeletal animation: skeleton/clip/pose types, glTF-semantics sampling
# (STEP, LINEAR, CUBICSPLINE), pose blending and skinning-matrix evaluation. No GPU or ECS dependency;
# the importer lives in Engine and the compute skinning pass in Renderer.
sturdy_add_package(Animation
    ENGINE_OPTIMIZED

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::GLM

    PRIVATE_DEPS
        Sturdy::Json
)

sturdy_add_tests(Animation TIMEOUT 20)
