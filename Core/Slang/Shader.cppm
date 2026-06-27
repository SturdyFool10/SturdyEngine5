module;

#include <memory>
#include <string_view>

export module Sturdy.Core:Shader;

import Sturdy.Foundation;

// Re-export all Slang sub-partitions so callers that import :Shader or Sturdy.Core get
// the full shader API without importing each partition individually.
export import :ShaderError;
export import :ShaderSource;
export import :ShaderTypes;
export import :ShaderReflection;

using std::shared_ptr;
using std::string_view;

export namespace SFT::Core::Slang {

    inline constexpr u64 shader_unbounded_size = ~u64{0};
    inline constexpr u64 shader_unknown_size = shader_unbounded_size - 1;

    struct ShaderCompilerState;
    struct ShaderState;

    class Shader {
      public:
        Shader() = default;
        ~Shader();

        Shader(const Shader &) = default;
        Shader &operator=(const Shader &) = default;
        Shader(Shader &&) noexcept = default;
        Shader &operator=(Shader &&) noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const ShaderReflection &reflection() const noexcept;
        [[nodiscard]] string_view module_name() const noexcept;
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(usize entry_point_index, usize target_index = 0) const;
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(string_view entry_point_name, usize target_index = 0) const;

      private:
        friend class ShaderCompiler;
        explicit Shader(shared_ptr<ShaderState> state) noexcept;

        shared_ptr<ShaderState> state_;
    };

    class ShaderCompiler {
      public:
        ShaderCompiler();
        ~ShaderCompiler();

        ShaderCompiler(const ShaderCompiler &) = default;
        ShaderCompiler &operator=(const ShaderCompiler &) = default;
        ShaderCompiler(ShaderCompiler &&) noexcept = default;
        ShaderCompiler &operator=(ShaderCompiler &&) noexcept = default;

        [[nodiscard]] ShaderExpected<Shader> compile(const ShaderSource &source, const ShaderCompileOptions &options = {});

        template <StaticShaderSource StaticShader>
        [[nodiscard]] ShaderExpected<Shader> compile(const ShaderCompileOptions &options = {}) {
            return compile(shader_source_from_type<StaticShader>(), options);
        }

      private:
        shared_ptr<ShaderCompilerState> state_;
    };

} // namespace SFT::Core::Slang
