#pragma once

#include <Foundation/Foundation.hpp>

namespace SFT::Reflection {

    /// Reports whether a reflected call threw a C++ exception — the analog of Java's
    /// `InvocationTargetException`. `invoke_method`/`invoke_static_method`/`construct_instance`
    /// accept an optional `InvokeException *out_exception`; when a compiled trampoline's real
    /// implementation throws, the exception is caught right there (never allowed to cross the
    /// `noexcept` `MethodInvokeFn`/`ConstructorInvokeFn` boundary, which would otherwise call
    /// `std::terminate`) and reported back through this struct instead.
    ///
    /// Deliberately does not change `MethodInvokeFn`/`ConstructorInvokeFn`'s signature — those
    /// function-pointer types are shared with `TypeInfoBuilder` (hand-written callbacks) and the
    /// FFI boundary (`sturdy_reflection_register_override`, which `reinterpret_cast`s a C ABI
    /// function pointer to `MethodInvokeFn`); adding a parameter there would silently break every
    /// existing call through a mismatched-arity function pointer. Instead, a compiled trampoline
    /// (`Detail::invoke_trampoline_impl` et al., in `Macros.hpp`) records the exception into a
    /// thread-local before returning, which `invoke_method`/`construct_instance` read immediately
    /// after the call and hand back to the caller who asked for it.
    struct InvokeException {
        bool threw = false;
        UString message;
    };

    namespace Detail {

        /// Scratch slot a compiled trampoline writes into when its real implementation throws.
        /// Thread-local because reflected calls can happen concurrently on different threads;
        /// reset before every trampoline call so a stale exception from a previous, unrelated
        /// call can never be misattributed to one that didn't throw.
        inline thread_local InvokeException g_last_invoke_exception{};

    } // namespace Detail

} // namespace SFT::Reflection
