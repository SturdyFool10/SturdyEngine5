# Sturdy.Physics — engine-side physics building blocks that do not depend on any rigid-body solver: the
# fixed-step accumulator, collision layers, ballistics, ragdoll constraints, XPBD cloth, the character controller
# and contact-event filtering. A solver adapter (PhysicsBox3D) feeds these through the small query interfaces they
# declare; nothing here includes a third-party physics header, so the base-dependency boundary tests stay green.
sturdy_add_package(Physics
    ENGINE_OPTIMIZED

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::GLM
        Sturdy::Animation
)

sturdy_add_tests(Physics TIMEOUT 20)
