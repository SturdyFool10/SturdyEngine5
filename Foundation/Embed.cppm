module;

#include <string_view>

export module Sturdy.Foundation:Embed;

using std::string_view;

export namespace SFT::Foundation {

    // Text baked into the binary at build time — a `string_view` over static storage, so it costs no
    // allocation and outlives everything. Used for embedded shader source and other compiled-in assets
    // (see `Core::Slang::StaticShaderSource`), letting them be compiled with zero runtime file I/O.
    using EmbeddedText = string_view;

} // namespace SFT::Foundation
