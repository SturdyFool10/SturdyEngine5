#include <Foundation/Cpu/CpuTopology.hpp>
#include <Foundation/Cpu/CpuId.hpp>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define STURDY_CPU_X86 1
#endif

#if defined(STURDY_CPU_X86)
    #include <cpuid.h>
    #include <cstring>
#endif

namespace SFT::Foundation::Cpu {

#if defined(STURDY_CPU_X86)

    namespace {

        /// Reads leaf from the associated source.
        ///
        /// @param leaf `leaf` value used by the operation.
        /// @param subleaf `subleaf` value used by the operation.
        /// @param eax `eax` value used by the operation.
        /// @param ebx `ebx` value used by the operation.
        /// @param ecx `ecx` value used by the operation.
        /// @param edx `edx` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void read_leaf(unsigned int leaf, unsigned int subleaf, unsigned int &eax, unsigned int &ebx, unsigned int &ecx, unsigned int &edx) noexcept {
            __get_cpuid_count(leaf, subleaf, &eax, &ebx, &ecx, &edx);
        }

        /// Reads x2apic ID from the associated source.
        ///
        /// @return Returns the current read x2apic ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 read_x2apic_id() noexcept {
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            read_leaf(0, 0, eax, ebx, ecx, edx);
            const unsigned int max_leaf = eax;


            if (max_leaf >= 0x1f) {
                read_leaf(0x1f, 0, eax, ebx, ecx, edx);
                return edx;
            }
            if (max_leaf >= 0xb) {
                read_leaf(0xb, 0, eax, ebx, ecx, edx);
                return edx;
            }
            read_leaf(1, 0, eax, ebx, ecx, edx);
            return ebx >> 24;
        }

        /// Reads core type from the associated source.
        ///
        /// @return Returns the current read core type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CoreType read_core_type() noexcept {
            const CpuFeatures &f = features();
            if (!f.hybrid) {
                return CoreType::Unknown;
            }


            if (std::strcmp(f.vendor, "GenuineIntel") != 0) {
                return CoreType::Unknown;
            }
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            read_leaf(0x1a, 0, eax, ebx, ecx, edx);
            const unsigned int core_type = eax >> 24;
            if (core_type == 0x20) {
                return CoreType::Efficiency;
            }
            if (core_type == 0x40) {
                return CoreType::Performance;
            }
            return CoreType::Unknown;
        }

    } // namespace

    /// Returns the current or globally available current core value.
    ///
    /// @return Returns the current current core value.
    /// @note This function does not throw exceptions.
    CurrentCore current_core() noexcept {
        return CurrentCore{
            .x2apic_id = read_x2apic_id(),
            .type = read_core_type(),
        };
    }

#else

    /// Returns the current or globally available current core value.
    ///
    /// @return Returns the current current core value.
    /// @note This function does not throw exceptions.
    CurrentCore current_core() noexcept { return CurrentCore{}; }

#endif

} // namespace SFT::Foundation::Cpu
