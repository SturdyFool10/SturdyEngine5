#include "ClipSpace.hpp"

namespace SFT::RHI {

    [[nodiscard]] glm::mat4 gpu_clip_flip(const glm::mat4 &m, BackendType backend) noexcept {
        switch (backend) {
            case BackendType::Vulkan:
                return m;
            case BackendType::D3D12:
            case BackendType::Metal:
            case BackendType::WebGpu: {
                glm::mat4 flipped = m;
                flipped[0][1] = -flipped[0][1];
                flipped[1][1] = -flipped[1][1];
                flipped[2][1] = -flipped[2][1];
                flipped[3][1] = -flipped[3][1];
                return flipped;
            }
        }
        return m;
    }

    [[nodiscard]] f32 gpu_clip_y_sign(BackendType backend) noexcept {
        switch (backend) {
            case BackendType::Vulkan:
                return 1.0f;
            case BackendType::D3D12:
            case BackendType::Metal:
            case BackendType::WebGpu:
                return -1.0f;
        }
        return 1.0f;
    }

} // namespace SFT::RHI
