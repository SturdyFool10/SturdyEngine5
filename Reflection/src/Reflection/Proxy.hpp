#pragma once

#include <Reflection/InvokeException.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

namespace SFT::Reflection {

    /// Dispatches one method call on a `ProxyObject`. `method_key` identifies which reflected
    /// method was called (the same key `MethodInfo::key`/`TypeInfo::find_method` use), so one
    /// dispatch callback handles every method of the proxied type instead of needing one function
    /// pointer per method. `args`/`out_return` follow the same already-typed,
    /// caller-marshalled-pointer convention as `MethodInvokeFn`. Exceptions must not cross this
    /// `noexcept` boundary — catch and report through `out_exception` instead (see
    /// `InvokeException`), the same discipline compiled trampolines follow.
    using ProxyDispatchFn = void (*)(void *user_data, TypeId method_key, const void *const *args, usize arg_count, void *out_return, InvokeException *out_exception) noexcept;

    /// A callback-backed stand-in for a reflected type — the analog of Java's
    /// `java.lang.reflect.Proxy`. Where `TypeInfoBuilder` lets a mod describe a brand-new type
    /// with no C++ definition behind it, `ProxyObject` lets a mod provide a fake *instance* of an
    /// existing reflected type: engine/game code that specifically wants to accept either a real
    /// object or a mod-supplied implementation calls `invoke_proxy_method` instead of
    /// `invoke_method`, and every call — regardless of which of the type's methods was
    /// requested — routes through one `dispatch` callback the mod supplies, keyed by `method_key`.
    ///
    /// This is opt-in on the *calling* side, not automatic: ordinary `invoke_method`/
    /// `SFT_REFLECT_INVOKE` call sites still only ever call real, compiled C++ objects — nothing
    /// about proxies is threaded through the normal, zero-overhead path. A `ProxyObject` is only
    /// ever `invoke_proxy_method`'d by code written to expect one (e.g. a scripting/FFI mod
    /// bridge that hands the engine a proxy standing in for a mod-authored implementation of some
    /// engine-defined interface-shaped type).
    struct ProxyObject {
        /// The `TypeInfo::key` this proxy stands in for — informational for the dispatcher; not
        /// consulted by `invoke_proxy_method` itself.
        TypeId type{};
        void *user_data = nullptr;
        ProxyDispatchFn dispatch = nullptr;
    };

    /// Invokes `method_key` on `proxy`, routing the call through `proxy.dispatch`.
    ///
    /// @return Returns `true` when `proxy.dispatch` is set (and was therefore called); `false`
    /// when the proxy has no dispatcher.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool invoke_proxy_method(const ProxyObject &proxy, TypeId method_key, const void *const *args, usize arg_count, void *out_return, InvokeException *out_exception = nullptr) noexcept {
        if (proxy.dispatch == nullptr) {
            return false;
        }
        proxy.dispatch(proxy.user_data, method_key, args, arg_count, out_return, out_exception);
        return true;
    }

} // namespace SFT::Reflection
