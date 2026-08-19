#include <Foundation/Cpu/CoreMap.hpp>
#include <Foundation/Cpu/Extensions.hpp>

#include <iostream>

namespace {

    using namespace SFT::Foundation::Cpu;

    /// Returns a human-readable name for the supplied core type value.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *core_type_name(CoreType type) noexcept {
        switch (type) {
            case CoreType::Performance: return "Performance";
            case CoreType::Efficiency: return "Efficiency";
            case CoreType::Unknown: return "Unknown";
        }
        return "?";
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const CoreMap &map = CoreMap::instance();

    if (map.core_count() == 0) {
        std::cerr << "CoreMap reported zero cores\n";
        return 1;
    }

    std::cout << "core_count=" << map.core_count() << " distinct_type_count=" << map.distinct_type_count()
               << " is_hybrid=" << (map.is_hybrid() ? "true" : "false") << "\n";

    for (SFT::Foundation::usize type_index = 0; type_index < map.distinct_type_count(); ++type_index) {
        const auto &members = map.core_indices_of_type(type_index);
        const CoreCapabilities &representative = map.core(members.front());

        SFT::Foundation::usize extension_count = 0;
        for (SFT::Foundation::usize bit = 0; bit < representative.extensions.size(); ++bit) {
            if (representative.extensions[bit]) {
                ++extension_count;
            }
        }

        std::cout << "  type[" << type_index << "] core_type=" << core_type_name(representative.type)
                   << " members=" << members.size() << " extensions_set=" << extension_count
                   << " l1d=" << representative.l1d_bytes << " l1i=" << representative.l1i_bytes
                   << " l2=" << representative.l2_bytes << " l3=" << representative.l3_bytes << "\n";
    }


    for (SFT::Foundation::usize i = 0; i < map.core_count(); ++i) {
        const SFT::Foundation::usize type_index = map.type_index_of_core(i);
        const auto &members = map.core_indices_of_type(type_index);
        bool found = false;
        for (const SFT::Foundation::usize member : members) {
            if (member == i) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "core " << i << " not found in its own type's member list\n";
            return 1;
        }
    }

    const CoreCapabilities *current = map.capabilities_of_current_core();
    if (current == nullptr) {
        std::cerr << "capabilities_of_current_core() returned nullptr for the calling thread\n";
        return 1;
    }

#if defined(__x86_64__) || defined(_M_X64)
    if (!current->has(Extension::X86_SSE2)) {
        std::cerr << "current core does not report SSE2 on an x86-64 build (baseline ABI requirement)\n";
        return 1;
    }
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    if (!current->has(Extension::ARM_NEON)) {
        std::cerr << "current core does not report NEON on an Arm64 build (mandatory AArch64 baseline)\n";
        return 1;
    }
#endif

    std::cout << "all CoreMap checks passed\n";
    return 0;
}
