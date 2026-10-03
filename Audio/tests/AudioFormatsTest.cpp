#include <Audio/Channels.hpp>
#include <Audio/Decoder.hpp>
#include <Audio/Effects.hpp>
#include <Audio/Encode.hpp>
#include <Audio/Fft.hpp>
#include <Audio/Kernels.hpp>
#include <Audio/Resample.hpp>

#include <Foundation/FileIo.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <random>

using namespace SFT::Audio;
using SFT::u32;
using SFT::u64;
using SFT::usize;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

    std::shared_ptr<SampleBuffer> make_buffer(u32 channels, u32 rate, double seconds, const std::vector<double> &frequencies, float amplitude = 0.4f) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = channels;
        b->sample_rate = rate;
        const usize frames = static_cast<usize>(rate * seconds);
        auto samples = std::make_shared<std::vector<float>>(frames * channels);
        for (usize i = 0; i < frames; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                const double f = frequencies[c % frequencies.size()] * (1.0 + 0.0 * c);
                (*samples)[i * channels + c] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / rate + 0.3 * c));
            }
        }
        b->samples = std::move(samples);
        return b;
    }

    float rms_of(const std::vector<float> &v, usize begin, usize end) {
        double sum = 0.0;
        for (usize i = begin; i < end && i < v.size(); ++i) sum += static_cast<double>(v[i]) * v[i];
        return end > begin ? static_cast<float>(std::sqrt(sum / static_cast<double>(end - begin))) : 0.0f;
    }

    /// Normalised correlation of one channel of two buffers, best over a small lag window (codecs add delay).
    float correlation(const SampleBuffer &a, const SampleBuffer &b, u32 channel, int max_lag = 0) {
        float best = -1.0f;
        for (int lag = -max_lag; lag <= max_lag; ++lag) {
            double dot = 0, na = 0, nb = 0;
            const usize frames = static_cast<usize>(std::min(a.frames(), b.frames()));
            for (usize i = 2000; i + 2000 < frames; ++i) {
                const long j = static_cast<long>(i) + lag;
                if (j < 0 || static_cast<usize>(j) >= frames) continue;
                const double x = (*a.samples)[i * a.channels + channel], y = (*b.samples)[static_cast<usize>(j) * b.channels + channel];
                dot += x * y;
                na += x * x;
                nb += y * y;
            }
            if (na > 0 && nb > 0) best = std::max(best, static_cast<float>(dot / std::sqrt(na * nb)));
        }
        return best;
    }

    struct TempDir {
        std::filesystem::path path;
        TempDir() {
            path = std::filesystem::temp_directory_path() / ("sturdy_audio_formats_" + std::to_string(std::random_device{}()));
            std::filesystem::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    std::shared_ptr<SampleBuffer> load(const std::filesystem::path &p) {
        auto r = load_sound_file(p);
        if (!r) {
            std::cerr << "  load failed: " << r.error() << '\n';
            return nullptr;
        }
        return *r;
    }
} // namespace

int main() {
    TempDir dir;

    // ---- kernels --------------------------------------------------------------------------------------------------------
    {
        std::vector<float> dst(37, 1.0f), src(37, 2.0f);
        Kernels::add_ramp(dst.data(), src.data(), 37, 0.0f, 1.0f);
        check(near(dst[36], 1.0f + 2.0f * 1.0f, 1e-4f) && near(dst[0], 1.0f + 2.0f * (1.0f / 37.0f), 1e-4f), "add_ramp slides gain from 0 to 1");
        std::vector<float> v(103);
        for (usize i = 0; i < v.size(); ++i) v[i] = static_cast<float>(i % 7) - 3.0f;
        v[50] = -9.5f;
        check(near(Kernels::peak(v.data(), v.size()), 9.5f), "peak finds the largest magnitude");
        double reference = 0;
        for (float s : v) reference += static_cast<double>(s) * s;
        check(std::fabs(Kernels::sum_squares(v.data(), v.size()) - reference) < 1e-3, "sum_squares matches a scalar sum");
        std::vector<float> tone = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f};
        std::vector<SFT::i16> i16(tone.size());
        Kernels::f32_to_i16(tone.data(), i16.data(), tone.size());
        check(i16[3] == 32767 && i16[4] == -32767 && i16[5] == 32767, "f32 to i16 clamps");
        std::vector<SFT::u8> i24(tone.size() * 3);
        Kernels::f32_to_i24(tone.data(), i24.data(), tone.size());
        std::vector<float> back(tone.size());
        Kernels::i24_to_f32(i24.data(), back.data(), tone.size());
        check(near(back[1], 0.5f, 1e-6f) && near(back[2], -0.5f, 1e-6f), "24-bit pack round trip");
        std::vector<float> planar_l = {1, 2, 3}, planar_r = {4, 5, 6}, mixed(6);
        const float *planes[] = {planar_l.data(), planar_r.data()};
        Kernels::interleave(planes, 2, mixed.data(), 3);
        check(mixed[0] == 1 && mixed[1] == 4 && mixed[4] == 3 && mixed[5] == 6, "interleave");
    }

    // ---- FFT ---------------------------------------------------------------------------------------------------------------
    {
        const u32 n = 1024;
        Fft fft(n);
        std::vector<float> signal(n), re(n / 2 + 1), im(n / 2 + 1), scratch(fft.real_scratch_size()), back(n);
        for (u32 i = 0; i < n; ++i) signal[i] = std::sin(2.0f * std::numbers::pi_v<float> * 64.0f * static_cast<float>(i) / static_cast<float>(n)) + 0.25f;
        fft.forward_real(signal.data(), re.data(), im.data(), scratch.data());
        u32 peak_bin = 1;
        for (u32 k = 1; k <= n / 2; ++k) {
            if (std::hypot(re[k], im[k]) > std::hypot(re[peak_bin], im[peak_bin])) peak_bin = k;
        }
        check(peak_bin == 64 && near(std::hypot(re[64], im[64]), n / 2.0f, 0.5f), "real FFT puts a sine in its bin");
        check(near(re[0], 0.25f * n, 0.1f), "and the DC term is the mean");
        fft.inverse_real(re.data(), im.data(), back.data(), scratch.data());
        float error = 0;
        for (u32 i = 0; i < n; ++i) error = std::max(error, std::fabs(back[i] - signal[i]));
        check(error < 1e-3f, "real FFT inverse reproduces the signal");
        std::vector<float> cr(n), ci(n, 0.0f), copy_r;
        for (u32 i = 0; i < n; ++i) cr[i] = static_cast<float>((i * 7) % 13) - 6.0f;
        copy_r = cr;
        fft.forward(cr.data(), ci.data());
        fft.inverse(cr.data(), ci.data());
        error = 0;
        for (u32 i = 0; i < n; ++i) error = std::max(error, std::fabs(cr[i] - copy_r[i]));
        check(error < 1e-3f, "complex FFT round trip");
    }

    // ---- resampling -------------------------------------------------------------------------------------------------------
    {
        auto tone = make_buffer(1, 44100, 1.0, {1000.0});
        auto converted = resample(*tone, 48000);
        check(converted->sample_rate == 48000 && converted->frames() == 48000, "resample gives exactly the scaled length");
        int crossings = 0;
        for (usize i = 1; i < converted->samples->size(); ++i) {
            if (((*converted->samples)[i - 1] < 0) != ((*converted->samples)[i] < 0)) ++crossings;
        }
        check(std::abs(crossings - 2000) <= 4, "the 1 kHz tone stays 1 kHz");
        check(rms_of(*converted->samples, 2000, 40000) > 0.26f && rms_of(*converted->samples, 2000, 40000) < 0.30f, "and keeps its level");

        // Block-wise streaming equals whole-buffer conversion.
        Resampler streaming(1, 44100, 48000);
        std::vector<float> pieces;
        const std::vector<float> &source = *tone->samples;
        for (usize i = 0; i < source.size(); i += 333) streaming.process(source.data() + i, std::min<usize>(333, source.size() - i), pieces);
        streaming.flush(pieces);
        float worst = 0;
        for (usize i = 100; i < 40000; ++i) worst = std::max(worst, std::fabs(pieces[i] - (*converted->samples)[i]));
        check(worst < 1e-4f, "streaming resampler matches the offline one");
    }

    // ---- channel layouts ---------------------------------------------------------------------------------------------------
    {
        const SpeakerLayout stereo = SpeakerLayout::stereo();
        auto five_one = ChannelLayoutInfo::from_speakers(SpeakerLayout::surround_5_1());
        const ChannelMatrix down = make_channel_matrix(five_one, stereo);
        const auto dense = down.dense(); // rows: output L, R; columns: FL FR C LFE SL SR
        check(near(dense[2], 0.7071f, 1e-3f) && near(dense[6 + 2], 0.7071f, 1e-3f), "5.1 centre folds to both fronts at -3 dB");
        check(near(dense[0], 1.0f) && near(dense[3], 0.0f), "front left passes through, LFE is dropped");
        check(near(dense[4], 0.7071f, 1e-3f) && near(dense[6 + 5], 0.7071f, 1e-3f), "surrounds fold to their side");
        check(near(dense[1], 0.0f) && near(dense[6 + 4], 0.0f), "and not to the opposite side");
        const ChannelMatrix mono_to_stereo = make_channel_matrix(ChannelLayoutInfo::guess(1), stereo);
        check(mono_to_stereo.taps.size() == 2 && near(mono_to_stereo.taps[0].gain, 0.7071f, 1e-3f), "mono spreads over a stereo pair at -3 dB");
        const ChannelMatrix discrete = make_channel_matrix(ChannelLayoutInfo::discrete(32), SpeakerLayout::surround_7_1());
        check(discrete.taps.size() == 8 && discrete.taps[5].source == 5 && discrete.taps[5].destination == 5, "a 32-channel discrete source maps channel i to speaker i and drops the rest");
        const ChannelMatrix ambi = make_channel_matrix(ChannelLayoutInfo::ambisonic(1), stereo);
        check(!ambi.taps.empty(), "ambisonic sources decode onto speakers");

        const u32 mask = wave_channel_mask(SpeakerLayout::surround_7_1());
        const auto round_trip = layout_from_wave_mask(mask, 8);
        check(round_trip.kind == ChannelKind::Speakers && round_trip.speakers.channel_count() == 8 && round_trip.speakers.speakers[3].role == ChannelRole::Lfe,
              "wave masks round trip");
        check(layout_from_wave_mask(0, 6).speakers.speakers[3].role == ChannelRole::Lfe, "an unlabelled 6-channel file is 5.1");

        // A WAVE-order 5.1 layout reordered for Vorbis: FL C FR RL RR LFE.
        const auto permutation = vorbis_channel_permutation(ChannelLayoutInfo::from_speakers(SpeakerLayout::surround_5_1()));
        check(permutation.size() == 6 && permutation[0] == 0 && permutation[1] == 2 && permutation[2] == 1 && permutation[5] == 3, "Vorbis channel order is derived from roles");

        // Ambisonic rotation: rotating the encoding of a direction equals encoding the rotated direction.
        AmbisonicRotator rotator(3);
        const glm::quat turn = glm::angleAxis(0.9f, glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
        rotator.set_rotation(turn);
        const glm::vec3 d = glm::normalize(glm::vec3(0.4f, 0.2f, -1.0f));
        std::array<float, 16> before{}, after{}, predicted{};
        encode_ambisonic(d, 3, before.data());
        encode_ambisonic(turn * d, 3, after.data());
        const auto m = rotator.matrix();
        float worst = 0;
        for (int o = 0; o < 16; ++o) {
            float sum = 0;
            for (int i = 0; i < 16; ++i) sum += m[static_cast<usize>(o) * 16 + i] * before[i];
            predicted[o] = sum;
            worst = std::max(worst, std::fabs(sum - after[o]));
        }
        check(worst < 5e-3f, "ambisonic rotation matrix rotates a sound field (third order)");

        const ChannelMatrix a_to_b = ambisonic_a_to_b_format();
        check(a_to_b.inputs == 4 && a_to_b.outputs == 4 && !a_to_b.taps.empty(), "A-format to B-format matrix exists");
    }

    // ---- file IO -------------------------------------------------------------------------------------------------------------
    {
        const auto path = dir.path / "io.bin";
        auto writer = SFT::Foundation::Io::FileWriter::create(path);
        check(writer.has_value(), "writer opens");
        std::vector<std::byte> payload(5000);
        for (usize i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i * 31);
        check(writer->write(payload), "writes");
        check(writer->seek(10) && writer->write(payload.data(), 4), "seeks back and patches");
        check(!std::filesystem::exists(path), "an atomic write is invisible until committed");
        check(writer->commit().has_value() && std::filesystem::exists(path), "commit moves it into place");
        auto blob = SFT::Foundation::Io::open_blob(path, SFT::Foundation::Io::AccessHint::Random, 1024);
        check(blob && (*blob)->mapped() && (*blob)->size() == 5000 && (*blob)->data()[10] == payload[0], "big files are memory mapped");
        auto reader = SFT::Foundation::Io::FileReader::open(path);
        std::byte chunk[8];
        check(reader && reader->read_at(20, chunk, 8) == 8 && chunk[0] == payload[20] && reader->read_at(4996, chunk, 8) == 4, "positional reads");
    }

    // ---- encode / decode round trips -------------------------------------------------------------------------------------------
    {
        auto stereo = make_buffer(2, 44100, 1.0, {440.0, 660.0});
        stereo->markers = {{"verse", 11025}, {"chorus", 22050}};
        stereo->loop = LoopRegion{5000, 30000};

        // WAV, 16-bit, with markers and loop.
        EncodeOptions wav;
        wav.format = AudioFormat::Wav;
        check(encode_buffer(dir.path / "a.wav", *stereo, wav).has_value(), "wav encodes");
        auto wav_back = load(dir.path / "a.wav");
        check(wav_back && wav_back->frames() == stereo->frames() && wav_back->channels == 2, "wav keeps length and channels");
        check(wav_back && correlation(*stereo, *wav_back, 0) > 0.9999f, "wav keeps the signal");
        check(wav_back && wav_back->markers.size() == 2 && wav_back->markers[1].name == UString{"chorus"} && wav_back->markers[1].frame == 22050, "wav keeps cue points");
        check(wav_back && wav_back->loop && wav_back->loop->start == 5000 && wav_back->loop->end == 30000, "wav keeps the loop region");

        // 24-bit float multichannel with a speaker mask.
        auto surround = make_buffer(6, 48000, 0.5, {200, 300, 400, 50, 600, 700});
        surround->layout = ChannelLayoutInfo::from_speakers(SpeakerLayout::surround_5_1());
        EncodeOptions float_wav;
        float_wav.format = AudioFormat::Wav;
        float_wav.sample_format = SampleFormat::Float32;
        check(encode_buffer(dir.path / "b.wav", *surround, float_wav).has_value(), "float surround wav encodes");
        auto surround_back = load(dir.path / "b.wav");
        check(surround_back && surround_back->channels == 6 && near((*surround_back->samples)[100], (*surround->samples)[100], 1e-6f), "float wav is bit exact");
        check(surround_back && surround_back->layout.kind == ChannelKind::Speakers && surround_back->layout.speakers.speakers[3].role == ChannelRole::Lfe, "the speaker mask survives");

        // Wave64 and AIFF.
        EncodeOptions w64;
        w64.format = AudioFormat::Wave64;
        w64.sample_format = SampleFormat::Int24;
        check(encode_buffer(dir.path / "c.w64", *stereo, w64).has_value(), "wave64 encodes");
        auto w64_back = load(dir.path / "c.w64");
        check(w64_back && w64_back->frames() == stereo->frames() && correlation(*stereo, *w64_back, 1) > 0.99999f, "wave64 round trips at 24 bits");

        EncodeOptions aiff;
        aiff.format = AudioFormat::Aiff;
        check(encode_buffer(dir.path / "d.aiff", *stereo, aiff).has_value(), "aiff encodes");
        auto aiff_back = load(dir.path / "d.aiff");
        check(aiff_back && aiff_back->frames() == stereo->frames() && aiff_back->sample_rate == 44100 && correlation(*stereo, *aiff_back, 0) > 0.9999f, "aiff round trips");
        check(aiff_back && aiff_back->markers.size() == 2, "aiff keeps markers");

#if STURDY_AUDIO_ENCODE_FLAC
        EncodeOptions flac;
        flac.format = AudioFormat::Flac;
        flac.sample_format = SampleFormat::Int24;
        flac.flac_compression = 8;
        check(encode_buffer(dir.path / "e.flac", *stereo, flac).has_value(), "flac encodes");
        auto flac_back = load(dir.path / "e.flac");
        check(flac_back && flac_back->frames() == stereo->frames() && correlation(*stereo, *flac_back, 0) > 0.999999f, "flac is lossless");
        check(std::filesystem::file_size(dir.path / "e.flac") < std::filesystem::file_size(dir.path / "c.w64"), "and smaller than the PCM it came from");
#endif
#if STURDY_AUDIO_ENCODE_VORBIS
        EncodeOptions vorbis;
        vorbis.format = AudioFormat::OggVorbis;
        vorbis.quality = 0.7f;
        vorbis.tags = {{"title", "Round trip"}};
        check(encode_buffer(dir.path / "f.ogg", *stereo, vorbis).has_value(), "vorbis encodes");
        auto vorbis_back = load(dir.path / "f.ogg");
        check(vorbis_back && vorbis_back->sample_rate == 44100 && std::abs(static_cast<long>(vorbis_back->frames()) - static_cast<long>(stereo->frames())) < 2048, "vorbis keeps the length");
        check(vorbis_back && correlation(*stereo, *vorbis_back, 0, 2200) > 0.98f, "vorbis keeps the signal");
        check(vorbis_back && vorbis_back->loop && vorbis_back->loop->start == 5000, "vorbis carries the loop as tags");

        // 5.1 through Vorbis: channels are reordered on the way in and described by roles on the way out.
        EncodeOptions vorbis_surround;
        vorbis_surround.format = AudioFormat::OggVorbis;
        check(encode_buffer(dir.path / "g.ogg", *surround, vorbis_surround).has_value(), "vorbis encodes 5.1");
        auto vorbis_surround_back = load(dir.path / "g.ogg");
        check(vorbis_surround_back && vorbis_surround_back->channels == 6 && vorbis_surround_back->layout.kind == ChannelKind::Speakers &&
                  vorbis_surround_back->layout.speakers.speakers[1].role == ChannelRole::Center,
              "vorbis 5.1 reports the Vorbis channel roles");
#endif
#if STURDY_AUDIO_ENCODE_OPUS
        EncodeOptions opus;
        opus.format = AudioFormat::Opus;
        opus.bitrate_bps = 128000;
        opus.tags = {{"title", "Opus trip"}};
        check(encode_buffer(dir.path / "h.opus", *stereo, opus).has_value(), "opus encodes (and resamples to 48 kHz)");
        auto opus_decoder = DecoderRegistry::global().open_file(dir.path / "h.opus");
        check(opus_decoder.has_value(), "opus opens");
        if (opus_decoder) {
            const AudioStreamInfo &info = (*opus_decoder)->info();
            check(info.sample_rate == 48000 && info.channels == 2, "opus is 48 kHz stereo");
            check(info.total_frames == 48000, "opus length is exact after pre-skip and end trimming");
            check(!info.tags.empty() && info.tags[0].first == UString{"title"} && info.tags[0].second == UString{"Opus trip"}, "opus tags");
            check(info.loop && info.loop->start == 5442, "the loop point is rescaled to the file's rate");
            // Seeking lands on the right sample: compare a block after a seek with the same block decoded straight through.
            std::vector<float> straight(2 * 4800), after_seek(2 * 4800);
            std::vector<float> skip(2 * 24000);
            (*opus_decoder)->read(skip.data(), 24000);
            (*opus_decoder)->read(straight.data(), 4800);
            check((*opus_decoder)->seek(24000), "opus seeks");
            (*opus_decoder)->read(after_seek.data(), 4800);
            float worst = 0;
            for (usize i = 0; i < straight.size(); ++i) worst = std::max(worst, std::fabs(straight[i] - after_seek[i]));
            check(worst < 2e-3f, "a seek is sample accurate");
        }
        // Many channels: a 12-channel discrete recording through Opus (family 255) keeps every channel.
        auto many = make_buffer(12, 48000, 0.5, {110, 220, 330, 440, 550, 660, 770, 880, 990, 1100, 1210, 1320}, 0.2f);
        EncodeOptions many_opts;
        many_opts.format = AudioFormat::Opus;
        many_opts.bitrate_bps = 384000;
        check(encode_buffer(dir.path / "many.opus", *many, many_opts).has_value(), "opus encodes 12 channels");
        auto many_back = load(dir.path / "many.opus");
        check(many_back && many_back->channels == 12 && many_back->layout.kind == ChannelKind::Discrete, "opus keeps 12 discrete channels");
        bool each = many_back != nullptr;
        for (u32 c = 0; each && c < 12; ++c) each = correlation(*many, *many_back, c, 1100) > 0.9f;
        check(each, "each of the 12 channels comes back as itself");
        // First-order ambisonics keeps its layout.
        auto ambi = make_buffer(4, 48000, 0.4, {200, 300, 400, 500}, 0.2f);
        ambi->layout = ChannelLayoutInfo::ambisonic(1);
        EncodeOptions ambi_opts;
        ambi_opts.format = AudioFormat::Opus;
        check(encode_buffer(dir.path / "ambi.opus", *ambi, ambi_opts).has_value(), "opus encodes ambisonics");
        auto ambi_back = load(dir.path / "ambi.opus");
        check(ambi_back && ambi_back->layout.kind == ChannelKind::Ambisonic && ambi_back->layout.ambisonic_order == 1, "and reports the ambisonic order");
#endif

        // Streaming transcode keeps tags and markers and honours the conversion options.
        EncodeOptions mono_down;
        mono_down.format = AudioFormat::Wav;
        mono_down.channels = 1;
        mono_down.sample_rate = 22050;
        check(transcode(dir.path / "a.wav", dir.path / "mono.wav", mono_down).has_value(), "transcode converts rate and channels");
        auto mono_back = load(dir.path / "mono.wav");
        check(mono_back && mono_back->channels == 1 && mono_back->sample_rate == 22050 && std::abs(static_cast<long>(mono_back->frames()) - 22050) < 3, "transcode output is mono at 22.05 kHz");
        check(mono_back && mono_back->markers.size() == 2 && mono_back->markers[1].frame == 11025, "markers are rescaled to the new rate");

        EncodeOptions flac_options;
        flac_options.format = AudioFormat::Flac;
        check(!encode_buffer(dir.path / "bad.flac", *make_buffer(10, 48000, 0.1, {100}), flac_options).has_value(), "flac refuses more than 8 channels with a clear error");
        check(format_from_extension(".OPUS") == AudioFormat::Opus && format_extension(AudioFormat::Flac) == ustr{".flac"}, "format names");
    }

    // ---- effects ---------------------------------------------------------------------------------------------------------------
    {
        const u32 rate = 48000;
        const auto tone = [&](double frequency, float amplitude = 0.5f) {
            AudioBuffer b(1, 4800);
            for (u32 i = 0; i < 4800; ++i) b.data(0)[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * i / rate));
            return b;
        };
        const auto level = [](const AudioBuffer &b) { return b.rms() * 1.41421356f; };

        auto lowpass = make_low_pass(1000.0f, 4);
        lowpass->prepare(static_cast<float>(rate), 1, 4800);
        AudioBuffer low = tone(100), high = tone(8000);
        lowpass->process(low);
        lowpass->reset();
        lowpass->process(high);
        check(level(low) > 0.45f, "a 4th-order low-pass passes 100 Hz");
        check(level(high) < 0.01f, "and removes 8 kHz (a decade above, 24 dB/oct)");

        auto highpass = make_high_pass(1000.0f, 2);
        highpass->prepare(static_cast<float>(rate), 1, 4800);
        AudioBuffer rumble = tone(60);
        highpass->process(rumble);
        check(level(rumble) < 0.02f, "a high-pass removes rumble");

        // Noise gate: quiet noise is silenced, a loud tone passes.
        auto gate = make_noise_gate(-40.0f, 0.5f, 20.0f, 50.0f);
        gate->prepare(static_cast<float>(rate), 1, 4800);
        AudioBuffer quiet = tone(300, 0.003f);
        gate->process(quiet);
        gate->process(quiet);
        check(level(quiet) < 0.0005f, "the gate silences a signal below its threshold");
        AudioBuffer loud = tone(300, 0.5f);
        gate->process(loud);
        gate->process(loud);
        check(level(loud) > 0.45f, "and opens for a loud one");

        // Compressor reduces level above threshold.
        auto compressor = make_compressor(-20.0f, 8.0f, 1.0f, 50.0f);
        compressor->prepare(static_cast<float>(rate), 1, 4800);
        AudioBuffer hot = tone(500, 0.8f);
        for (int i = 0; i < 3; ++i) { hot = tone(500, 0.8f); compressor->process(hot); }
        check(level(hot) < 0.45f && level(hot) > 0.1f, "the compressor pulls a loud tone down without killing it");

        // Limiter holds its ceiling and reports its latency.
        auto limiter = make_limiter(-6.0f, 50.0f, 2.0f);
        limiter->prepare(static_cast<float>(rate), 1, 4800);
        AudioBuffer slam = tone(200, 1.0f);
        limiter->process(slam);
        limiter->process(slam);
        check(slam.peak() <= 0.5012f + 1e-3f, "the look-ahead limiter never exceeds its -6 dB ceiling");
        check(limiter->latency_frames() == 96, "and reports 2 ms of latency");

        // Hum filter notches 50 Hz and keeps speech-range content.
        auto hum = make_hum_filter(50.0f, 2);
        hum->prepare(static_cast<float>(rate), 1, 4800);
        AudioBuffer mains = tone(50, 0.5f), voice = tone(500, 0.5f);
        for (int i = 0; i < 12; ++i) {
            mains = tone(50, 0.5f); // a notch this narrow needs a fraction of a second to settle
            hum->process(mains);
        }
        hum->reset();
        hum->process(voice);
        check(level(mains) < 0.05f && level(voice) > 0.45f, "the hum filter removes 50 Hz and leaves 500 Hz");

        // Convolution with a delayed delta reproduces the input shifted.
        auto delta = std::make_shared<SampleBuffer>();
        delta->channels = 1;
        delta->sample_rate = rate;
        auto ir = std::make_shared<std::vector<float>>(1000, 0.0f);
        (*ir)[100] = 1.0f;
        delta->samples = ir;
        auto convolver = make_convolution(delta, 1.0f, 0.0f, 0.0f, 0.0f);
        convolver->prepare(static_cast<float>(rate), 1, 256);
        AudioBuffer impulse(1, 256);
        impulse.data(0)[0] = 1.0f;
        convolver->process(impulse);
        check(near(impulse.data(0)[100], 1.0f, 1e-3f) && near(impulse.data(0)[99], 0.0f, 1e-3f), "convolution places the impulse where the IR says");
        AudioBuffer next(1, 256);
        convolver->process(next);
        check(next.peak() < 1e-3f, "and nothing else rings");

        // Effects by name, and a typo is loud.
        auto spec = make_effect(EffectSpec(EffectKind::NoiseGate, {{"threshold", -30.0f}, {"hold", 10.0f}}), static_cast<float>(rate), 2, 512);
        check(spec.has_value() && (*spec)->parameter((*spec)->find_parameter("threshold")) == -30.0f, "effects are built from specs by parameter name");
        auto typo = make_effect(EffectSpec(EffectKind::Compressor, {{"thresold", -30.0f}}));
        check(!typo.has_value() && typo.error().find(std::string_view{"valid:"}) != std::string::npos, "an unknown parameter name is reported with the valid ones");
        check(effect_parameters(EffectKind::Equalizer).size() == 32 && all_effect_kinds().size() == 15, "the catalogue lists every kind and its parameters");

        // Offline application keeps the length and removes look-ahead delay.
        auto sine = make_buffer(2, rate, 0.5, {300.0, 300.0}, 0.9f);
        std::vector<std::unique_ptr<AudioEffect>> chain;
        chain.push_back(make_limiter(-12.0f));
        chain.push_back(make_low_pass(5000.0f));
        auto processed = apply_effects(*sine, chain, false);
        check(processed.has_value() && (*processed)->frames() == sine->frames(), "apply_effects keeps the length");
        check(processed.has_value() && (*processed)->samples && rms_of(*(*processed)->samples, 4000, 20000) * 1.414f < 0.26f, "and limits to the ceiling");
    }

    return failures == 0 ? 0 : 1;
}
