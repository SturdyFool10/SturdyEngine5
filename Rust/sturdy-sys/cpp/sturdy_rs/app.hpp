// Rust <-> SturdyEngine 5 shim for application-loop control. See src/app.rs.
#pragma once

#include <cstdint>
#include <memory>

#include <rust/cxx.h>

#include <Engine/Engine.hpp>

#include "sturdy_rs/shim.hpp"

namespace sturdy_rs::app {

    using EngineView = ::sturdy_rs::EngineView;
    using RedrawHandle = SFT::Engine::RedrawHandle;

    enum class FrameMode : std::uint8_t;

    void set_frame_mode(EngineView &engine, FrameMode mode) noexcept;
    FrameMode frame_mode(const EngineView &engine) noexcept;
    std::unique_ptr<RedrawHandle> redraw_handle(const EngineView &engine);
    void redraw_request(const RedrawHandle &handle) noexcept;
    void redraw_request_after(const RedrawHandle &handle, std::uint64_t nanoseconds) noexcept;

} // namespace sturdy_rs::app
