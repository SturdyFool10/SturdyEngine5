#include <Audio/Decoder.hpp>
#include <Audio/Encode.hpp>

namespace SFT::Audio {

    // No system codec framework here: AAC/M4A/WMA are not decoded on this platform (the royalty-bearing codecs are left to
    // the OS rather than bundled). Everything else is handled by the built-in backends.
    std::unique_ptr<DecoderBackend> make_platform_backend() { return nullptr; }

    // Likewise for encoding: AAC and ALAC need an OS codec, and there is none to ask here. MP3 export works when libmp3lame is
    // installed (see EncodeMp3.cpp); FLAC, Vorbis and Opus are built in.
    std::unique_ptr<EncoderBackend> make_platform_aac_encoder() { return nullptr; }
    std::unique_ptr<EncoderBackend> make_platform_alac_encoder() { return nullptr; }

} // namespace SFT::Audio
