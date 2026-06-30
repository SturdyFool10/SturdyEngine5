export module Sturdy.Foundation;

export import :Concepts;
export import :Constants;
export import :Embed;
export import :Log;
export import :Math;
export import :Memory;
export import :NumericConcepts;
export import :Types;
export import :Utils;
export import :Wide;
export import :WideTraits;

import :MemoryImpl;

namespace SFT::Foundation::Detail {

    [[maybe_unused]] const bool memory_initialized = []() noexcept {
        Memory::initialize();
        return true;
    }();

} // namespace SFT::Foundation::Detail
