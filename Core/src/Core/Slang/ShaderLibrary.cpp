#include <Foundation/Foundation.hpp>

#include <Core/Slang/ShaderLibrary.hpp>

#include <filesystem>
#include <map>

#include <Async/Mutex.hpp>

namespace SFT::Core::Slang {

    namespace {

        [[nodiscard]] std::string normalize_module_name(std::string_view name) {
            std::string text{name};
            constexpr std::string_view extension = ".slang";
            if (text.size() > extension.size() && text.compare(text.size() - extension.size(), extension.size(), extension) == 0) {
                text.resize(text.size() - extension.size());
            }
            return text;
        }

        Async::Mutex<std::map<std::string, std::string>> &overrides() {
            static auto *table = new Async::Mutex<std::map<std::string, std::string>>();
            return *table;
        }

    } // namespace

    void override_shader_module(std::string_view name, std::string source) {
        auto guard = overrides().lock();
        (*guard)[normalize_module_name(name)] = std::move(source);
    }

    bool remove_shader_module_override(std::string_view name) {
        auto guard = overrides().lock();
        return guard->erase(normalize_module_name(name)) > 0;
    }

    void clear_shader_module_overrides() {
        auto guard = overrides().lock();
        guard->clear();
    }

    std::optional<std::string> find_shader_module_override(std::string_view name) {
        auto guard = overrides().lock();
        const auto found = guard->find(normalize_module_name(name));
        if (found == guard->end()) {
            return std::nullopt;
        }
        return found->second;
    }

    std::optional<std::string> find_shader_module_override_for_path(std::string_view path) {
        return find_shader_module_override(std::filesystem::path{std::string{path}}.stem().string());
    }

    u64 shader_override_fingerprint() {
        auto guard = overrides().lock();
        if (guard->empty()) {
            return 0;
        }
        u64 hash = 14695981039346656037ull;
        const auto mix = [&hash](std::string_view text) {
            for (const char c : text) {
                hash = (hash ^ static_cast<u8>(c)) * 1099511628211ull;
            }
            hash = (hash ^ 0xFFu) * 1099511628211ull;
        };
        for (const auto &[name, source] : *guard) {
            mix(name);
            mix(source);
        }
        return hash == 0 ? 1 : hash;
    }

} // namespace SFT::Core::Slang
