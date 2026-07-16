#include <Ecs/System.hpp>

#include <Async/Scheduler.hpp>
#include <Async/Task.hpp>

namespace SFT::Ecs {

// Greedy graph coloring over the conflict graph: each stage takes every not-yet-placed system that
// doesn't conflict with anything already placed in that stage, leaving the rest for the next stage.
// Systems within one stage are mutually non-conflicting by construction; the resulting stage list is
// otherwise stable-ordered (a system never lands earlier than its first-registered conflicting
// predecessor), matching what the caller would get from hand-sequencing but without having to.
void Schedule::rebuild_stages() {
    stages_.clear();
    std::vector<usize> remaining(systems_.size());
    for (usize i = 0; i < systems_.size(); ++i) {
        remaining[i] = i;
    }

    while (!remaining.empty()) {
        std::vector<usize> stage;
        std::vector<usize> next_remaining;
        for (usize index : remaining) {
            bool conflicts = false;
            for (usize placed_index : stage) {
                if (system_access_conflicts(systems_[index].access, systems_[placed_index].access)) {
                    conflicts = true;
                    break;
                }
            }
            if (conflicts) {
                next_remaining.push_back(index);
            } else {
                stage.push_back(index);
            }
        }
        stages_.push_back(std::move(stage));
        remaining = std::move(next_remaining);
    }
    stages_dirty_ = false;
}

void Schedule::run(World &world) {
    if (stages_dirty_) {
        rebuild_stages();
    }
    for (const std::vector<usize> &stage : stages_) {
        if (stage.size() == 1) {
            // No conflicting sibling this frame — run inline rather than paying for a scheduler
            // round trip just to immediately wait on it alone.
            systems_[stage.front()].run(world);
            continue;
        }
        std::vector<Async::TaskHandle<void>> handles;
        handles.reserve(stage.size());
        for (usize index : stage) {
            handles.push_back(Async::Scheduler::spawn([&world, index, this] { systems_[index].run(world); }));
        }
        for (Async::TaskHandle<void> &handle : handles) {
            handle.wait();
        }
    }
}

} // namespace SFT::Ecs
