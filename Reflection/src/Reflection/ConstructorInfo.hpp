#pragma once

#include <Reflection/Attribute.hpp>
#include <Reflection/InvokeException.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <vector>

namespace SFT::Reflection {


    /// Placement-constructs a value into `destination`, unpacking `args` (one already-typed,
    /// caller-marshalled pointer per parameter, matching `ConstructorInfo::param_types`) into the
    /// real constructor call. `user_data` exists only for signature symmetry with
    /// `MethodInvokeFn`/`Multicast::ListenerFn` — constructors have no override/hook mechanism.
    using ConstructorInvokeFn = void (*)(void *destination, const void *const *args, void *user_data) noexcept;

    /// One parameterized constructor, declared via `SFT_REFLECT_CONSTRUCTOR(ArgType, ...)` — the
    /// analog of Java's `Constructor.newInstance(args)`. Unlike fields/methods, constructors have
    /// no name in C++; `TypeInfo::constructors` is searched by matching `param_types` instead (see
    /// `TypeInfo::find_constructor`). `TypeInfo::default_construct`/`copy_construct` remain the
    /// dedicated fast paths for the zero-argument and single-argument-by-const-ref cases — this
    /// exists for everything else.
    struct ConstructorInfo {
        std::vector<TypeId> param_types;
        ConstructorInvokeFn invoke = nullptr;
        /// Arbitrary tooling/mod-facing metadata. See `find_attribute`.
        std::vector<Attribute> attributes;
    };

    /// Calls `ctor`'s constructor, placement-constructing into `destination`.
    ///
    /// @param out_exception When non-null, cleared to "did not throw" before the call and filled
    /// in if the real constructor threw a C++ exception — see `InvokeException` and
    /// `invoke_method`'s matching parameter.
    ///
    /// @return Returns `true` on success; `false` when `arg_count` does not match
    /// `ctor.param_types.size()` or the constructor has no invokable representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool construct_instance(const ConstructorInfo &ctor, void *destination, const void *const *args, usize arg_count, InvokeException *out_exception = nullptr) noexcept {
        if (arg_count != ctor.param_types.size() || ctor.invoke == nullptr) {
            return false;
        }
        Detail::g_last_invoke_exception = InvokeException{};
        ctor.invoke(destination, args, nullptr);
        if (out_exception != nullptr) {
            *out_exception = Detail::g_last_invoke_exception;
        }
        return true;
    }


} // namespace SFT::Reflection
