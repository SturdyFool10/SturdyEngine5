#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <vector>
#pragma endregion

#include <Foundation/Foundation.hpp>

#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Presentation-engine timing for the Vulkan RHI bridge: present ids (VK_KHR_present_id / present_id2), present
// wait (VK_KHR_present_wait / present_wait2), and display timing feedback (VK_EXT_present_timing). This is the
// display-clock half of the engine's frame pacing (plans/frame-pacing.md): it tells the pacing controller when
// frames *actually* reached the screen, how long a refresh cycle is, and whether the display runs at a variable
// refresh rate, so pacing is driven by the display rather than guessed from CPU timings.

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        /// Timing results the presentation engine may hold before they are drained. The renderer drains every
        /// frame, so a few frames' worth is enough; the headroom only matters if draining ever stalls.
        constexpr u32 present_timing_queue_size = 64;

        /// The host clock `std::chrono::steady_clock` reads, as a Vulkan time domain.
        [[nodiscard]] constexpr VkTimeDomainKHR host_steady_time_domain() noexcept {
#if defined(_WIN32)
            return VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
            return VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif
        }

        /// Converts a raw host-domain timestamp (CLOCK_MONOTONIC nanoseconds, or QPC ticks on Windows) into
        /// `std::chrono::steady_clock` nanoseconds since that clock's epoch.
        [[nodiscard]] i64 host_timestamp_to_steady_ns(u64 host) noexcept {
#if defined(_WIN32)
            // MSVC's steady_clock is QueryPerformanceCounter scaled to nanoseconds; scale the same way (split to
            // avoid overflow) so the two agree exactly.
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            const i64 freq = frequency.QuadPart;
            const i64 ticks = static_cast<i64>(host);
            return (ticks / freq) * 1'000'000'000LL + (ticks % freq) * 1'000'000'000LL / freq;
#else
            // libstdc++ and libc++ steady_clock read CLOCK_MONOTONIC in nanoseconds.
            return static_cast<i64>(host);
#endif
        }

        /// The latest (closest to "photons") single stage the presentation engine can report.
        [[nodiscard]] VkPresentStageFlagsEXT preferred_present_stage(VkPresentStageFlagsEXT supported) noexcept {
            for (VkPresentStageFlagsEXT stage : {VkPresentStageFlagsEXT{VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT},
                                                 VkPresentStageFlagsEXT{VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT},
                                                 VkPresentStageFlagsEXT{VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT}}) {
                if ((supported & stage) != 0) {
                    return stage;
                }
            }
            return 0;
        }

    } // namespace

    VulkanRhiDeviceBridge::SwapchainRecord::PresentTiming VulkanRhiDeviceBridge::negotiate_present_timing(
        VkSurfaceKHR surface, VkSwapchainCreateFlagsKHR &flags) const {
        ZoneScopedN("VulkanRhiDeviceBridge::negotiate_present_timing");
        SwapchainRecord::PresentTiming timing{};
        timing.shared = std::make_unique<SwapchainRecord::PresentTiming::Shared>();
        if (backend_ == nullptr || physical_device_ == nullptr) {
            return timing;
        }
        const VulkanBackend::PresentTimingEnabled &enabled = backend_->present_timing_enabled();

        if (enabled.present_id_version == 1) {
            // The v1 extensions need no swapchain flags and have no per-surface capability query.
            timing.present_id_version = 1;
            timing.present_wait_version = enabled.present_wait_version == 1 ? 1u : 0u;
        } else if (enabled.present_id_version == 2 && backend_->surface_capabilities2_enabled() &&
                   vkGetPhysicalDeviceSurfaceCapabilities2KHR != nullptr) {
            // The "2" family is per surface: a compositor may not support ids/waits/timing on every surface.
            VkPresentTimingSurfaceCapabilitiesEXT timing_caps{.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
            VkSurfaceCapabilitiesPresentWait2KHR wait2_caps{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR};
            VkSurfaceCapabilitiesPresentId2KHR id2_caps{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
            void *chain = nullptr;
            if (enabled.present_timing) {
                timing_caps.pNext = chain;
                chain = &timing_caps;
            }
            if (enabled.present_wait_version == 2) {
                wait2_caps.pNext = chain;
                chain = &wait2_caps;
            }
            id2_caps.pNext = chain;
            const VkPhysicalDeviceSurfaceInfo2KHR surface_info{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                .surface = surface,
            };
            VkSurfaceCapabilities2KHR caps{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &id2_caps};
            if (vkGetPhysicalDeviceSurfaceCapabilities2KHR(physical_device_->vk_handle(), &surface_info, &caps) == VK_SUCCESS &&
                id2_caps.presentId2Supported) {
                timing.present_id_version = 2;
                flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
                if (enabled.present_wait_version == 2 && wait2_caps.presentWait2Supported) {
                    timing.present_wait_version = 2;
                    flags |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
                }
                if (enabled.present_timing && timing_caps.presentTimingSupported) {
                    timing.stage = preferred_present_stage(timing_caps.presentStageQueries);
                    if (timing.stage != 0) {
                        timing.timing = true;
                        flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
                        timing.capabilities.target_display_time =
                            enabled.present_at_absolute_time && timing_caps.presentAtAbsoluteTimeSupported;
                        timing.capabilities.minimum_display_duration =
                            enabled.present_at_relative_time && timing_caps.presentAtRelativeTimeSupported;
                    }
                }
            }
        }

        timing.capabilities.present_id = timing.present_id_version != 0;
        timing.capabilities.present_wait = timing.present_wait_version != 0;
        return timing;
    }

    void VulkanRhiDeviceBridge::finish_present_timing_setup(SwapchainRecord &record) const {
        ZoneScopedN("VulkanRhiDeviceBridge::finish_present_timing_setup");
        SwapchainRecord::PresentTiming &timing = record.present_timing;
        if (!timing.timing) {
            return;
        }
        const VkDevice device = logical_device_->vk_handle();
        const VkSwapchainKHR swapchain = record.swapchain.vk_handle();

        if (vkSetSwapchainPresentTimingQueueSizeEXT(device, swapchain, present_timing_queue_size) != VK_SUCCESS) {
            Foundation::log_warn("VK_EXT_present_timing: could not size the timing queue; display timing feedback disabled.");
            timing.timing = false;
            timing.capabilities.target_display_time = false;
            timing.capabilities.minimum_display_duration = false;
            return;
        }
        timing.queue_size = present_timing_queue_size;

        // Pick a time domain. The host's own steady clock needs no calibration; anything else (Mesa reports
        // PRESENT_STAGE_LOCAL) is calibrated against it.
        VkSwapchainTimeDomainPropertiesEXT domains_query{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT};
        u64 domains_counter = 0;
        if (vkGetSwapchainTimeDomainPropertiesEXT(device, swapchain, &domains_query, &domains_counter) == VK_SUCCESS &&
            domains_query.timeDomainCount > 0) {
            std::vector<VkTimeDomainKHR> domains(domains_query.timeDomainCount);
            std::vector<u64> ids(domains_query.timeDomainCount);
            domains_query.pTimeDomains = domains.data();
            domains_query.pTimeDomainIds = ids.data();
            const VkResult listed = vkGetSwapchainTimeDomainPropertiesEXT(device, swapchain, &domains_query, &domains_counter);
            if (listed == VK_SUCCESS || listed == VK_INCOMPLETE) {
                const u32 count = std::min<u32>(domains_query.timeDomainCount, static_cast<u32>(domains.size()));
                u32 chosen = 0;
                for (u32 i = 0; i < count; ++i) {
                    if (domains[i] == host_steady_time_domain()) {
                        chosen = i;
                        break;
                    }
                }
                timing.time_domain = domains[chosen];
                timing.time_domain_id = ids[chosen];
            }
        }

        if (!calibrate_present_timing(record)) {
            Foundation::log_warn(
                "VK_EXT_present_timing: could not relate the presentation engine's clock to the host clock; display "
                "times will not be reported for this swapchain.");
        }
        timing.capabilities.display_timing_feedback = true;
        Foundation::log_info(
            "Present timing enabled: stage={}, time domain={}, absolute target={}, relative target={}, present wait v{}.",
            timing.stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT ? "first pixel visible"
            : timing.stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT   ? "first pixel out"
                                                                               : "request dequeued",
            static_cast<i32>(timing.time_domain), timing.capabilities.target_display_time,
            timing.capabilities.minimum_display_duration, timing.present_wait_version);
    }

    bool VulkanRhiDeviceBridge::calibrate_present_timing(const SwapchainRecord &record) const {
        const SwapchainRecord::PresentTiming &timing = record.present_timing;
        if (!timing.timing || !timing.shared) {
            return false;
        }
        if (timing.time_domain == host_steady_time_domain()) {
            timing.shared->offset_ns.store(0, std::memory_order_relaxed);
            timing.shared->calibrated.store(true, std::memory_order_release);
            return true;
        }

        const VkSwapchainCalibratedTimestampInfoEXT swapchain_info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT,
            .swapchain = record.swapchain.vk_handle(),
            .presentStage = timing.stage,
            .timeDomainId = timing.time_domain_id,
        };
        const std::array<VkCalibratedTimestampInfoKHR, 2> infos{
            VkCalibratedTimestampInfoKHR{.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
                                         .pNext = &swapchain_info,
                                         .timeDomain = timing.time_domain},
            VkCalibratedTimestampInfoKHR{.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
                                         .timeDomain = host_steady_time_domain()},
        };
        std::array<u64, 2> timestamps{};
        u64 max_deviation = 0;
        if (vkGetCalibratedTimestampsKHR != nullptr &&
            vkGetCalibratedTimestampsKHR(logical_device_->vk_handle(), 2, infos.data(), timestamps.data(), &max_deviation) ==
                VK_SUCCESS &&
            timestamps[0] != 0) {
            const i64 offset = host_timestamp_to_steady_ns(timestamps[1]) - static_cast<i64>(timestamps[0]);
            timing.shared->offset_ns.store(offset, std::memory_order_relaxed);
            timing.shared->calibrated.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    rhi::PresentTimingCapabilities VulkanRhiDeviceBridge::present_timing_capabilities(rhi::SwapchainHandle swapchain) const noexcept {
        const SwapchainRecord *record = swapchains_.find(swapchain);
        return record != nullptr ? record->present_timing.capabilities : rhi::PresentTimingCapabilities{};
    }

    rhi::RhiExpected<rhi::SwapchainTiming> VulkanRhiDeviceBridge::swapchain_timing(rhi::SwapchainHandle swapchain) {
        ZoneScopedN("VulkanRhiDeviceBridge::swapchain_timing");
        const SwapchainRecord *record = swapchains_.find(swapchain);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "swapchain_timing: unknown swapchain handle.");
        }
        if (!record->present_timing.timing) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported, "swapchain_timing: present timing is not active on this swapchain.");
        }
        VkSwapchainTimingPropertiesEXT properties{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
        u64 counter = 0;
        const VkResult result = vkGetSwapchainTimingPropertiesEXT(logical_device_->vk_handle(), record->swapchain.vk_handle(),
                                                                  &properties, &counter);
        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "vkGetSwapchainTimingPropertiesEXT failed.");
        }
        // refreshInterval == refreshDuration: fixed refresh; UINT64_MAX: variable refresh (refreshDuration is the
        // minimum cycle); 0: the presentation engine cannot tell (yet).
        rhi::SwapchainTiming timing{.refresh_duration_ns = properties.refreshDuration};
        if (properties.refreshInterval == std::numeric_limits<u64>::max()) {
            timing.variable_refresh = true;
            timing.variable_refresh_known = true;
        } else if (properties.refreshInterval != 0) {
            timing.variable_refresh = false;
            timing.variable_refresh_known = true;
        }
        return timing;
    }

    rhi::RhiExpected<bool> VulkanRhiDeviceBridge::wait_for_present(rhi::SwapchainHandle swapchain, u64 present_id, u64 timeout_ns) {
        ZoneScopedN("VulkanRhiDeviceBridge::wait_for_present");
        const SwapchainRecord *record = swapchains_.find(swapchain);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "wait_for_present: unknown swapchain handle.");
        }
        if (present_id == 0) {
            return true;
        }
        VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
        if (record->present_timing.present_wait_version == 2) {
            const VkPresentWait2InfoKHR info{
                .sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR,
                .presentId = present_id,
                .timeout = timeout_ns,
            };
            result = vkWaitForPresent2KHR(logical_device_->vk_handle(), record->swapchain.vk_handle(), &info);
        } else if (record->present_timing.present_wait_version == 1) {
            result = vkWaitForPresentKHR(logical_device_->vk_handle(), record->swapchain.vk_handle(), present_id, timeout_ns);
        } else {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported, "wait_for_present: present wait is not active on this swapchain.");
        }
        switch (result) {
            case VK_SUCCESS:
            case VK_SUBOPTIMAL_KHR:
            // An out-of-date swapchain will never present this id; waiting longer cannot help.
            case VK_ERROR_OUT_OF_DATE_KHR:
                return true;
            case VK_TIMEOUT:
                return false;
            case VK_ERROR_DEVICE_LOST:
                return rhi::rhi_error(rhi::RhiErrorCode::DeviceLost, "wait_for_present: device lost.");
            case VK_ERROR_SURFACE_LOST_KHR:
                return rhi::rhi_error(rhi::RhiErrorCode::SurfaceLost, "wait_for_present: surface lost.");
            default:
                return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "wait_for_present: vkWaitForPresent failed.");
        }
    }

    rhi::RhiResult VulkanRhiDeviceBridge::drain_past_presentation_timings(rhi::SwapchainHandle swapchain,
                                                                          vector<rhi::PastPresentTiming> &out) {
        ZoneScopedN("VulkanRhiDeviceBridge::drain_past_presentation_timings");
        const SwapchainRecord *record = swapchains_.find(swapchain);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "drain_past_presentation_timings: unknown swapchain handle.");
        }
        const SwapchainRecord::PresentTiming &timing = record->present_timing;
        if (!timing.timing) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "drain_past_presentation_timings: present timing is not active on this swapchain.");
        }
        const VkDevice device = logical_device_->vk_handle();
        const VkPastPresentationTimingInfoEXT info{
            .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,
            .swapchain = record->swapchain.vk_handle(),
        };

        // Count, then fetch (each fetched completed result is removed from the presentation engine's queue).
        VkPastPresentationTimingPropertiesEXT properties{.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
        if (vkGetPastPresentationTimingEXT(device, &info, &properties) != VK_SUCCESS) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "vkGetPastPresentationTimingEXT (count) failed.");
        }
        const u32 available = properties.presentationTimingCount;
        if (available == 0) {
            return {};
        }

        // Every present asked for exactly one stage; the spec requires room for as many stages as were requested,
        // so one per result would do, but leave headroom for implementations that report more than asked.
        constexpr u32 stages_per_result = 4;
        thread_local std::vector<VkPastPresentationTimingEXT> results;
        thread_local std::vector<VkPresentStageTimeEXT> stages;
        results.assign(available, VkPastPresentationTimingEXT{.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT});
        stages.assign(static_cast<usize>(available) * stages_per_result, VkPresentStageTimeEXT{});
        for (u32 i = 0; i < available; ++i) {
            results[i].presentStageCount = stages_per_result;
            results[i].pPresentStages = &stages[static_cast<usize>(i) * stages_per_result];
        }
        properties.presentationTimingCount = available;
        properties.pPresentationTimings = results.data();
        const VkResult fetched = vkGetPastPresentationTimingEXT(device, &info, &properties);
        if (fetched != VK_SUCCESS && fetched != VK_INCOMPLETE) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "vkGetPastPresentationTimingEXT (fetch) failed.");
        }
        const u32 returned = std::min(properties.presentationTimingCount, available);

        // Re-calibrate occasionally so slow drift between the two clocks never accumulates.
        if (timing.shared) {
            const u32 before = timing.shared->outstanding.load(std::memory_order_relaxed);
            timing.shared->outstanding.store(before > returned ? before - returned : 0u, std::memory_order_relaxed);
        }
        thread_local u32 drains_since_calibration = 0;
        if (++drains_since_calibration >= 240 || !timing.shared->calibrated.load(std::memory_order_acquire)) {
            drains_since_calibration = 0;
            (void)calibrate_present_timing(*record);
        }
        const bool calibrated = timing.shared->calibrated.load(std::memory_order_acquire);
        const i64 offset = timing.shared->offset_ns.load(std::memory_order_relaxed);

        for (u32 i = 0; i < returned; ++i) {
            const VkPastPresentationTimingEXT &result = results[i];
            rhi::PastPresentTiming entry{.present_id = result.presentId};
            if (calibrated) {
                for (u32 s = 0; s < std::min(result.presentStageCount, stages_per_result); ++s) {
                    if (result.pPresentStages[s].stage == timing.stage && result.pPresentStages[s].time != 0) {
                        entry.display_time_ns = static_cast<u64>(static_cast<i64>(result.pPresentStages[s].time) + offset);
                        break;
                    }
                }
            }
            out.push_back(entry);
        }
        return {};
    }

} // namespace SFT::Core::Vulkan
