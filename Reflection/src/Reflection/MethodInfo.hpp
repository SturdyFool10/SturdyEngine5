#pragma once

#include <Reflection/Attribute.hpp>
#include <Reflection/InvokeException.hpp>
#include <Reflection/Multicast.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <span>
#include <vector>

namespace SFT::Reflection {


    /// A type-erased method call: `object` is the receiver, `args` is an array of already-typed,
    /// caller-marshalled pointers (one per parameter), `out_return` is uninitialized storage for
    /// the return value (ignored when the method returns `void`), `user_data` is only meaningful
    /// when this fn ptr is installed as a mod override (see `MethodInfo::override_fn`).
    using MethodInvokeFn = void (*)(void *object, const void *const *args, void *out_return, void *user_data) noexcept;


    struct MethodInfo {
        TypeId key{};
        UString name;
        TypeId return_type{};
        std::vector<TypeId> param_types;
        /// The compiled-in real implementation. Never null once built by `Detail::build_method_info`.
        MethodInvokeFn invoke = nullptr;
        /// A mod-installed override, checked before `invoke` at every reflected call site. Null in
        /// the unmodded case, making the check a single branch-predicted-away load.
        std::atomic<MethodInvokeFn> override_fn{nullptr};
        void *override_user_data = nullptr;
        /// Fired (in registration order, with the same `args` the real call receives) just
        /// before `invoke`/`override_fn` runs. Unlike `override_fn` (single slot, full replace),
        /// any number of mods can subscribe here without clobbering each other — this is the
        /// additive alternative for "run something alongside this call" rather than "replace
        /// this call".
        Multicast before_invoke;
        /// Fired the same way as `before_invoke`, after the real call/override returns.
        Multicast after_invoke;
        /// Arbitrary tooling/mod-facing metadata. Never consulted by `invoke_method`; see
        /// `find_attribute`.
        std::vector<Attribute> attributes;
        /// Set for a method built by `Detail::build_static_method_info` (`SFT_REFLECT_STATIC_METHOD`).
        /// `invoke`'s `object` parameter is unused/ignored for these — call through
        /// `invoke_static_method` rather than passing a real object pointer.
        bool is_static = false;

        MethodInfo() = default;
        MethodInfo(const MethodInfo &) = delete;
        MethodInfo &operator=(const MethodInfo &) = delete;

        /// Constructs a `MethodInfo` by moving from `other`.
        ///
        /// @note `std::atomic` has no implicit move; this reads `other`'s override slot with a
        /// relaxed load since move construction only ever happens during single-threaded
        /// registration, before the method is reachable from any other thread. `Multicast` is a
        /// shared-state handle (cheap to move/copy), so `before_invoke`/`after_invoke` move
        /// trivially.
        /// @note This function does not throw exceptions.
        MethodInfo(MethodInfo &&other) noexcept
            : key(other.key),
              name(std::move(other.name)),
              return_type(other.return_type),
              param_types(std::move(other.param_types)),
              invoke(other.invoke),
              override_fn(other.override_fn.load(std::memory_order_relaxed)),
              override_user_data(other.override_user_data),
              before_invoke(std::move(other.before_invoke)),
              after_invoke(std::move(other.after_invoke)),
              attributes(std::move(other.attributes)),
              is_static(other.is_static) {}

        /// Assigns a new value to this `MethodInfo` by moving from `other`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        MethodInfo &operator=(MethodInfo &&other) noexcept {
            key = other.key;
            name = std::move(other.name);
            return_type = other.return_type;
            param_types = std::move(other.param_types);
            invoke = other.invoke;
            override_fn.store(other.override_fn.load(std::memory_order_relaxed), std::memory_order_relaxed);
            override_user_data = other.override_user_data;
            before_invoke = std::move(other.before_invoke);
            after_invoke = std::move(other.after_invoke);
            attributes = std::move(other.attributes);
            is_static = other.is_static;
            return *this;
        }
    };

    /// Invokes `method` on `object`, firing `before_invoke`/`after_invoke` around the call and
    /// dispatching to a mod-installed `override_fn` instead of `invoke` when one is present.
    ///
    /// The override check is a single acquire-load compared against `nullptr`
    /// (`[[unlikely]]`-annotated); each hook multicast adds one relaxed atomic integer load. In
    /// the fully unmodded case — no override, no hooks — this is three cheap, predicted-away
    /// checks over a direct call, none of which touch a lock or allocate.
    ///
    /// @param out_exception When non-null, cleared to "did not throw" before the call and filled
    /// in if the *real* (non-override) implementation threw a C++ exception — see
    /// `InvokeException`. A mod-installed `override_fn` is never wrapped this way (it is the
    /// mod's own responsibility to stay `noexcept`-safe, unchanged from before); only the compiled
    /// trampoline behind `method.invoke` is.
    ///
    /// @return Returns `true` on success; `false` when `arg_count` does not match the method's
    /// declared arity or the method has no invokable representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool invoke_method(const MethodInfo &method, void *object, const void *const *args, usize arg_count, void *out_return, InvokeException *out_exception = nullptr) noexcept {
        if (arg_count != method.param_types.size()) {
            return false;
        }
        MethodInvokeFn override_fn = method.override_fn.load(std::memory_order_acquire);
        if (override_fn == nullptr && method.invoke == nullptr) {
            return false;
        }
        method.before_invoke.fire(object, args);
        if (override_fn != nullptr) [[unlikely]] {
            if (out_exception != nullptr) {
                *out_exception = InvokeException{};
            }
            override_fn(object, args, out_return, method.override_user_data);
        } else {
            Detail::g_last_invoke_exception = InvokeException{};
            method.invoke(object, args, out_return, nullptr);
            if (out_exception != nullptr) {
                *out_exception = Detail::g_last_invoke_exception;
            }
        }
        method.after_invoke.fire(object, args);
        return true;
    }

    /// Same as `invoke_method`, additionally rejecting the call when `expected_return_type` or
    /// any entry of `expected_param_types` does not match `method`'s declared signature.
    ///
    /// `invoke_method` only checks argument *count* — it trusts that each `args[i]` really does
    /// point at a `method.param_types[i]`-typed value, because the caller (typically
    /// `SFT_REFLECT_INVOKE`, which knows the real C++ signature at compile time) already
    /// guarantees it. A caller that can't make that guarantee (an FFI boundary marshaling values
    /// from another language, an untrusted mod) should use this instead: the cost is one
    /// `TypeId` comparison per parameter, paid only where asked for.
    ///
    /// @return Returns `true` on success; `false` on any signature mismatch, or whatever
    /// `invoke_method` would have returned otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool invoke_method_checked(const MethodInfo &method, TypeId expected_return_type, std::span<const TypeId> expected_param_types, void *object, const void *const *args, usize arg_count, void *out_return, InvokeException *out_exception = nullptr) noexcept {
        if (method.return_type != expected_return_type || expected_param_types.size() != method.param_types.size()) {
            return false;
        }
        for (usize i = 0; i < expected_param_types.size(); ++i) {
            if (expected_param_types[i] != method.param_types[i]) {
                return false;
            }
        }
        return invoke_method(method, object, args, arg_count, out_return, out_exception);
    }

    /// Invokes a static method (`method.is_static == true`). There is no receiver — the
    /// trampoline `Detail::build_static_method_info` generates ignores `invoke_method`'s `object`
    /// parameter entirely, so this is just `invoke_method` with that parameter fixed to `nullptr`.
    /// Overrides/hooks (`set_method_override`, `add_method_before_hook`/`_after_hook`) work
    /// identically for static methods — they operate on the same `MethodInfo`.
    ///
    /// @return Returns `true` on success; `false` otherwise (see `invoke_method`).
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool invoke_static_method(const MethodInfo &method, const void *const *args, usize arg_count, void *out_return, InvokeException *out_exception = nullptr) noexcept {
        return invoke_method(method, nullptr, args, arg_count, out_return, out_exception);
    }

    /// Type-checked variant of `invoke_static_method`. See `invoke_method_checked`.
    [[nodiscard]] inline bool invoke_static_method_checked(const MethodInfo &method, TypeId expected_return_type, std::span<const TypeId> expected_param_types, const void *const *args, usize arg_count, void *out_return, InvokeException *out_exception = nullptr) noexcept {
        return invoke_method_checked(method, expected_return_type, expected_param_types, nullptr, args, arg_count, out_return, out_exception);
    }


} // namespace SFT::Reflection
