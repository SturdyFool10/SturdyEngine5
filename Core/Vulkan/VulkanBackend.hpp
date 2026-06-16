#pragma once

#include "../EngineBackend.hpp"

namespace SFT::Core::Vulkan {

    class VulkanBackend final : public EngineBackend {
        public:
            ~VulkanBackend() override = default;

        private:
            friend class ::SFT::Core::EngineBackend;

            explicit VulkanBackend(ConstructorKey key);
    };

}
