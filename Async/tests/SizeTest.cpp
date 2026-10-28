#include <Async/Size.hpp>

#include <cstddef>

using namespace SFT::Async::Literals;

static_assert(0_B == 0);
static_assert(512_B == 512);
static_assert(1_KiB == 1024);
static_assert(1_MiB == 1024 * 1024);
static_assert(1_GiB == 1024ULL * 1024ULL * 1024ULL);
static_assert(1_kB == 1000);
static_assert(1_MB == 1000 * 1000);
static_assert(1_GB == 1000ULL * 1000ULL * 1000ULL);
static_assert(1.5_MiB == 1536 * 1024);
static_assert(1.5_MB == 1'500'000);
static_assert(0.5_B == 0);

int main() {
    constexpr std::size_t binary_size = 2.25_GiB;
    constexpr std::size_t decimal_size = 2.25_GB;
    return binary_size == 2'415'919'104ULL && decimal_size == 2'250'000'000ULL ? 0 : 1;
}
