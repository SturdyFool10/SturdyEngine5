#include <Animation/Clip.hpp>
#include <Animation/Morph.hpp>

#include <cmath>
#include <iostream>

using namespace SFT::Animation;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    // Three vertices; target "smile" moves vertices 0 and 2, target "blink" moves only vertex 2.
    MorphBuilder builder(3);
    const std::vector<glm::vec3> smile{{1, 0, 0}, {0, 0, 0}, {0, 2, 0}};
    const std::vector<glm::vec3> blink{{0, 0, 0}, {0, 0, 0}, {0, 0, 4}};
    builder.add_target("smile", smile);
    builder.add_target("blink", blink, {}, 0.5f);
    const MorphTargetSet set = builder.build();

    check(set.target_count() == 2 && set.vertex_count() == 3, "counts");
    check(set.vertex_offsets == std::vector<u32>({0, 1, 1, 3}), "CSR offsets only store moved vertices");
    check(near(set.default_weights[1], 0.5f), "default weight kept");

    glm::vec3 p(0.0f), n(0, 1, 0);
    const std::vector<f32> weights{0.5f, 1.0f};
    apply_morph(set, 2, weights, p, n);
    check(near(p.y, 1.0f) && near(p.z, 4.0f), "vertex 2 gets both targets");
    p = glm::vec3(0.0f);
    apply_morph(set, 1, weights, p, n);
    check(near(glm::length(p), 0.0f), "untouched vertex unchanged");

    // Weight track: two weights, linear, 0 -> 1 over one second.
    Clip clip;
    MorphTrack track;
    track.target = "Face";
    track.weight_count = 2;
    track.track.times = {0.0f, 1.0f};
    track.track.values = {0.0f, 1.0f, 1.0f, 0.0f};
    clip.morph_tracks.push_back(track);
    clip.recompute_duration();
    check(near(clip.duration, 1.0f), "morph track contributes to duration");
    check(clip.find_morph_track("Face") != nullptr && clip.find_morph_track("") != nullptr &&
              clip.find_morph_track("Other") == nullptr,
          "track lookup by name / first");
    std::vector<f32> out;
    sample_weights(*clip.find_morph_track("Face"), 0.25f, out);
    check(out.size() == 2 && near(out[0], 0.25f) && near(out[1], 0.75f), "weight interpolation");

    return failures == 0 ? 0 : 1;
}
