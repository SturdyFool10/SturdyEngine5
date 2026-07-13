module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#pragma endregion

export module Sturdy.Platform:WindowGeometry;

export namespace SFT::Platform::Windowing {

    using WindowExtent = glm::u32vec2;
    using WindowPosition = glm::i32vec2;

} // namespace SFT::Platform::Windowing
