#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

/// Acoustic modelling is optional and pluggable. The mixer only ever consumes `AcousticsResult`s; where they come from
/// (nothing at all, ray queries against the engine's BVH, Steam Audio, baked wave fields) is a provider's business, so
/// a game that wants plain distance attenuation pays nothing, and one that wants occlusion, diffraction and room reverb
/// turns a provider on without touching its sound code.
namespace SFT::Audio {

    using AcousticMaterialId = u32;
    inline constexpr AcousticMaterialId no_acoustic_material = 0xFFFFFFFFu;

    /// Frequency bands the acoustic model works in: below ~250 Hz, 250 Hz - 4 kHz, above 4 kHz.
    inline constexpr u32 acoustic_bands = 3;

    struct AcousticMaterial {
        UString name;
        /// Fraction of incident energy absorbed per band (reverb decay).
        std::array<f32, acoustic_bands> absorption{0.1f, 0.1f, 0.1f};
        /// Fraction of amplitude that passes through one surface of this material per band (occlusion).
        std::array<f32, acoustic_bands> transmission{0.1f, 0.05f, 0.02f};
        /// How diffusely the surface reflects: 0 is a mirror (sound bounces at the angle it arrived), 1 scatters evenly like a
        /// rough wall or foliage. Specular bounces make clear echoes; diffuse ones make the room's wash.
        f32 scattering = 0.3f;

        /// Fraction of incident energy reflected in a band: what is neither absorbed nor transmitted.
        [[nodiscard]] f32 reflectivity(u32 band) const noexcept {
            const f32 passed = transmission[band] * transmission[band];
            const f32 r = 1.0f - absorption[band] - passed;
            return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
        }
    };

    class AcousticMaterialTable {
      public:
        AcousticMaterialId add(AcousticMaterial material);
        [[nodiscard]] AcousticMaterialId find(const ustr &name) const noexcept;
        [[nodiscard]] const AcousticMaterial *get(AcousticMaterialId id) const noexcept;
        /// Common building materials (concrete, brick, wood, glass, drywall, carpet, metal, fabric, foliage).
        static AcousticMaterialTable with_defaults();

      private:
        std::vector<AcousticMaterial> materials_;
    };

    // ---- ray scene -------------------------------------------------------------------------------------------------

    struct AudioRayHit {
        f32 distance = 0.0f;
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        AcousticMaterialId material = no_acoustic_material;
    };

    /// What raytraced acoustics needs from the world: closest-hit queries. Adapt the engine's BVH, a physics solver's
    /// raycast, or hardware ray queries by implementing this one method; `TriangleBvh` below works with no GPU at all.
    class AudioRayScene {
      public:
        virtual ~AudioRayScene() = default;
        /// Nearest surface along `direction` (unit) within `max_distance`.
        [[nodiscard]] virtual bool closest_hit(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance, AudioRayHit &hit) const = 0;
        /// Whether anything at all lies along the ray within `max_distance`. Visibility questions ("can the source see the
        /// listener?") need no more than this, which a hierarchy answers far faster than the nearest hit (it stops at the
        /// first triangle it finds). The default asks `closest_hit`; implementations should override it.
        [[nodiscard]] virtual bool any_hit(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance) const {
            AudioRayHit ignored;
            return closest_hit(origin, direction, max_distance, ignored);
        }
    };

    /// The audio geometry's resolution is the size of one voxel, in metres: the scene is divided into a grid of cubes this big
    /// and everything smaller than a cube is merged with its neighbours of the same material and facing into one oriented quad.
    /// A wall tessellated into a thousand triangles becomes a few quads, a rock a rough shell. 0 keeps every triangle; typical
    /// values are 0.05 (furniture detail), 0.25 (rooms with clutter), 1.0 (buildings and big obstacles only).
    inline constexpr f32 kFullResolution = 0.0f;

    enum class BvhSplit : u8 {
        Median,  ///< split at the median centroid: fastest build, fine for audio query counts
        Sah,     ///< binned surface-area heuristic: slower build, faster traversal for large scenes
    };

    struct BvhSettings {
        /// Voxel size in metres (see `kFullResolution`): triangles whose longest edge is below it are merged per voxel,
        /// material and facing into oriented quads. Smaller keeps detail, larger builds and traces faster.
        f32 voxel_size = kFullResolution;
        /// Drops the smallest triangles until at most this many remain (0 = no limit): a hard budget for huge levels.
        u32 max_triangles = 0;
        /// Triangles per leaf (more: shallower tree, more tests per leaf).
        u32 max_leaf_triangles = 4;
        BvhSplit split = BvhSplit::Median;
        u32 sah_bins = 16;
    };

    /// CPU bounding-volume hierarchy over triangles for audio queries. Static geometry in, queries from any thread
    /// afterwards (it is immutable once built).
    class TriangleBvh final : public AudioRayScene {
      public:
        /// Adds a mesh (positions + triangle indices) transformed to world space, all with one acoustic material.
        void add_mesh(std::span<const glm::vec3> positions, std::span<const u32> indices, AcousticMaterialId material,
                      const glm::mat4 &transform = glm::mat4(1.0f));
        /// Builds the hierarchy; call once after adding meshes. Simplifies the geometry first according to `settings()`.
        void build();
        void clear();
        void set_settings(const BvhSettings &settings) noexcept { settings_ = settings; }
        [[nodiscard]] const BvhSettings &settings() const noexcept { return settings_; }
        /// Triangles added minus triangles in the tree (what simplification removed or merged).
        [[nodiscard]] usize triangles_removed() const noexcept { return added_triangles_ - triangles_.size(); }

        [[nodiscard]] usize triangle_count() const noexcept { return triangles_.size(); }
        [[nodiscard]] usize node_count() const noexcept { return nodes_.size(); }
        [[nodiscard]] bool closest_hit(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance, AudioRayHit &hit) const override;
        [[nodiscard]] bool any_hit(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance) const override;

      private:
        /// Traversal shared by both queries: nearer child first, subtrees farther than the best hit skipped, and (for `AnyHit`)
        /// an immediate return on the first intersection.
        template <bool AnyHit>
        [[nodiscard]] bool traverse(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance, AudioRayHit *hit) const;
        /// A triangle laid out for the intersection test: one vertex and the two edges from it, stored in leaf order.
        struct PackedTriangle {
            glm::vec3 a;
            glm::vec3 e1;
            glm::vec3 e2;
            AcousticMaterialId material;
        };
        struct Triangle {
            glm::vec3 a, b, c;
            AcousticMaterialId material;
        };
        struct Node {
            glm::vec3 min{0.0f}, max{0.0f};
            u32 left_or_first = 0; // interior: index of the left child (right = left + 1); leaf: first triangle slot
            u32 count = 0;         // 0 for interior nodes
        };

        void fill_node(u32 index, u32 first, u32 count, u32 depth);
        void simplify();

        BvhSettings settings_;
        usize added_triangles_ = 0;
        std::vector<Triangle> triangles_;
        std::vector<u32> order_; // triangle indices, permuted by the build so leaves are contiguous
        std::vector<PackedTriangle> packed_; // the same triangles in `order_` sequence, ready to intersect
        std::vector<Node> nodes_;
    };

    // ---- provider interface -----------------------------------------------------------------------------------------

    /// The effects raytraced acoustics can apply; any can be switched off globally (settings) or per source (query).
    enum class AcousticEffect : u32 {
        Muffling = 1u << 0,         ///< occlusion: walls and doors dull and quiet a sound, a sealed room nearly silences it
        Directionality = 1u << 1,   ///< the sound arrives from where it last bounced, not from the source's true position
        PropagationDelay = 1u << 2, ///< the sound takes distance / speed-of-sound seconds to arrive
        Reverb = 1u << 3,           ///< the listener's room colours everything (decay time and send level)
        MaterialBounces = 1u << 4,  ///< reflected sound finds its way round obstacles, shaped by the surfaces it hits
        Doppler = 1u << 5,          ///< moving sources and listeners shift pitch
    };
    inline constexpr u32 all_acoustic_effects = 0x3Fu;

    struct AcousticsQuery {
        glm::vec3 source{0.0f};
        glm::vec3 listener{0.0f};
        /// Rough physical size of the source in metres (sets how much of it must be hidden to count as occluded).
        f32 source_radius = 0.25f;
        glm::vec3 source_velocity{0.0f};
        glm::vec3 listener_velocity{0.0f};
        /// Effects (bits of `AcousticEffect`) this source takes part in; clear a bit to exempt it (a UI voice, a narrator).
        u32 enabled_effects = all_acoustic_effects;
        /// Per-source strength of each effect, multiplying the global settings (0 = off, 2 = exaggerated).
        f32 muffling_scale = 1.0f;
        f32 reverb_scale = 1.0f;
        f32 delay_scale = 1.0f;
        f32 doppler_scale = 1.0f;
    };

    struct AcousticsResult {
        /// Broadband attenuation from occlusion/transmission (1 = unobstructed), applied on top of distance gain.
        f32 broadband_gain = 1.0f;
        /// Per-band transmission, for EQ stages that want more than a low-pass.
        std::array<f32, acoustic_bands> band_gain{1.0f, 1.0f, 1.0f};
        /// Low-pass cutoff the occlusion implies (Hz); 20000 = none.
        f32 lowpass_cutoff = 20000.0f;
        /// Seconds the voice is delayed by: the whole propagation time when the provider models it, or just the detour's
        /// excess when pitch Doppler is in use.
        f32 extra_delay_seconds = 0.0f;
        /// Where the sound appears to come from when the direct path is blocked and a detour exists (world direction
        /// from the listener); `has_apparent_direction` false means use the true direction.
        glm::vec3 apparent_direction{0.0f, 0.0f, -1.0f};
        bool has_apparent_direction = false;
        /// Aux-bus reverb send (0..1) and the decay time of the room the listener is in.
        f32 reverb_send = 0.0f;
        f32 reverb_rt60 = 0.0f;
        /// Pitch ratio from the provider's Doppler model (along the path the sound really travels); the mixer uses its own
        /// straight-line estimate when `has_doppler` is false.
        f32 doppler = 1.0f;
        bool has_doppler = false;
        /// Length (metres) of the shortest path sound can take to the listener, and whether the straight line is clear.
        f32 path_length = 0.0f;
        bool line_of_sight = true;
        /// Decay time of the listener's room per band.
        std::array<f32, acoustic_bands> reverb_band_rt60{0.0f, 0.0f, 0.0f};
    };

    struct RoomEstimate {
        /// Mean distance to the surrounding surfaces (metres), 0 when open.
        f32 mean_distance = 0.0f;
        /// Fraction of rays that escaped (1 = open air).
        f32 openness = 1.0f;
        /// Eyring reverberation time from the mean free path and the walls' absorption (mid band).
        f32 rt60 = 0.0f;
        /// The same per band: carpeted rooms lose their highs first, concrete halls ring in all of them.
        std::array<f32, acoustic_bands> rt60_bands{0.0f, 0.0f, 0.0f};
    };

    /// The reverberant character of a space measured ahead of time: a grid of room estimates over a box, baked offline from the level's
    /// geometry (in parallel) and looked up at run time with trilinear interpolation, so the listener's room costs nothing per frame.
    /// Serialises to bytes to ship with the level.
    class AcousticField {
      public:
        struct Settings {
            glm::vec3 min{-50.0f};
            glm::vec3 max{50.0f};
            /// Metres between samples; smaller is finer and costs `room_rays` rays per cell to bake.
            f32 cell_size = 2.0f;
        };

        [[nodiscard]] static std::shared_ptr<const AcousticField> bake(const class RaycastAcoustics &acoustics, const Settings &settings);
        [[nodiscard]] static std::expected<std::shared_ptr<const AcousticField>, UString> deserialize(std::span<const std::byte> bytes);
        [[nodiscard]] std::vector<std::byte> serialize() const;

        /// The room at `position`; false outside the baked box.
        [[nodiscard]] bool lookup(const glm::vec3 &position, RoomEstimate &out) const noexcept;
        [[nodiscard]] const Settings &settings() const noexcept { return settings_; }
        [[nodiscard]] glm::uvec3 dimensions() const noexcept { return dims_; }
        [[nodiscard]] usize cell_count() const noexcept { return cells_.size(); }

      private:
        AcousticField() = default;
        [[nodiscard]] const RoomEstimate &at(u32 x, u32 y, u32 z) const noexcept { return cells_[(static_cast<usize>(z) * dims_.y + y) * dims_.x + x]; }
        Settings settings_;
        glm::uvec3 dims_{0u};
        std::vector<RoomEstimate> cells_;
    };

    /// Decides how the world shapes one source's sound. Runs off the audio thread (a job at 10-60 Hz); results are
    /// pushed to the mixer, which smooths them.
    class AcousticsProvider {
      public:
        virtual ~AcousticsProvider() = default;
        virtual void evaluate(std::span<const AcousticsQuery> queries, std::span<AcousticsResult> results) = 0;
        /// The room the listener was last found in, for providers that survey one (drives a reverb bus).
        [[nodiscard]] virtual bool listener_room(RoomEstimate & /*out*/) const { return false; }
    };

    /// Distance attenuation only: every result is the neutral one.
    class NullAcoustics final : public AcousticsProvider {
      public:
        void evaluate(std::span<const AcousticsQuery> queries, std::span<AcousticsResult> results) override;
    };

    enum class DopplerMode : u8 {
        Off,
        /// Resamples the voice (cheap, exact pitch, scales with `factor`). The propagation delay then only carries the detour's
        /// excess, so the two do not both bend the pitch.
        Pitch,
        /// Lets the changing propagation delay bend the pitch, the way air does (needs `propagation.enabled`).
        Delay,
    };

    /// Muffling: how much a blocked sound is dulled and quietened.
    struct MufflingSettings {
        bool enabled = true;
        /// 0..1+ strength: 0 leaves occluded sound untouched, 1 uses the materials' transmission as measured, above 1 exaggerates.
        f32 strength = 1.0f;
        /// The low-pass cutoff range the occlusion maps onto (a blocked high band goes toward `min_cutoff_hz`).
        f32 min_cutoff_hz = 900.0f;
        f32 max_cutoff_hz = 20000.0f;
        /// Quietest broadband gain an occluded sound is allowed to reach (before the no-path rule).
        f32 min_gain = 0.0f;
        /// When no route at all exists (no line of sight, no reflection, no way round), the sound is dulled to this cutoff and
        /// scaled by this extra gain on top of whatever leaks through the walls.
        f32 no_path_cutoff_hz = 500.0f;
        f32 no_path_gain = 0.6f;
        /// Rays across the source's disc used to measure how hidden it is.
        u32 occlusion_rays = 9;
        /// Surfaces a ray may cross before it counts as fully blocked.
        u32 max_crossings = 6;
        /// Candidate points tried when looking for a way round (a doorway, a corner); 0 turns diffraction off.
        u32 diffraction_probes = 16;
    };

    struct DirectionalitySettings {
        bool enabled = true;
        /// How far the arrival direction moves from the line of sight toward the last bounce: 0 keeps the line of sight,
        /// 1 uses only the bounce, 0.5 averages them.
        f32 bounce_blend = 0.5f;
        /// Reflected energy below this (relative to the unobstructed sound) is ignored when picking the arrival direction.
        f32 min_path_power = 0.002f;
    };

    struct PropagationSettings {
        bool enabled = true;
        f32 speed_of_sound = 343.0f;
        /// Scales the delay (1 = physical, 0.5 = half the lag, 2 = exaggerated).
        f32 delay_scale = 1.0f;
        f32 max_delay_seconds = 0.5f;
    };

    struct BounceSettings {
        bool enabled = true;
        /// Rays cast from the listener per query.
        u32 rays = 48;
        /// Reflections a ray follows.
        u32 max_bounces = 3;
        /// Overall level of reflected sound, an artistic control (1 = as computed).
        f32 gain = 1.0f;
        /// Probe the mirror image of the source in each wall the rays hit, finding exact specular echoes the random rays miss.
        bool specular_paths = true;
        /// Shifts the ray pattern a little every evaluation, so thin gaps are found over time instead of never.
        bool jitter = true;
        /// Rays stop when their energy falls below this fraction.
        f32 min_energy = 0.01f;
    };

    struct ReverbSettings {
        bool enabled = true;
        /// Rays used to survey the listener's room.
        u32 room_rays = 96;
        /// Scales the measured decay time and the send level (artistic overrides).
        f32 rt60_scale = 1.0f;
        f32 send_scale = 1.0f;
        f32 min_rt60 = 0.1f;
        f32 max_rt60 = 10.0f;
        /// Extra send for occluded sources (a muffled sound is mostly reverb).
        f32 occluded_boost = 0.3f;
    };

    struct DopplerSettings {
        DopplerMode mode = DopplerMode::Pitch;
        /// 1 = physical; larger exaggerates the shift, smaller reduces it.
        f32 factor = 1.0f;
        /// Pitch ratio limits.
        f32 min_ratio = 0.5f;
        f32 max_ratio = 2.0f;
        /// Measure velocity along the direction the sound arrives from (reflections, detours) instead of the straight line.
        bool along_arrival_path = true;
    };

    /// Reuse of the listener-side work between evaluations. The rays cast from the listener (and the room survey) depend only on
    /// where the listener is, so while it hardly moves they are reused, and each source costs only its own visibility tests.
    struct PathCacheSettings {
        bool enabled = true;
        /// How far the listener may move (metres) before the cached rays are thrown away.
        f32 listener_distance = 0.25f;
        /// Evaluations after which they are refreshed regardless (the ray pattern jitters on every refresh, finding thin gaps
        /// over time).
        u32 max_age = 8;
    };

    struct RaycastAcousticsSettings {
        PathCacheSettings cache;
        MufflingSettings muffling;
        DirectionalitySettings directionality;
        PropagationSettings propagation;
        BounceSettings bounces;
        ReverbSettings reverb;
        DopplerSettings doppler;
        f32 max_distance = 80.0f;

        // ---- the original flat names (still honoured: they set the nested values at construction when changed from default)
        u32 occlusion_rays = 0;
        u32 diffraction_probes = 0;
        u32 room_rays = 0;
        u32 max_crossings = 0;
    };

    /// Raytraced acoustics over any `AudioRayScene`: muffling, reflection-aware directionality, speed-of-sound delay, room
    /// reverb, material-shaped bounces and path-based Doppler, each separately switchable and tunable.
    class RaycastAcoustics final : public AcousticsProvider {
      public:
        RaycastAcoustics(std::shared_ptr<const AudioRayScene> scene, std::shared_ptr<const AcousticMaterialTable> materials,
                         const RaycastAcousticsSettings &settings = {});

        void evaluate(std::span<const AcousticsQuery> queries, std::span<AcousticsResult> results) override;
        [[nodiscard]] bool listener_room(RoomEstimate &out) const override;

        /// Changes the settings between evaluations (same thread as `evaluate`).
        void set_settings(const RaycastAcousticsSettings &settings);
        [[nodiscard]] const RaycastAcousticsSettings &settings() const noexcept { return settings_; }
        /// Uses a baked field for the listener's room instead of tracing room rays (falls back to tracing outside the field).
        void set_baked_field(std::shared_ptr<const AcousticField> field) { baked_field_ = std::move(field); }
        /// Replaces the geometry (a level change, or a rebuilt BVH).
        void set_scene(std::shared_ptr<const AudioRayScene> scene) {
            scene_ = std::move(scene);
            cached_.reset();
        }

        /// How many times the listener's rays were traced afresh (the rest of the evaluations reused them).
        [[nodiscard]] u64 listener_traces() const noexcept { return listener_traces_; }

        /// Surveys the space around `position` with rays (also used by `evaluate` for the listener).
        [[nodiscard]] RoomEstimate estimate_room(const glm::vec3 &position) const;

      private:
        /// Product of the transmission of every surface crossed between two points, per band, plus how many were crossed.
        [[nodiscard]] std::array<f32, acoustic_bands> transmission_along(const glm::vec3 &from, const glm::vec3 &to, u32 &crossings) const;
        [[nodiscard]] bool clear_between(const glm::vec3 &from, const glm::vec3 &to) const;

        std::shared_ptr<const AudioRayScene> scene_;
        std::shared_ptr<const AcousticMaterialTable> materials_;
        std::shared_ptr<const AcousticField> baked_field_;
        RaycastAcousticsSettings settings_;
        RoomEstimate last_room_;
        bool have_room_ = false;
        u32 jitter_counter_ = 0;
        // The listener's side of the paths, kept while the listener stays put.
        struct CachedVertex;
        std::shared_ptr<std::vector<CachedVertex>> cached_;
        glm::vec3 cached_listener_{0.0f};
        u32 cache_age_ = 0;
        u64 listener_traces_ = 0;
    };

} // namespace SFT::Audio
