#pragma once

#include <Audio/AudioBuffer.hpp>
#include <Audio/Spatial.hpp>

#include <Foundation/Foundation.hpp>

#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

/// Channel layouts of audio *sources* (files, microphones, network streams) and the maths that moves them onto an output.
/// Outputs are limited to the speaker layouts in Spatial.hpp (at most `max_channels`); sources are not: a 32-channel
/// microphone array or a 64-channel console feed is just a source with `channels == 32/64`, and these helpers decide how
/// it lands on a stereo pair, a 7.1.4 bed, or an ambisonic decoder.
namespace SFT::Audio {

    /// Practical ceiling for a source's channel count (WAVE_FORMAT_EXTENSIBLE, Dante, MADI-over-USB interfaces).
    inline constexpr u32 max_source_channels = 256;

    enum class ChannelKind : u8 {
        /// Independent signals with no spatial meaning (mic inputs, stems, tracks): channel i goes to output i.
        Discrete,
        /// Loudspeaker feeds; each channel has a role (front-left, centre, ...), so a 5.1 file knows how to downmix.
        Speakers,
        /// Ambisonic B-format, ACN order, SN3D normalisation (the AmbiX convention), full-sphere.
        Ambisonic,
    };

    struct ChannelLayoutInfo {
        ChannelKind kind = ChannelKind::Discrete;
        u32 channels = 0;
        /// For `Speakers`: one entry per channel, in the source's channel order.
        SpeakerLayout speakers;
        /// For `Ambisonic`: the order (channels == (order + 1)^2).
        u32 ambisonic_order = 0;

        [[nodiscard]] static ChannelLayoutInfo discrete(u32 channels);
        [[nodiscard]] static ChannelLayoutInfo from_speakers(SpeakerLayout layout);
        [[nodiscard]] static ChannelLayoutInfo ambisonic(u32 order);
        /// The conventional meaning of an unlabelled channel count: 1 mono, 2 stereo, 4 quad, 6 5.1, 8 7.1, 10 5.1.4,
        /// 12 7.1.4, 16 9.1.6; anything else is discrete.
        [[nodiscard]] static ChannelLayoutInfo guess(u32 channels);
        [[nodiscard]] UString describe() const;
    };

    // ---- matrices ----------------------------------------------------------------------------------------------------

    struct MatrixTap {
        u16 destination = 0;
        u16 source = 0;
        f32 gain = 0.0f;
    };

    /// A sparse channel mix: `out[destination] += in[source] * gain` for every tap.
    struct ChannelMatrix {
        u32 inputs = 0;
        u32 outputs = 0;
        std::vector<MatrixTap> taps;

        [[nodiscard]] static ChannelMatrix identity(u32 channels);
        /// Dense row-major (`outputs` rows of `inputs`) to sparse; entries with magnitude below `epsilon` are dropped.
        [[nodiscard]] static ChannelMatrix from_dense(u32 inputs, u32 outputs, std::span<const f32> gains, f32 epsilon = 1e-6f);
        [[nodiscard]] std::vector<f32> dense() const;
        /// `out[:frames] += in * matrix` (accumulates; clear `out` first for a plain mix).
        void apply(const AudioBuffer &in, AudioBuffer &out, u32 frames) const noexcept;
        /// The same as `apply` with the whole matrix scaled by a gain that slides from `from_gain` to `to_gain`.
        void apply_ramped(const AudioBuffer &in, AudioBuffer &out, u32 frames, f32 from_gain, f32 to_gain) const noexcept;
    };

    struct ChannelMixOptions {
        /// Mix the LFE channel into the main channels when the target has no LFE (off by default, as in most players).
        bool fold_lfe = false;
        /// LFE gain when folded or when both layouts have one.
        f32 lfe_gain = 1.0f;
        /// Level of the centre channel in a stereo fold-down (-3 dB, ITU-R BS.775).
        f32 center_downmix = 0.70710678f;
        /// Level of surround channels in a fold-down (-3 dB).
        f32 surround_downmix = 0.70710678f;
        /// Level of height channels folded onto the horizontal plane.
        f32 height_downmix = 0.70710678f;
        /// Scale each output row so its coefficients sum to at most one: no clipping from stacked downmix terms, at the cost
        /// of a quieter fold-down. Off by default (the master limiter catches peaks).
        bool normalize = false;
    };

    /// How a source of layout `from` maps onto the speakers of `to` when it is not being spatialised: matching roles pass
    /// through, missing ones are folded down (ITU-R BS.775 coefficients) or dropped, discrete channels pass through by
    /// index, ambisonic sources are decoded with the max-rE decoder.
    [[nodiscard]] ChannelMatrix make_channel_matrix(const ChannelLayoutInfo &from, const SpeakerLayout &to, const ChannelMixOptions &options = {});

    // ---- ambisonics -----------------------------------------------------------------------------------------------------

    /// Rotates an ambisonic sound field (ACN/SN3D, any order up to 3) by a rotation, so a field recorded in the world can be
    /// heard from a turning head. Built once per rotation and applied to every block; the matrix comes from sampling the
    /// spherical harmonics, so any order and any rotation (not just yaw) work.
    class AmbisonicRotator {
      public:
        explicit AmbisonicRotator(u32 order);
        /// `rotation` maps vectors of the original frame into the rotated frame (for a listener, the inverse of their orientation).
        void set_rotation(const glm::quat &rotation);
        [[nodiscard]] u32 order() const noexcept { return order_; }
        [[nodiscard]] u32 channels() const noexcept { return channels_; }
        /// Dense row-major matrix, `channels x channels`.
        [[nodiscard]] std::span<const f32> matrix() const noexcept { return matrix_; }
        /// `out = M * in` over `frames` frames; `in` and `out` hold at least `channels()` channels and must be distinct.
        void apply(const AudioBuffer &in, AudioBuffer &out, u32 frames) const noexcept;

      private:
        u32 order_;
        u32 channels_;
        std::vector<f32> projection_; // K x channels: the pseudo-inverse factor of the sample matrix
        std::vector<glm::vec3> samples_;
        std::vector<f32> matrix_;
    };

    /// First-order tetrahedral microphone (A-format capsules: front-left-up, front-right-down, back-left-down,
    /// back-right-up, the order Ambeo/Zoom H3-VR/Soundfield-style arrays use) to AmbiX B-format (W, Y, Z, X). This is the
    /// ideal coincident conversion; apply the manufacturer's calibration filters first for the best result.
    [[nodiscard]] ChannelMatrix ambisonic_a_to_b_format();

    // ---- Vorbis / Opus channel order ---------------------------------------------------------------------------------------------

    /// Vorbis and Opus (mapping family 1) store 1-8 channels in their own order (front left, centre, front right, ... LFE last),
    /// which differs from WAVE's. Decoders report the file's order as roles in `ChannelLayoutInfo`, so nothing is shuffled on
    /// the way in; encoders use these to reorder on the way out. Discrete for counts the spec has no layout for.
    [[nodiscard]] ChannelLayoutInfo vorbis_channel_layout(u32 channels);
    /// For each channel of `vorbis_channel_layout(source.channels)`, the index of the `source` channel that supplies it (roles
    /// are matched, side and rear surrounds are treated as equivalent). Identity for anything without roles.
    [[nodiscard]] std::vector<u32> vorbis_channel_permutation(const ChannelLayoutInfo &source);

    // ---- file/device channel orders -------------------------------------------------------------------------------------------

    /// WAVE_FORMAT_EXTENSIBLE speaker mask for a layout (roles the format cannot express are skipped).
    [[nodiscard]] u32 wave_channel_mask(const SpeakerLayout &layout) noexcept;
    /// Layout for a WAVE_FORMAT_EXTENSIBLE mask with `channels` channels; channels beyond the mask's bits are appended as
    /// discrete (mono role) so the count always matches. A zero mask falls back to `ChannelLayoutInfo::guess`.
    [[nodiscard]] ChannelLayoutInfo layout_from_wave_mask(u32 mask, u32 channels);

} // namespace SFT::Audio
