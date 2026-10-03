#include <Audio/DeviceSink.hpp>
#include <Foundation/Iter.hpp>
#include <Audio/Text.hpp>

#include <Audio/Kernels.hpp>
#include <Audio/Realtime.hpp>

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

namespace SFT::Audio {

    namespace {

        ma_channel to_miniaudio(ChannelRole role) noexcept {
            switch (role) {
                case ChannelRole::FrontLeft: return MA_CHANNEL_FRONT_LEFT;
                case ChannelRole::FrontRight: return MA_CHANNEL_FRONT_RIGHT;
                case ChannelRole::Center: return MA_CHANNEL_FRONT_CENTER;
                case ChannelRole::Lfe: return MA_CHANNEL_LFE;
                case ChannelRole::FrontLeftWide: return MA_CHANNEL_FRONT_LEFT_CENTER;
                case ChannelRole::FrontRightWide: return MA_CHANNEL_FRONT_RIGHT_CENTER;
                case ChannelRole::SideLeft: return MA_CHANNEL_SIDE_LEFT;
                case ChannelRole::SideRight: return MA_CHANNEL_SIDE_RIGHT;
                case ChannelRole::RearLeft: return MA_CHANNEL_BACK_LEFT;
                case ChannelRole::RearRight: return MA_CHANNEL_BACK_RIGHT;
                case ChannelRole::TopFrontLeft: return MA_CHANNEL_TOP_FRONT_LEFT;
                case ChannelRole::TopFrontRight: return MA_CHANNEL_TOP_FRONT_RIGHT;
                case ChannelRole::TopRearLeft: return MA_CHANNEL_TOP_BACK_LEFT;
                case ChannelRole::TopRearRight: return MA_CHANNEL_TOP_BACK_RIGHT;
                // miniaudio has no top-side positions: the nearest named top channels.
                case ChannelRole::TopMiddleLeft: return MA_CHANNEL_AUX_0;
                case ChannelRole::TopMiddleRight: return MA_CHANNEL_AUX_1;
                case ChannelRole::Mono: return MA_CHANNEL_MONO;
                case ChannelRole::BackCenter: return MA_CHANNEL_BACK_CENTER;
                case ChannelRole::TopCenter: return MA_CHANNEL_TOP_CENTER;
                case ChannelRole::TopFrontCenter: return MA_CHANNEL_TOP_FRONT_CENTER;
                case ChannelRole::TopRearCenter: return MA_CHANNEL_TOP_BACK_CENTER;
                // Floor-level and second-LFE positions have no miniaudio name; auxiliary channels keep them distinct.
                case ChannelRole::BottomFrontCenter: return MA_CHANNEL_AUX_2;
                case ChannelRole::BottomFrontLeft: return MA_CHANNEL_AUX_3;
                case ChannelRole::BottomFrontRight: return MA_CHANNEL_AUX_4;
                case ChannelRole::Lfe2: return MA_CHANNEL_AUX_5;
                case ChannelRole::Count: break;
            }
            return MA_CHANNEL_NONE;
        }

        struct Callback {
            AudioEngine *engine = nullptr;
            OutputId output = 0;
            u32 channels = 0;
        };

        void data_callback(ma_device *device, void *output, const void *, ma_uint32 frames) {
            // The device thread is the audio thread: flush denormals and ask for audio scheduling (both once per thread).
            prepare_realtime_thread(RealtimeOptions{.period_seconds = static_cast<f64>(frames) / 48000.0});
            auto *cb = static_cast<Callback *>(device->pUserData);
            cb->engine->pull(cb->output, static_cast<f32 *>(output), frames);
        }

    } // namespace

    namespace {

        // Both directions list the same way; `capture` picks which side of the context's device table to read.
        std::vector<AudioDeviceInfo> enumerate_devices(bool capture) {
            std::vector<AudioDeviceInfo> devices;
            ma_context context;
            if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
                return devices;
            }
            ma_device_info *list = nullptr;
            ma_uint32 count = 0;
            const ma_result result = capture ? ma_context_get_devices(&context, nullptr, nullptr, &list, &count)
                                             : ma_context_get_devices(&context, &list, &count, nullptr, nullptr);
            if (result == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < count; ++i) {
                    AudioDeviceInfo info;
                    info.name = list[i].name;
                    info.is_default = list[i].isDefault != 0;
                    // Capabilities need a second query per device; not every backend can answer it.
                    ma_device_info detail{};
                    if (ma_context_get_device_info(&context, capture ? ma_device_type_capture : ma_device_type_playback, &list[i].id, &detail) == MA_SUCCESS) {
                        for (ma_uint32 f = 0; f < detail.nativeDataFormatCount; ++f) {
                            info.max_channels = std::max<u32>(info.max_channels, detail.nativeDataFormats[f].channels);
                            const u32 rate = detail.nativeDataFormats[f].sampleRate;
                            if (rate != 0 && !Foundation::iter(info.sample_rates).any(
                                                 [rate](u32 existing) { return existing == rate; })) {
                                info.sample_rates.push_back(rate);
                            }
                        }
                        std::sort(info.sample_rates.begin(), info.sample_rates.end());
                    }
                    devices.push_back(std::move(info));
                }
            }
            ma_context_uninit(&context);
            return devices;
        }

    } // namespace

    std::vector<AudioDeviceInfo> enumerate_playback_devices() { return enumerate_devices(false); }
    std::vector<AudioDeviceInfo> enumerate_capture_devices() { return enumerate_devices(true); }

    struct DeviceSink::Impl {
        struct Running {
            ma_device device{};
            Callback callback;
            std::vector<ma_channel> channel_map;
            bool started = false;
        };
        ma_context context{};
        bool context_ready = false;
        std::vector<std::unique_ptr<Running>> devices;
    };

    DeviceSink::DeviceSink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    DeviceSink::~DeviceSink() {
        if (!impl_) {
            return;
        }
        for (auto &running : impl_->devices) {
            ma_device_uninit(&running->device); // stops the callback before the engine can go away
        }
        impl_->devices.clear();
        if (impl_->context_ready) {
            ma_context_uninit(&impl_->context);
        }
    }

    u32 DeviceSink::device_count() const noexcept { return static_cast<u32>(impl_->devices.size()); }

    f64 DeviceSink::output_latency_seconds() const noexcept {
        if (impl_->devices.empty()) {
            return 0.0;
        }
        const ma_device &device = impl_->devices.front()->device;
        const f64 frames = static_cast<f64>(device.playback.internalPeriodSizeInFrames) * static_cast<f64>(device.playback.internalPeriods);
        return device.playback.internalSampleRate > 0 ? frames / static_cast<f64>(device.playback.internalSampleRate) : 0.0;
    }

    std::expected<std::unique_ptr<DeviceSink>, UString> DeviceSink::start(AudioEngine &engine, const DeviceSinkConfig &config) {
        auto impl = std::make_unique<Impl>();
        if (ma_context_init(nullptr, 0, nullptr, &impl->context) != MA_SUCCESS) {
            return std::unexpected("audio: could not initialise the audio backend");
        }
        impl->context_ready = true;

        ma_device_info *playback = nullptr;
        ma_uint32 playback_count = 0;
        if (ma_context_get_devices(&impl->context, &playback, &playback_count, nullptr, nullptr) != MA_SUCCESS) {
            playback_count = 0;
        }

        for (OutputId output = 0; output < engine.output_count(); ++output) {
            const UString name = output < config.device_names.size() ? config.device_names[output] : UString{};
            if (name.empty() && output != primary_output) {
                continue;
            }
            const ma_device_id *device_id = nullptr;
            if (!name.empty()) {
                for (ma_uint32 i = 0; i < playback_count; ++i) {
                    if (name == text_from_bytes(playback[i].name)) {
                        device_id = &playback[i].id;
                        break;
                    }
                }
                if (device_id == nullptr) {
                    return std::unexpected(UString{std::format("audio: no playback device named '{}'", name)});
                }
            }

            const OutputDesc &desc = engine.config().outputs[output];
            const SpeakerLayout layout = desc.kind == OutputDesc::Kind::Binaural ? SpeakerLayout::stereo() : desc.layout;
            auto running = std::make_unique<Impl::Running>();
            running->callback = Callback{&engine, output, layout.channel_count()};
            for (const Speaker &s : layout.speakers) {
                running->channel_map.push_back(to_miniaudio(s.role));
            }

            ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
            device_config.playback.pDeviceID = device_id;
            device_config.playback.format = ma_format_f32;
            device_config.playback.channels = layout.channel_count();
            device_config.playback.pChannelMap = running->channel_map.data();
            device_config.sampleRate = engine.config().sample_rate;
            device_config.periodSizeInFrames = engine.config().block_frames;
            device_config.performanceProfile = ma_performance_profile_low_latency;
            device_config.dataCallback = data_callback;
            device_config.pUserData = &running->callback;
            if (ma_device_init(&impl->context, &device_config, &running->device) != MA_SUCCESS) {
                return std::unexpected(UString{std::format("audio: could not open the playback device for output '{}'", desc.name)});
            }
            impl->devices.push_back(std::move(running));
        }

        // Start the secondaries first so their FIFOs are being drained by the time the primary begins feeding them.
        for (usize i = impl->devices.size(); i-- > 0;) {
            if (ma_device_start(&impl->devices[i]->device) != MA_SUCCESS) {
                return std::unexpected("audio: could not start a playback device");
            }
            impl->devices[i]->started = true;
        }
        return std::unique_ptr<DeviceSink>(new DeviceSink(std::move(impl)));
    }

    // ---- capture --------------------------------------------------------------------------------------------------

    namespace {
        constexpr usize kMaxTaps = 8; // slot 0 is the playable source

        struct CaptureContext {
            std::array<std::atomic<LiveSource *>, kMaxTaps> sinks{};
            std::unique_ptr<std::atomic<f32>[]> peaks;
            u32 channels = 0;
            std::atomic<u64> dropped{0};
        };

        void capture_callback(ma_device *device, void *, const void *input, ma_uint32 frames) {
            prepare_realtime_thread(RealtimeOptions{.period_seconds = static_cast<f64>(frames) / 48000.0});
            auto *context = static_cast<CaptureContext *>(device->pUserData);
            const f32 *samples = static_cast<const f32 *>(input);
            for (auto &slot : context->sinks) {
                if (LiveSource *sink = slot.load(std::memory_order_acquire)) {
                    const usize accepted = sink->push(samples, frames);
                    if (accepted < frames) {
                        context->dropped.fetch_add(frames - accepted, std::memory_order_relaxed);
                    }
                }
            }
            // Per-channel peaks for metering: planar reads of one interleaved block.
            const u32 channels = context->channels;
            for (u32 c = 0; c < channels; ++c) {
                f32 peak = 0.0f;
                for (ma_uint32 i = 0; i < frames; ++i) {
                    peak = std::max(peak, std::fabs(samples[static_cast<usize>(i) * channels + c]));
                }
                f32 current = context->peaks[c].load(std::memory_order_relaxed);
                while (peak > current && !context->peaks[c].compare_exchange_weak(current, peak, std::memory_order_relaxed)) {
                }
            }
        }
    } // namespace

    struct CaptureDevice::Impl {
        ma_context context{};
        bool context_ready = false;
        ma_device device{};
        bool device_ready = false;
        std::vector<std::shared_ptr<LiveSource>> sources; // [0] is the playable source, the rest are taps
        u32 sample_rate = 48000;
        u32 channels = 1;
        UString name;
        ChannelLayoutInfo layout;
        CaptureContext callback_context;
        usize ring_frames = 0;
    };

    CaptureDevice::CaptureDevice(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    CaptureDevice::~CaptureDevice() {
        if (!impl_) {
            return;
        }
        if (impl_->device_ready) {
            ma_device_uninit(&impl_->device); // the callback is stopped before the rings go away
        }
        if (impl_->context_ready) {
            ma_context_uninit(&impl_->context);
        }
    }

    std::shared_ptr<LiveSource> CaptureDevice::source() const { return impl_->sources.front(); }
    u32 CaptureDevice::channels() const noexcept { return impl_->channels; }
    u32 CaptureDevice::sample_rate() const noexcept { return impl_->sample_rate; }
    const UString &CaptureDevice::device_name() const noexcept { return impl_->name; }
    ChannelLayoutInfo CaptureDevice::layout() const { return impl_->layout; }
    u64 CaptureDevice::dropped_samples() const noexcept { return impl_->callback_context.dropped.load(std::memory_order_relaxed); }

    f32 CaptureDevice::take_channel_peak(u32 channel) noexcept {
        return channel < impl_->channels ? impl_->callback_context.peaks[channel].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
    }

    std::shared_ptr<LiveSource> CaptureDevice::add_tap(f32 buffer_seconds) {
        Impl &m = *impl_;
        if (m.sources.size() >= kMaxTaps) {
            return nullptr;
        }
        const usize frames = static_cast<usize>(std::max(buffer_seconds, 0.1f) * static_cast<f32>(m.sample_rate));
        // Taps deliver every frame exactly once at the capture rate: no cushion, no drift steering, no dropping.
        auto tap = std::make_shared<LiveSource>(m.channels, m.sample_rate, m.sample_rate, frames, LiveSourceOptions{.adaptive = false});
        tap->set_layout(m.layout);
        m.sources.push_back(tap);
        m.callback_context.sinks[m.sources.size() - 1].store(tap.get(), std::memory_order_release);
        return tap;
    }

    std::expected<std::unique_ptr<CaptureDevice>, UString> CaptureDevice::start(const UString &device_name, u32 sample_rate, f32 buffer_seconds) {
        CaptureConfig config;
        config.device_name = device_name;
        config.sample_rate = sample_rate;
        config.buffer_seconds = buffer_seconds;
        return start(config);
    }

    std::expected<std::unique_ptr<CaptureDevice>, UString> CaptureDevice::start(const CaptureConfig &requested) {
        auto impl = std::make_unique<Impl>();
        if (ma_context_init(nullptr, 0, nullptr, &impl->context) != MA_SUCCESS) {
            return std::unexpected("audio: could not initialise the audio backend");
        }
        impl->context_ready = true;

        const ma_device_id *device_id = nullptr;
        ma_device_info *capture = nullptr;
        ma_uint32 capture_count = 0;
        if (ma_context_get_devices(&impl->context, nullptr, nullptr, &capture, &capture_count) != MA_SUCCESS) {
            capture_count = 0;
        }
        ma_device_info chosen{};
        bool have_info = false;
        for (ma_uint32 i = 0; i < capture_count; ++i) {
            const bool match = requested.device_name.empty() ? capture[i].isDefault != 0 : requested.device_name == text_from_bytes(capture[i].name);
            if (match) {
                if (!requested.device_name.empty() || device_id == nullptr) {
                    device_id = &capture[i].id;
                    impl->name = capture[i].name;
                    have_info = ma_context_get_device_info(&impl->context, ma_device_type_capture, &capture[i].id, &chosen) == MA_SUCCESS;
                }
                if (!requested.device_name.empty()) {
                    break;
                }
            }
        }
        if (!requested.device_name.empty() && device_id == nullptr) {
            return std::unexpected(UString{std::format("audio: no capture device named '{}'", requested.device_name)});
        }

        // Everything the hardware offers unless told otherwise: a stereo mic gets 2, an interface with 32 inputs gets 32.
        u32 channels = requested.channels;
        if (channels == 0) {
            channels = 0;
            if (have_info) {
                for (ma_uint32 f = 0; f < chosen.nativeDataFormatCount; ++f) {
                    channels = std::max<u32>(channels, chosen.nativeDataFormats[f].channels);
                }
            }
            if (channels == 0) {
                channels = 1; // a backend that cannot say: start with one and read what the device settles on
            }
        }
        channels = std::min(channels, max_source_channels);

        ma_device_config config = ma_device_config_init(ma_device_type_capture);
        config.capture.pDeviceID = device_id;
        config.capture.format = ma_format_f32;
        config.capture.channels = channels;
        config.sampleRate = requested.sample_rate;
        config.performanceProfile = ma_performance_profile_low_latency;
        config.dataCallback = capture_callback;
        config.pUserData = &impl->callback_context;
        impl->callback_context.channels = channels;
        impl->callback_context.peaks = std::make_unique<std::atomic<f32>[]>(channels);
        for (u32 c = 0; c < channels; ++c) {
            impl->callback_context.peaks[c].store(0.0f, std::memory_order_relaxed);
        }
        if (ma_device_init(&impl->context, &config, &impl->device) != MA_SUCCESS) {
            return std::unexpected("audio: could not open the capture device (" + std::to_string(channels) + " channels at " + std::to_string(requested.sample_rate) + " Hz)");
        }
        impl->device_ready = true;
        // The device may have settled on other values than requested; the source is built from what it really delivers.
        impl->channels = impl->device.capture.channels;
        impl->sample_rate = impl->device.sampleRate;
        if (impl->channels != channels) {
            impl->callback_context.channels = impl->channels;
            impl->callback_context.peaks = std::make_unique<std::atomic<f32>[]>(impl->channels);
            for (u32 c = 0; c < impl->channels; ++c) {
                impl->callback_context.peaks[c].store(0.0f, std::memory_order_relaxed);
            }
        }
        impl->layout = requested.layout && requested.layout->channels == impl->channels
                           ? *requested.layout
                           : (impl->channels <= 2 ? ChannelLayoutInfo::guess(impl->channels) : ChannelLayoutInfo::discrete(impl->channels));

        const u32 engine_rate = requested.engine_rate != 0 ? requested.engine_rate : impl->sample_rate;
        const usize ring = static_cast<usize>(std::max(requested.buffer_seconds, 0.1f) * static_cast<f32>(impl->sample_rate));
        LiveSourceOptions options;
        options.prefill_frames = static_cast<u32>(std::max(requested.cushion_seconds, 0.0f) * static_cast<f32>(impl->sample_rate));
        options.max_latency_frames = options.prefill_frames * 8;
        options.adaptive = options.prefill_frames > 0;
        auto primary = std::make_shared<LiveSource>(impl->channels, impl->sample_rate, engine_rate, ring, options);
        primary->set_layout(impl->layout);
        impl->sources.push_back(primary);
        impl->callback_context.sinks[0].store(primary.get(), std::memory_order_release);

        if (ma_device_start(&impl->device) != MA_SUCCESS) {
            return std::unexpected("audio: could not start the capture device");
        }
        return std::unique_ptr<CaptureDevice>(new CaptureDevice(std::move(impl)));
    }

} // namespace SFT::Audio
