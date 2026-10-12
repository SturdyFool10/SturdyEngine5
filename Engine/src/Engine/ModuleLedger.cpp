#include <Engine/ModuleLedger.hpp>

#include <map>
#include <mutex>
#include <vector>

namespace SFT::Engine {

    namespace {

        thread_local std::string t_current_module;

        struct Key {
            const Ecs::World *world;
            std::string module;
            [[nodiscard]] auto operator<=>(const Key &) const = default;
        };

        std::mutex g_mutex;
        std::map<Key, std::vector<std::function<void()>>> g_ledger;

    } // namespace

    ModuleScope::ModuleScope(std::string module) : previous_(std::move(t_current_module)) { t_current_module = std::move(module); }

    ModuleScope::~ModuleScope() { t_current_module = std::move(previous_); }

    std::string_view ModuleScope::current() noexcept { return t_current_module; }

    bool ModuleLedger::note(const Ecs::World &world, std::function<void()> release) {
        if (t_current_module.empty()) {
            return false;
        }
        const std::lock_guard<std::mutex> lock{g_mutex};
        g_ledger[Key{&world, t_current_module}].push_back(std::move(release));
        return true;
    }

    usize ModuleLedger::release(const Ecs::World &world, std::string_view module) {
        std::vector<std::function<void()>> releases;
        {
            const std::lock_guard<std::mutex> lock{g_mutex};
            const auto found = g_ledger.find(Key{&world, std::string{module}});
            if (found == g_ledger.end()) {
                return 0;
            }
            releases = std::move(found->second);
            g_ledger.erase(found);
        }
        // Outside the lock: an undo may itself register or note something, and must not deadlock on the ledger.
        for (auto it = releases.rbegin(); it != releases.rend(); ++it) {
            (*it)();
        }
        return releases.size();
    }

    usize ModuleLedger::count(const Ecs::World &world, std::string_view module) {
        const std::lock_guard<std::mutex> lock{g_mutex};
        const auto found = g_ledger.find(Key{&world, std::string{module}});
        return found == g_ledger.end() ? 0 : found->second.size();
    }

} // namespace SFT::Engine
