// Rust <-> SturdyEngine 5 shim for live graphics-device changes. See src/device.rs.
#pragma once

#include <cstdint>

#include <rust/cxx.h>

#include "sturdy_rs/shim.hpp"

namespace sturdy_rs::device {

    using EngineView = ::sturdy_rs::EngineView;

    enum class DeviceBackend : std::uint8_t;
    struct AdapterInfo;
    struct DeviceRequestInfo;
    struct DeviceSwitchInfo;
    struct DeviceChangeInfo;

    rust::Vec<AdapterInfo> enumerate_adapters();
    rust::Vec<DeviceBackend> available_backends(const EngineView &engine);
    rust::Vec<AdapterInfo> available_adapters(EngineView &engine);
    DeviceSwitchInfo switch_device(EngineView &engine, const DeviceRequestInfo &request);
    std::uint64_t device_generation(const EngineView &engine) noexcept;
    rust::Vec<DeviceChangeInfo> take_device_change_events(EngineView &engine);

} // namespace sturdy_rs::device
