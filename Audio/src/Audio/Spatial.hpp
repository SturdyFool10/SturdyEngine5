#pragma once

#include <Audio/AudioBuffer.hpp>
#include <Audio/Dsp.hpp>

#include <Foundation/Foundation.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

/// Spatial rendering. Conventions: world and listener space are right-handed with +X right, +Y up and -Z forward
/// (the engine's camera convention). Azimuth is measured from forward, positive toward the listener's LEFT (the
/// convention of ITU/AES loudspeaker layouts); elevation is positive upward.
namespace SFT::Audio {

    // ---- speaker layouts -----------------------------------------------------------------------------------------

    /// What a loudspeaker channel is, so device adapters can map to their own channel order (WASAPI masks, CoreAudio
    /// layouts, ALSA maps, SMPTE vs Dolby order) instead of trusting an index.
    enum class ChannelRole : u8 {
        FrontLeft, FrontRight, Center, Lfe,
        FrontLeftWide, FrontRightWide,
        SideLeft, SideRight, RearLeft, RearRight,
        TopFrontLeft, TopFrontRight, TopMiddleLeft, TopMiddleRight, TopRearLeft, TopRearRight,
        Mono,
        // Centre-line channels some formats carry (WAVE_FORMAT_EXTENSIBLE, 6.1/7.1 layouts); output layouts rarely have them.
        BackCenter, TopCenter, TopFrontCenter, TopRearCenter,
        // Floor-level and second-LFE positions of NHK 22.2.
        BottomFrontCenter, BottomFrontLeft, BottomFrontRight, Lfe2,
        Count
    };

    /// Either low-frequency channel (NHK 22.2 has two).
    [[nodiscard]] constexpr bool is_lfe(ChannelRole role) noexcept { return role == ChannelRole::Lfe || role == ChannelRole::Lfe2; }

    struct Speaker {
        ChannelRole role = ChannelRole::Mono;
        f32 azimuth_degrees = 0.0f;   // positive = left of forward
        f32 elevation_degrees = 0.0f; // positive = up
    };

    struct SpeakerLayout {
        UString name;
        std::vector<Speaker> speakers;

        [[nodiscard]] u32 channel_count() const noexcept { return static_cast<u32>(speakers.size()); }
        /// Index of the (first) LFE channel or ~0u.
        [[nodiscard]] u32 lfe_channel() const noexcept;
        /// True when any speaker is meaningfully above or below the horizon.
        [[nodiscard]] bool has_height() const noexcept;
        [[nodiscard]] u32 channel_of(ChannelRole role) const noexcept;

        static SpeakerLayout mono();
        static SpeakerLayout stereo();
        static SpeakerLayout quad();
        static SpeakerLayout surround_5_1();
        static SpeakerLayout surround_7_1();
        static SpeakerLayout surround_5_1_4();
        /// The Dolby Atmos home-theatre bed (7.1 plus four height speakers).
        static SpeakerLayout surround_7_1_4();
        static SpeakerLayout surround_9_1_6();
        static SpeakerLayout surround_3_0();
        static SpeakerLayout surround_5_0();
        static SpeakerLayout surround_6_1();
        static SpeakerLayout surround_7_1_2();
        static SpeakerLayout surround_9_1_4();
        /// NHK 22.2: three layers (floor, middle, top), two LFE channels, 24 in all.
        static SpeakerLayout surround_22_2();
        /// Any list of positions (azimuth degrees positive to the left, elevation positive up); roles are filled in from the
        /// angles so downmix and format conversion still know front from rear. `lfe_channels` lists indices that carry bass only.
        static SpeakerLayout custom(UString name, std::span<const std::pair<f32, f32>> azimuth_elevation, std::span<const u32> lfe_channels = {});
        /// The conventional layout for a device or file that only reports a channel count: the named presets for the usual
        /// counts (3, 5, 7, 9, 11, 14 included), else an even horizontal ring. Never fails for 1..max_channels.
        static SpeakerLayout from_channel_count(u32 channels);
    };

    /// Unit direction (listener space) of an azimuth/elevation pair, in degrees.
    [[nodiscard]] glm::vec3 direction_from_angles(f32 azimuth_degrees, f32 elevation_degrees) noexcept;
    /// Inverse of `direction_from_angles` for a (not necessarily unit) direction.
    void angles_from_direction(const glm::vec3 &direction, f32 &azimuth_degrees, f32 &elevation_degrees) noexcept;

    // ---- VBAP ----------------------------------------------------------------------------------------------------

    /// Vector-base amplitude panning (Pulkki) onto any layout: speaker triplets for layouts with height, adjacent pairs
    /// for horizontal ones. Directions below the lowest speaker fall back to the horizontal ring.
    class VbapPanner {
      public:
        VbapPanner() = default;
        explicit VbapPanner(const SpeakerLayout &layout) { build(layout); }
        void build(const SpeakerLayout &layout);

        /// Power-normalised gains per layout channel (the LFE channel is always 0). `spread` in [0, 1] blends toward
        /// an even spread over all speakers (a large or close source).
        void gains(const glm::vec3 &direction, f32 spread, std::array<f32, max_channels> &out) const noexcept;

        [[nodiscard]] u32 channel_count() const noexcept { return channel_count_; }

      private:
        struct Triplet {
            std::array<u32, 3> speaker{};
            // Row-major inverse of the transposed speaker matrix.
            std::array<f32, 9> inverse{};
        };
        struct Pair {
            std::array<u32, 2> speaker{};
            std::array<f32, 4> inverse{}; // 2x2 in the horizontal plane (x, z)
            f32 azimuth_a = 0.0f, azimuth_b = 0.0f;
        };

        void nearest_speaker(const glm::vec3 &direction, std::array<f32, max_channels> &out) const noexcept;

        u32 channel_count_ = 0;
        std::vector<glm::vec3> directions_; // per layout channel
        std::vector<u32> panned_channels_;  // non-LFE channels
        std::vector<Triplet> triplets_;
        std::vector<Pair> ring_pairs_;
    };

    // ---- ambisonics ----------------------------------------------------------------------------------------------

    inline constexpr u32 max_ambisonic_order = 3;
    [[nodiscard]] constexpr u32 ambisonic_channels(u32 order) noexcept { return (order + 1) * (order + 1); }

    /// Real spherical-harmonic coefficients (ACN channel order, SN3D normalisation: the AmbiX convention) of a unit
    /// direction in listener space, up to `order` (<= 3). `out` must hold `ambisonic_channels(order)` values.
    void encode_ambisonic(const glm::vec3 &direction, u32 order, f32 *out) noexcept;

    /// Max-rE sampling decoder from an ambisonic bed to a speaker layout, normalised so a diffuse field keeps its level.
    class AmbisonicDecoder {
      public:
        void build(const SpeakerLayout &layout, u32 order);
        /// `out += decode(bed)`; `bed` has `ambisonic_channels(order)` channels, `out` the layout's.
        void decode(const AudioBuffer &bed, AudioBuffer &out) const noexcept;
        [[nodiscard]] u32 order() const noexcept { return order_; }
        [[nodiscard]] u32 speaker_count() const noexcept { return speaker_count_; }
        /// Speaker-major decode matrix: `matrix()[speaker * ambisonic_channels(order()) + acn]`.
        [[nodiscard]] std::span<const f32> matrix() const noexcept { return matrix_; }

      private:
        u32 order_ = 1;
        u32 speaker_count_ = 0;
        std::vector<f32> matrix_; // speaker-major: matrix_[speaker * channels + acn]
    };

    // ---- distance, doppler, air ----------------------------------------------------------------------------------

    enum class Rolloff : u8 { Inverse, Linear, Exponential, Logarithmic };

    struct DistanceModel {
        Rolloff rolloff = Rolloff::Inverse;
        f32 min_distance = 1.0f;   // full volume inside this
        f32 max_distance = 60.0f;  // inaudible (Linear) or no further decay (others) beyond this
        f32 rolloff_factor = 1.0f;
    };

    [[nodiscard]] f32 distance_gain(const DistanceModel &model, f32 distance) noexcept;

    /// Playback-rate multiplier for a moving source/listener: positive when they approach each other.
    [[nodiscard]] f32 doppler_ratio(const glm::vec3 &source_position, const glm::vec3 &source_velocity,
                                    const glm::vec3 &listener_position, const glm::vec3 &listener_velocity,
                                    f32 speed_of_sound = 343.0f, f32 factor = 1.0f) noexcept;

    /// Low-pass cutoff (Hz) a source at `distance` metres picks up from air absorption (ISO 9613 flavour: highs go first).
    [[nodiscard]] f32 air_absorption_cutoff(f32 distance, f32 humidity_factor = 1.0f) noexcept;

    // ---- binaural --------------------------------------------------------------------------------------------------

    /// Renders one mono source to two ears for headphones. Implementations may be a measured HRTF set (SOFA) or the
    /// built-in model; the mixer creates one per voice per binaural output (on the game thread, never while mixing).
    class BinauralFilter {
      public:
        virtual ~BinauralFilter() = default;
        /// `direction` is the unit direction from the listener's head to the source in listener space.
        virtual void process(const f32 *mono, f32 *left, f32 *right, u32 frames, const glm::vec3 &direction, f32 distance) = 0;
    };
    using BinauralFilterFactory = std::function<std::unique_ptr<BinauralFilter>()>;

    /// Spherical-head model: Woodworth interaural time difference, a head-shadow low-pass on the far ear and a
    /// pinna-style darkening for sources behind. No data files, works everywhere; replace with a SOFA-backed filter for
    /// personalised results.
    [[nodiscard]] std::unique_ptr<BinauralFilter> make_spherical_head_filter(f32 sample_rate, f32 head_radius_meters = 0.0875f);

} // namespace SFT::Audio
