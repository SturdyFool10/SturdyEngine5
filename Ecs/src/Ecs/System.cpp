#include <Ecs/System.hpp>


namespace SFT::Ecs {

    /// Performs the system access conflicts operation for `Ecs` using the supplied arguments.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool system_access_conflicts(const SystemAccess &a, const SystemAccess &b) noexcept {
        ZoneScopedN("system_access_conflicts");
        return access_sets_conflict(a.reads, a.writes, b.reads, b.writes) ||
               access_sets_conflict(a.resource_reads, a.resource_writes, b.resource_reads, b.resource_writes);
    }

} // namespace SFT::Ecs


namespace SFT::Ecs {

    /// Schedules the supplied work for later execution.
    ///
    /// @param config Configuration values controlling the operation.
    ///
    /// @note This function does not throw exceptions.
    Schedule::Schedule(ScheduleConfig config) noexcept : config_(config) {}

} // namespace SFT::Ecs

