/// plan_bloom_levels() decides the bloom pyramid; the numbers below are what the previous private
/// implementation produced for a 1920x1080 source at the default ratio and for the UI glow's halving chain.

#include <Renderer/Bloom.hpp>

#include <iostream>

namespace {
    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }
} // namespace

int main() {
    using SFT::Core::Extent2D;
    using SFT::Renderer::plan_bloom_levels;

    const auto main_chain = plan_bloom_levels(Extent2D{1920, 1080}, 6, 1.61803398875f);
    check(main_chain.size() == 6, "six levels requested and available");
    check(main_chain.size() == 6 && main_chain[0] == Extent2D{1186, 667}, "level 0 is floor(source / ratio)");
    check(main_chain.size() == 6 && main_chain[1] == Extent2D{732, 412}, "level 1 divides level 0");

    const auto clamped = plan_bloom_levels(Extent2D{1920, 1080}, 100, 0.5f);
    check(clamped.size() <= 12 && !clamped.empty(), "levels and ratio are clamped");

    const auto tiny = plan_bloom_levels(Extent2D{8, 8}, 6, 2.0f);
    check(tiny.size() == 1 && tiny[0] == Extent2D{4, 4}, "stops before a level drops under the minimum axis");

    const auto ui = plan_bloom_levels(Extent2D{64, 32}, 3, 2.0f, 1);
    check(ui.size() == 3 && ui[0] == Extent2D{32, 16} && ui[1] == Extent2D{16, 8} && ui[2] == Extent2D{8, 4},
          "UI glow: three halving levels");

    const auto one_pixel = plan_bloom_levels(Extent2D{1, 1}, 4, 2.0f);
    check(one_pixel.size() == 1 && one_pixel[0] == Extent2D{1, 1}, "a 1x1 source yields a single 1x1 level");
    return failures == 0 ? 0 : 1;
}
