#include <Audio/Waveform.hpp>
#include <Audio/Interleaved.hpp>
#include <Audio/Text.hpp>

#include <Audio/Decoder.hpp>
#include <Audio/Fft.hpp>

#include <Foundation/FileIo.hpp>

#include <algorithm>
#include <span>
#include <ranges>
#include <numeric>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace SFT::Audio {

    // ================================================================================================================
    // PeakPyramid
    // ================================================================================================================

    PeakPyramid::PeakPyramid(u32 channels, u32 sample_rate, u32 base_frames_per_bucket)
        : channels_(std::max(channels, 1u)), sample_rate_(sample_rate), base_(std::max(base_frames_per_bucket, 16u)) {
        partial_min_.assign(channels_, std::numeric_limits<f32>::max());
        partial_max_.assign(channels_, std::numeric_limits<f32>::lowest());
        partial_square_.assign(channels_, 0.0f);
    }

    void PeakPyramid::push_bucket(u32 level, const f32 *mins, const f32 *maxs, const f32 *squares) {
        if (levels_.size() <= level) {
            levels_.resize(level + 1);
            pending_.resize(level + 1);
            levels_[level].frames_per_bucket = static_cast<u64>(base_) << level;
        }
        Level &l = levels_[level];
        l.min.insert(l.min.end(), mins, mins + channels_);
        l.max.insert(l.max.end(), maxs, maxs + channels_);
        l.mean_square.insert(l.mean_square.end(), squares, squares + channels_);
        std::vector<f32> &waiting = pending_[level];
        if (waiting.empty()) {
            waiting.resize(static_cast<usize>(channels_) * 3);
            std::copy_n(mins, channels_, waiting.begin());
            std::copy_n(maxs, channels_, waiting.begin() + channels_);
            std::copy_n(squares, channels_, waiting.begin() + 2 * channels_);
            return;
        }
        // A sibling is waiting: together they form one bucket one level up.
        std::vector<f32> merged(static_cast<usize>(channels_) * 3);
        for (u32 c = 0; c < channels_; ++c) {
            merged[c] = std::min(waiting[c], mins[c]);
            merged[channels_ + c] = std::max(waiting[channels_ + c], maxs[c]);
            merged[2 * channels_ + c] = 0.5f * (waiting[2 * channels_ + c] + squares[c]);
        }
        waiting.clear();
        push_bucket(level + 1, merged.data(), merged.data() + channels_, merged.data() + 2 * channels_);
    }

    void PeakPyramid::flush_partial() {
        for (f32 &square : partial_square_) square /= static_cast<f32>(partial_frames_);
        push_bucket(0, partial_min_.data(), partial_max_.data(), partial_square_.data());
        std::ranges::fill(partial_min_, std::numeric_limits<f32>::max());
        std::ranges::fill(partial_max_, std::numeric_limits<f32>::lowest());
        std::ranges::fill(partial_square_, 0.0f);
        partial_frames_ = 0;
    }

    void PeakPyramid::append(const f32 *interleaved, usize frames) {
        const u32 c = channels_;
        for (usize i = 0; i < frames; ++i) {
            for (u32 k = 0; k < c; ++k) {
                const f32 v = interleaved[i * c + k];
                partial_min_[k] = std::min(partial_min_[k], v);
                partial_max_[k] = std::max(partial_max_[k], v);
                partial_square_[k] += v * v;
            }
            if (++partial_frames_ == base_) {
                flush_partial();
            }
        }
        total_frames_ += frames;
    }

    void PeakPyramid::finish() {
        if (partial_frames_ > 0) {
            flush_partial();
        }
        // Unpaired buckets at the top of each level are carried upward so every level covers the whole audio.
        for (u32 level = 0; level < pending_.size(); ++level) {
            if (pending_[level].empty()) {
                continue;
            }
            std::vector<f32> lone = std::move(pending_[level]);
            pending_[level].clear();
            const bool top = level + 1 >= pending_.size();
            if (top && levels_[level].buckets(channels_) <= 1) {
                continue; // a single bucket: nothing coarser to build
            }
            push_bucket(level + 1, lone.data(), lone.data() + channels_, lone.data() + 2 * channels_);
        }
    }

    PeakPyramid PeakPyramid::from_buffer(const SampleBuffer &buffer, u32 base) {
        PeakPyramid pyramid(std::max(buffer.channels, 1u), buffer.sample_rate, base);
        if (buffer.samples) {
            pyramid.append(buffer.samples->data(), static_cast<usize>(buffer.frames()));
        }
        pyramid.finish();
        return pyramid;
    }

    std::vector<PeakColumn> PeakPyramid::columns(u64 start, u64 end, u32 columns, u32 channel) const {
        std::vector<PeakColumn> out(columns);
        end = std::min(end, total_frames_);
        if (columns == 0 || levels_.empty() || start >= end) {
            return out;
        }
        const u64 span = end - start;
        for (u32 i = 0; i < columns; ++i) {
            const u64 s = start + span * i / columns;
            u64 e = start + span * (static_cast<u64>(i) + 1) / columns;
            if (e <= s) e = s + 1;
            // The coarsest level whose buckets still fit inside one column.
            usize level = 0;
            for (usize l = 0; l < levels_.size(); ++l) {
                if (levels_[l].frames_per_bucket <= e - s) level = l;
            }
            const Level &lv = levels_[level];
            const u64 b0 = s / lv.frames_per_bucket;
            const u64 b1 = std::min<u64>((e + lv.frames_per_bucket - 1) / lv.frames_per_bucket, lv.buckets(channels_));
            f32 mn = std::numeric_limits<f32>::max(), mx = std::numeric_limits<f32>::lowest(), sq = 0.0f;
            u32 count = 0;
            for (u64 b = b0; b < b1; ++b) {
                for (u32 c = 0; c < channels_; ++c) {
                    if (channel != ~0u && c != channel) continue;
                    mn = std::min(mn, lv.min[b * channels_ + c]);
                    mx = std::max(mx, lv.max[b * channels_ + c]);
                    sq += lv.mean_square[b * channels_ + c];
                    ++count;
                }
            }
            if (count > 0) out[i] = PeakColumn{mn, mx, std::sqrt(sq / static_cast<f32>(count))};
        }
        return out;
    }

    PeakColumn PeakPyramid::range(u64 start, u64 end, u32 channel) const {
        const auto one = columns(start, end, 1, channel);
        return one.empty() ? PeakColumn{} : one.front();
    }

    // ---- persistence ----------------------------------------------------------------------------------------------------------

    namespace {

        struct FileStamp {
            u64 size = 0;
            i64 modified = 0;
        };

        FileStamp stamp_of(const std::filesystem::path &path) {
            FileStamp stamp;
            std::error_code ec;
            if (path.empty()) return stamp;
            stamp.size = std::filesystem::file_size(path, ec);
            if (ec) stamp.size = 0;
            const auto time = std::filesystem::last_write_time(path, ec);
            if (!ec) stamp.modified = static_cast<i64>(time.time_since_epoch().count());
            return stamp;
        }

        template <typename T>
        void put(std::vector<std::byte> &out, T value) {
            const auto *p = reinterpret_cast<const std::byte *>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }
        template <typename T>
        bool get(const std::byte *&p, const std::byte *end, T &value) {
            if (static_cast<usize>(end - p) < sizeof(T)) return false;
            std::memcpy(&value, p, sizeof(T));
            p += sizeof(T);
            return true;
        }

    } // namespace

    std::expected<void, UString> PeakPyramid::save(const std::filesystem::path &path, const std::filesystem::path &source) const {
        std::vector<std::byte> bytes;
        const char magic[4] = {'S', 'P', 'K', '1'};
        bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(magic), reinterpret_cast<const std::byte *>(magic) + 4);
        const FileStamp stamp = stamp_of(source);
        put<u32>(bytes, channels_);
        put<u32>(bytes, sample_rate_);
        put<u32>(bytes, base_);
        put<u64>(bytes, total_frames_);
        put<u64>(bytes, stamp.size);
        put<i64>(bytes, stamp.modified);
        put<u32>(bytes, static_cast<u32>(levels_.size()));
        for (const Level &l : levels_) {
            put<u64>(bytes, l.min.size());
            for (const auto *array : {&l.min, &l.max, &l.mean_square}) {
                const auto *p = reinterpret_cast<const std::byte *>(array->data());
                bytes.insert(bytes.end(), p, p + array->size() * sizeof(f32));
            }
        }
        auto writer = Foundation::Io::FileWriter::create(path);
        if (!writer) return std::unexpected(io_error(writer.error()));
        if (!writer->write(bytes)) return std::unexpected(io_error(writer->error()));
        return writer->commit();
    }

    std::expected<PeakPyramid, UString> PeakPyramid::load(const std::filesystem::path &path, const std::filesystem::path &source) {
        auto bytes = Foundation::Io::read_file(path);
        if (!bytes) return std::unexpected(io_error(bytes.error()));
        const std::byte *p = bytes->data();
        const std::byte *end = p + bytes->size();
        if (bytes->size() < 4 || std::memcmp(p, "SPK1", 4) != 0) return std::unexpected("audio: not a peak file");
        p += 4;
        u32 channels = 0, rate = 0, base = 0, level_count = 0;
        u64 frames = 0, size = 0;
        i64 modified = 0;
        if (!get(p, end, channels) || !get(p, end, rate) || !get(p, end, base) || !get(p, end, frames) || !get(p, end, size) || !get(p, end, modified) ||
            !get(p, end, level_count) || channels == 0 || channels > max_source_channels || level_count > 64) {
            return std::unexpected("audio: the peak file is damaged");
        }
        const FileStamp stamp = stamp_of(source);
        if (!source.empty() && (stamp.size != size || stamp.modified != modified)) {
            return std::unexpected("audio: the peak file is out of date");
        }
        PeakPyramid pyramid(channels, rate, base);
        pyramid.total_frames_ = frames;
        pyramid.levels_.resize(level_count);
        pyramid.pending_.resize(level_count);
        for (u32 i = 0; i < level_count; ++i) {
            u64 count = 0;
            if (!get(p, end, count) || static_cast<u64>(end - p) < count * 3 * sizeof(f32)) return std::unexpected("audio: the peak file is truncated");
            Level &l = pyramid.levels_[i];
            l.frames_per_bucket = static_cast<u64>(base) << i;
            for (auto *array : {&l.min, &l.max, &l.mean_square}) {
                array->resize(count);
                std::memcpy(array->data(), p, count * sizeof(f32));
                p += count * sizeof(f32);
            }
        }
        return pyramid;
    }

    std::expected<PeakPyramid, UString> PeakPyramid::from_file(const std::filesystem::path &path, u32 base, const std::function<bool(f32)> &progress,
                                                                    const std::optional<std::filesystem::path> &cache_file) {
        if (cache_file && std::filesystem::exists(*cache_file)) {
            if (auto cached = load(*cache_file, path)) {
                return cached;
            }
        }
        auto decoder = DecoderRegistry::global().open_file(path);
        if (!decoder) return std::unexpected(decoder.error());
        const AudioStreamInfo &info = (*decoder)->info();
        PeakPyramid pyramid(info.channels, info.sample_rate, base);
        constexpr u64 chunk = 16384;
        std::vector<f32> block(static_cast<usize>(chunk) * info.channels);
        u64 done = 0;
        for (;;) {
            const u64 got = (*decoder)->read(block.data(), chunk);
            if (got == 0) break;
            pyramid.append(block.data(), static_cast<usize>(got));
            done += got;
            if (progress && !progress(info.total_frames > 0 ? static_cast<f32>(static_cast<f64>(done) / static_cast<f64>(info.total_frames)) : 0.0f)) {
                return std::unexpected("audio: cancelled");
            }
        }
        pyramid.finish();
        if (cache_file) {
            (void)pyramid.save(*cache_file, path); // a cache that cannot be written is not an error
        }
        return pyramid;
    }

    std::expected<std::shared_ptr<SampleBuffer>, UString> read_frames(const std::filesystem::path &path, u64 start, u64 frames) {
        auto decoder = DecoderRegistry::global().open_file(path);
        if (!decoder) return std::unexpected(decoder.error());
        const AudioStreamInfo &info = (*decoder)->info();
        (*decoder)->seek(start);
        auto samples = std::make_shared<std::vector<f32>>(static_cast<usize>(frames) * info.channels);
        const u64 got = (*decoder)->read(samples->data(), frames);
        samples->resize(static_cast<usize>(got) * info.channels);
        auto buffer = std::make_shared<SampleBuffer>();
        buffer->channels = info.channels;
        buffer->sample_rate = info.sample_rate;
        buffer->layout = info.layout;
        buffer->samples = std::move(samples);
        return buffer;
    }

    // ================================================================================================================
    // Drawing
    // ================================================================================================================

    namespace {

        struct Rgba {
            f32 r, g, b, a;
        };
        Rgba unpack(Color c) { return Rgba{static_cast<f32>((c >> 24) & 0xFF), static_cast<f32>((c >> 16) & 0xFF), static_cast<f32>((c >> 8) & 0xFF), static_cast<f32>(c & 0xFF) / 255.0f}; }

        Image make_image(u32 w, u32 h, Color background) {
            Image img;
            img.width = w;
            img.height = h;
            img.rgba.resize(static_cast<usize>(w) * h * 4);
            const Rgba bg = unpack(background);
            const std::array<u8, 4> pixel{static_cast<u8>(bg.r), static_cast<u8>(bg.g), static_cast<u8>(bg.b), static_cast<u8>(bg.a * 255.0f + 0.5f)};
            for (const auto px : std::views::chunk(img.rgba, 4)) {
                std::ranges::copy(pixel, px.begin());
            }
            return img;
        }

        // Source-over blend of `color` with extra `coverage` onto one pixel (straight alpha).
        void blend(Image &img, i64 x, i64 y, Color color, f32 coverage = 1.0f) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
            const Rgba s = unpack(color);
            const f32 sa = std::clamp(s.a * coverage, 0.0f, 1.0f);
            if (sa <= 0.0f) return;
            u8 *px = img.rgba.data() + (static_cast<usize>(y) * img.width + static_cast<usize>(x)) * 4;
            const f32 da = static_cast<f32>(px[3]) / 255.0f;
            const f32 oa = sa + da * (1.0f - sa);
            const auto mix = [&](f32 sc, u8 dc) { return oa > 0.0f ? (sc * sa + static_cast<f32>(dc) * da * (1.0f - sa)) / oa : 0.0f; };
            px[0] = static_cast<u8>(std::clamp(mix(s.r, px[0]), 0.0f, 255.0f));
            px[1] = static_cast<u8>(std::clamp(mix(s.g, px[1]), 0.0f, 255.0f));
            px[2] = static_cast<u8>(std::clamp(mix(s.b, px[2]), 0.0f, 255.0f));
            px[3] = static_cast<u8>(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        // A vertical span [y0, y1) with fractional ends, anti-aliased top and bottom.
        void fill_column(Image &img, i64 x, f32 y0, f32 y1, Color color, f32 alpha = 1.0f) {
            if (y1 < y0) std::swap(y0, y1);
            const i64 first = static_cast<i64>(std::floor(y0)), last = static_cast<i64>(std::ceil(y1)) - 1;
            for (i64 y = first; y <= last; ++y) {
                const f32 cover = std::min(static_cast<f32>(y + 1), y1) - std::max(static_cast<f32>(y), y0);
                if (cover > 0.0f) blend(img, x, y, color, cover * alpha);
            }
        }

        // Xiaolin Wu's anti-aliased line.
        void draw_line(Image &img, f32 x0, f32 y0, f32 x1, f32 y1, Color color, f32 alpha = 1.0f) {
            const bool steep = std::fabs(y1 - y0) > std::fabs(x1 - x0);
            if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
            if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
            const f32 dx = x1 - x0, dy = y1 - y0;
            const f32 gradient = dx == 0.0f ? 1.0f : dy / dx;
            const auto plot = [&](i64 x, i64 y, f32 c) { if (steep) blend(img, y, x, color, c * alpha); else blend(img, x, y, color, c * alpha); };
            f32 y = y0;
            for (i64 x = static_cast<i64>(std::floor(x0)); x <= static_cast<i64>(std::ceil(x1)); ++x) {
                const f32 fy = y - std::floor(y);
                plot(x, static_cast<i64>(std::floor(y)), 1.0f - fy);
                plot(x, static_cast<i64>(std::floor(y)) + 1, fy);
                y += gradient;
            }
        }

        f32 shape(f32 v, const WaveformStyle &style) {
            v *= style.vertical_zoom;
            if (style.decibel_scale) {
                const f32 magnitude = std::fabs(v);
                const f32 db = magnitude > 1e-6f ? 20.0f * std::log10(magnitude) : -200.0f;
                const f32 scaled = std::clamp((db - style.decibel_floor) / -style.decibel_floor, 0.0f, 1.0f);
                v = v < 0.0f ? -scaled : scaled;
            }
            return std::clamp(v, -1.0f, 1.0f);
        }

        // "Nice" time grid step (seconds) giving roughly one line per 80 pixels.
        f64 grid_step(f64 seconds_per_pixel) {
            const f64 target = seconds_per_pixel * 80.0;
            static constexpr f64 steps[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
            for (f64 s : steps) if (s >= target) return s;
            return 3600.0;
        }

    } // namespace

    Image render_waveform(const PeakPyramid &peaks, u64 start, u64 end, u32 width, u32 height, const WaveformStyle &style, std::span<const AudioMarker> markers,
                          const SampleBuffer *exact) {
        Image img = make_image(width, height, style.background);
        if (width == 0 || height == 0 || end <= start) {
            return img;
        }
        const u64 span = end - start;
        const f64 frames_per_pixel = static_cast<f64>(span) / static_cast<f64>(width);
        const auto x_of = [&](u64 frame) { return (static_cast<f64>(frame) - static_cast<f64>(start)) / frames_per_pixel; };

        if (style.draw_grid && peaks.sample_rate() > 0) {
            const f64 step = grid_step(frames_per_pixel / static_cast<f64>(peaks.sample_rate()));
            const f64 first = std::ceil(static_cast<f64>(start) / peaks.sample_rate() / step) * step;
            for (f64 t = first; t * peaks.sample_rate() < static_cast<f64>(end); t += step) {
                const i64 x = static_cast<i64>(x_of(static_cast<u64>(t * peaks.sample_rate())));
                for (u32 y = 0; y < height; ++y) blend(img, x, y, style.grid);
            }
        }
        if (style.selection_range) {
            const i64 x0 = static_cast<i64>(std::floor(x_of(style.selection_range->first))), x1 = static_cast<i64>(std::ceil(x_of(style.selection_range->second)));
            for (i64 x = std::max<i64>(x0, 0); x < std::min<i64>(x1, width); ++x)
                for (u32 y = 0; y < height; ++y) blend(img, x, y, style.selection);
        }

        const u32 channels = peaks.channels();
        const bool stacked = style.channels == ChannelArrangement::Stacked && channels > 1;
        const u32 lanes = stacked ? channels : 1;
        const u32 gaps = (lanes - 1) * style.lane_gap;
        const f32 lane_height = static_cast<f32>(height > gaps ? height - gaps : height) / static_cast<f32>(lanes);

        for (u32 lane = 0; lane < lanes; ++lane) {
            const f32 top = static_cast<f32>(lane) * (lane_height + static_cast<f32>(style.lane_gap));
            const f32 middle = top + lane_height * 0.5f;
            const f32 half = lane_height * 0.5f;
            if (style.draw_center_line) {
                for (u32 x = 0; x < width; ++x) blend(img, x, static_cast<i64>(middle), style.center_line);
            }
            // Which channels this lane draws.
            std::vector<u32> drawn;
            if (stacked) drawn = {lane};
            else if (style.channels == ChannelArrangement::Overlay) for (u32 c = 0; c < channels; ++c) drawn.push_back(c);
            else drawn = {~0u};
            const f32 alpha = drawn.size() > 1 ? 0.6f : 1.0f;
            for (u32 channel : drawn) {
                const auto y_of = [&](f32 v) { return middle - shape(v, style) * half; };
                if (style.mode == WaveformMode::Bars) {
                    const u32 stride = std::max(1u, style.bar_width + style.bar_gap);
                    const u32 bars = (width + stride - 1) / stride;
                    const auto cols = peaks.columns(start, end, bars, channel);
                    for (u32 b = 0; b < bars; ++b) {
                        const f32 amplitude = std::max(std::fabs(cols[b].min), std::fabs(cols[b].max));
                        const f32 h = std::max(shape(amplitude, style) * half, 1.0f);
                        for (u32 dx = 0; dx < style.bar_width && b * stride + dx < width; ++dx)
                            fill_column(img, b * stride + dx, middle - h, middle + h, style.wave, alpha);
                    }
                    continue;
                }
                const bool zoomed_in = exact != nullptr && style.mode == WaveformMode::Line && frames_per_pixel <= 0.75 && exact->samples && channel != ~0u;
                if (zoomed_in) {
                    const usize c = exact->channels;
                    const auto &samples = *exact->samples;
                    f32 px = 0, py = 0;
                    bool have = false;
                    const u64 last = std::min<u64>(end + 1, exact->frames());
                    for (u64 f = start > 0 ? start - 1 : 0; f < last; ++f) {
                        const f32 x = static_cast<f32>(x_of(f)), y = y_of(samples[f * c + std::min<usize>(channel, c - 1)]);
                        if (have) draw_line(img, px, py, x, y, style.wave, alpha);
                        px = x;
                        py = y;
                        have = true;
                    }
                    continue;
                }
                const auto cols = peaks.columns(start, end, width, channel);
                for (u32 x = 0; x < width; ++x) {
                    f32 y_top = y_of(cols[x].max), y_bottom = y_of(cols[x].min);
                    if (y_bottom - y_top < 1.0f) { // silence still gets a hairline
                        y_top = middle - 0.5f;
                        y_bottom = middle + 0.5f;
                    }
                    fill_column(img, x, y_top, y_bottom, style.wave, alpha);
                    if (style.mode == WaveformMode::EnvelopeRms) {
                        const f32 core = std::min(cols[x].rms, std::max(std::fabs(cols[x].min), std::fabs(cols[x].max)));
                        fill_column(img, x, y_of(core), y_of(-core), style.rms, alpha);
                    }
                    if (style.draw_clipping) {
                        if (cols[x].max >= 0.999f) fill_column(img, x, middle - half, middle - half + 2.0f, style.clipping);
                        if (cols[x].min <= -0.999f) fill_column(img, x, middle + half - 2.0f, middle + half, style.clipping);
                    }
                }
            }
        }

        if (style.draw_markers) {
            for (const AudioMarker &m : markers) {
                if (m.frame < start || m.frame >= end) continue;
                const i64 x = static_cast<i64>(x_of(m.frame));
                for (u32 y = 0; y < height; ++y) blend(img, x, y, style.marker);
            }
        }
        if (style.playhead_frame && *style.playhead_frame >= start && *style.playhead_frame < end) {
            const i64 x = static_cast<i64>(x_of(*style.playhead_frame));
            for (u32 y = 0; y < height; ++y) blend(img, x, y, style.playhead);
        }
        return img;
    }

    Image render_waveform(const SampleBuffer &buffer, u64 start, u64 end, u32 width, u32 height, const WaveformStyle &style) {
        const PeakPyramid pyramid = PeakPyramid::from_buffer(buffer, 64);
        return render_waveform(pyramid, start, end, width, height, style, buffer.markers, &buffer);
    }

    // ================================================================================================================
    // Spectrograms
    // ================================================================================================================

    namespace {

        f32 mel_of(f32 hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); }
        f32 hz_of_mel(f32 mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); }

        f32 row_frequency(const Spectrogram &s, f32 u) {
            switch (s.scale) {
                case FrequencyScale::Linear: return s.min_hz + u * (s.max_hz - s.min_hz);
                case FrequencyScale::Log: return s.min_hz * std::pow(s.max_hz / s.min_hz, u);
                case FrequencyScale::Mel: return hz_of_mel(mel_of(s.min_hz) + u * (mel_of(s.max_hz) - mel_of(s.min_hz)));
            }
            return s.min_hz;
        }

    } // namespace

    f32 Spectrogram::frequency_of_row(u32 row) const { return row_frequency(*this, (static_cast<f32>(row) + 0.5f) / static_cast<f32>(std::max(rows, 1u))); }

    Spectrogram compute_spectrogram(const SampleBuffer &buffer, u64 start, u64 end, const SpectrogramOptions &o) {
        Spectrogram s;
        s.sample_rate = buffer.sample_rate;
        s.scale = o.scale;
        s.start_frame = start;
        s.end_frame = end;
        const u64 total = buffer.frames();
        end = std::min(end, total);
        if (!buffer.samples || buffer.channels == 0 || end <= start) {
            return s;
        }
        u32 n = 64;
        while (n < o.fft_size && n < 65536) n <<= 1;
        const f32 nyquist = static_cast<f32>(buffer.sample_rate) * 0.5f;
        s.min_hz = std::clamp(o.min_hz, 1.0f, nyquist * 0.99f);
        s.max_hz = o.max_hz > 0.0f ? std::min(o.max_hz, nyquist) : nyquist;
        if (s.max_hz <= s.min_hz) s.max_hz = nyquist;
        s.rows = std::max(o.rows, 1u);
        u64 hop = o.hop;
        if (hop == 0) hop = std::max<u64>(1, (end - start) / std::max(o.columns, 1u));
        s.columns = static_cast<u32>(std::max<u64>(1, (end - start + hop - 1) / hop));
        s.values.assign(static_cast<usize>(s.rows) * s.columns, 0.0f);

        // Window, scaled so a full-scale sine reads 0 dB.
        std::vector<f32> window(n);
        f32 window_sum = 0.0f;
        for (u32 i = 0; i < n; ++i) {
            const f32 x = 2.0f * std::numbers::pi_v<f32> * static_cast<f32>(i) / static_cast<f32>(n - 1);
            switch (o.window) {
                case WindowFunction::Hann: window[i] = 0.5f - 0.5f * std::cos(x); break;
                case WindowFunction::Hamming: window[i] = 0.54f - 0.46f * std::cos(x); break;
                case WindowFunction::BlackmanHarris: window[i] = 0.35875f - 0.48829f * std::cos(x) + 0.14128f * std::cos(2 * x) - 0.01168f * std::cos(3 * x); break;
            }
            window_sum += window[i];
        }
        const f32 scale = 2.0f / window_sum;
        const Fft fft(n);
        const auto &data = *buffer.samples;
        const usize channels = buffer.channels;
        const usize pick = std::min<usize>(o.channel, channels - 1);
        // One FFT per column, each independent of the rest: spread them over the workers, each with its own scratch.
        const auto column = [&](u32 col) {
            struct Scratch {
                std::vector<f32> frame, re, im, fft_scratch, magnitude_db;
            };
            thread_local Scratch work;
            work.frame.resize(n);
            work.re.resize(n / 2 + 1);
            work.im.resize(n / 2 + 1);
            work.fft_scratch.resize(fft.real_scratch_size());
            work.magnitude_db.resize(n / 2 + 1);
            const i64 centre = static_cast<i64>(start + static_cast<u64>(col) * hop + hop / 2);
            for (const auto [i, windowed] : std::views::zip(std::views::iota(u32{0}), work.frame)) {
                const i64 f = centre - static_cast<i64>(n / 2) + static_cast<i64>(i);
                f32 v = 0.0f;
                if (f >= 0 && static_cast<u64>(f) < total) {
                    const std::span<const f32> frame_samples = std::span<const f32>{data}.subspan(static_cast<usize>(f) * channels, channels);
                    v = o.channel == ~0u ? std::accumulate(frame_samples.begin(), frame_samples.end(), 0.0f) / static_cast<f32>(channels) : frame_samples[pick];
                }
                windowed = v * window[i];
            }
            fft.forward_real(work.frame.data(), work.re.data(), work.im.data(), work.fft_scratch.data());
            for (const auto [mag_db, re, im] : std::views::zip(work.magnitude_db, work.re, work.im)) {
                const f32 mag = std::sqrt(re * re + im * im) * scale;
                mag_db = 20.0f * std::log10(std::max(mag, 1e-9f));
            }
            const auto &magnitude_db = work.magnitude_db;
            for (u32 row = 0; row < s.rows; ++row) {
                const f32 u0 = static_cast<f32>(row) / static_cast<f32>(s.rows), u1 = static_cast<f32>(row + 1) / static_cast<f32>(s.rows);
                const f32 f0 = row_frequency(s, u0), f1 = row_frequency(s, u1);
                const f32 b0 = f0 * static_cast<f32>(n) / static_cast<f32>(buffer.sample_rate), b1 = f1 * static_cast<f32>(n) / static_cast<f32>(buffer.sample_rate);
                f32 db;
                if (b1 - b0 <= 1.0f) {
                    // Finer than the FFT's resolution: interpolate between the neighbouring bins.
                    const f32 b = 0.5f * (b0 + b1);
                    const u32 lo = std::min<u32>(static_cast<u32>(b), n / 2 - 1);
                    const f32 t = b - static_cast<f32>(lo);
                    db = magnitude_db[lo] * (1.0f - t) + magnitude_db[lo + 1] * t;
                } else {
                    db = -200.0f;
                    for (u32 k = static_cast<u32>(b0); k <= std::min<u32>(static_cast<u32>(b1), n / 2); ++k) db = std::max(db, magnitude_db[k]);
                }
                s.values[static_cast<usize>(row) * s.columns + col] = std::clamp((db - o.db_floor) / std::max(o.db_ceiling - o.db_floor, 1.0f), 0.0f, 1.0f);
            }
        };
        for_each_parallel(std::views::iota(u32{0}, s.columns), column, /*threshold=*/16);
        return s;
    }

    Color colormap_color(ColorMap map, f32 value) {
        using Stop = std::array<u8, 3>;
        static constexpr std::array<Stop, 9> viridis{{{68, 1, 84}, {71, 44, 122}, {59, 81, 139}, {44, 113, 142}, {33, 144, 141}, {39, 173, 129}, {92, 200, 99}, {170, 220, 50}, {253, 231, 37}}};
        static constexpr std::array<Stop, 9> magma{{{0, 0, 4}, {28, 16, 68}, {79, 18, 123}, {129, 37, 129}, {181, 54, 122}, {229, 80, 100}, {251, 135, 97}, {254, 194, 135}, {252, 253, 191}}};
        static constexpr std::array<Stop, 9> inferno{{{0, 0, 4}, {31, 12, 72}, {85, 15, 109}, {136, 34, 106}, {186, 54, 85}, {227, 89, 51}, {249, 140, 10}, {249, 201, 50}, {252, 255, 164}}};
        static constexpr std::array<Stop, 9> heat{{{0, 0, 0}, {64, 0, 0}, {128, 0, 0}, {192, 32, 0}, {255, 64, 0}, {255, 128, 0}, {255, 192, 32}, {255, 240, 128}, {255, 255, 255}}};
        static constexpr std::array<Stop, 9> ice{{{0, 0, 0}, {0, 8, 40}, {0, 24, 90}, {0, 56, 150}, {0, 104, 200}, {0, 168, 230}, {80, 216, 240}, {176, 240, 250}, {255, 255, 255}}};
        value = std::clamp(value, 0.0f, 1.0f);
        if (map == ColorMap::Gray) {
            const u32 g = static_cast<u32>(value * 255.0f + 0.5f);
            return (g << 24) | (g << 16) | (g << 8) | 0xFFu;
        }
        const auto &stops = map == ColorMap::Viridis ? viridis : map == ColorMap::Magma ? magma : map == ColorMap::Inferno ? inferno : map == ColorMap::Heat ? heat : ice;
        const f32 position = value * 8.0f;
        const usize i = std::min<usize>(static_cast<usize>(position), 7);
        const f32 t = position - static_cast<f32>(i);
        const auto lerp = [&](usize k) { return static_cast<u32>(static_cast<f32>(stops[i][k]) * (1.0f - t) + static_cast<f32>(stops[i + 1][k]) * t + 0.5f); };
        return (lerp(0) << 24) | (lerp(1) << 16) | (lerp(2) << 8) | 0xFFu;
    }

    Image render_spectrogram(const Spectrogram &s, u32 width, u32 height, ColorMap map) {
        Image img = make_image(width, height, 0x000000FF);
        if (s.columns == 0 || s.rows == 0) {
            return img;
        }
        const auto paint_row = [&](const auto &indexed_row) {
            const auto [y, pixels] = indexed_row;
            const u32 row = std::min(s.rows - 1, static_cast<u32>((static_cast<u64>(height - 1 - y) * s.rows) / height)); // low frequencies at the bottom
            for (const auto [x, px] : indexed_frames(std::span<u8>{pixels}, 4)) {
                const u32 col = std::min(s.columns - 1, static_cast<u32>((static_cast<u64>(x) * s.columns) / width));
                const Color c = colormap_color(map, s.at(row, col));
                px[0] = static_cast<u8>(c >> 24);
                px[1] = static_cast<u8>(c >> 16);
                px[2] = static_cast<u8>(c >> 8);
                px[3] = 255;
            }
        };
        for_each_parallel(indexed_frames(std::span<u8>{img.rgba}, static_cast<usize>(width) * 4), paint_row, /*threshold=*/256);
        return img;
    }

    // ================================================================================================================
    // Scopes
    // ================================================================================================================

    Image render_oscilloscope(std::span<const f32> samples, u32 width, u32 height, Color trace, Color background, bool trigger) {
        Image img = make_image(width, height, background);
        if (samples.size() < 2 || width < 2 || height < 2) {
            return img;
        }
        usize start = 0;
        if (trigger) {
            for (usize i = 1; i < samples.size() / 2; ++i) {
                if (samples[i - 1] <= 0.0f && samples[i] > 0.0f) {
                    start = i;
                    break;
                }
            }
        }
        const usize available = samples.size() - start;
        const f32 half = static_cast<f32>(height) * 0.5f;
        f32 px = 0.0f, py = half;
        for (u32 x = 0; x < width; ++x) {
            const usize i = start + static_cast<usize>(static_cast<u64>(x) * (available - 1) / (width - 1));
            const f32 y = half - std::clamp(samples[i], -1.0f, 1.0f) * (half - 1.0f);
            if (x > 0) draw_line(img, px, py, static_cast<f32>(x), y, trace);
            px = static_cast<f32>(x);
            py = y;
        }
        return img;
    }

    Image render_goniometer(std::span<const f32> left, std::span<const f32> right, u32 size, Color trace, Color background) {
        Image img = make_image(size, size, background);
        const usize n = std::min(left.size(), right.size());
        const f32 half = static_cast<f32>(size) * 0.5f;
        std::vector<f32> density(static_cast<usize>(size) * size, 0.0f);
        for (usize i = 0; i < n; ++i) {
            const f32 side = (left[i] - right[i]) * 0.70710678f, mid = (left[i] + right[i]) * 0.70710678f;
            const i64 x = static_cast<i64>(half + side * half), y = static_cast<i64>(half - mid * half);
            if (x >= 0 && y >= 0 && x < size && y < size) density[static_cast<usize>(y) * size + static_cast<usize>(x)] += 1.0f;
        }
        for (usize i = 0; i < density.size(); ++i) {
            if (density[i] > 0.0f) blend(img, static_cast<i64>(i % size), static_cast<i64>(i / size), trace, std::min(1.0f, 0.25f + density[i] * 0.25f));
        }
        return img;
    }

    Image render_bars(std::span<const f32> values, u32 width, u32 height, Color bar, Color peak, Color background, u32 gap) {
        Image img = make_image(width, height, background);
        if (values.empty() || width == 0 || height == 0) {
            return img;
        }
        const f32 slot = static_cast<f32>(width) / static_cast<f32>(values.size());
        for (usize i = 0; i < values.size(); ++i) {
            const i64 x0 = static_cast<i64>(std::round(static_cast<f32>(i) * slot)), x1 = static_cast<i64>(std::round(static_cast<f32>(i + 1) * slot)) - static_cast<i64>(gap);
            const f32 h = std::clamp(values[i], 0.0f, 1.0f) * static_cast<f32>(height);
            for (i64 x = x0; x < std::max(x1, x0 + 1); ++x) {
                fill_column(img, x, static_cast<f32>(height) - h, static_cast<f32>(height), bar);
                if (h > 2.0f) fill_column(img, x, static_cast<f32>(height) - h, static_cast<f32>(height) - h + 2.0f, peak);
            }
        }
        return img;
    }

    f32 stereo_correlation(std::span<const f32> left, std::span<const f32> right) {
        const usize n = std::min(left.size(), right.size());
        f64 lr = 0.0, ll = 0.0, rr = 0.0;
        for (usize i = 0; i < n; ++i) {
            lr += static_cast<f64>(left[i]) * right[i];
            ll += static_cast<f64>(left[i]) * left[i];
            rr += static_cast<f64>(right[i]) * right[i];
        }
        const f64 d = std::sqrt(ll * rr);
        return d > 1e-12 ? static_cast<f32>(lr / d) : 0.0f;
    }

    // ================================================================================================================
    // PNG (stored deflate)
    // ================================================================================================================

    namespace {
        u32 crc32_update(u32 crc, const u8 *data, usize n) {
            static const std::array<u32, 256> table = [] {
                std::array<u32, 256> t{};
                for (u32 i = 0; i < 256; ++i) {
                    u32 c = i;
                    for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            for (usize i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return crc;
        }
        void be32(std::vector<u8> &v, u32 x) { v.push_back(static_cast<u8>(x >> 24)); v.push_back(static_cast<u8>(x >> 16)); v.push_back(static_cast<u8>(x >> 8)); v.push_back(static_cast<u8>(x)); }
        void chunk(std::vector<u8> &out, const char (&type)[5], const std::vector<u8> &body) {
            be32(out, static_cast<u32>(body.size()));
            const usize at = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), body.begin(), body.end());
            be32(out, ~crc32_update(0xFFFFFFFFu, out.data() + at, out.size() - at));
        }
    } // namespace

    std::expected<void, UString> write_png(const std::filesystem::path &path, const Image &image) {
        if (image.empty() || image.width == 0 || image.height == 0) {
            return std::unexpected("audio: nothing to write");
        }
        std::vector<u8> raw;
        raw.reserve((static_cast<usize>(image.width) * 4 + 1) * image.height);
        for (u32 y = 0; y < image.height; ++y) {
            raw.push_back(0); // filter: none
            raw.insert(raw.end(), image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(y) * image.width * 4),
                       image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(y + 1) * image.width * 4));
        }
        std::vector<u8> zlib = {0x78, 0x01};
        for (usize at = 0; at < raw.size();) {
            const usize n = std::min<usize>(65535, raw.size() - at);
            const bool last = at + n >= raw.size();
            zlib.push_back(last ? 1 : 0);
            zlib.push_back(static_cast<u8>(n & 0xFF));
            zlib.push_back(static_cast<u8>(n >> 8));
            zlib.push_back(static_cast<u8>(~n & 0xFF));
            zlib.push_back(static_cast<u8>((~n >> 8) & 0xFF));
            zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
        }
        u32 a = 1, b = 0;
        for (u8 byte : raw) {
            a = (a + byte) % 65521u;
            b = (b + a) % 65521u;
        }
        be32(zlib, (b << 16) | a);

        std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::vector<u8> header;
        be32(header, image.width);
        be32(header, image.height);
        header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
        chunk(png, "IHDR", header);
        chunk(png, "IDAT", zlib);
        chunk(png, "IEND", {});
        auto writer = Foundation::Io::FileWriter::create(path);
        if (!writer) return std::unexpected(io_error(writer.error()));
        if (!writer->write(png.data(), png.size())) return std::unexpected(io_error(writer->error()));
        return writer->commit();
    }

} // namespace SFT::Audio
