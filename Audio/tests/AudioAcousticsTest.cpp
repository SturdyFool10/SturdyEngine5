#include <Async/Scheduler.hpp>
#include <Audio/Acoustics.hpp>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <iostream>

using namespace SFT::Audio;
using SFT::u32;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    // A flat rectangle of `cells` x `cells` squares (two triangles each), centred at `centre`, spanned by half-axes `u` and `v`.
    void add_quad(TriangleBvh &bvh, glm::vec3 centre, glm::vec3 u, glm::vec3 v, AcousticMaterialId material, u32 cells = 1) {
        std::vector<glm::vec3> positions;
        std::vector<u32> indices;
        for (u32 j = 0; j <= cells; ++j)
            for (u32 i = 0; i <= cells; ++i)
                positions.push_back(centre + u * (2.0f * static_cast<float>(i) / static_cast<float>(cells) - 1.0f) + v * (2.0f * static_cast<float>(j) / static_cast<float>(cells) - 1.0f));
        for (u32 j = 0; j < cells; ++j)
            for (u32 i = 0; i < cells; ++i) {
                const u32 a = j * (cells + 1) + i, b = a + 1, c = a + cells + 1, d = c + 1;
                indices.insert(indices.end(), {a, b, d, a, d, c});
            }
        bvh.add_mesh(positions, indices, material);
    }

    void add_box(TriangleBvh &bvh, glm::vec3 centre, glm::vec3 half, AcousticMaterialId material) {
        add_quad(bvh, centre + glm::vec3(half.x, 0, 0), {0, half.y, 0}, {0, 0, half.z}, material);
        add_quad(bvh, centre - glm::vec3(half.x, 0, 0), {0, half.y, 0}, {0, 0, half.z}, material);
        add_quad(bvh, centre + glm::vec3(0, half.y, 0), {half.x, 0, 0}, {0, 0, half.z}, material);
        add_quad(bvh, centre - glm::vec3(0, half.y, 0), {half.x, 0, 0}, {0, 0, half.z}, material);
        add_quad(bvh, centre + glm::vec3(0, 0, half.z), {half.x, 0, 0}, {0, half.y, 0}, material);
        add_quad(bvh, centre - glm::vec3(0, 0, half.z), {half.x, 0, 0}, {0, half.y, 0}, material);
    }

    AcousticsResult evaluate(RaycastAcoustics &a, AcousticsQuery q) {
        AcousticsResult r;
        a.evaluate({&q, 1}, {&r, 1});
        return r;
    }
} // namespace

int main() {
    auto materials = std::make_shared<AcousticMaterialTable>(AcousticMaterialTable::with_defaults());
    const AcousticMaterialId concrete = materials->find("concrete"), carpet = materials->find("carpet"), metal = materials->find("metal");

    // ---- materials and the BVH's resolution --------------------------------------------------------------------------------------
    {
        const AcousticMaterial *c = materials->get(concrete);
        check(c && c->reflectivity(1) > 0.9f && materials->get(carpet)->reflectivity(2) < 0.4f, "concrete reflects, carpet soaks up the highs");
        check(materials->get(carpet)->scattering > materials->get(metal)->scattering, "carpet scatters, metal is mirror-like");

        const auto make = [&](BvhSettings settings) {
            auto bvh = std::make_shared<TriangleBvh>();
            bvh->set_settings(settings);
            add_quad(*bvh, {0, 0, 0}, {20, 0, 0}, {0, 0, 20}, concrete, 80); // a 40 m floor of 12800 triangles
            add_quad(*bvh, {0, 0, 0}, {0.05f, 0, 0}, {0, 0.05f, 0.0f}, concrete, 1); // plus a tiny bit of clutter
            bvh->build();
            return bvh;
        };
        const auto full = make({});
        BvhSettings low;
        low.voxel_size = 1.0f;
        const auto coarse = make(low);
        BvhSettings budget;
        budget.max_triangles = 500;
        budget.split = BvhSplit::Sah;
        const auto limited = make(budget);
        check(full->triangle_count() == 12802, "full resolution keeps every triangle");
        check(coarse->triangle_count() < full->triangle_count() / 3 && coarse->triangles_removed() > 5000, "a lower resolution merges a tessellated floor into a few quads");
        check(limited->triangle_count() == 500, "a triangle budget is honoured");
        AudioRayHit hit;
        check(full->closest_hit({3, 5, 4}, {0, -1, 0}, 100.0f, hit) && std::fabs(hit.distance - 5.0f) < 1e-3f, "the full tree finds the floor");
        check(coarse->closest_hit({3, 5, 4}, {0, -1, 0}, 100.0f, hit) && std::fabs(hit.distance - 5.0f) < 0.05f, "and so does the coarse one");
        BvhSettings sah;
        sah.split = BvhSplit::Sah;
        sah.max_leaf_triangles = 8;
        const auto tree = make(sah);
        check(tree->closest_hit({-7, 2, 9}, {0, -1, 0}, 100.0f, hit) && std::fabs(hit.distance - 2.0f) < 1e-3f, "an SAH tree with larger leaves answers the same");
    }

    // ---- muffling: a wall dulls and quietens, a sealed box nearly silences ---------------------------------------------------------
    {
        auto bvh = std::make_shared<TriangleBvh>();
        add_quad(*bvh, {5, 0, 0}, {0, 20, 0}, {0, 0, 20}, concrete); // a wall at x = 5
        bvh->build();
        RaycastAcoustics acoustics(bvh, materials);
        AcousticsQuery q;
        q.listener = {0, 1, 0};
        q.source = {10, 1, 0};
        const AcousticsResult behind = evaluate(acoustics, q);
        check(!behind.line_of_sight && behind.broadband_gain < 0.5f && behind.lowpass_cutoff < 15000.0f && behind.band_gain[2] < behind.band_gain[0], "a wall muffles: quieter and darker, the highs most");
        q.source = {-10, 1, 0};
        const AcousticsResult clear = evaluate(acoustics, q);
        check(clear.line_of_sight && clear.broadband_gain > 0.99f && clear.lowpass_cutoff > 19000.0f, "an unobstructed source is untouched");
        q.source = {10, 1, 0};
        q.enabled_effects = all_acoustic_effects & ~static_cast<u32>(AcousticEffect::Muffling);
        const AcousticsResult exempt = evaluate(acoustics, q);
        check(exempt.broadband_gain > 0.99f && exempt.lowpass_cutoff > 19000.0f, "a source can opt out of muffling");
        q.enabled_effects = all_acoustic_effects;
        q.muffling_scale = 0.0f;
        check(evaluate(acoustics, q).broadband_gain > 0.99f, "or scale it to zero");

        auto settings = acoustics.settings();
        settings.muffling.strength = 2.0f;
        acoustics.set_settings(settings);
        q.muffling_scale = 1.0f;
        check(evaluate(acoustics, q).broadband_gain < behind.broadband_gain, "a higher strength muffles more");
        settings.muffling.strength = 1.0f;
        settings.muffling.enabled = false;
        acoustics.set_settings(settings);
        check(evaluate(acoustics, q).broadband_gain > 0.99f, "and the effect switches off globally");

        // A source sealed in a concrete box: no route at all.
        auto sealed = std::make_shared<TriangleBvh>();
        add_box(*sealed, {10, 1, 0}, {1.5f, 1.5f, 1.5f}, concrete);
        sealed->build();
        RaycastAcoustics closed(sealed, materials);
        AcousticsQuery inside;
        inside.listener = {-5, 1, 0};
        inside.source = {10, 1, 0};
        const AcousticsResult shut = evaluate(closed, inside);
        check(shut.lowpass_cutoff <= 600.0f && shut.broadband_gain < 0.1f, "with no path at all the sound is dull and faint");
        auto relaxed = closed.settings();
        relaxed.muffling.no_path_cutoff_hz = 20000.0f;
        relaxed.muffling.no_path_gain = 1.0f;
        closed.set_settings(relaxed);
        check(evaluate(closed, inside).lowpass_cutoff > shut.lowpass_cutoff, "the no-path behaviour is configurable");
    }

    // ---- directionality: line of sight averaged with the last bounce -------------------------------------------------------------------
    {
        auto bvh = std::make_shared<TriangleBvh>();
        add_quad(*bvh, {0, 0, 0}, {40, 0, 0}, {0, 0, 40}, metal); // a mirror floor
        bvh->build();
        RaycastAcousticsSettings settings;
        settings.directionality.bounce_blend = 1.0f;
        RaycastAcoustics acoustics(bvh, materials, settings);
        AcousticsQuery q;
        q.listener = {0, 1.5f, 0};
        q.source = {10, 1.5f, 0};
        const AcousticsResult r = evaluate(acoustics, q);
        check(r.has_apparent_direction && r.apparent_direction.y < -0.05f && r.apparent_direction.x > 0.5f, "the sound arrives partly from the floor it bounced off");
        settings.directionality.bounce_blend = 0.0f;
        acoustics.set_settings(settings);
        check(!evaluate(acoustics, q).has_apparent_direction, "with no blend the line of sight is kept");
        settings.directionality.bounce_blend = 0.5f;
        acoustics.set_settings(settings);
        const AcousticsResult half = evaluate(acoustics, q);
        check(half.has_apparent_direction && half.apparent_direction.y < 0.0f && half.apparent_direction.y > r.apparent_direction.y, "half a blend sits between the two");
        settings.directionality.enabled = false;
        acoustics.set_settings(settings);
        check(!evaluate(acoustics, q).has_apparent_direction, "and it can be switched off");
    }

    // ---- reflections find a way round an obstacle ------------------------------------------------------------------------------------------
    {
        auto bvh = std::make_shared<TriangleBvh>();
        add_quad(*bvh, {5, 5, 0}, {0, 6, 0}, {0, 0, 1.0f}, concrete);   // a narrow partition between them
        add_quad(*bvh, {5, 0, -6}, {20, 0, 0}, {0, 20, 0}, metal);       // a reflective wall beside the route
        bvh->build();
        RaycastAcousticsSettings settings;
        settings.muffling.diffraction_probes = 0;
        RaycastAcoustics acoustics(bvh, materials, settings);
        AcousticsQuery q;
        q.listener = {0, 1, 0};
        q.source = {10, 1, 0};
        const AcousticsResult with = evaluate(acoustics, q);
        settings.bounces.enabled = false;
        acoustics.set_settings(settings);
        const AcousticsResult without = evaluate(acoustics, q);
        check(with.broadband_gain > without.broadband_gain * 1.5f, "reflected sound gets round the partition, so it is louder with bounces on");
        settings.bounces.enabled = true;
        settings.directionality.bounce_blend = 1.0f;
        acoustics.set_settings(settings);
        const AcousticsResult heard = evaluate(acoustics, q);
        check(heard.has_apparent_direction && heard.apparent_direction.z < -0.2f, "and it arrives from the reflecting wall's side");
        check(heard.path_length > 10.5f, "along a path longer than the straight line");
    }

    // ---- propagation delay and Doppler ----------------------------------------------------------------------------------------------------
    {
        auto bvh = std::make_shared<TriangleBvh>();
        add_quad(*bvh, {0, -50, 0}, {1, 0, 0}, {0, 0, 1}, concrete); // something to hold on to, far out of the way
        bvh->build();
        RaycastAcousticsSettings settings;
        settings.doppler.mode = DopplerMode::Delay;
        RaycastAcoustics acoustics(bvh, materials, settings);
        AcousticsQuery q;
        q.listener = {0, 0, 0};
        q.source = {34.3f, 0, 0};
        const AcousticsResult plain = evaluate(acoustics, q);
        check(std::fabs(plain.extra_delay_seconds - 0.1f) < 0.002f, "34.3 m is a tenth of a second of travel at the speed of sound");
        q.delay_scale = 2.0f;
        check(std::fabs(evaluate(acoustics, q).extra_delay_seconds - 0.2f) < 0.004f, "the delay can be exaggerated");
        q.delay_scale = 1.0f;
        q.enabled_effects = all_acoustic_effects & ~static_cast<u32>(AcousticEffect::PropagationDelay);
        check(evaluate(acoustics, q).extra_delay_seconds == 0.0f, "or removed for one source");
        q.enabled_effects = all_acoustic_effects;
        check(evaluate(acoustics, q).doppler == 1.0f, "in Delay mode the changing delay bends the pitch, so no resampling ratio is given");

        settings.doppler.mode = DopplerMode::Pitch;
        acoustics.set_settings(settings);
        q.source_velocity = {-34.3f, 0, 0}; // coming toward the listener at a tenth of the speed of sound
        const AcousticsResult approaching = evaluate(acoustics, q);
        check(std::fabs(approaching.doppler - 343.0f / (343.0f - 34.3f)) < 0.01f && approaching.has_doppler, "an approaching source is shifted up by c / (c - v)");
        check(approaching.extra_delay_seconds < 0.001f, "and with pitch Doppler the delay does not also bend the pitch");
        q.source_velocity = {34.3f, 0, 0};
        check(evaluate(acoustics, q).doppler < 0.95f, "a receding one is shifted down");
        q.doppler_scale = 2.0f;
        check(evaluate(acoustics, q).doppler < 0.9f, "the shift can be exaggerated per source");
        q.doppler_scale = 1.0f;
        q.enabled_effects = all_acoustic_effects & ~static_cast<u32>(AcousticEffect::Doppler);
        const AcousticsResult off = evaluate(acoustics, q);
        check(off.doppler == 1.0f && off.has_doppler, "a source can opt out (and the mixer's own estimate stays out of it)");
        settings.doppler.mode = DopplerMode::Off;
        acoustics.set_settings(settings);
        q.enabled_effects = all_acoustic_effects;
        check(evaluate(acoustics, q).doppler == 1.0f, "Doppler can be off globally");
        settings.doppler.mode = DopplerMode::Pitch;
        settings.doppler.max_ratio = 1.05f;
        acoustics.set_settings(settings);
        q.source_velocity = {-100.0f, 0, 0};
        check(evaluate(acoustics, q).doppler <= 1.0501f, "and its range is limited");
    }

    // ---- reverb: the room the listener is in -------------------------------------------------------------------------------------------------
    {
        const auto room_rt60 = [&](AcousticMaterialId material, RaycastAcousticsSettings settings = {}) {
            auto bvh = std::make_shared<TriangleBvh>();
            add_box(*bvh, {0, 0, 0}, {6, 4, 6}, material);
            bvh->build();
            RaycastAcoustics acoustics(bvh, materials, settings);
            AcousticsQuery q;
            q.listener = {0, 0, 0};
            q.source = {3, 0, 1};
            const AcousticsResult r = evaluate(acoustics, q);
            RoomEstimate room;
            check(acoustics.listener_room(room), "the provider reports the room it surveyed");
            return std::pair{r, room};
        };
        const auto [hall, hall_room] = room_rt60(concrete);
        const auto [lounge, lounge_room] = room_rt60(carpet);
        check(hall.reverb_rt60 > 1.0f && hall.reverb_send > 0.2f && hall.reverb_rt60 > lounge.reverb_rt60 * 2.0f, "a concrete room rings far longer than a carpeted one");
        check(lounge_room.rt60_bands[2] < lounge_room.rt60_bands[0], "the carpeted room loses its highs first");
        RaycastAcousticsSettings scaled;
        scaled.reverb.rt60_scale = 0.5f;
        scaled.reverb.send_scale = 0.0f;
        const auto [tuned, tuned_room] = room_rt60(concrete, scaled);
        check(std::fabs(tuned.reverb_rt60 - hall.reverb_rt60 * 0.5f) < 0.05f && tuned.reverb_send == 0.0f, "decay and send can be scaled by the artist");
        RaycastAcousticsSettings off;
        off.reverb.enabled = false;
        check(room_rt60(concrete, off).first.reverb_rt60 == 0.0f, "reverb switches off");

        auto open = std::make_shared<TriangleBvh>();
        add_quad(*open, {0, -50, 0}, {1, 0, 0}, {0, 0, 1}, concrete);
        open->build();
        RaycastAcoustics outdoors(open, materials);
        AcousticsQuery q;
        q.source = {5, 0, 0};
        check(evaluate(outdoors, q).reverb_rt60 < 0.01f, "open air has no reverb");
    }

    // ---- the listener's rays are reused while it stays put ---------------------------------------------------------------------------
    {
        auto bvh = std::make_shared<TriangleBvh>();
        add_box(*bvh, {0, 0, 0}, {6, 4, 6}, concrete);
        bvh->build();
        RaycastAcoustics acoustics(bvh, materials);
        AcousticsQuery q;
        q.listener = {0, 0, 0};
        q.source = {3, 0, 1};
        for (int i = 0; i < 6; ++i) {
            q.listener.x = 0.01f * static_cast<float>(i);
            (void)evaluate(acoustics, q);
        }
        check(acoustics.listener_traces() == 1, "six evaluations with a nearly still listener trace its rays once");
        q.listener = {3, 0, 0};
        (void)evaluate(acoustics, q);
        check(acoustics.listener_traces() == 2, "moving across the room traces them again");
        auto settings = acoustics.settings();
        settings.cache.enabled = false;
        acoustics.set_settings(settings);
        for (int i = 0; i < 3; ++i) (void)evaluate(acoustics, q);
        check(acoustics.listener_traces() == 5, "and the cache can be switched off");
        // Visibility queries stop at the first hit but agree with the nearest-hit answer.
        AudioRayHit hit;
        bool agree = true;
        for (int i = 0; i < 200; ++i) {
            const glm::vec3 d = glm::normalize(glm::vec3(std::sin(static_cast<float>(i)), std::cos(static_cast<float>(i) * 1.3f), std::sin(static_cast<float>(i) * 0.7f)));
            for (float range : {2.0f, 5.0f, 50.0f}) agree = agree && (bvh->closest_hit({0, 0, 0}, d, range, hit) == bvh->any_hit({0, 0, 0}, d, range));
        }
        check(agree, "any_hit and closest_hit agree on every ray");
    }

    // ---- baked acoustic field: a room measured ahead of time, looked up instead of traced ----------------------------------------------
    {
        SFT::Async::Scheduler::initialize(2);
        auto bvh = std::make_shared<TriangleBvh>();
        add_box(*bvh, {0, 0, 0}, {6, 3, 6}, concrete); // a closed concrete room, 12 x 6 x 12 m
        bvh->build();
        RaycastAcoustics acoustics(bvh, materials);
        AcousticField::Settings bake;
        bake.min = {-5, -2.5f, -5};
        bake.max = {5, 2.5f, 5};
        bake.cell_size = 2.5f;
        const auto field = AcousticField::bake(acoustics, bake);
        check(field && field->dimensions() == glm::uvec3(5, 3, 5) && field->cell_count() == 75, "a field covers its box with a grid of samples");
        RoomEstimate looked, traced = acoustics.estimate_room({0, 0, 0});
        check(field->lookup({0, 0, 0}, looked) && std::fabs(looked.rt60 - traced.rt60) < 1e-3f && std::fabs(looked.mean_distance - traced.mean_distance) < 1e-3f, "a lookup at a sample point equals tracing there");
        RoomEstimate between;
        check(field->lookup({1.1f, 0.3f, -0.7f}, between) && between.rt60 > 0.5f && between.openness < 0.1f, "between samples it interpolates a closed, reverberant room");
        check(!field->lookup({20, 0, 0}, between), "outside the box there is no answer");
        const auto bytes = field->serialize();
        const auto restored = AcousticField::deserialize(bytes);
        RoomEstimate again;
        check(restored.has_value() && (*restored)->lookup({1.1f, 0.3f, -0.7f}, again) && std::fabs(again.rt60 - between.rt60) < 1e-6f, "it round-trips through bytes");
        check(!AcousticField::deserialize(std::span<const std::byte>(bytes).first(bytes.size() / 2)).has_value(), "a truncated field is refused");
        // The provider takes the baked room instead of tracing.
        acoustics.set_baked_field(field);
        AcousticsQuery q;
        q.listener = {0, 0, 0};
        q.source = {3, 0, 0};
        (void)evaluate(acoustics, q);
        RoomEstimate used;
        check(acoustics.listener_room(used) && std::fabs(used.rt60 - traced.rt60) < 1e-3f, "the provider reports the baked room");
        SFT::Async::Scheduler::shutdown();
    }

    return failures == 0 ? 0 : 1;
}
