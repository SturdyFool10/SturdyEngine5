#pragma once

#include <concepts>
#include <memory>
#include <utility>

namespace SFT::Core {

    class EngineBackend {
        protected:
            struct ConstructorKey {
                private:
                    friend class EngineBackend;
                    constexpr ConstructorKey() = default;
            };

            explicit constexpr EngineBackend(ConstructorKey) noexcept {}

        public:
            virtual ~EngineBackend() = default;

            EngineBackend(const EngineBackend&) = delete;
            EngineBackend& operator=(const EngineBackend&) = delete;
            EngineBackend(EngineBackend&&) = delete;
            EngineBackend& operator=(EngineBackend&&) = delete;

            template <typename Backend, typename... Args>
            requires std::derived_from<Backend, EngineBackend>
            && requires(Args&&... args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
            [[nodiscard]]
            static std::unique_ptr<Backend> create(Args&&... args)
            {
                return std::unique_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
            }

            template <typename Backend, typename... Args>
            requires std::derived_from<Backend, EngineBackend>
            && requires(Args&&... args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
            [[nodiscard]]
            static std::shared_ptr<Backend> create_shared(Args&&... args)
            {
                return std::shared_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
            }
    };

} // namespace SFT::Core
