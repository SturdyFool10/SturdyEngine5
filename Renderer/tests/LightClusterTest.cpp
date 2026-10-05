/// CPU-only checks of `build_light_clusters` (Renderer/LightClusters.cpp). The central property is conservativeness:
/// any world position a light can reach must land, via the same cluster lookup the shaders use
/// (`lightClusterIndex` in Shaders/sturdy_lighting_data.slang, reimplemented here), in a cluster that lists the light.

#include <Renderer/LightClusters.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

namespace {

    using SFT::Renderer::build_light_clusters;
    using SFT::Renderer::ClusterLight;
    using SFT::Renderer::light_bounding_sphere;
    using SFT::Renderer::LightClusters;

    int failures = 0;

    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    constexpr u32 kWidth = 1280;
    constexpr u32 kHeight = 720;
    constexpr float kNear = 0.1f;

    /// The shader's lookup: tile from the view-projection, slice from the log of the view depth.
    u32 shader_cluster_index(const LightClusters &clusters, const glm::mat4 &view, const glm::mat4 &projection, const glm::vec3 &world) {
        const auto &grid = clusters.grid;
        const glm::vec4 clip = projection * view * glm::vec4{world, 1.0f};
        const glm::vec2 ndc = glm::vec2{clip} / std::max(std::abs(clip.w), 1e-6f);
        const float px = (ndc.x * 0.5f + 0.5f) * static_cast<float>(kWidth);
        const float py = (0.5f - ndc.y * 0.5f) * static_cast<float>(kHeight);
        const u32 x = static_cast<u32>(std::clamp(std::floor(px / static_cast<float>(grid.tile_px)), 0.0f, static_cast<float>(grid.tiles_x - 1)));
        const u32 y = static_cast<u32>(std::clamp(std::floor(py / static_cast<float>(grid.tile_px)), 0.0f, static_cast<float>(grid.tiles_y - 1)));
        const float depth = std::max(std::abs((view * glm::vec4{world, 1.0f}).z), grid.near_plane);
        const float slice = std::floor(std::log(depth) * grid.slice_scale + grid.slice_bias);
        const u32 s = static_cast<u32>(std::clamp(slice, 0.0f, static_cast<float>(grid.slices - 1)));
        return grid.cluster_index(x, y, s);
    }

    bool cluster_lists(const LightClusters &clusters, u32 cluster, u32 light) {
        const glm::uvec2 range = clusters.ranges[cluster];
        for (u32 k = 0; k < range.y; ++k) {
            if (clusters.indices[range.x + k] == light) return true;
        }
        return false;
    }

    bool in_view(const glm::mat4 &view, const glm::mat4 &projection, const glm::vec3 &world) {
        const glm::vec4 clip = projection * view * glm::vec4{world, 1.0f};
        if (clip.w <= kNear) return false;
        const glm::vec3 ndc = glm::vec3{clip} / clip.w;
        return std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f;
    }

} // namespace

int main() {
    const glm::mat4 view = glm::lookAt(glm::vec3{0.0f, 2.0f, 5.0f}, glm::vec3{0.0f, 1.0f, -10.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 projection = glm::perspective(glm::radians(70.0f), static_cast<float>(kWidth) / kHeight, kNear, 500.0f);
    const SFT::RenderSettings::LightingSettings settings{};

    // A scatter of point and spot lights, some partly or wholly off screen, one behind the camera.
    std::mt19937 rng{1234u};
    std::uniform_real_distribution<float> unit{0.0f, 1.0f};
    std::vector<ClusterLight> lights;
    for (u32 i = 0; i < 200; ++i) {
        ClusterLight light;
        light.position = glm::vec3{(unit(rng) - 0.5f) * 80.0f, unit(rng) * 10.0f, 4.0f - unit(rng) * 120.0f};
        light.range = 0.5f + unit(rng) * 12.0f;
        if (i % 3 == 0) {
            light.direction = glm::normalize(glm::vec3{unit(rng) - 0.5f, -1.0f, unit(rng) - 0.5f});
            light.cone_cos = std::cos(glm::radians(10.0f + unit(rng) * 70.0f));
        }
        lights.push_back(light);
    }
    lights.push_back(ClusterLight{.position = {0.0f, 2.0f, 40.0f}, .range = 3.0f}); // behind the camera, out of reach
    const u32 behind = static_cast<u32>(lights.size() - 1);

    const LightClusters clusters = build_light_clusters(lights, view, projection, kWidth, kHeight, kNear, settings);
    check(clusters.grid.cluster_count() > 0, "a perspective camera produces a grid");
    check(clusters.ranges.size() == clusters.grid.cluster_count(), "one range per cluster");
    check(clusters.dropped == 0, "the default per-cluster cap holds this scene");

    // Ranges tile the index list exactly, and every entry names a real light.
    u32 expected_offset = 0;
    bool contiguous = true, valid_indices = true, behind_listed = false;
    for (const glm::uvec2 &range : clusters.ranges) {
        contiguous = contiguous && range.x == expected_offset;
        expected_offset += range.y;
        for (u32 k = 0; k < range.y; ++k) {
            const u32 index = clusters.indices[range.x + k];
            valid_indices = valid_indices && index < lights.size();
            behind_listed = behind_listed || index == behind;
        }
    }
    check(contiguous && expected_offset == clusters.indices.size(), "cluster ranges are contiguous and cover the index list");
    check(valid_indices, "every listed index is a light");
    check(!behind_listed, "a light wholly behind the camera is in no cluster");

    // Conservative: random points inside each light's reach (on screen, within the grid's depth) find the light.
    u32 samples = 0, misses = 0;
    for (u32 i = 0; i < lights.size(); ++i) {
        const glm::vec4 sphere = light_bounding_sphere(lights[i]);
        for (u32 n = 0; n < 64; ++n) {
            const glm::vec3 direction = glm::normalize(glm::vec3{unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f} + 1e-4f);
            const glm::vec3 point = glm::vec3{sphere} + direction * (sphere.w * std::cbrt(unit(rng)) * 0.999f);
            const float depth = -(view * glm::vec4{point, 1.0f}).z;
            if (!in_view(view, projection, point) || depth < clusters.grid.near_plane || depth > clusters.grid.far_plane) continue;
            ++samples;
            if (!cluster_lists(clusters, shader_cluster_index(clusters, view, projection, point), i)) ++misses;
        }
    }
    check(samples > 1000, "enough on-screen samples to mean something");
    check(misses == 0, "every reachable on-screen point's cluster lists the light");

    // A narrow spot's bounding sphere is much smaller than its range sphere.
    const glm::vec4 narrow = light_bounding_sphere(ClusterLight{.position = {}, .range = 10.0f, .direction = {0, -1, 0}, .cone_cos = std::cos(glm::radians(15.0f))});
    check(narrow.w < 6.0f && narrow.y < 0.0f, "a narrow spot is bounded around its cone, below the apex");

    // The per-cluster cap drops the later (less important) lights first.
    SFT::RenderSettings::LightingSettings tight = settings;
    tight.max_lights_per_cluster = 1;
    const std::vector<ClusterLight> stacked = {ClusterLight{.position = {0.0f, 1.0f, -10.0f}, .range = 2.0f},
                                               ClusterLight{.position = {0.0f, 1.0f, -10.0f}, .range = 2.0f}};
    const LightClusters capped = build_light_clusters(stacked, view, projection, kWidth, kHeight, kNear, tight);
    check(capped.dropped > 0, "a full cluster counts dropped entries");
    bool only_first = !capped.indices.empty();
    for (const u32 index : capped.indices) only_first = only_first && index == 0;
    check(only_first, "a full cluster keeps the earlier light");

    // Orthographic cameras are not clustered.
    const glm::mat4 ortho = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f);
    check(build_light_clusters(lights, view, ortho, kWidth, kHeight, kNear, settings).grid.cluster_count() == 0,
          "an orthographic projection yields an empty grid");

    if (failures == 0) {
        std::cout << "LightClusterTest: all checks passed (" << samples << " reach samples)\n";
    }
    return failures == 0 ? 0 : 1;
}
