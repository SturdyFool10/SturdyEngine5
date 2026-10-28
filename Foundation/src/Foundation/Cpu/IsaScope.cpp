#include <Foundation/Cpu/IsaScope.hpp>

#include <Foundation/Cpu/CoreMap.hpp>

#include <array>
#include <utility>

#if defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#elif defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace SFT::Foundation::Cpu {

    namespace {
        thread_local IsaSet t_current_requirements;
        thread_local std::array<Detail::IsaScopeState *, 64> t_scope_stack{};
        thread_local usize t_scope_depth = 0;

#if defined(__linux__) || defined(_WIN32)
        [[nodiscard]] bool supports_any_core(const IsaSet &required) noexcept {
            const CoreMap &map = CoreMap::instance();
            for (usize i = 0; i < map.core_count(); ++i) {
                if (map.core(i).supports_isa(required)) {
                    return true;
                }
            }
            return false;
        }
#endif
    }

    namespace Detail {
        struct IsaScopeState {
            IsaSet previous_requirements;
            IsaSet required;
#if defined(__linux__)
            cpu_set_t previous_affinity{};
            bool affinity_saved = false;
#elif defined(_WIN32)
            GROUP_AFFINITY previous_affinity{};
            bool affinity_saved = false;
#endif
            bool entered = false;

            ~IsaScopeState() noexcept {
                if (!entered) {
                    return;
                }
#if defined(__linux__)
                if (affinity_saved) {
                    pthread_setaffinity_np(pthread_self(), sizeof(previous_affinity), &previous_affinity);
                }
#elif defined(_WIN32)
                if (affinity_saved) {
                    SetThreadGroupAffinity(GetCurrentThread(), &previous_affinity, nullptr);
                }
#endif
                if (t_scope_depth > 0 && t_scope_stack[t_scope_depth - 1] == this) {
                    t_scope_stack[--t_scope_depth] = nullptr;
                }
                t_current_requirements = previous_requirements;
            }
        };

        struct IsaSuspensionState {
#if defined(__linux__)
            cpu_set_t active_affinity{};
#elif defined(_WIN32)
            GROUP_AFFINITY active_affinity{};
#endif
            IsaSet active_requirements;
            bool active = false;

            ~IsaSuspensionState() noexcept {
                if (!active) {
                    return;
                }
#if defined(__linux__)
                pthread_setaffinity_np(pthread_self(), sizeof(active_affinity), &active_affinity);
#elif defined(_WIN32)
                SetThreadGroupAffinity(GetCurrentThread(), &active_affinity, nullptr);
#endif
                t_current_requirements = active_requirements;
            }
        };
    }

    IsaScope::IsaScope(const IsaSet &required) : state_(std::make_unique<Detail::IsaScopeState>()) {
        if (t_scope_depth == t_scope_stack.size()) {
            return;
        }
        const CoreMap &map = CoreMap::instance();

#if defined(__linux__)
        cpu_set_t allowed{};
        if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) != 0) {
            return;
        }
        cpu_set_t target{};
        CPU_ZERO(&target);
        for (usize i = 0; i < map.core_count(); ++i) {
            const CoreCapabilities &candidate = map.core(i);
            if (candidate.logical_cpu < CPU_SETSIZE &&
                CPU_ISSET(static_cast<int>(candidate.logical_cpu), &allowed) && candidate.supports_isa(required)) {
                CPU_SET(static_cast<int>(candidate.logical_cpu), &target);
            }
        }
        if (CPU_COUNT(&target) == 0) {
            return;
        }
        state_->previous_affinity = allowed;
        state_->affinity_saved = true;
        if (pthread_setaffinity_np(pthread_self(), sizeof(target), &target) != 0) {
            state_->affinity_saved = false;
            return;
        }
#elif defined(_WIN32)
        GROUP_AFFINITY allowed{};
        if (!GetThreadGroupAffinity(GetCurrentThread(), &allowed)) {
            return;
        }
        GROUP_AFFINITY target{};
        target.Group = allowed.Group;
        for (usize i = 0; i < map.core_count(); ++i) {
            const CoreCapabilities &candidate = map.core(i);
            if (candidate.logical_cpu < sizeof(DWORD_PTR) * 8 &&
                candidate.processor_group == allowed.Group &&
                (allowed.Mask & (DWORD_PTR{1} << candidate.logical_cpu)) != 0 && candidate.supports_isa(required)) {
                target.Mask |= DWORD_PTR{1} << candidate.logical_cpu;
            }
        }
        if (target.Mask == 0) {
            return;
        }
        if (!SetThreadGroupAffinity(GetCurrentThread(), &target, &state_->previous_affinity)) {
            return;
        }
        state_->affinity_saved = true;
#else
        (void)map;
        if (!required.empty()) {
            return;
        }
#endif

        state_->previous_requirements = t_current_requirements;
        state_->required = required;
        state_->entered = true;
        t_scope_stack[t_scope_depth++] = state_.get();
        t_current_requirements = required;
        valid_ = true;
    }

    IsaScope::~IsaScope() noexcept = default;

    bool IsaScope::valid() const noexcept { return valid_; }

    bool IsaScope::can_enter(const IsaSet &required) noexcept {
        if (required.empty()) {
            return true;
        }
        const CoreMap &map = CoreMap::instance();
#if defined(__linux__)
        cpu_set_t allowed{};
        if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) != 0) {
            return false;
        }
        for (usize i = 0; i < map.core_count(); ++i) {
            const CoreCapabilities &candidate = map.core(i);
            if (candidate.logical_cpu < CPU_SETSIZE &&
                CPU_ISSET(static_cast<int>(candidate.logical_cpu), &allowed) && candidate.supports_isa(required)) {
                return true;
            }
        }
        return false;
#elif defined(_WIN32)
        GROUP_AFFINITY allowed{};
        if (!GetThreadGroupAffinity(GetCurrentThread(), &allowed)) {
            return false;
        }
        for (usize i = 0; i < map.core_count(); ++i) {
            const CoreCapabilities &candidate = map.core(i);
            if (candidate.logical_cpu < sizeof(DWORD_PTR) * 8 &&
                candidate.processor_group == allowed.Group &&
                (allowed.Mask & (DWORD_PTR{1} << candidate.logical_cpu)) != 0 && candidate.supports_isa(required)) {
                return true;
            }
        }
        return false;
#else
        (void)map;
        return false;
#endif
    }

    bool IsaScope::has_compatible_core(const IsaSet &required) noexcept {
        if (required.empty()) {
            return true;
        }
#if defined(__linux__) || defined(_WIN32)
        return supports_any_core(required);
#else
        return false;
#endif
    }

    bool IsaScope::try_set_requirements(const IsaSet &required) noexcept {
        if (t_scope_depth == 0) {
            return false;
        }
        Detail::IsaScopeState *active = t_scope_stack[t_scope_depth - 1];
        if (!active || !active->entered) {
            return false;
        }
#if defined(__linux__)
        cpu_set_t target{};
        CPU_ZERO(&target);
        const CoreMap &map = CoreMap::instance();
        for (usize i = 0; i < map.core_count(); ++i) {
            const CoreCapabilities &candidate = map.core(i);
            if (candidate.logical_cpu < CPU_SETSIZE &&
                CPU_ISSET(static_cast<int>(candidate.logical_cpu), &active->previous_affinity) &&
                candidate.supports_isa(required)) {
                CPU_SET(static_cast<int>(candidate.logical_cpu), &target);
            }
        }
        if (CPU_COUNT(&target) == 0 ||
            pthread_setaffinity_np(pthread_self(), sizeof(target), &target) != 0) {
            return false;
        }
#elif defined(_WIN32)
        GROUP_AFFINITY target{};
        target.Group = active->previous_affinity.Group;
        const CoreMap &map = CoreMap::instance();
        for (usize i = 0; i < map.core_count(); ++i) {
            const CoreCapabilities &candidate = map.core(i);
            if (candidate.processor_group == target.Group && candidate.logical_cpu < sizeof(DWORD_PTR) * 8 &&
                (active->previous_affinity.Mask & (DWORD_PTR{1} << candidate.logical_cpu)) != 0 &&
                candidate.supports_isa(required)) {
                target.Mask |= DWORD_PTR{1} << candidate.logical_cpu;
            }
        }
        if (target.Mask == 0 || !SetThreadGroupAffinity(GetCurrentThread(), &target, nullptr)) {
            return false;
        }
#else
        if (!required.empty()) {
            return false;
        }
#endif
        active->required = required;
        t_current_requirements = required;
        return true;
    }

    IsaScopeSuspension::IsaScopeSuspension() : state_(std::make_unique<Detail::IsaSuspensionState>()) {
        if (t_scope_depth == 0) {
            return;
        }
        Detail::IsaScopeState *outermost = t_scope_stack[0];
        if (!outermost || !outermost->entered || !outermost->affinity_saved) {
            return;
        }
#if defined(__linux__)
        if (pthread_getaffinity_np(pthread_self(), sizeof(state_->active_affinity), &state_->active_affinity) != 0 ||
            pthread_setaffinity_np(pthread_self(), sizeof(outermost->previous_affinity), &outermost->previous_affinity) != 0) {
            return;
        }
#elif defined(_WIN32)
        if (!GetThreadGroupAffinity(GetCurrentThread(), &state_->active_affinity) ||
            !SetThreadGroupAffinity(GetCurrentThread(), &outermost->previous_affinity, nullptr)) {
            return;
        }
#else
        return;
#endif
        state_->active_requirements = t_current_requirements;
        t_current_requirements = IsaSet{};
        state_->active = true;
    }

    IsaScopeSuspension::~IsaScopeSuspension() noexcept = default;

    const IsaSet &current_isa_requirements() noexcept { return t_current_requirements; }

} // namespace SFT::Foundation::Cpu
