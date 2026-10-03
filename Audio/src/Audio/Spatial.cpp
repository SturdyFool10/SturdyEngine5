#include <Audio/Spatial.hpp>
#include <Foundation/Iter.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat2x2.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

namespace SFT::Audio {

    // ---- layouts -------------------------------------------------------------------------------------------------

    u32 SpeakerLayout::lfe_channel() const noexcept { return channel_of(ChannelRole::Lfe); }

    u32 SpeakerLayout::channel_of(ChannelRole role) const noexcept {
        const auto index = Foundation::iter(speakers).position(
            [role](const Speaker &speaker) { return speaker.role == role; });
        return index ? static_cast<u32>(*index) : ~0u;
    }

    bool SpeakerLayout::has_height() const noexcept {
        return Foundation::iter(speakers).any(
            [](const Speaker &speaker) { return std::fabs(speaker.elevation_degrees) > 10.0f; });
    }

    namespace {
        using R = ChannelRole;
        SpeakerLayout make(const char *name, std::vector<Speaker> speakers) {
            return SpeakerLayout{name, std::move(speakers)};
        }
    } // namespace

    SpeakerLayout SpeakerLayout::mono() { return make("mono", {{R::Mono, 0, 0}}); }
    SpeakerLayout SpeakerLayout::stereo() { return make("stereo", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}}); }
    SpeakerLayout SpeakerLayout::quad() {
        return make("quad", {{R::FrontLeft, 45, 0}, {R::FrontRight, -45, 0}, {R::RearLeft, 135, 0}, {R::RearRight, -135, 0}});
    }
    SpeakerLayout SpeakerLayout::surround_5_1() {
        return make("5.1", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0},
                            {R::SideLeft, 110, 0}, {R::SideRight, -110, 0}});
    }
    SpeakerLayout SpeakerLayout::surround_7_1() {
        return make("7.1", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0},
                            {R::SideLeft, 90, 0}, {R::SideRight, -90, 0}, {R::RearLeft, 150, 0}, {R::RearRight, -150, 0}});
    }
    SpeakerLayout SpeakerLayout::surround_5_1_4() {
        return make("5.1.4", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0},
                              {R::SideLeft, 110, 0}, {R::SideRight, -110, 0},
                              {R::TopFrontLeft, 45, 45}, {R::TopFrontRight, -45, 45}, {R::TopRearLeft, 135, 45}, {R::TopRearRight, -135, 45}});
    }
    SpeakerLayout SpeakerLayout::surround_7_1_4() {
        return make("7.1.4", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0},
                              {R::SideLeft, 90, 0}, {R::SideRight, -90, 0}, {R::RearLeft, 150, 0}, {R::RearRight, -150, 0},
                              {R::TopFrontLeft, 45, 45}, {R::TopFrontRight, -45, 45}, {R::TopRearLeft, 135, 45}, {R::TopRearRight, -135, 45}});
    }
    SpeakerLayout SpeakerLayout::surround_9_1_6() {
        return make("9.1.6", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0},
                              {R::FrontLeftWide, 60, 0}, {R::FrontRightWide, -60, 0},
                              {R::SideLeft, 90, 0}, {R::SideRight, -90, 0}, {R::RearLeft, 150, 0}, {R::RearRight, -150, 0},
                              {R::TopFrontLeft, 45, 45}, {R::TopFrontRight, -45, 45}, {R::TopMiddleLeft, 90, 45},
                              {R::TopMiddleRight, -90, 45}, {R::TopRearLeft, 135, 45}, {R::TopRearRight, -135, 45}});
    }

    SpeakerLayout SpeakerLayout::surround_3_0() { return make("3.0", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}}); }
    SpeakerLayout SpeakerLayout::surround_5_0() {
        return make("5.0", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::SideLeft, 110, 0}, {R::SideRight, -110, 0}});
    }
    SpeakerLayout SpeakerLayout::surround_6_1() {
        return make("6.1", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0},
                            {R::SideLeft, 90, 0}, {R::SideRight, -90, 0}, {R::BackCenter, 180, 0}});
    }
    SpeakerLayout SpeakerLayout::surround_7_1_2() {
        return make("7.1.2", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0},
                              {R::SideLeft, 90, 0}, {R::SideRight, -90, 0}, {R::RearLeft, 150, 0}, {R::RearRight, -150, 0},
                              {R::TopMiddleLeft, 90, 45}, {R::TopMiddleRight, -90, 45}});
    }
    SpeakerLayout SpeakerLayout::surround_9_1_4() {
        return make("9.1.4", {{R::FrontLeft, 30, 0}, {R::FrontRight, -30, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0},
                              {R::FrontLeftWide, 60, 0}, {R::FrontRightWide, -60, 0},
                              {R::SideLeft, 90, 0}, {R::SideRight, -90, 0}, {R::RearLeft, 150, 0}, {R::RearRight, -150, 0},
                              {R::TopFrontLeft, 45, 45}, {R::TopFrontRight, -45, 45}, {R::TopRearLeft, 135, 45}, {R::TopRearRight, -135, 45}});
    }
    SpeakerLayout SpeakerLayout::surround_22_2() {
        return make("22.2", {// middle layer
                             {R::FrontLeft, 60, 0}, {R::FrontRight, -60, 0}, {R::Center, 0, 0}, {R::Lfe, 0, 0}, {R::RearLeft, 135, 0}, {R::RearRight, -135, 0},
                             {R::FrontLeftWide, 30, 0}, {R::FrontRightWide, -30, 0}, {R::BackCenter, 180, 0}, {R::Lfe2, 0, 0}, {R::SideLeft, 90, 0}, {R::SideRight, -90, 0},
                             // top layer
                             {R::TopFrontCenter, 0, 45}, {R::TopFrontLeft, 45, 45}, {R::TopFrontRight, -45, 45}, {R::TopRearLeft, 135, 45}, {R::TopRearRight, -135, 45},
                             {R::TopMiddleLeft, 90, 45}, {R::TopMiddleRight, -90, 45}, {R::TopRearCenter, 180, 45}, {R::TopCenter, 0, 90},
                             // bottom layer
                             {R::BottomFrontCenter, 0, -25}, {R::BottomFrontLeft, 45, -25}, {R::BottomFrontRight, -45, -25}});
    }

    SpeakerLayout SpeakerLayout::custom(UString name, std::span<const std::pair<f32, f32>> azimuth_elevation, std::span<const u32> lfe_channels) {
        SpeakerLayout layout;
        layout.name = std::move(name);
        bool front_left = false, front_right = false, center = false;
        for (const auto [index, angles] : std::views::zip(std::views::iota(u32{0}), azimuth_elevation)) {
            const auto [azimuth, elevation] = angles;
            Speaker speaker{R::Mono, azimuth, elevation};
            const f32 a = std::fabs(azimuth);
            const bool left = azimuth > 0.0f;
            if (std::ranges::find(lfe_channels, index) != lfe_channels.end()) {
                speaker.role = layout.lfe_channel() == ~0u ? R::Lfe : R::Lfe2;
            } else if (elevation > 20.0f) {
                speaker.role = a < 20.0f ? R::TopFrontCenter : a < 70.0f ? (left ? R::TopFrontLeft : R::TopFrontRight) : a < 120.0f ? (left ? R::TopMiddleLeft : R::TopMiddleRight) : (left ? R::TopRearLeft : R::TopRearRight);
            } else if (elevation < -15.0f) {
                speaker.role = a < 20.0f ? R::BottomFrontCenter : left ? R::BottomFrontLeft : R::BottomFrontRight;
            } else if (a <= 12.0f && !center) {
                speaker.role = R::Center;
                center = true;
            } else if (a > 160.0f) {
                speaker.role = R::BackCenter;
            } else if (a <= 50.0f) {
                bool &taken = left ? front_left : front_right;
                speaker.role = taken ? (left ? R::FrontLeftWide : R::FrontRightWide) : (left ? R::FrontLeft : R::FrontRight);
                taken = true;
            } else if (a <= 120.0f) {
                speaker.role = left ? R::SideLeft : R::SideRight;
            } else {
                speaker.role = left ? R::RearLeft : R::RearRight;
            }
            layout.speakers.push_back(speaker);
        }
        return layout;
    }

    SpeakerLayout SpeakerLayout::from_channel_count(u32 channels) {
        switch (channels) {
            case 1: return mono();
            case 2: return stereo();
            case 3: return surround_3_0();
            case 4: return quad();
            case 5: return surround_5_0();
            case 6: return surround_5_1();
            case 7: return surround_6_1();
            case 8: return surround_7_1();
            case 10: return surround_5_1_4();
            case 12: return surround_7_1_4();
            case 14: return surround_9_1_4();
            case 16: return surround_9_1_6();
            case 24: return surround_22_2();
            default: break;
        }
        // 9 and 11 are 7.1 and 7.1.2 with a front pair added or a bed channel dropped in real installations; an even ring is the
        // honest answer for anything unlisted.
        channels = std::clamp(channels, 1u, max_channels);
        std::vector<std::pair<f32, f32>> angles;
        for (u32 i = 0; i < channels; ++i) {
            angles.emplace_back(180.0f - 360.0f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(channels) + 180.0f, 0.0f);
        }
        // Wrap to (-180, 180] with 0 straight ahead.
        for (auto &[azimuth, elevation] : angles) {
            azimuth = std::remainder(azimuth, 360.0f);
        }
        return custom(UString{std::format("{}-speaker ring", channels)}, angles);
    }

    glm::vec3 direction_from_angles(f32 azimuth_degrees, f32 elevation_degrees) noexcept {
        const f32 az = glm::radians(azimuth_degrees), el = glm::radians(elevation_degrees);
        const f32 ce = std::cos(el);
        return {-std::sin(az) * ce, std::sin(el), -std::cos(az) * ce};
    }

    void angles_from_direction(const glm::vec3 &direction, f32 &azimuth_degrees, f32 &elevation_degrees) noexcept {
        const f32 length = glm::length(direction);
        if (length < 1e-9f) {
            azimuth_degrees = 0.0f;
            elevation_degrees = 0.0f;
            return;
        }
        elevation_degrees = glm::degrees(std::asin(std::clamp(direction.y / length, -1.0f, 1.0f)));
        azimuth_degrees = glm::degrees(std::atan2(-direction.x, -direction.z));
    }

    // ---- VBAP ----------------------------------------------------------------------------------------------------

    void VbapPanner::build(const SpeakerLayout &layout) {
        channel_count_ = layout.channel_count();
        directions_.clear();
        panned_channels_.clear();
        triplets_.clear();
        ring_pairs_.clear();
        for (u32 i = 0; i < channel_count_; ++i) {
            const Speaker &s = layout.speakers[i];
            directions_.push_back(direction_from_angles(s.azimuth_degrees, s.elevation_degrees));
            if (!is_lfe(s.role)) {
                panned_channels_.push_back(i);
            }
        }

        // Triplets: faces of the convex hull of the speaker directions that are not degenerate through the origin.
        if (layout.has_height()) {
            const usize n = panned_channels_.size();
            for (usize i = 0; i < n; ++i) {
                for (usize j = i + 1; j < n; ++j) {
                    for (usize k = j + 1; k < n; ++k) {
                        const glm::vec3 &a = directions_[panned_channels_[i]];
                        const glm::vec3 &b = directions_[panned_channels_[j]];
                        const glm::vec3 &c = directions_[panned_channels_[k]];
                        if (std::fabs(glm::dot(a, glm::cross(b, c))) < 1e-3f) {
                            continue; // the three speakers and the listener are coplanar
                        }
                        glm::vec3 normal = glm::cross(b - a, c - a);
                        if (glm::dot(normal, a) < 0.0f) {
                            normal = -normal;
                        }
                        bool face = true;
                        for (usize m = 0; m < n && face; ++m) {
                            if (m == i || m == j || m == k) {
                                continue;
                            }
                            face = glm::dot(normal, directions_[panned_channels_[m]] - a) <= 1e-5f;
                        }
                        if (!face) {
                            continue;
                        }
                        const glm::mat3 basis(a, b, c); // columns
                        const glm::mat3 inverse = glm::inverse(basis);
                        Triplet t;
                        t.speaker = {panned_channels_[i], panned_channels_[j], panned_channels_[k]};
                        for (int col = 0; col < 3; ++col) {
                            for (int row = 0; row < 3; ++row) {
                                t.inverse[static_cast<usize>(col * 3 + row)] = inverse[col][row];
                            }
                        }
                        triplets_.push_back(t);
                    }
                }
            }
        }

        // Horizontal ring: speakers near the horizon, adjacent pairs by azimuth.
        struct Ring {
            u32 channel;
            f32 azimuth;
        };
        std::vector<Ring> ring;
        for (u32 channel : panned_channels_) {
            const Speaker &s = layout.speakers[channel];
            if (std::fabs(s.elevation_degrees) <= 10.0f) {
                f32 az = std::fmod(s.azimuth_degrees, 360.0f);
                if (az > 180.0f) az -= 360.0f;
                if (az <= -180.0f) az += 360.0f;
                ring.push_back({channel, az});
            }
        }
        std::sort(ring.begin(), ring.end(), [](const Ring &a, const Ring &b) { return a.azimuth < b.azimuth; });
        for (usize i = 0; i < ring.size() && ring.size() >= 2; ++i) {
            const Ring &a = ring[i];
            const Ring &b = ring[(i + 1) % ring.size()];
            f32 gap = b.azimuth - a.azimuth;
            if (gap <= 0.0f) gap += 360.0f;
            if (gap >= 180.0f - 1e-3f || gap < 1e-3f) {
                continue;
            }
            const glm::vec3 &da = directions_[a.channel];
            const glm::vec3 &db = directions_[b.channel];
            glm::vec2 ua(da.x, da.z), ub(db.x, db.z);
            ua /= std::max(glm::length(ua), 1e-6f);
            ub /= std::max(glm::length(ub), 1e-6f);
            const glm::mat2 inverse = glm::inverse(glm::mat2(ua, ub));
            Pair p;
            p.speaker = {a.channel, b.channel};
            p.inverse = {inverse[0][0], inverse[0][1], inverse[1][0], inverse[1][1]}; // column-major
            p.azimuth_a = a.azimuth;
            p.azimuth_b = b.azimuth;
            ring_pairs_.push_back(p);
        }
    }

    void VbapPanner::nearest_speaker(const glm::vec3 &direction, std::array<f32, max_channels> &out) const noexcept {
        f32 best = -2.0f;
        u32 channel = panned_channels_.empty() ? 0 : panned_channels_.front();
        for (u32 c : panned_channels_) {
            const f32 d = glm::dot(direction, directions_[c]);
            if (d > best) {
                best = d;
                channel = c;
            }
        }
        out[channel] = 1.0f;
    }

    void VbapPanner::gains(const glm::vec3 &direction, f32 spread, std::array<f32, max_channels> &out) const noexcept {
        out.fill(0.0f);
        if (panned_channels_.empty() || channel_count_ > max_channels) {
            return;
        }
        glm::vec3 d = direction;
        const f32 length = glm::length(d);
        d = length > 1e-9f ? d / length : glm::vec3(0.0f, 0.0f, -1.0f);

        bool found = false;
        for (const Triplet &t : triplets_) {
            std::array<f32, 3> g{};
            for (int row = 0; row < 3; ++row) {
                g[static_cast<usize>(row)] = t.inverse[static_cast<usize>(0 * 3 + row)] * d.x + t.inverse[static_cast<usize>(1 * 3 + row)] * d.y +
                                             t.inverse[static_cast<usize>(2 * 3 + row)] * d.z;
            }
            if (g[0] >= -1e-4f && g[1] >= -1e-4f && g[2] >= -1e-4f) {
                for (int i = 0; i < 3; ++i) {
                    out[t.speaker[static_cast<usize>(i)]] = std::max(g[static_cast<usize>(i)], 0.0f);
                }
                found = true;
                break;
            }
        }
        if (!found && !ring_pairs_.empty()) {
            glm::vec2 p(d.x, d.z);
            const f32 plane = glm::length(p);
            p = plane > 1e-6f ? p / plane : glm::vec2(0.0f, -1.0f);
            for (const Pair &pair : ring_pairs_) {
                const f32 g0 = pair.inverse[0] * p.x + pair.inverse[2] * p.y;
                const f32 g1 = pair.inverse[1] * p.x + pair.inverse[3] * p.y;
                if (g0 >= -1e-4f && g1 >= -1e-4f) {
                    out[pair.speaker[0]] = std::max(g0, 0.0f);
                    out[pair.speaker[1]] = std::max(g1, 0.0f);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            nearest_speaker(d, out);
        }

        auto normalise = [&]() {
            f32 power = 0.0f;
            for (u32 c : panned_channels_) power += out[c] * out[c];
            if (power > 1e-12f) {
                const f32 inv = 1.0f / std::sqrt(power);
                for (u32 c : panned_channels_) out[c] *= inv;
            }
        };
        normalise();
        if (spread > 0.0f && panned_channels_.size() > 1) {
            const f32 s = std::clamp(spread, 0.0f, 1.0f);
            const f32 even = 1.0f / std::sqrt(static_cast<f32>(panned_channels_.size()));
            for (u32 c : panned_channels_) out[c] = (1.0f - s) * out[c] + s * even;
            normalise();
        }
    }

    // ---- ambisonics ----------------------------------------------------------------------------------------------

    void encode_ambisonic(const glm::vec3 &direction, u32 order, f32 *out) noexcept {
        order = std::min(order, max_ambisonic_order);
        const f32 length = glm::length(direction);
        const glm::vec3 d = length > 1e-9f ? direction / length : glm::vec3(0.0f, 0.0f, -1.0f);
        // AmbiX axes: x forward, y left, z up.
        const f32 x = -d.z, y = -d.x, z = d.y;
        const f32 s3 = std::sqrt(3.0f), s15 = std::sqrt(15.0f), s58 = std::sqrt(5.0f / 8.0f), s38 = std::sqrt(3.0f / 8.0f);
        out[0] = 1.0f;
        if (order >= 1) {
            out[1] = y;
            out[2] = z;
            out[3] = x;
        }
        if (order >= 2) {
            out[4] = s3 * x * y;
            out[5] = s3 * y * z;
            out[6] = 0.5f * (3.0f * z * z - 1.0f);
            out[7] = s3 * x * z;
            out[8] = 0.5f * s3 * (x * x - y * y);
        }
        if (order >= 3) {
            out[9] = s58 * y * (3.0f * x * x - y * y);
            out[10] = s15 * x * y * z;
            out[11] = s38 * y * (5.0f * z * z - 1.0f);
            out[12] = 0.5f * z * (5.0f * z * z - 3.0f);
            out[13] = s38 * x * (5.0f * z * z - 1.0f);
            out[14] = 0.5f * s15 * z * (x * x - y * y);
            out[15] = s58 * x * (x * x - 3.0f * y * y);
        }
    }

    void AmbisonicDecoder::build(const SpeakerLayout &layout, u32 order) {
        order_ = std::min(std::max(order, 1u), max_ambisonic_order);
        speaker_count_ = layout.channel_count();
        const u32 channels = ambisonic_channels(order_);
        matrix_.assign(static_cast<usize>(speaker_count_) * channels, 0.0f);

        // Max-rE order weights keep the decoded image tight without hot spots (Zotter & Frank).
        const f32 x = std::cos(2.406809f / (static_cast<f32>(order_) + 1.51f));
        const std::array<f32, 4> weights{1.0f, x, 0.5f * (3.0f * x * x - 1.0f), 0.5f * (5.0f * x * x * x - 3.0f * x)};

        std::array<f32, max_channels> harmonics{};
        for (u32 s = 0; s < speaker_count_; ++s) {
            if (is_lfe(layout.speakers[s].role)) {
                continue;
            }
            encode_ambisonic(direction_from_angles(layout.speakers[s].azimuth_degrees, layout.speakers[s].elevation_degrees), order_, harmonics.data());
            for (u32 acn = 0; acn < channels; ++acn) {
                const u32 degree = static_cast<u32>(std::floor(std::sqrt(static_cast<f32>(acn))));
                // (2l+1) turns the SN3D coefficients into the sampling decoder's N3D basis.
                matrix_[static_cast<usize>(s) * channels + acn] = weights[degree] * harmonics[acn] * static_cast<f32>(2 * degree + 1);
            }
        }

        // Normalise so a diffuse field keeps its level whatever the layout: average speaker power over test directions.
        std::vector<glm::vec3> tests;
        for (int xi = -1; xi <= 1; ++xi) {
            for (int yi = -1; yi <= 1; ++yi) {
                for (int zi = -1; zi <= 1; ++zi) {
                    if (xi != 0 || yi != 0 || zi != 0) {
                        tests.push_back(glm::normalize(glm::vec3(static_cast<f32>(xi), static_cast<f32>(yi), static_cast<f32>(zi))));
                    }
                }
            }
        }
        f64 total = 0.0;
        for (const glm::vec3 &d : tests) {
            encode_ambisonic(d, order_, harmonics.data());
            for (u32 s = 0; s < speaker_count_; ++s) {
                f32 g = 0.0f;
                for (u32 acn = 0; acn < channels; ++acn) {
                    g += matrix_[static_cast<usize>(s) * channels + acn] * harmonics[acn];
                }
                total += static_cast<f64>(g) * g;
            }
        }
        const f32 mean_power = static_cast<f32>(total / static_cast<f64>(tests.size()));
        if (mean_power > 1e-9f) {
            const f32 scale = 1.0f / std::sqrt(mean_power);
            for (f32 &v : matrix_) v *= scale;
        }
    }

    void AmbisonicDecoder::decode(const AudioBuffer &bed, AudioBuffer &out) const noexcept {
        const u32 channels = ambisonic_channels(order_);
        const u32 frames = std::min(bed.frames(), out.frames());
        for (u32 s = 0; s < std::min(speaker_count_, out.channels()); ++s) {
            f32 *dst = out.data(s);
            for (u32 acn = 0; acn < std::min(channels, bed.channels()); ++acn) {
                const f32 gain = matrix_[static_cast<usize>(s) * channels + acn];
                if (gain == 0.0f) {
                    continue;
                }
                const f32 *src = bed.data(acn);
                for (u32 i = 0; i < frames; ++i) {
                    dst[i] += src[i] * gain;
                }
            }
        }
    }

    // ---- distance, doppler, air ----------------------------------------------------------------------------------

    f32 distance_gain(const DistanceModel &model, f32 distance) noexcept {
        const f32 min_d = std::max(model.min_distance, 1e-4f);
        const f32 max_d = std::max(model.max_distance, min_d + 1e-3f);
        if (distance <= min_d) {
            return 1.0f;
        }
        const f32 d = std::min(distance, max_d);
        switch (model.rolloff) {
            case Rolloff::Inverse: return min_d / (min_d + model.rolloff_factor * (d - min_d));
            case Rolloff::Linear: return std::clamp(1.0f - model.rolloff_factor * (d - min_d) / (max_d - min_d), 0.0f, 1.0f);
            case Rolloff::Exponential: return std::pow(d / min_d, -model.rolloff_factor);
            case Rolloff::Logarithmic:
                return std::clamp(1.0f - model.rolloff_factor * std::log(d / min_d) / std::log(max_d / min_d), 0.0f, 1.0f);
        }
        return 1.0f;
    }

    f32 doppler_ratio(const glm::vec3 &source_position, const glm::vec3 &source_velocity, const glm::vec3 &listener_position,
                      const glm::vec3 &listener_velocity, f32 speed_of_sound, f32 factor) noexcept {
        const glm::vec3 to_source = source_position - listener_position;
        const f32 distance = glm::length(to_source);
        if (distance < 1e-4f || factor <= 0.0f) {
            return 1.0f;
        }
        const glm::vec3 u = to_source / distance;
        const f32 limit = speed_of_sound * 0.9f;
        const f32 listener_along = std::clamp(glm::dot(listener_velocity, u) * factor, -limit, limit);
        const f32 source_along = std::clamp(glm::dot(source_velocity, u) * factor, -limit, limit);
        return std::clamp((speed_of_sound + listener_along) / (speed_of_sound + source_along), 0.5f, 2.0f);
    }

    f32 air_absorption_cutoff(f32 distance, f32 humidity_factor) noexcept {
        return std::clamp(20000.0f * std::exp(-std::max(distance, 0.0f) * 0.012f * std::max(humidity_factor, 0.0f)), 1500.0f, 20000.0f);
    }

    // ---- binaural --------------------------------------------------------------------------------------------------

    namespace {

        class SphericalHeadFilter final : public BinauralFilter {
          public:
            SphericalHeadFilter(f32 sample_rate, f32 head_radius) : sample_rate_(sample_rate), head_radius_(head_radius) {
                const u32 max_delay = static_cast<u32>(sample_rate * 0.002f) + 8;
                left_.resize(max_delay);
                right_.resize(max_delay);
            }

            void process(const f32 *mono, f32 *left, f32 *right, u32 frames, const glm::vec3 &direction, f32) override {
                const f32 length = glm::length(direction);
                const glm::vec3 d = length > 1e-9f ? direction / length : glm::vec3(0.0f, 0.0f, -1.0f);
                // Lateral angle: positive when the source is on the listener's left.
                const f32 lateral = std::asin(std::clamp(-d.x, -1.0f, 1.0f));
                const f32 itd_seconds = (head_radius_ / 343.0f) * (std::fabs(lateral) + std::sin(std::fabs(lateral)));
                const f32 far_delay = 1.0f + itd_seconds * sample_rate_;
                const f32 target_left_delay = lateral >= 0.0f ? 1.0f : far_delay;
                const f32 target_right_delay = lateral >= 0.0f ? far_delay : 1.0f;

                // Head shadow darkens the far ear; sources behind lose some top end at both (pinna).
                const f32 shadow = std::fabs(std::sin(lateral));
                const f32 behind = std::max(0.0f, d.z);
                const auto coefficient = [this](f32 cutoff) { return 1.0f - std::exp(-glm::two_pi<f32>() * cutoff / sample_rate_); };
                const f32 rear_cutoff = 20000.0f + (6000.0f - 20000.0f) * behind;
                const f32 far_cutoff = std::min(rear_cutoff, 20000.0f + (1800.0f - 20000.0f) * shadow);
                const f32 target_left_coeff = coefficient(lateral >= 0.0f ? rear_cutoff : far_cutoff);
                const f32 target_right_coeff = coefficient(lateral >= 0.0f ? far_cutoff : rear_cutoff);

                if (!initialised_) {
                    // The first block starts at its target so a new voice does not sweep in from a centred position.
                    left_delay_ = target_left_delay;
                    right_delay_ = target_right_delay;
                    left_coeff_ = target_left_coeff;
                    right_coeff_ = target_right_coeff;
                    initialised_ = true;
                }
                for (u32 n = 0; n < frames; ++n) {
                    const f32 t = frames > 1 ? static_cast<f32>(n + 1) / static_cast<f32>(frames) : 1.0f;
                    const f32 dl = left_delay_ + (target_left_delay - left_delay_) * t;
                    const f32 dr = right_delay_ + (target_right_delay - right_delay_) * t;
                    const f32 cl = left_coeff_ + (target_left_coeff - left_coeff_) * t;
                    const f32 cr = right_coeff_ + (target_right_coeff - right_coeff_) * t;
                    left_.write(mono[n]);
                    right_.write(mono[n]);
                    left_state_ += (left_.read(dl) - left_state_) * cl;
                    right_state_ += (right_.read(dr) - right_state_) * cr;
                    left[n] += left_state_;
                    right[n] += right_state_;
                }
                left_delay_ = target_left_delay;
                right_delay_ = target_right_delay;
                left_coeff_ = target_left_coeff;
                right_coeff_ = target_right_coeff;
            }

          private:
            f32 sample_rate_;
            f32 head_radius_;
            DelayLine left_, right_;
            f32 left_delay_ = 1.0f, right_delay_ = 1.0f;
            f32 left_coeff_ = 1.0f, right_coeff_ = 1.0f;
            f32 left_state_ = 0.0f, right_state_ = 0.0f;
            bool initialised_ = false;
        };

    } // namespace

    std::unique_ptr<BinauralFilter> make_spherical_head_filter(f32 sample_rate, f32 head_radius_meters) {
        return std::make_unique<SphericalHeadFilter>(sample_rate, head_radius_meters);
    }

} // namespace SFT::Audio
