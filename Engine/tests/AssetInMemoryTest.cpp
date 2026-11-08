// Files and sounds made from bytes already in memory, and replaced under a stable handle. No GPU: a default-constructed
// Renderer is enough for these asset types.
#include <Engine/AssetManager.hpp>

#include <Renderer/RendererModule.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    void put32(std::vector<std::byte> &out, uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    }
    void put16(std::vector<std::byte> &out, uint16_t v) {
        for (int i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    }
    void tag(std::vector<std::byte> &out, const char *t) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(t[i]));
    }

    /// A mono 16-bit 8 kHz WAV of `frames` frames.
    std::vector<std::byte> wav(uint32_t frames) {
        std::vector<std::byte> out;
        const uint32_t data = frames * 2;
        tag(out, "RIFF"); put32(out, 36 + data); tag(out, "WAVE");
        tag(out, "fmt "); put32(out, 16); put16(out, 1); put16(out, 1); put32(out, 8000); put32(out, 16000); put16(out, 2); put16(out, 16);
        tag(out, "data"); put32(out, data);
        for (uint32_t i = 0; i < frames; ++i) put16(out, static_cast<uint16_t>((i % 64) * 256));
        return out;
    }

    std::vector<std::byte> bytes_of(const char *text) {
        std::vector<std::byte> out(std::strlen(text));
        std::memcpy(out.data(), text, out.size());
        return out;
    }

} // namespace

int main() {
    using namespace SFT;
    Renderer::Renderer renderer;
    Engine::AssetManager assets(renderer);

    auto file = assets.create_file_from_bytes("notes", bytes_of("hello"));
    check(file.has_value(), "file from bytes");
    auto again = assets.create_file_from_bytes("notes", bytes_of("other"));
    check(again && *again == *file, "same name returns the same asset");
    check((*assets.file_bytes(*file))->size() == 5, "file size");

    const auto held = *assets.file_bytes(*file);
    check(assets.replace_file(*file, bytes_of("hello world")).has_value(), "replace file");
    check((*assets.file_bytes(*file))->size() == 11, "replaced file contents");
    check(held->size() == 5, "old readers keep the old bytes");
    check(assets.contains(*file), "file handle survives replace");

    auto sound = assets.create_sound_from_bytes("beep", wav(800), ".wav");
    check(sound.has_value(), "sound from bytes");
    if (sound) {
        check(assets.sound_info(*sound)->frame_count == 800, "sound frames");
        check(assets.replace_sound(*sound, wav(1600), ".wav").has_value(), "replace sound");
        check(assets.sound_info(*sound)->frame_count == 1600, "replaced sound frames");
        check(assets.contains(*sound), "sound handle survives replace");
        check(!assets.replace_sound(*sound, bytes_of("not audio")).has_value(), "bad audio refused");
        check(assets.sound_info(*sound)->frame_count == 1600, "failed replace leaves the sound alone");
    }
    check(!assets.replace_sound(*file, wav(10)).has_value(), "replace_sound on a file is a type error");

    if (failures == 0) std::cout << "OK\n";
    return failures == 0 ? 0 : 1;
}
