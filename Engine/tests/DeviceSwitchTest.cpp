#include <Engine/DeviceSwitch.hpp>

#include <cstdio>
#include <string>

// Explicit checks rather than assert(): asserts compile to nothing under NDEBUG, which would turn this into a no-op.
#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

namespace {

    using namespace SFT;
    using namespace SFT::Engine;

    RHI::GpuApiSupport api(RHI::BackendType backend) {
        RHI::GpuApiSupport support;
        support.adapter.backend = backend;
        return support;
    }

    RHI::PhysicalGpu gpu(const char *name, RHI::DeviceType type, u32 vendor, u32 device, const char *physical_id,
                         std::vector<RHI::BackendType> backends) {
        RHI::PhysicalGpu out;
        out.name = name;
        out.device_type = type;
        out.vendor_id = vendor;
        out.device_id = device;
        out.physical_device_id = physical_id;
        for (RHI::BackendType backend : backends) {
            out.api_support.push_back(api(backend));
        }
        return out;
    }

    RHI::GpuInventory inventory() {
        using B = RHI::BackendType;
        RHI::GpuInventory inv;
        inv.gpus.push_back(gpu("Radeon iGPU", RHI::DeviceType::IntegratedGpu, 0x1002, 0x164e, "igpu-uuid", {B::Vulkan, B::WebGpu}));
        inv.gpus.push_back(gpu("Radeon RX 9070", RHI::DeviceType::DiscreteGpu, 0x1002, 0x7550, "dgpu-uuid", {B::Vulkan, B::D3D12, B::WebGpu}));
        return inv;
    }

} // namespace

int main() {
    int failures = 0;
    using B = RHI::BackendType;
    const RHI::GpuInventory inv = inventory();

    // Stable ids: deterministic, distinct per GPU, independent of the backend list and the backend-specific id.
    {
        RHI::PhysicalGpu a = inv.gpus[1];
        RHI::PhysicalGpu b = inv.gpus[1];
        b.physical_device_id = "different-uuid-after-driver-update";
        b.api_support.pop_back();
        CHECK(adapter_stable_id(a) == adapter_stable_id(b));
        CHECK(adapter_stable_id(inv.gpus[0]) != adapter_stable_id(inv.gpus[1]));
        CHECK(adapter_stable_id(a).size() == 16);
    }

    // Auto prefers the high-performance adapter and keeps the running API when it is supported.
    {
        const DeviceResolution r = resolve_device_request(inv, {}, B::Vulkan);
        CHECK(r.ok && r.gpu_index == 1 && r.backend == B::Vulkan && r.fallback_reason.empty());
        const DeviceResolution keep = resolve_device_request(inv, {}, B::WebGpu);
        CHECK(keep.ok && keep.backend == B::WebGpu);
        // Running API the chosen adapter lacks: Auto moves to the first one it has instead of failing.
        const DeviceResolution moved = resolve_device_request(inv, {}, B::Metal);
        CHECK(moved.ok && moved.backend == B::Vulkan);
    }

    // Low power picks the integrated GPU; the *Only constraints never downgrade.
    {
        CHECK(resolve_device_request(inv, {.preference = DevicePreference::LowPower}, B::Vulkan).gpu_index == 0);
        CHECK(resolve_device_request(inv, {.preference = DevicePreference::IntegratedOnly}, B::Vulkan).gpu_index == 0);
        RHI::GpuInventory only_integrated;
        only_integrated.gpus.push_back(inv.gpus[0]);
        const DeviceResolution r = resolve_device_request(only_integrated, {.preference = DevicePreference::DiscreteOnly}, B::Vulkan);
        CHECK(!r.ok && !r.error.empty());
    }

    // An explicit API nothing supports is an error, not a silent downgrade to another API.
    {
        const DeviceResolution r = resolve_device_request(inv, {.backend = B::Metal}, B::Vulkan);
        CHECK(!r.ok && r.error.find("Metal") != std::string::npos);
    }

    // A saved adapter is found by stable id or by physical id, and honoured exactly.
    {
        const std::string igpu = adapter_stable_id(inv.gpus[0]);
        const DeviceResolution by_stable = resolve_device_request(inv, {.adapter_id = igpu}, B::Vulkan);
        CHECK(by_stable.ok && by_stable.gpu_index == 0 && by_stable.fallback_reason.empty());
        const DeviceResolution by_physical = resolve_device_request(inv, {.adapter_id = std::string{"igpu-uuid"}}, B::Vulkan);
        CHECK(by_physical.ok && by_physical.gpu_index == 0);
    }

    // The saved adapter is gone (unplugged / driver change): fall back to the profile and say so.
    {
        const DeviceResolution r = resolve_device_request(
            inv, {.adapter_id = std::string{"0123456789abcdef"}, .preference = DevicePreference::LowPower}, B::Vulkan);
        CHECK(r.ok && r.gpu_index == 0 && !r.fallback_reason.empty());
    }

    // The saved adapter exists but lacks the requested API: another adapter that has it is a flagged fallback.
    {
        const std::string igpu = adapter_stable_id(inv.gpus[0]);
        const DeviceResolution r = resolve_device_request(inv, {.backend = B::D3D12, .adapter_id = igpu}, B::Vulkan);
        CHECK(r.ok && r.gpu_index == 1 && r.backend == B::D3D12 && !r.fallback_reason.empty());
    }

    // Enumeration: backends in priority order, the running adapter marked active and presentable.
    {
        const std::vector<RHI::BackendType> backends = available_backends(inv);
        CHECK(backends.size() == 3 && backends[0] == B::Vulkan && backends[1] == B::D3D12 && backends[2] == B::WebGpu);
        const std::vector<AdapterDescription> described = describe_adapters(inv, B::Vulkan, "dgpu-uuid");
        CHECK(described.size() == 2);
        CHECK(described[1].backends[0].active && described[1].backends[0].presentable == AdapterPresentability::Yes);
        CHECK(!described[1].backends[1].active && described[1].backends[1].presentable == AdapterPresentability::Unknown);
        CHECK(!described[0].backends[0].active);
    }

    // An empty inventory is a typed failure.
    CHECK(!resolve_device_request({}, {}, B::Vulkan).ok);

    if (failures == 0) {
        std::puts("DeviceSwitchTest: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
