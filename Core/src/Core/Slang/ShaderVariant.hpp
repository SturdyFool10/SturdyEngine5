#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Slang/Shader.hpp>
#include <Core/Slang/ShaderCache.hpp>
#include <Core/Slang/ShaderError.hpp>
#include <Core/Slang/ShaderSource.hpp>
#include <Core/Slang/ShaderTypes.hpp>

using std::string;
using std::string_view;
using std::unordered_map;
using std::vector;

namespace SFT::Core::Slang {


    class ShaderVariantKey {
      public:
        /// Constructs a `ShaderVariantKey` in its default state.
        ///
        /// @note This function does not throw exceptions.
        ShaderVariantKey() = default;


        /// Constructs a `ShaderVariantKey` from the supplied initialization values.
        ///
        /// @param defines `defines` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ShaderVariantKey(std::initializer_list<ShaderMacro> defines);


        /// Performs the set operation for `ShaderVariantKey` using the supplied arguments.
        ///
        /// @param name Name used to identify or label the target.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ShaderVariantKey &set(string name, string value = "1");


        /// Performs the unset operation for `ShaderVariantKey` using the supplied arguments.
        ///
        /// @param name Name used to identify or label the target.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ShaderVariantKey &unset(string_view name);

        /// Performs the has operation for `ShaderVariantKey` using the supplied arguments.
        ///
        /// @param name Name used to identify or label the target.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has(string_view name) const noexcept;

        /// Reports whether this `ShaderVariantKey` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept;
        /// Returns the current or globally available defines value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<ShaderMacro> &defines() const noexcept;


        /// Converts the value to macros representation.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<ShaderMacro> &to_macros() const noexcept;


        /// Returns the current or globally available canonical value.
        ///
        /// @return Returns the current canonical value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string canonical() const;


        /// Hashes the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the current hash value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 hash() const noexcept;

        /// Compares the operands for equality.
        ///
        /// @param a `a` value used by the operation.
        /// @param b `b` value used by the operation.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend bool operator==(const ShaderVariantKey &a, const ShaderVariantKey &b) noexcept;

      private:

        vector<ShaderMacro> defines_;
    };


    class ShaderVariantCache {
      public:
        /// Constructs a `ShaderVariantCache` in its default state.
        ///
        /// @note This function does not throw exceptions.
        ShaderVariantCache() = default;


        /// Constructs a `ShaderVariantCache` from the supplied initialization values.
        ///
        /// @param source Source value or resource.
        /// @param base_options Configuration values controlling the operation.
        /// @param compiler `compiler` value used by the operation.
        /// @param enable_disk_cache Whether the associated behavior is enabled.
        /// @param disk_cache_directory `disk_cache_directory` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ShaderVariantCache(ShaderSource source, ShaderCompileOptions base_options = {}, ShaderCompiler compiler = {},
                           bool enable_disk_cache = false,
                           std::filesystem::path disk_cache_directory = std::filesystem::path{string{default_shader_cache_directory}});

        /// Returns the current or globally available source value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ShaderSource &source() const noexcept;
        /// Returns the current or globally available base options value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ShaderCompileOptions &base_options() const noexcept;


        /// Sets the source for this `ShaderVariantCache`.
        ///
        /// @param source Source value or resource.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_source(ShaderSource source);


        /// Performs the invalidate operation for `ShaderVariantCache` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void invalidate() noexcept;


        /// Releases compiler memory using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void release_compiler_memory() noexcept;

        /// Returns the size for this `ShaderVariantCache`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `ShaderVariantCache`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept;
        /// Reports whether contains holds for this `ShaderVariantCache`.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool contains(const ShaderVariantKey &key) const;


        /// Returns the or compile associated with this `ShaderVariantCache`.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] ShaderExpected<Shader> get_or_compile(const ShaderVariantKey &key);


        /// Returns the or compile base associated with this `ShaderVariantCache`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] ShaderExpected<Shader> get_or_compile_base();

      private:
        ShaderCompiler compiler_{};
        ShaderSource source_{};
        ShaderCompileOptions base_options_{};
        bool enable_disk_cache_ = false;
        std::filesystem::path disk_cache_directory_{string{default_shader_cache_directory}};

        unordered_map<string, Shader> variants_;
    };

} // namespace SFT::Core::Slang
