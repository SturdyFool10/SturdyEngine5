#include <Audio/Edit.hpp>
#include <Audio/Encode.hpp>
#include <Audio/AudioBuffer.hpp>
#include <Audio/Decoder.hpp>
#include <Audio/Kernels.hpp>
#include <Audio/Loudness.hpp>
#include <Audio/VorbisComments.hpp>
#include <Audio/Waveform.hpp>

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
    bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

    std::shared_ptr<SampleBuffer> tone(u32 channels, u32 rate, double seconds, double frequency, float amplitude = 0.5f) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = channels;
        b->sample_rate = rate;
        const usize frames = static_cast<usize>(rate * seconds);
        auto samples = std::make_shared<std::vector<float>>(frames * channels);
        for (usize i = 0; i < frames; ++i)
            for (u32 c = 0; c < channels; ++c) (*samples)[i * channels + c] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate));
        b->samples = std::move(samples);
        return b;
    }

    // Counts upward zero crossings per second: a cheap pitch estimate for a clean tone.
    double estimate_frequency(const SampleBuffer &b, usize from = 2000) {
        const auto &d = *b.samples;
        long crossings = 0;
        for (usize i = from + 1; i + 2000 < b.frames(); ++i) {
            if (d[(i - 1) * b.channels] < 0.0f && d[i * b.channels] >= 0.0f) ++crossings;
        }
        const double seconds = static_cast<double>(b.frames() - from - 2000) / b.sample_rate;
        return static_cast<double>(crossings) / seconds;
    }
} // namespace

int main() {
    // ---- structure with markers ------------------------------------------------------------------------------------------------
    {
        auto base = tone(2, 48000, 1.0, 440.0);
        base->markers = {{"a", 4800}, {"b", 24000}, {"c", 40000}};
        base->loop = LoopRegion{30000, 44000};
        const auto cut = edit::remove(*base, 20000, 30000);
        check(cut->frames() == 38000, "remove shortens by the cut");
        check(cut->markers.size() == 2 && cut->markers[0].name == UString{"a"} && cut->markers[1].name == UString{"c"} && cut->markers[1].frame == 30000, "markers inside a cut vanish and later ones move up");
        check(cut->loop && cut->loop->start == 20000 && cut->loop->end == 34000, "the loop region shifts with them");
        const auto piece = edit::slice(*base, 20000, 30000);
        check(piece->frames() == 10000 && piece->markers.size() == 1 && piece->markers[0].frame == 4000, "slice keeps markers inside it, relative to its start");
        const auto joined = edit::insert(*cut, 20000, *piece);
        check(joined->frames() == base->frames() && joined->markers.size() == 2, "insert puts it back");
        check((*joined->samples)[30000 * 2] == (*base->samples)[30000 * 2], "and the audio is identical");
        const auto doubled = edit::repeat(*base, 2);
        check(doubled->frames() == 96000 && doubled->markers.size() == 6 && doubled->markers[3].frame == 48000 + 4800, "repeat repeats markers too");
        const auto reversed = edit::reverse(*base);
        check((*reversed->samples)[0] == (*base->samples)[(base->frames() - 1) * 2], "reverse flips the samples");
    }

    // ---- fades, mixing, crossfade ------------------------------------------------------------------------------------------------
    {
        auto loud = tone(1, 48000, 1.0, 100.0, 1.0f);
        std::fill(const_cast<std::vector<float> &>(*loud->samples).begin(), const_cast<std::vector<float> &>(*loud->samples).end(), 1.0f);
        const auto faded = edit::fade_in(*loud, 4800, edit::FadeCurve::Linear);
        check((*faded->samples)[0] < 0.001f && near((*faded->samples)[2400], 0.5f, 0.01) && near((*faded->samples)[9000], 1.0f, 1e-6), "a linear fade-in ramps and then holds");
        const auto out = edit::fade_out(*loud, 4800, edit::FadeCurve::Linear);
        check(near((*out->samples)[47999], 0.0, 0.001) && near((*out->samples)[100], 1.0, 1e-6), "a fade-out ends at silence");
        const auto envelope = edit::gain_envelope(*loud, std::vector<std::pair<u64, float>>{{0, 0.0f}, {1000, 1.0f}, {2000, 0.5f}});
        check(near((*envelope->samples)[500], 0.5, 0.01) && near((*envelope->samples)[1500], 0.75, 0.01) && near((*envelope->samples)[5000], 0.5, 1e-6), "gain envelopes interpolate between points");

        auto a = tone(1, 48000, 1.0, 200.0), b = tone(1, 48000, 1.0, 300.0);
        const std::vector<const SampleBuffer *> pair{a.get(), b.get()};
        const auto cross = edit::concat(pair, 4800);
        check(cross && (*cross)->frames() == 96000 - 4800, "a crossfaded join overlaps by the fade length");
        const auto straight = edit::concat(pair, 0);
        check(straight && (*straight)->frames() == 96000, "and a plain join does not");
        const auto mixed = edit::mix(*a, *b, 24000, 1.0f, 0.5f);
        check(mixed->frames() == 48000 + 24000, "mix extends to the longer of the two");
    }

    // ---- loudness and normalisation -----------------------------------------------------------------------------------------------
    {
        auto quiet = tone(2, 48000, 5.0, 1000.0, 0.05f);
        auto louder = edit::gain(*quiet, 6.0f);
        const double l0 = measure_loudness(*quiet).integrated, l1 = measure_loudness(*louder).integrated;
        check(near(l1 - l0, 6.0, 0.05), "loudness follows gain 1:1");
        edit::NormalizeOptions options;
        options.target_lufs = -23.0f;
        const auto leveled = edit::normalize(*quiet, options);
        check(leveled && near(measure_loudness(**leveled).integrated, -23.0, 0.1), "normalising to -23 LUFS lands on -23 LUFS");
        const LoudnessResult m = measure_loudness(*tone(2, 48000, 3.0, 1000.0, 1.0f));
        check(m.true_peak_db >= m.sample_peak_db - 1e-6 && near(m.sample_peak_db, 0.0, 0.05), "peak readings");
        edit::NormalizeOptions peak;
        peak.target_peak_db = -3.0f;
        const auto peaked = edit::normalize(*quiet, peak);
        check(peaked && near(measure_loudness(**peaked).sample_peak_db, -3.0, 0.05), "peak normalisation hits its target");
    }

    // ---- silence ------------------------------------------------------------------------------------------------------------------------
    {
        auto head = tone(1, 48000, 0.5, 440.0), gap = edit::silence(*head, 24000), tail = tone(1, 48000, 0.5, 440.0);
        const std::vector<const SampleBuffer *> parts{head.get(), gap.get(), tail.get()};
        auto whole = *edit::concat(parts);
        const auto found = edit::find_silence(*whole, -60.0f, 100.0f);
        check(found.size() == 1 && found[0].start >= 23990 && found[0].start <= 24010 && found[0].length() >= 23900, "the silent stretch is found");
        auto padded = *edit::concat(std::vector<const SampleBuffer *>{gap.get(), head.get(), gap.get()});
        const auto trimmed = edit::trim_silence(*padded, -60.0f, 0.0f);
        check(trimmed->frames() >= 23990 && trimmed->frames() <= 24100, "leading and trailing silence is trimmed");
    }

    // ---- channels, rate, time and pitch -------------------------------------------------------------------------------------------------
    {
        auto surround = std::make_shared<SampleBuffer>();
        surround->channels = 6;
        surround->sample_rate = 48000;
        surround->layout = ChannelLayoutInfo::from_speakers(SpeakerLayout::surround_5_1());
        surround->samples = std::make_shared<std::vector<float>>(6 * 1000, 0.0f);
        for (usize i = 0; i < 1000; ++i) const_cast<std::vector<float> &>(*surround->samples)[i * 6 + 2] = 0.5f; // centre
        const auto stereo = edit::convert_channels(*surround, 2);
        check(stereo->channels == 2 && near((*stereo->samples)[0], 0.3536, 1e-3) && near((*stereo->samples)[1], 0.3536, 1e-3), "5.1 folds to stereo by role");
        const auto picked = edit::extract_channels(*surround, std::vector<u32>{2, 0});
        check(picked->channels == 2 && near((*picked->samples)[0], 0.5), "channels can be picked out");

        auto sine = tone(1, 48000, 2.0, 440.0);
        check(near(estimate_frequency(*sine), 440.0, 2.0), "the reference tone reads 440 Hz");
        const auto stretched = edit::time_stretch(*sine, 1.5);
        check(stretched->frames() == 144000, "time stretch changes the length by the ratio");
        check(near(estimate_frequency(*stretched), 440.0, 8.0), "and keeps the pitch");
        const auto shorter = edit::time_stretch(*sine, 0.5);
        check(shorter->frames() == 48000 && near(estimate_frequency(*shorter), 440.0, 8.0), "also when compressing");
        const auto octave = edit::pitch_shift(*sine, 12.0);
        check(std::abs(static_cast<long>(octave->frames()) - 96000) < 400 && near(estimate_frequency(*octave), 880.0, 12.0), "pitch shift up an octave keeps the length");
        const auto fast = edit::change_speed(*sine, 2.0);
        check(std::abs(static_cast<long>(fast->frames()) - 48000) < 4 && near(estimate_frequency(*fast), 880.0, 6.0), "change speed is varispeed: half the length, an octave up");
        const auto down = edit::convert_rate(*sine, 22050);
        check(down->sample_rate == 22050 && std::abs(static_cast<long>(down->frames()) - 44100) < 3, "rate conversion");
        const auto crushed = edit::quantize(*sine, 4, false);
        std::vector<float> levels(*crushed->samples);
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        check(levels.size() <= 18, "a 4-bit quantiser leaves a few distinct levels");
    }

    // ---- waveform data and images -----------------------------------------------------------------------------------------------------
    {
        auto stereo = tone(2, 48000, 4.0, 220.0, 0.8f);
        stereo->markers = {{"mid", 96000}};
        const PeakPyramid pyramid = PeakPyramid::from_buffer(*stereo, 256);
        check(pyramid.frames() == 192000 && pyramid.channels() == 2, "pyramid knows its length");
        const auto cols = pyramid.columns(0, 192000, 100, 0);
        check(cols.size() == 100 && near(cols[10].max, 0.8, 0.01) && near(cols[10].min, -0.8, 0.01), "columns report the sine's extremes");
        check(near(cols[50].rms, 0.8 / std::sqrt(2.0), 0.02), "and its RMS");
        const PeakColumn one = pyramid.range(1000, 2000, 1);
        check(one.max > 0.5f, "arbitrary ranges can be summarised");

        const auto path = std::filesystem::temp_directory_path() / "sturdy_edit_test.peaks";
        check(pyramid.save(path).has_value(), "peaks save");
        const auto loaded = PeakPyramid::load(path);
        check(loaded && loaded->frames() == pyramid.frames() && near(loaded->columns(0, 192000, 100, 0)[10].max, 0.8, 0.01), "and load back");
        std::filesystem::remove(path);

        // Build from a file in blocks, with a cache.
        const auto wav = std::filesystem::temp_directory_path() / "sturdy_edit_test.wav";
        EncodeOptions wav_options;
        check(encode_buffer(wav, *stereo, wav_options).has_value(), "test wav written");
        const auto cache = std::filesystem::temp_directory_path() / "sturdy_edit_test_wav.peaks";
        const auto from_file = PeakPyramid::from_file(wav, 256, {}, cache);
        check(from_file && from_file->frames() == 192000 && std::filesystem::exists(cache), "a pyramid builds from a file and caches itself");
        const auto cached = PeakPyramid::from_file(wav, 256, {}, cache);
        check(cached && cached->frames() == 192000, "the cache is reused");
        const auto range = read_frames(wav, 1000, 500);
        check(range && (*range)->frames() == 500, "frames can be read from a file for deep zoom");
        std::filesystem::remove(wav);
        std::filesystem::remove(cache);

        const Image image = render_waveform(pyramid, 0, 192000, 400, 120, WaveformStyle{}, stereo->markers);
        check(image.width == 400 && image.height == 120 && image.rgba.size() == 400 * 120 * 4, "the waveform image has the requested size");
        // Stacked lanes of 59 px: the first lane's centre column has wave colour above the middle line.
        const auto pixel = [&](u32 x, u32 y) { return std::array<int, 3>{image.rgba[(y * 400 + x) * 4], image.rgba[(y * 400 + x) * 4 + 1], image.rgba[(y * 400 + x) * 4 + 2]}; };
        check(pixel(10, 8) != pixel(10, 30) || pixel(10, 20) != pixel(10, 3), "pixels differ between the wave and the background");
        const Image deep = render_waveform(*stereo, 5000, 5300, 300, 80, WaveformStyle{.mode = WaveformMode::Line});
        check(!deep.empty(), "line mode draws individual samples when zoomed far in");

        // Spectrogram of a pure tone peaks at the tone's frequency.
        auto probe = tone(1, 48000, 2.0, 1000.0, 0.8f);
        SpectrogramOptions options;
        options.scale = FrequencyScale::Linear;
        options.min_hz = 0.0f + 20.0f;
        options.max_hz = 4000.0f;
        options.rows = 400;
        options.columns = 40;
        const Spectrogram spectrogram = compute_spectrogram(*probe, 0, 96000, options);
        u32 best_row = 0;
        for (u32 r = 0; r < spectrogram.rows; ++r) if (spectrogram.at(r, 20) > spectrogram.at(best_row, 20)) best_row = r;
        check(near(spectrogram.frequency_of_row(best_row), 1000.0, 25.0) && spectrogram.at(best_row, 20) > 0.9f, "the spectrogram shows a 1 kHz tone at 1 kHz");
        const Image sg = render_spectrogram(spectrogram, 200, 100, ColorMap::Magma);
        check(sg.width == 200 && sg.rgba.size() == 200 * 100 * 4, "and renders to an image");
        const Image scope = render_oscilloscope(*probe->samples, 300, 100);
        const Image gonio = render_goniometer(*stereo->samples, *stereo->samples, 128);
        check(!scope.empty() && !gonio.empty(), "scopes render");
        std::vector<float> left(1000), right(1000);
        for (usize i = 0; i < 1000; ++i) { left[i] = std::sin(0.1f * static_cast<float>(i)); right[i] = -left[i]; }
        check(stereo_correlation(left, right) < -0.99f && stereo_correlation(left, left) > 0.99f, "stereo correlation");
        const auto png = std::filesystem::temp_directory_path() / "sturdy_waveform_test.png";
        check(write_png(png, image).has_value() && std::filesystem::file_size(png) > 1000, "images save as PNG");
        std::filesystem::remove(png);
    }

    // Frames above the parallel threshold take the scheduler path and must agree with the serial path frame for frame.
    {
        auto small = tone(2, 48000, 0.02, 440.0);
        auto big = tone(2, 48000, 4.0, 440.0); // 192000 frames, over the threshold
        const auto small_51 = edit::convert_channels(*small, 6);
        const auto big_51 = edit::convert_channels(*big, 6);
        bool same = small_51->samples->size() == small->frames() * 6 && big_51->samples->size() == big->frames() * 6;
        for (usize i = 0; same && i < small_51->samples->size(); ++i) same = (*small_51->samples)[i] == (*big_51->samples)[i];
        check(same, "converting channels in parallel matches the serial result");
        const u32 picks[] = {1, 7, 0};
        const auto picked = edit::extract_channels(*big, picks);
        bool extracted = picked->channels == 3 && picked->frames() == big->frames();
        for (usize f = 0; extracted && f < picked->frames(); f += 997) {
            extracted = (*picked->samples)[f * 3] == (*big->samples)[f * 2 + 1] && (*picked->samples)[f * 3 + 1] == 0.0f && (*picked->samples)[f * 3 + 2] == (*big->samples)[f * 2];
        }
        check(extracted, "extracting channels (including one that does not exist) in parallel");
    }

    // Text helpers and the shared Vorbis-comment reader.
    {
        check(parse_number<int>("  42 rest") == 42 && !parse_number<int>("x").has_value() && number_or<u32>("", 7u) == 7u, "numbers parse leniently");
        const auto pieces = split("a;b;;c", ';');
        check(pieces.size() == 4 && pieces[2].empty() && pieces[3] == "c"_ustr, "split keeps empty pieces");
        check(trim_end("hi \r\n") == "hi"_ustr, "trim_end drops line endings");
        AudioStreamInfo info;
        info.total_frames = 1000;
        VorbisCommentReader reader{info};
        reader.add("TITLE=Wind");
        reader.add("junk without equals");
        reader.add("LoopStart=100");
        reader.add("LOOPLENGTH=250");
        reader.finish();
        check(info.tags.size() == 1 && info.tags[0].first == "title"_ustr && info.tags[0].second == "Wind"_ustr, "comments become lower-case tags");
        check(info.loop && info.loop->start == 100 && info.loop->end == 350, "LOOPSTART/LOOPLENGTH become a loop region");
        AudioStreamInfo open_ended;
        open_ended.total_frames = 900;
        VorbisCommentReader to_end{open_ended};
        to_end.add("LOOPSTART=300");
        to_end.finish();
        check(open_ended.loop && open_ended.loop->end == 900, "a loop with no end runs to the end of the stream");
    }

    // Interleaved <-> planar conversion with matching and mismatched channel counts.
    {
        std::vector<float> three(5 * 3);
        for (usize i = 0; i < three.size(); ++i) three[i] = static_cast<float>(i);
        AudioBuffer two_channels(2, 8);
        two_channels.load_interleaved(three, 3, 1); // 5 frames of 3 channels into a 2 channel buffer, starting at frame 1
        check(two_channels.channel(0)[0] == 0.0f && two_channels.channel(0)[1] == 0.0f && two_channels.channel(0)[2] == 3.0f && two_channels.channel(1)[5] == 13.0f, "loading wider interleaved data keeps the shared channels");
        check(two_channels.channel(0)[7] == 0.0f, "and stops at the end of the data");
        AudioBuffer same(3, 5);
        same.load_interleaved(three, 3);
        std::vector<float> back(5 * 4, -1.0f);
        same.store_interleaved(back, 4, 5);
        bool round_trip = true;
        for (usize f = 0; f < 5; ++f) round_trip = round_trip && back[f * 4] == three[f * 3] && back[f * 4 + 2] == three[f * 3 + 2] && back[f * 4 + 3] == 0.0f;
        check(round_trip, "storing into wider frames zero-fills the extra channels");
        const std::vector<float> clamped_short(2, 1.0f);
        AudioBuffer tiny(2, 4);
        tiny.load_interleaved(clamped_short, 2);
        check(tiny.channel(0)[0] == 1.0f && tiny.channel(1)[0] == 1.0f && tiny.channel(0)[1] == 0.0f, "a short input fills only the frames it has");
        check(Kernels::catmull_rom(0.0f, 1.0f, 2.0f, 3.0f, 0.5f) == 1.5f && Kernels::catmull_rom(5.0f, 1.0f, 2.0f, 9.0f, 0.0f) == 1.0f, "Catmull-Rom hits its knots and is exact on a line");
    }

    return failures == 0 ? 0 : 1;
}
