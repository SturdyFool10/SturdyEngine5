#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <memory>
#include <string_view>
#pragma endregion

#pragma region Imports


#include <Core/Slang/ShaderError.hpp>
#include <Core/Slang/ShaderSource.hpp>
#include <Core/Slang/ShaderTypes.hpp>
#include <Core/Slang/ShaderReflection.hpp>
#pragma endregion

using std::shared_ptr;
using std::string_view;

namespace SFT::Core::Slang {


    inline constexpr u64 shader_unbounded_size = ~u64{0};


    inline constexpr u64 shader_unknown_size = shader_unbounded_size - 1;

    struct ShaderCompilerState;
    struct ShaderState;


    class Shader {
      public:

        /// Constructs a `Shader` in its default state.
        ///
        /// @note This function does not throw exceptions.
        Shader() = default;
        /// Destroys the `Shader` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Shader();


        /// Constructs a `Shader` from another instance.
        ///
        /// @note This function does not throw exceptions.
        Shader(const Shader &) = default;
        /// Assigns a new value to this `Shader`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Shader &operator=(const Shader &) = default;
        /// Constructs a `Shader` from another instance.
        ///
        /// @note This function does not throw exceptions.
        Shader(Shader &&) noexcept = default;
        /// Assigns a new value to this `Shader`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Shader &operator=(Shader &&) noexcept = default;


        /// Converts the `Shader` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;


        /// Returns the current or globally available reflection value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ShaderReflection &reflection() const noexcept;


        /// Returns a human-readable name for the supplied module value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] string_view module_name() const noexcept;


        /// Retrieves or produces the entry point code selected by the supplied arguments.
        ///
        /// @param entry_point_index Zero-based index of the target element or entry.
        /// @param target_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(usize entry_point_index, usize target_index = 0) const;


        /// Retrieves or produces the entry point code selected by the supplied arguments.
        ///
        /// @param entry_point_index Zero-based index of the target element or entry.
        /// @param target `target` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(usize entry_point_index, ShaderTargetFormat target) const;


        /// Retrieves or produces the entry point code selected by the supplied arguments.
        ///
        /// @param entry_point_name Name used to identify or label the target.
        /// @param target_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(string_view entry_point_name, usize target_index = 0) const;


        /// Retrieves or produces the entry point code selected by the supplied arguments.
        ///
        /// @param entry_point_name Name used to identify or label the target.
        /// @param target `target` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(string_view entry_point_name, ShaderTargetFormat target) const;


        /// Releases compiler state using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void release_compiler_state() noexcept;

      private:
        friend class ShaderCompiler;
        /// Constructs a `Shader` from the supplied initialization values.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit Shader(shared_ptr<ShaderState> state) noexcept;

        shared_ptr<ShaderState> state_;
    };


    class ShaderCompiler {
      public:


        /// Constructs a `ShaderCompiler` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ShaderCompiler();
        /// Destroys the `ShaderCompiler` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~ShaderCompiler();


        /// Constructs a `ShaderCompiler` from another instance.
        ///
        /// @note This function does not throw exceptions.
        ShaderCompiler(const ShaderCompiler &) = default;
        /// Assigns a new value to this `ShaderCompiler`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        ShaderCompiler &operator=(const ShaderCompiler &) = default;
        /// Constructs a `ShaderCompiler` from another instance.
        ///
        /// @note This function does not throw exceptions.
        ShaderCompiler(ShaderCompiler &&) noexcept = default;
        /// Assigns a new value to this `ShaderCompiler`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        ShaderCompiler &operator=(ShaderCompiler &&) noexcept = default;


        /// Compiles the supplied source or pipeline state.
        ///
        /// @param source Source value or resource.
        /// @param options Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] ShaderExpected<Shader> compile(const ShaderSource &source, const ShaderCompileOptions &options = {});


        /// Compiles the supplied source or pipeline state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        template <StaticShaderSource StaticShader>
        [[nodiscard]] ShaderExpected<Shader> compile(const ShaderCompileOptions &options = {}) {
            return compile(shader_source_from_type<StaticShader>(), options);
        }


        /// Performs the reflect operation for `ShaderCompiler` using the supplied arguments.
        ///
        /// @param source Source value or resource.
        /// @param options Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] ShaderExpected<ShaderReflection> reflect(const ShaderSource &source, const ShaderCompileOptions &options = {});


        /// Retrieves or produces the from cached bytecode selected by the supplied arguments.
        ///
        /// @param module_name Name used to identify or label the target.
        /// @param targets `targets` value used by the operation.
        /// @param reflection `reflection` value used by the operation.
        /// @param bytecode `bytecode` value used by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Shader from_cached_bytecode(
            string module_name,
            vector<ShaderTarget> targets,
            ShaderReflection reflection,
            vector<ShaderBytecode> bytecode) const;


        /// Releases session using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void release_session() noexcept;

      private:
        shared_ptr<ShaderCompilerState> state_;
    };

} // namespace SFT::Core::Slang
