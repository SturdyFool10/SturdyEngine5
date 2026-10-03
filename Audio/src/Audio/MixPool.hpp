#pragma once

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace SFT::Audio {

    /// A small fork-join pool for the mixer: `run` executes one function on the calling thread and on every helper at once and
    /// returns when all have finished. Helpers sleep on an atomic between blocks (no polling, no allocation), run at audio
    /// priority with denormals flushed, and the work itself is split by the caller (typically an atomic cursor over voices),
    /// so a block that has little to do costs one wake-up check and nothing more.
    class MixPool {
      public:
        using Function = void (*)(void *context, u32 participant);

        /// `helpers` extra threads; participants are numbered 0 (the caller) .. helpers.
        explicit MixPool(u32 helpers);
        ~MixPool();
        MixPool(const MixPool &) = delete;
        MixPool &operator=(const MixPool &) = delete;

        [[nodiscard]] u32 participants() const noexcept { return static_cast<u32>(threads_.size()) + 1; }

        /// Runs `function(context, i)` for every participant `i` and waits. With no helpers this is a direct call.
        void run(Function function, void *context);

        /// Helper count to use for a requested `mix_threads` (UINT32_MAX = derive from the hardware).
        [[nodiscard]] static u32 resolve_helpers(u32 requested) noexcept;

      private:
        void helper_main(u32 participant);

        std::vector<std::thread> threads_;
        std::atomic<u32> epoch_{0};
        std::atomic<u32> remaining_{0};
        std::atomic<bool> stop_{false};
        Function function_ = nullptr;
        void *context_ = nullptr;
    };

} // namespace SFT::Audio
