#include <Ecs/src/System.hpp>


namespace SFT::Ecs {

    bool system_access_conflicts(const SystemAccess &a, const SystemAccess &b) noexcept {
        ZoneScopedN("system_access_conflicts");
        return access_sets_conflict(a.reads, a.writes, b.reads, b.writes) ||
               access_sets_conflict(a.resource_reads, a.resource_writes, b.resource_reads, b.resource_writes);
    }

} // namespace SFT::Ecs


namespace SFT::Ecs {

    Schedule::Schedule(ScheduleConfig config) noexcept : config_(config) {}

} // namespace SFT::Ecs

