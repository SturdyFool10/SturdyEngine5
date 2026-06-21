module;

#include "Engine/Application.hpp"
#include "Engine/Engine.hpp"

export module Sturdy.Engine;

export import Sturdy.Core;
export import Sturdy.Foundation;
export import Sturdy.Platform;

export namespace SFT::Engine {
    using ::SFT::Engine::Application;
    using ::SFT::Engine::Engine;
    using ::SFT::Engine::EngineConfig;
}
