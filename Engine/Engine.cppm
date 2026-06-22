module;

#include "Engine/Application.hpp"
#include "Engine/Engine.hpp"

export module Sturdy.Engine;

export import Sturdy.Core;
export import Sturdy.Foundation;
export import Sturdy.Platform;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused"
#endif

export namespace SFT::Engine {
    using ::SFT::Engine::Application;
    using ::SFT::Engine::Engine;
    using ::SFT::Engine::EngineConfig;
} // namespace SFT::Engine

#if defined(__clang__)
#pragma clang diagnostic pop
#endif // namespace SFT::Engine
