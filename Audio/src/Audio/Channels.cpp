#include <Audio/Channels.hpp>

#include <Audio/Kernels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <numeric>
#include <format>

namespace SFT::Audio {

    // ---- layout info ------------------------------------------------------------------------------------------------

    ChannelLayoutInfo ChannelLayoutInfo::discrete(u32 channels) {
        ChannelLayoutInfo info;
        info.kind = ChannelKind::Discrete;
        info.channels = channels;
        return info;
    }

    ChannelLayoutInfo ChannelLayoutInfo::from_speakers(SpeakerLayout layout) {
        ChannelLayoutInfo info;
        info.kind = ChannelKind::Speakers;
        info.channels = layout.channel_count();
        info.speakers = std::move(layout);
        return info;
    }

    ChannelLayoutInfo ChannelLayoutInfo::ambisonic(u32 order) {
        ChannelLayoutInfo info;
        info.kind = ChannelKind::Ambisonic;
        info.ambisonic_order = std::min(order, max_ambisonic_order);
        info.channels = ambisonic_channels(info.ambisonic_order);
        return info;
    }

    ChannelLayoutInfo ChannelLayoutInfo::guess(u32 channels) {
        switch (channels) {
            case 1: return from_speakers(SpeakerLayout::mono());
            case 2: return from_speakers(SpeakerLayout::stereo());
            case 4: return from_speakers(SpeakerLayout::quad());
            case 6: return from_speakers(SpeakerLayout::surround_5_1());
            case 8: return from_speakers(SpeakerLayout::surround_7_1());
            case 10: return from_speakers(SpeakerLayout::surround_5_1_4());
            case 12: return from_speakers(SpeakerLayout::surround_7_1_4());
            case 16: return from_speakers(SpeakerLayout::surround_9_1_6());
            default: return discrete(channels);
        }
    }

    UString ChannelLayoutInfo::describe() const {
        switch (kind) {
            case ChannelKind::Speakers: return speakers.name.empty() ? UString{std::format("{} speakers", channels)} : speakers.name;
            case ChannelKind::Ambisonic: return std::format("ambisonic order {}", ambisonic_order);
            case ChannelKind::Discrete: break;
        }
        return std::format("{} discrete channels", channels);
    }

    // ---- matrices ---------------------------------------------------------------------------------------------------

    ChannelMatrix ChannelMatrix::identity(u32 channels) {
        ChannelMatrix m;
        m.inputs = m.outputs = channels;
        for (u32 c = 0; c < channels; ++c) {
            m.taps.push_back(MatrixTap{static_cast<u16>(c), static_cast<u16>(c), 1.0f});
        }
        return m;
    }

    ChannelMatrix ChannelMatrix::from_dense(u32 inputs, u32 outputs, std::span<const f32> gains, f32 epsilon) {
        ChannelMatrix m;
        m.inputs = inputs;
        m.outputs = outputs;
        for (u32 o = 0; o < outputs; ++o) {
            for (u32 i = 0; i < inputs; ++i) {
                const usize index = static_cast<usize>(o) * inputs + i;
                if (index < gains.size() && std::fabs(gains[index]) >= epsilon) {
                    m.taps.push_back(MatrixTap{static_cast<u16>(o), static_cast<u16>(i), gains[index]});
                }
            }
        }
        return m;
    }

    std::vector<f32> ChannelMatrix::dense() const {
        std::vector<f32> out(static_cast<usize>(inputs) * outputs, 0.0f);
        for (const MatrixTap &t : taps) {
            out[static_cast<usize>(t.destination) * inputs + t.source] += t.gain;
        }
        return out;
    }

    void ChannelMatrix::apply(const AudioBuffer &in, AudioBuffer &out, u32 frames) const noexcept {
        apply_ramped(in, out, frames, 1.0f, 1.0f);
    }

    void ChannelMatrix::apply_ramped(const AudioBuffer &in, AudioBuffer &out, u32 frames, f32 from_gain, f32 to_gain) const noexcept {
        frames = std::min({frames, in.frames(), out.frames()});
        for (const MatrixTap &t : taps) {
            if (t.destination < out.channels() && t.source < in.channels()) {
                Kernels::add_ramp(out.channel(t.destination).first(frames), in.channel(t.source).first(frames), from_gain * t.gain, to_gain * t.gain);
            }
        }
    }

    namespace {

        bool is_left(ChannelRole r) {
            switch (r) {
                case ChannelRole::FrontLeft: case ChannelRole::FrontLeftWide: case ChannelRole::SideLeft: case ChannelRole::RearLeft:
                case ChannelRole::TopFrontLeft: case ChannelRole::TopMiddleLeft: case ChannelRole::TopRearLeft: case ChannelRole::BottomFrontLeft: return true;
                default: return false;
            }
        }
        bool is_right(ChannelRole r) {
            switch (r) {
                case ChannelRole::FrontRight: case ChannelRole::FrontRightWide: case ChannelRole::SideRight: case ChannelRole::RearRight:
                case ChannelRole::TopFrontRight: case ChannelRole::TopMiddleRight: case ChannelRole::TopRearRight: case ChannelRole::BottomFrontRight: return true;
                default: return false;
            }
        }
        bool is_height(ChannelRole r) {
            switch (r) {
                case ChannelRole::TopFrontLeft: case ChannelRole::TopFrontRight: case ChannelRole::TopMiddleLeft: case ChannelRole::TopMiddleRight:
                case ChannelRole::TopRearLeft: case ChannelRole::TopRearRight: case ChannelRole::TopCenter: case ChannelRole::TopFrontCenter:
                case ChannelRole::TopRearCenter: return true;
                default: return false;
            }
        }
        /// The substitute roles for a source role the target lacks, best first, each with its gain scale (1 = unscaled).
        struct Substitute {
            ChannelRole role;
            f32 scale;
        };
        // Fills `out` with up to four substitutes; returns the count.
        usize substitutes(ChannelRole role, const ChannelMixOptions &o, std::array<Substitute, 4> &out) {
            const f32 half = 0.70710678f;
            using R = ChannelRole;
            switch (role) {
                case R::Center: out = {{{R::FrontLeft, o.center_downmix}, {R::FrontRight, o.center_downmix}}}; return 2;
                case R::Mono: out = {{{R::FrontLeft, half}, {R::FrontRight, half}}}; return 2;
                case R::FrontLeftWide: out = {{{R::FrontLeft, 1.0f}}}; return 1;
                case R::FrontRightWide: out = {{{R::FrontRight, 1.0f}}}; return 1;
                case R::SideLeft: out = {{{R::RearLeft, 1.0f}, {R::FrontLeft, o.surround_downmix}}}; return 2;
                case R::SideRight: out = {{{R::RearRight, 1.0f}, {R::FrontRight, o.surround_downmix}}}; return 2;
                case R::RearLeft: out = {{{R::SideLeft, 1.0f}, {R::FrontLeft, o.surround_downmix}}}; return 2;
                case R::RearRight: out = {{{R::SideRight, 1.0f}, {R::FrontRight, o.surround_downmix}}}; return 2;
                case R::BackCenter:
                    out = {{{R::RearLeft, half}, {R::RearRight, half}, {R::SideLeft, half}, {R::SideRight, half}}};
                    return 4;
                case R::TopFrontLeft: out = {{{R::TopMiddleLeft, 1.0f}, {R::FrontLeft, o.height_downmix}}}; return 2;
                case R::TopFrontRight: out = {{{R::TopMiddleRight, 1.0f}, {R::FrontRight, o.height_downmix}}}; return 2;
                case R::TopRearLeft: out = {{{R::TopMiddleLeft, 1.0f}, {R::RearLeft, o.height_downmix}}}; return 2;
                case R::TopRearRight: out = {{{R::TopMiddleRight, 1.0f}, {R::RearRight, o.height_downmix}}}; return 2;
                case R::TopMiddleLeft: out = {{{R::TopFrontLeft, 1.0f}, {R::FrontLeft, o.height_downmix}}}; return 2;
                case R::TopMiddleRight: out = {{{R::TopFrontRight, 1.0f}, {R::FrontRight, o.height_downmix}}}; return 2;
                case R::TopCenter:
                case R::TopFrontCenter:
                case R::TopRearCenter: out = {{{R::FrontLeft, o.height_downmix * half}, {R::FrontRight, o.height_downmix * half}}}; return 2;
                default: return 0;
            }
        }

        void add_tap(ChannelMatrix &m, u32 destination, u32 source, f32 gain) {
            if (gain != 0.0f) {
                m.taps.push_back(MatrixTap{static_cast<u16>(destination), static_cast<u16>(source), gain});
            }
        }

        ChannelMatrix speakers_to_speakers(const SpeakerLayout &from, const SpeakerLayout &to, const ChannelMixOptions &options) {
            ChannelMatrix m;
            m.inputs = from.channel_count();
            m.outputs = to.channel_count();
            for (u32 in = 0; in < from.channel_count(); ++in) {
                const ChannelRole role = from.speakers[in].role;
                const u32 direct = to.channel_of(role);
                if (is_lfe(role)) {
                    if (direct != ~0u) {
                        add_tap(m, direct, in, options.lfe_gain);
                    } else if (options.fold_lfe) {
                        const u32 l = to.channel_of(ChannelRole::FrontLeft), r = to.channel_of(ChannelRole::FrontRight);
                        if (l != ~0u) add_tap(m, l, in, options.lfe_gain * 0.70710678f);
                        if (r != ~0u) add_tap(m, r, in, options.lfe_gain * 0.70710678f);
                    }
                    continue;
                }
                // Mono content in a layout that has a mono speaker goes there; stereo-like targets use the substitutes below.
                if (direct != ~0u) {
                    add_tap(m, direct, in, 1.0f);
                    continue;
                }
                std::array<Substitute, 4> subs{};
                const usize count = substitutes(role, options, subs);
                bool placed = false;
                for (usize k = 0; k < count && !placed; ++k) {
                    const u32 target = to.channel_of(subs[k].role);
                    if (target == ~0u) {
                        continue;
                    }
                    // Roles that split a signal across a pair (Mono, Center, BackCenter) place on every available member.
                    if (role == ChannelRole::Mono || role == ChannelRole::Center || role == ChannelRole::BackCenter || (is_height(role) && !is_left(role) && !is_right(role))) {
                        for (usize j = k; j < count; ++j) {
                            const u32 t = to.channel_of(subs[j].role);
                            if (t != ~0u) {
                                add_tap(m, t, in, subs[j].scale);
                            }
                        }
                        placed = true;
                        break;
                    }
                    add_tap(m, target, in, subs[k].scale);
                    placed = true;
                }
                if (!placed && to.channel_count() > 0) {
                    // Last resort for exotic targets (a lone mono speaker): share the signal equally.
                    add_tap(m, 0, in, 1.0f / std::sqrt(static_cast<f32>(std::max(from.channel_count(), 1u))));
                }
            }
            return m;
        }

    } // namespace

    ChannelMatrix make_channel_matrix(const ChannelLayoutInfo &from, const SpeakerLayout &to, const ChannelMixOptions &options) {
        ChannelMatrix m;
        switch (from.kind) {
            case ChannelKind::Discrete: {
                m.inputs = from.channels;
                m.outputs = to.channel_count();
                for (u32 c = 0; c < from.channels && c < to.channel_count(); ++c) {
                    add_tap(m, c, c, 1.0f);
                }
                break;
            }
            case ChannelKind::Speakers: {
                if (from.speakers.channel_count() == 0) {
                    return make_channel_matrix(ChannelLayoutInfo::discrete(from.channels), to, options);
                }
                m = speakers_to_speakers(from.speakers, to, options);
                break;
            }
            case ChannelKind::Ambisonic: {
                AmbisonicDecoder decoder;
                decoder.build(to, std::max(from.ambisonic_order, 1u));
                const u32 decoder_channels = ambisonic_channels(decoder.order());
                m.inputs = from.channels;
                m.outputs = to.channel_count();
                const auto matrix = decoder.matrix();
                for (u32 s = 0; s < decoder.speaker_count(); ++s) {
                    for (u32 acn = 0; acn < decoder_channels && acn < from.channels; ++acn) {
                        add_tap(m, s, acn, matrix[static_cast<usize>(s) * decoder_channels + acn]);
                    }
                }
                break;
            }
        }
        if (options.normalize) {
            std::vector<f32> row_sum(m.outputs, 0.0f);
            for (const MatrixTap &t : m.taps) {
                row_sum[t.destination] += std::fabs(t.gain);
            }
            for (MatrixTap &t : m.taps) {
                if (row_sum[t.destination] > 1.0f) {
                    t.gain /= row_sum[t.destination];
                }
            }
        }
        return m;
    }

    // ---- ambisonic rotation ---------------------------------------------------------------------------------------------

    namespace {

        /// Solves the symmetric positive-definite system A X = B in place (Gauss-Jordan with partial pivoting); `a` is n x n,
        /// `b` is n x m, both row-major doubles. Returns false when the system is singular.
        bool solve_dense(std::vector<f64> &a, std::vector<f64> &b, u32 n, u32 m) {
            for (u32 col = 0; col < n; ++col) {
                u32 pivot = col;
                for (u32 r = col + 1; r < n; ++r) {
                    if (std::fabs(a[r * n + col]) > std::fabs(a[pivot * n + col])) {
                        pivot = r;
                    }
                }
                if (std::fabs(a[pivot * n + col]) < 1e-12) {
                    return false;
                }
                if (pivot != col) {
                    for (u32 c = 0; c < n; ++c) std::swap(a[pivot * n + c], a[col * n + c]);
                    for (u32 c = 0; c < m; ++c) std::swap(b[pivot * m + c], b[col * m + c]);
                }
                const f64 inv = 1.0 / a[col * n + col];
                for (u32 c = 0; c < n; ++c) a[col * n + c] *= inv;
                for (u32 c = 0; c < m; ++c) b[col * m + c] *= inv;
                for (u32 r = 0; r < n; ++r) {
                    if (r == col) continue;
                    const f64 factor = a[r * n + col];
                    if (factor == 0.0) continue;
                    for (u32 c = 0; c < n; ++c) a[r * n + c] -= factor * a[col * n + c];
                    for (u32 c = 0; c < m; ++c) b[r * m + c] -= factor * b[col * m + c];
                }
            }
            return true;
        }

        /// Well-spread unit vectors (Fibonacci sphere).
        std::vector<glm::vec3> sphere_samples(u32 count) {
            std::vector<glm::vec3> out;
            out.reserve(count);
            const f64 golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
            for (u32 i = 0; i < count; ++i) {
                const f64 y = 1.0 - 2.0 * (static_cast<f64>(i) + 0.5) / static_cast<f64>(count);
                const f64 radius = std::sqrt(std::max(0.0, 1.0 - y * y));
                const f64 theta = golden * static_cast<f64>(i);
                out.emplace_back(static_cast<f32>(std::cos(theta) * radius), static_cast<f32>(y), static_cast<f32>(std::sin(theta) * radius));
            }
            return out;
        }

    } // namespace

    AmbisonicRotator::AmbisonicRotator(u32 order) : order_(std::min(order, max_ambisonic_order)), channels_(ambisonic_channels(order_)) {
        const u32 k = std::max(64u, channels_ * 4);
        samples_ = sphere_samples(k);
        // Y: K x N (rows are the harmonics of each sample direction). projection = Y (Y^T Y)^-1 (K x N).
        std::vector<f64> y(static_cast<usize>(k) * channels_);
        std::array<f32, ambisonic_channels(max_ambisonic_order)> row{};
        for (u32 i = 0; i < k; ++i) {
            encode_ambisonic(samples_[i], order_, row.data());
            for (u32 c = 0; c < channels_; ++c) {
                y[static_cast<usize>(i) * channels_ + c] = row[c];
            }
        }
        // Normal equations: (Y^T Y) P^T = Y^T, so P^T = (Y^T Y)^-1 Y^T (N x K).
        std::vector<f64> gram(static_cast<usize>(channels_) * channels_, 0.0);
        for (u32 a = 0; a < channels_; ++a) {
            for (u32 b = 0; b < channels_; ++b) {
                f64 sum = 0.0;
                for (u32 i = 0; i < k; ++i) {
                    sum += y[static_cast<usize>(i) * channels_ + a] * y[static_cast<usize>(i) * channels_ + b];
                }
                gram[static_cast<usize>(a) * channels_ + b] = sum;
            }
        }
        std::vector<f64> yt(static_cast<usize>(channels_) * k);
        for (u32 c = 0; c < channels_; ++c) {
            for (u32 i = 0; i < k; ++i) {
                yt[static_cast<usize>(c) * k + i] = y[static_cast<usize>(i) * channels_ + c];
            }
        }
        solve_dense(gram, yt, channels_, k); // yt becomes P^T (N x K)
        projection_.assign(static_cast<usize>(k) * channels_, 0.0f);
        for (u32 c = 0; c < channels_; ++c) {
            for (u32 i = 0; i < k; ++i) {
                projection_[static_cast<usize>(i) * channels_ + c] = static_cast<f32>(yt[static_cast<usize>(c) * k + i]);
            }
        }
        matrix_.assign(static_cast<usize>(channels_) * channels_, 0.0f);
        set_rotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    }

    void AmbisonicRotator::set_rotation(const glm::quat &rotation) {
        // M = Y_R^T P with Y_R holding the harmonics of the *rotated* sample directions: Y(R d) = M Y(d) for every d.
        const u32 k = static_cast<u32>(samples_.size());
        std::array<f32, ambisonic_channels(max_ambisonic_order)> row{};
        std::fill(matrix_.begin(), matrix_.end(), 0.0f);
        for (u32 i = 0; i < k; ++i) {
            encode_ambisonic(rotation * samples_[i], order_, row.data());
            for (u32 out = 0; out < channels_; ++out) {
                const f32 rotated = row[out];
                const f32 *p = projection_.data() + static_cast<usize>(i) * channels_;
                f32 *m = matrix_.data() + static_cast<usize>(out) * channels_;
                for (u32 in = 0; in < channels_; ++in) {
                    m[in] += rotated * p[in];
                }
            }
        }
    }

    void AmbisonicRotator::apply(const AudioBuffer &in, AudioBuffer &out, u32 frames) const noexcept {
        frames = std::min({frames, in.frames(), out.frames()});
        const u32 n = std::min({channels_, in.channels(), out.channels()});
        for (u32 o = 0; o < n; ++o) {
            std::ranges::fill(out.channel(o).first(frames), 0.0f);
            for (u32 i = 0; i < n; ++i) {
                const f32 g = matrix_[static_cast<usize>(o) * channels_ + i];
                if (std::fabs(g) > 1e-7f) {
                    Kernels::add_gain(out.channel(o).first(frames), in.channel(i).first(frames), g);
                }
            }
        }
    }

    ChannelMatrix ambisonic_a_to_b_format() {
        // Capsule directions (x forward, y left, z up): FLU (+,+,+), FRD (+,-,-), BLD (-,+,-), BRU (-,-,+), normalised by sqrt(3).
        // B-format (ACN: W, Y, Z, X) from cardioid capsules A_i = 0.5 (w + d_i . v):  W = sum / 2, and v = (3 / 2) sum_i d_i A_i.
        const f32 w = 0.5f;
        const f32 s = 1.5f / std::sqrt(3.0f);
        const std::array<f32, 16> dense{
            // inputs:  FLU    FRD    BLD    BRU
            w,  w,  w,  w,   // W
            s, -s,  s, -s,   // Y (left)
            s, -s, -s,  s,   // Z (up)
            s,  s, -s, -s,   // X (forward)
        };
        return ChannelMatrix::from_dense(4, 4, dense);
    }

    // ---- Vorbis / Opus order ----------------------------------------------------------------------------------------------------

    ChannelLayoutInfo vorbis_channel_layout(u32 channels) {
        using R = ChannelRole;
        std::vector<R> roles;
        switch (channels) {
            case 1: roles = {R::Mono}; break;
            case 2: roles = {R::FrontLeft, R::FrontRight}; break;
            case 3: roles = {R::FrontLeft, R::Center, R::FrontRight}; break;
            case 4: roles = {R::FrontLeft, R::FrontRight, R::RearLeft, R::RearRight}; break;
            case 5: roles = {R::FrontLeft, R::Center, R::FrontRight, R::RearLeft, R::RearRight}; break;
            case 6: roles = {R::FrontLeft, R::Center, R::FrontRight, R::RearLeft, R::RearRight, R::Lfe}; break;
            case 7: roles = {R::FrontLeft, R::Center, R::FrontRight, R::SideLeft, R::SideRight, R::BackCenter, R::Lfe}; break;
            case 8: roles = {R::FrontLeft, R::Center, R::FrontRight, R::SideLeft, R::SideRight, R::RearLeft, R::RearRight, R::Lfe}; break;
            default: return ChannelLayoutInfo::discrete(channels);
        }
        SpeakerLayout layout;
        layout.name = UString{std::format("{} channels (Vorbis order)", channels)};
        for (R role : roles) {
            // Nominal angles; only the roles matter for mixing and the order for mapping.
            f32 azimuth = 0.0f;
            switch (role) {
                case R::FrontLeft: azimuth = 30; break;
                case R::FrontRight: azimuth = -30; break;
                case R::SideLeft: azimuth = 90; break;
                case R::SideRight: azimuth = -90; break;
                case R::RearLeft: azimuth = 135; break;
                case R::RearRight: azimuth = -135; break;
                case R::BackCenter: azimuth = 180; break;
                default: break;
            }
            layout.speakers.push_back(Speaker{role, azimuth, 0.0f});
        }
        return ChannelLayoutInfo::from_speakers(std::move(layout));
    }

    std::vector<u32> vorbis_channel_permutation(const ChannelLayoutInfo &source) {
        std::vector<u32> identity(source.channels);
        std::iota(identity.begin(), identity.end(), u32{0});
        if (source.kind != ChannelKind::Speakers || source.speakers.channel_count() != source.channels) {
            return identity;
        }
        const ChannelLayoutInfo target = vorbis_channel_layout(source.channels);
        if (target.kind != ChannelKind::Speakers) {
            return identity;
        }
        const auto equivalent = [](ChannelRole a, ChannelRole b) {
            const auto group = [](ChannelRole r) {
                switch (r) {
                    case ChannelRole::SideLeft: case ChannelRole::RearLeft: return 1;
                    case ChannelRole::SideRight: case ChannelRole::RearRight: return 2;
                    default: return 0;
                }
            };
            return group(a) != 0 && group(a) == group(b);
        };
        std::vector<u32> result(source.channels, ~0u);
        std::vector<bool> used(source.channels, false);
        // Exact roles first, then the side/rear equivalents, then whatever is left in order.
        for (int pass = 0; pass < 2; ++pass) {
            for (u32 slot = 0; slot < source.channels; ++slot) {
                if (result[slot] != ~0u) continue;
                for (u32 c = 0; c < source.channels; ++c) {
                    const ChannelRole have = source.speakers.speakers[c].role;
                    const ChannelRole want = target.speakers.speakers[slot].role;
                    if (!used[c] && (pass == 0 ? have == want : equivalent(have, want))) {
                        result[slot] = c;
                        used[c] = true;
                        break;
                    }
                }
            }
        }
        u32 next = 0;
        for (u32 slot = 0; slot < source.channels; ++slot) {
            if (result[slot] != ~0u) continue;
            while (next < source.channels && used[next]) ++next;
            result[slot] = next < source.channels ? next : slot;
            if (next < source.channels) used[next] = true;
        }
        return result;
    }

    // ---- WAVE channel masks ----------------------------------------------------------------------------------------------

    namespace {

        struct MaskBit {
            u32 bit;
            ChannelRole role;
            f32 azimuth;
            f32 elevation;
        };
        // Bits in speaker-position order, which is also the channel order WAVE_FORMAT_EXTENSIBLE requires.
        constexpr std::array<MaskBit, 18> kMaskBits{{
            {0x1, ChannelRole::FrontLeft, 30, 0},       {0x2, ChannelRole::FrontRight, -30, 0},
            {0x4, ChannelRole::Center, 0, 0},           {0x8, ChannelRole::Lfe, 0, 0},
            {0x10, ChannelRole::RearLeft, 150, 0},      {0x20, ChannelRole::RearRight, -150, 0},
            {0x40, ChannelRole::FrontLeftWide, 15, 0},  {0x80, ChannelRole::FrontRightWide, -15, 0},
            {0x100, ChannelRole::BackCenter, 180, 0},   {0x200, ChannelRole::SideLeft, 90, 0},
            {0x400, ChannelRole::SideRight, -90, 0},    {0x800, ChannelRole::TopCenter, 0, 90},
            {0x1000, ChannelRole::TopFrontLeft, 45, 45}, {0x2000, ChannelRole::TopFrontCenter, 0, 45},
            {0x4000, ChannelRole::TopFrontRight, -45, 45}, {0x8000, ChannelRole::TopRearLeft, 135, 45},
            {0x10000, ChannelRole::TopRearCenter, 180, 45}, {0x20000, ChannelRole::TopRearRight, -135, 45},
        }};

    } // namespace

    u32 wave_channel_mask(const SpeakerLayout &layout) noexcept {
        u32 mask = 0;
        for (const Speaker &s : layout.speakers) {
            for (const MaskBit &bit : kMaskBits) {
                if (bit.role == s.role) {
                    mask |= bit.bit;
                }
            }
        }
        return mask;
    }

    namespace {

        // The speaker masks WAVE, FLAC and most players assume for an unlabelled file of a given channel count.
        u32 conventional_wave_mask(u32 channels) noexcept {
            switch (channels) {
                case 3: return 0x7;      // FL FR FC
                case 4: return 0x33;     // FL FR BL BR
                case 5: return 0x37;     // FL FR FC BL BR
                case 6: return 0x3F;     // 5.1
                case 7: return 0x70F;    // FL FR FC LFE BC SL SR
                case 8: return 0x63F;    // 7.1
                case 10: return 0x3F | 0x1000 | 0x4000 | 0x8000 | 0x20000;          // 5.1.4
                case 12: return 0x63F | 0x1000 | 0x4000 | 0x8000 | 0x20000;         // 7.1.4
                default: return 0;
            }
        }

    } // namespace

    ChannelLayoutInfo layout_from_wave_mask(u32 mask, u32 channels) {
        if (mask == 0) {
            mask = conventional_wave_mask(channels);
        }
        if (mask == 0) {
            return ChannelLayoutInfo::guess(channels);
        }
        SpeakerLayout layout;
        layout.name = UString{std::format("{} channels", channels)};
        for (const MaskBit &bit : kMaskBits) {
            if ((mask & bit.bit) != 0 && layout.speakers.size() < channels) {
                layout.speakers.push_back(Speaker{bit.role, bit.azimuth, bit.elevation});
            }
        }
        // Mask bits fewer than the channel count: the remainder are unlabelled extras.
        while (layout.speakers.size() < channels) {
            layout.speakers.push_back(Speaker{ChannelRole::Mono, 0.0f, 0.0f});
        }
        if (layout.speakers.size() > channels) {
            layout.speakers.resize(channels);
        }
        return ChannelLayoutInfo::from_speakers(std::move(layout));
    }

} // namespace SFT::Audio
