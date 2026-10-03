// Drives the audio systems the way the update schedule does (begin_frame, offer_listener, update_audio_source, end_frame) and listens to
// the mixer: anchoring (world / entity / listener) must decide where the sound is heard. Written with checks, not assert(), so it
// means the same in every build type.
#include <Engine/EcsAudio.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <numbers>

using namespace SFT;
using namespace SFT::Engine;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    std::shared_ptr<const Audio::SampleBuffer> tone() {
        auto b = std::make_shared<Audio::SampleBuffer>();
        b->channels = 1;
        b->sample_rate = 48000;
        auto samples = std::make_shared<std::vector<float>>(48000);
        for (usize i = 0; i < samples->size(); ++i) (*samples)[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 300.0 * static_cast<double>(i) / 48000.0));
        b->samples = std::move(samples);
        return b;
    }

    struct Heard {
        double left = 0.0, right = 0.0;
    };

    /// One frame of the schedule, then a few blocks of audio, returning the energy per ear.
    Heard frame(AudioWorld &world, AudioSource &source, const glm::mat4 &emitter, const glm::mat4 &listener, int blocks = 12) {
        world.begin_frame();
        world.offer_listener(0.0f, listener);
        update_audio_source(world, Ecs::Entity{}, source, emitter, 1.0f / 60.0f);
        world.end_frame(1.0f / 60.0f);
        Heard heard;
        Audio::AudioEngine &engine = *world.engine();
        for (int b = 0; b < blocks; ++b) {
            engine.render_block();
            const Audio::AudioBuffer &bed = engine.bed(0);
            for (u32 i = 0; i < bed.frames(); ++i) {
                heard.left += static_cast<double>(bed.data(0)[i]) * bed.data(0)[i];
                heard.right += static_cast<double>(bed.data(1)[i]) * bed.data(1)[i];
            }
        }
        return heard;
    }

    AudioWorld make_world() {
        AudioWorld world;
        Audio::AudioEngineConfig config;
        config.outputs = {Audio::OutputDesc{Audio::OutputDesc::Kind::Speakers, "main", Audio::SpeakerLayout::stereo()}};
        const UString error = world.enable(std::move(config), /*open_device=*/false);
        check(error.empty(), "audio enables without a device");
        return world;
    }
} // namespace

int main() {
    const glm::mat4 listener{1.0f}; // at the origin looking down -Z, +X on the right

    // World anchoring: a fixed point to the right is heard on the right, to the left on the left.
    {
        AudioWorld world = make_world();
        AudioSource source;
        source.sound = tone();
        source.loop = true;
        source.attachment = AudioAttachment::World;
        source.offset = {6.0f, 0.0f, 0.0f};
        const Heard heard = frame(world, source, glm::mat4{1.0f}, listener);
        check(heard.right > heard.left * 4.0 && heard.right > 0.0, "a world source on the right is heard on the right");
        check(world.engine()->stats().voices == 1, "the source became one voice");
    }

    // Entity anchoring: the sound follows the entity's transform ("parented" to a moving object).
    {
        AudioWorld world = make_world();
        AudioSource source;
        source.sound = tone();
        source.loop = true;
        source.attachment = AudioAttachment::Entity;
        const Heard on_left = frame(world, source, glm::translate(glm::mat4{1.0f}, glm::vec3{-6.0f, 0.0f, 0.0f}), listener);
        check(on_left.left > on_left.right * 4.0, "an entity-attached source starts where its entity is (left)");
        Heard later;
        for (int i = 0; i < 40; ++i) later = frame(world, source, glm::translate(glm::mat4{1.0f}, glm::vec3{6.0f, 0.0f, 0.0f}), listener, 4);
        check(later.right > later.left * 4.0, "and follows the entity when it moves (right)");
        // The same source offset in the entity's local space: entity turned 180 degrees about Y puts a +X offset on the left.
        AudioWorld second = make_world();
        AudioSource offset_source;
        offset_source.sound = tone();
        offset_source.loop = true;
        offset_source.offset = {6.0f, 0.0f, 0.0f};
        const glm::mat4 turned = glm::rotate(glm::mat4{1.0f}, std::numbers::pi_v<float>, glm::vec3{0.0f, 1.0f, 0.0f});
        const Heard rotated = frame(second, offset_source, turned, listener);
        check(rotated.left > rotated.right * 4.0, "a local offset turns with its entity");
    }

    // Listener anchoring: head-locked sounds stay put however the listener turns.
    {
        AudioWorld world = make_world();
        AudioSource source;
        source.sound = tone();
        source.loop = true;
        source.attachment = AudioAttachment::Listener;
        source.offset = {5.0f, 0.0f, 0.0f};
        const glm::mat4 turned = glm::rotate(glm::mat4{1.0f}, std::numbers::pi_v<float>, glm::vec3{0.0f, 1.0f, 0.0f});
        Heard heard;
        for (int i = 0; i < 10; ++i) heard = frame(world, source, glm::mat4{1.0f}, turned, 4);
        check(heard.right > heard.left * 4.0, "a listener-attached source stays on the listener's right while the listener turns");
    }

    // Un-positioned (UI) sound plays evenly in both ears.
    {
        AudioWorld world = make_world();
        AudioSource source;
        source.sound = tone();
        source.loop = true;
        source.spatial = false;
        const Heard heard = frame(world, source, glm::mat4{1.0f}, listener);
        check(heard.left > 0.0 && std::fabs(heard.left - heard.right) < heard.left * 0.05, "a 2D sound is centred");
    }

    // Acoustics run on a worker: a slow provider must not stretch the frame, and its answer must still arrive.
    {
        struct SlowProvider final : Audio::AcousticsProvider {
            std::atomic<int> evaluations{0};
            std::atomic<std::thread::id> thread{};
            void evaluate(std::span<const Audio::AcousticsQuery> queries, std::span<Audio::AcousticsResult> results) override {
                thread = std::this_thread::get_id();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                for (auto &r : results) r.lowpass_cutoff = 500.0f; // very muffled
                (void)queries;
                ++evaluations;
            }
        };
        Async::Scheduler::initialize(2);
        AudioWorld world = make_world();
        auto provider = std::make_shared<SlowProvider>();
        world.set_acoustics(provider);
        world.set_acoustics_rate(0.0f, 4);
        AudioSource source;
        source.sound = tone();
        source.loop = true;
        source.attachment = AudioAttachment::World;
        source.offset = {6.0f, 0.0f, 0.0f};
        const auto start = std::chrono::steady_clock::now();
        const Heard first = frame(world, source, glm::mat4{1.0f}, listener); // frame() calls end_frame, which schedules the job
        for (int i = 0; i < 3; ++i) (void)frame(world, source, glm::mat4{1.0f}, listener, 1); // tracked_ is filled by update_audio_source each frame
        const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        check(elapsed_ms < 140.0, "a slow acoustics provider does not stall the frame");
        for (int i = 0; i < 100 && provider->evaluations.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(provider->evaluations.load() >= 1 && provider->thread.load() != std::this_thread::get_id(), "the provider ran on another thread");
        // Applying the result: the muffled voice is much quieter in the high-frequency tone's energy than before.
        Heard later;
        for (int i = 0; i < 40; ++i) later = frame(world, source, glm::mat4{1.0f}, listener, 4);
        check(first.right > 0.0 && later.right < first.right * 0.5, "and its answer (a 500 Hz low-pass) reaches the mixer");
        bool edited = false;
        world.edit_acoustics([&](Audio::AcousticsProvider &) { edited = true; });
        check(edited, "settings can be edited between evaluations");
        world.disable();
        Async::Scheduler::shutdown();
    }

    return failures == 0 ? 0 : 1;
}
