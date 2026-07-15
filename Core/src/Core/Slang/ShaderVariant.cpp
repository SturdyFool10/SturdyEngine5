#include "ShaderVariant.hpp"

namespace SFT::Core::Slang {

ShaderVariantKey::ShaderVariantKey(std::initializer_list<ShaderMacro> defines) {
            for (const ShaderMacro &define : defines) {
                set(define.name, define.value);
            }
        }

ShaderVariantKey &ShaderVariantKey::set(string name, string value) {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, const string &key) { return macro.name < key; });
            if (it != defines_.end() && it->name == name) {
                it->value = std::move(value);
            } else {
                defines_.insert(it, ShaderMacro{std::move(name), std::move(value)});
            }
            return *this;
        }

ShaderVariantKey &ShaderVariantKey::unset(string_view name) {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, string_view key) { return macro.name < key; });
            if (it != defines_.end() && it->name == name) {
                defines_.erase(it);
            }
            return *this;
        }

[[nodiscard]] bool ShaderVariantKey::has(string_view name) const noexcept {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, string_view key) { return macro.name < key; });
            return it != defines_.end() && it->name == name;
        }

[[nodiscard]] bool ShaderVariantKey::empty() const noexcept { return defines_.empty(); }

[[nodiscard]] const vector<ShaderMacro> &ShaderVariantKey::defines() const noexcept { return defines_; }

[[nodiscard]] const vector<ShaderMacro> &ShaderVariantKey::to_macros() const noexcept { return defines_; }

[[nodiscard]] string ShaderVariantKey::canonical() const {
            string out;
            for (const ShaderMacro &define : defines_) {
                if (!out.empty()) {
                    out.push_back(';');
                }
                out.append(define.name);
                out.push_back('=');
                out.append(define.value);
            }
            return out;
        }

[[nodiscard]] u64 ShaderVariantKey::hash() const noexcept {
            u64 value = 0xcbf29ce484222325ull;
            for (const ShaderMacro &define : defines_) {
                for (const char c : define.name) {
                    value = (value ^ static_cast<u8>(c)) * 0x100000001b3ull;
                }
                value = (value ^ static_cast<u8>('=')) * 0x100000001b3ull;
                for (const char c : define.value) {
                    value = (value ^ static_cast<u8>(c)) * 0x100000001b3ull;
                }
                value = (value ^ static_cast<u8>(';')) * 0x100000001b3ull;
            }
            return value;
        }

ShaderVariantCache::ShaderVariantCache(ShaderSource source, ShaderCompileOptions base_options, ShaderCompiler compiler)
            : compiler_(std::move(compiler)), source_(std::move(source)), base_options_(std::move(base_options)) {}

[[nodiscard]] const ShaderSource &ShaderVariantCache::source() const noexcept { return source_; }

[[nodiscard]] const ShaderCompileOptions &ShaderVariantCache::base_options() const noexcept { return base_options_; }

void ShaderVariantCache::set_source(ShaderSource source) {
            source_ = std::move(source);
            variants_.clear();
        }

void ShaderVariantCache::invalidate() noexcept { variants_.clear(); }

[[nodiscard]] usize ShaderVariantCache::size() const noexcept { return variants_.size(); }

[[nodiscard]] bool ShaderVariantCache::contains(const ShaderVariantKey &key) const {
            return variants_.find(key.canonical()) != variants_.end();
        }

[[nodiscard]] ShaderExpected<Shader> ShaderVariantCache::get_or_compile(const ShaderVariantKey &key) {
            const string canonical = key.canonical();
            if (const auto it = variants_.find(canonical); it != variants_.end()) {
                return it->second;
            }

            ShaderCompileOptions options = base_options_;
            options.macros.reserve(options.macros.size() + key.defines().size());
            for (const ShaderMacro &define : key.to_macros()) {
                options.macros.push_back(define);
            }

            ShaderExpected<Shader> compiled = compiler_.compile(source_, options);
            if (!compiled) {
                return compiled;
            }
            const auto [inserted, _] = variants_.emplace(canonical, std::move(*compiled));
            return inserted->second;
        }

[[nodiscard]] ShaderExpected<Shader> ShaderVariantCache::get_or_compile_base() { return get_or_compile(ShaderVariantKey{}); }

} // namespace SFT::Core::Slang
