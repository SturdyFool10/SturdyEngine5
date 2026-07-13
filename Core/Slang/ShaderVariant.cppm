module;

#pragma region Imports
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#pragma endregion

export module Sturdy.Core:ShaderVariant;

import Sturdy.Foundation;
import :Shader;
import :ShaderError;
import :ShaderSource;
import :ShaderTypes;

using std::string;
using std::string_view;
using std::unordered_map;
using std::vector;

export namespace SFT::Core::Slang {

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    //  Shader variants: one `.slang` source compiled many ways.
    //
    //  A *variant* is a permutation of a shader selected by a fixed set of preprocessor defines —
    //  `SKINNED=1`, `ALPHA_TEST=1`, `MAX_LIGHTS=8` — each producing a structurally different SPIR-V
    //  blob. `ShaderVariantKey` is the value that names one permutation; `ShaderVariantCache` compiles
    //  and memoises them so a given permutation is built at most once. See plans/shader-variants-and-
    //  hot-reload.md. Purely numeric tunables that don't change control-flow shape belong in
    //  specialization constants (patched at pipeline-creation time), not here — defines multiply the
    //  cache, so reserve them for genuinely different code paths.
    // ─────────────────────────────────────────────────────────────────────────────────────────────

    // A set of preprocessor defines that selects one compiled permutation of a shader. Defines are kept
    // sorted by name and deduplicated, so two keys with the same defines in any insertion order are
    // equal and hash the same — the whole point, since a variant must map to exactly one cache slot.
    //
    // ```cpp
    // ShaderVariantKey key;
    // key.set("SKINNED");            // value defaults to "1"
    // key.set("MAX_LIGHTS", "8");
    // auto shader = cache.get_or_compile(key);
    // ```
    class ShaderVariantKey {
      public:
        ShaderVariantKey() = default;

        // Build a key from an explicit define list (order irrelevant — normalised on construction).
        ShaderVariantKey(std::initializer_list<ShaderMacro> defines) {
            for (const ShaderMacro &define : defines) {
                set(define.name, define.value);
            }
        }

        // Define `name` to `value` (defaults to "1"). Re-defining an existing name overwrites its value.
        // Returns `*this` so calls chain.
        ShaderVariantKey &set(string name, string value = "1") {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, const string &key) { return macro.name < key; });
            if (it != defines_.end() && it->name == name) {
                it->value = std::move(value);
            } else {
                defines_.insert(it, ShaderMacro{std::move(name), std::move(value)});
            }
            return *this;
        }

        // Removes `name` if defined. No-op if it isn't. Returns `*this`.
        ShaderVariantKey &unset(string_view name) {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, string_view key) { return macro.name < key; });
            if (it != defines_.end() && it->name == name) {
                defines_.erase(it);
            }
            return *this;
        }

        [[nodiscard]] bool has(string_view name) const noexcept {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, string_view key) { return macro.name < key; });
            return it != defines_.end() && it->name == name;
        }

        [[nodiscard]] bool empty() const noexcept { return defines_.empty(); }
        [[nodiscard]] const vector<ShaderMacro> &defines() const noexcept { return defines_; }

        // The defines as `ShaderMacro`s, ready to splice into `ShaderCompileOptions::macros`.
        [[nodiscard]] const vector<ShaderMacro> &to_macros() const noexcept { return defines_; }

        // A stable, human-readable identity: `"ALPHA_TEST=1;MAX_LIGHTS=8;SKINNED=1"` (sorted, so it is a
        // canonical fingerprint of the permutation). Used both as the cache key and for logs/debugging.
        [[nodiscard]] string canonical() const {
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

        // 64-bit FNV-1a of `canonical()` — a cheap content hash for coarse keying/logging. The cache keys
        // on `canonical()` directly (collision-free), so this is a convenience, not the cache's identity.
        [[nodiscard]] u64 hash() const noexcept {
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

        [[nodiscard]] friend bool operator==(const ShaderVariantKey &a, const ShaderVariantKey &b) noexcept {
            if (a.defines_.size() != b.defines_.size()) {
                return false;
            }
            for (usize i = 0; i < a.defines_.size(); ++i) {
                if (a.defines_[i].name != b.defines_[i].name || a.defines_[i].value != b.defines_[i].value) {
                    return false;
                }
            }
            return true;
        }

      private:
        // Sorted by `name`, unique — invariant maintained by set()/unset().
        vector<ShaderMacro> defines_;
    };

    // A lazily-populated cache of compiled shader permutations for one source.
    //
    // Holds the `ShaderSource`, a base `ShaderCompileOptions` (targets/entry-points/optimization shared
    // by every variant), and a `ShaderCompiler`. `get_or_compile(key)` returns the cached `Shader` for a
    // permutation or compiles it (base macros + the key's defines) and caches it on first request.
    //
    // Not internally synchronised: drive it from one thread (the main/render thread). A hot-reload edit
    // that changes the source calls `set_source()` / `invalidate()` to drop stale permutations so the
    // next request recompiles — see plans/shader-variants-and-hot-reload.md. `Shader` is `shared_ptr`-
    // backed, so cached entries are cheap to hand out by copy.
    class ShaderVariantCache {
      public:
        ShaderVariantCache() = default;

        ShaderVariantCache(ShaderSource source, ShaderCompileOptions base_options = {}, ShaderCompiler compiler = {})
            : compiler_(std::move(compiler)), source_(std::move(source)), base_options_(std::move(base_options)) {}

        [[nodiscard]] const ShaderSource &source() const noexcept { return source_; }
        [[nodiscard]] const ShaderCompileOptions &base_options() const noexcept { return base_options_; }

        // Point the cache at fresh source (e.g. a hot-reloaded file) and drop every compiled permutation,
        // so the next `get_or_compile()` recompiles against the new code.
        void set_source(ShaderSource source) {
            source_ = std::move(source);
            variants_.clear();
        }

        // Drop every compiled permutation without changing the source — used to force a recompile after
        // an edit to the same file, or to reclaim memory.
        void invalidate() noexcept { variants_.clear(); }

        [[nodiscard]] usize size() const noexcept { return variants_.size(); }
        [[nodiscard]] bool contains(const ShaderVariantKey &key) const {
            return variants_.find(key.canonical()) != variants_.end();
        }

        // The compiled `Shader` for `key`, compiling+caching it on the first request. Errors are not
        // cached — a fix-and-retry after a failed compile recompiles rather than returning the stale
        // failure. Compiling with an empty source returns `OperationFailed`.
        [[nodiscard]] ShaderExpected<Shader> get_or_compile(const ShaderVariantKey &key) {
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

        // Convenience for the common "no defines" base permutation.
        [[nodiscard]] ShaderExpected<Shader> get_or_compile_base() { return get_or_compile(ShaderVariantKey{}); }

      private:
        ShaderCompiler compiler_{};
        ShaderSource source_{};
        ShaderCompileOptions base_options_{};
        // Keyed by ShaderVariantKey::canonical() — a collision-free, order-independent fingerprint.
        unordered_map<string, Shader> variants_;
    };

} // namespace SFT::Core::Slang
