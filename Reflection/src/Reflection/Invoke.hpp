#pragma once

#include <Reflection/Macros.hpp>
#include <Reflection/MethodInfo.hpp>
#include <Reflection/TypeRegistry.hpp>

#include <Foundation/Foundation.hpp>

#include <array>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

/// A game that never wants to expose modding can define this to `0` for its own targets (see
/// `Reflection/CMakeLists.txt`), turning `SFT_REFLECT_INVOKE`/`SFT_REFLECT_FIRE_EVENT` into a
/// literal direct call/no-op with zero reflection machinery instantiated. Defaults to enabled so
/// existing call sites work with no build-system changes required.
#ifndef STURDY_REFLECTION_ENABLE_MODDING
#define STURDY_REFLECTION_ENABLE_MODDING 1
#endif

#if STURDY_REFLECTION_ENABLE_MODDING

namespace SFT::Reflection::Detail {


    /// A compile-time string usable as a non-type template parameter (a "structural type" per
    /// C++20's NTTP rules), letting `SFT_REFLECT_FIRE_EVENT` cache its `EventInfo*` per
    /// `(T, event name)` pair the same way `invoke_reflected` caches per `(T, Member)` — events
    /// have no member pointer to template on, so the name itself becomes the template argument.
    template <usize N>
    struct FixedString {
        char data[N]{};

        consteval FixedString(const char (&str)[N]) noexcept {
            for (usize i = 0; i < N; ++i) {
                data[i] = str[i];
            }
        }

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return std::string_view(data, N - 1);
        }
    };

    /// Call-site implementation behind `SFT_REFLECT_FIRE_EVENT`.
    ///
    /// The `EventInfo*` is resolved once per `(T, Name)` pair via a function-local `static`, the
    /// same caching shape `invoke_reflected` uses for methods. Firing then costs only what
    /// `Multicast::fire` costs — a relaxed atomic integer load, `[[likely]]`-taken when nobody
    /// has subscribed.
    template <class T, FixedString Name, class... Args>
    void fire_reflected_event(T *object, Args &&...args) {
        static const EventInfo *cached = TypeRegistry::instance().type<T>().find_event(Name.view());
        if (cached == nullptr) [[unlikely]] {
            return;
        }
        std::array<const void *, sizeof...(Args)> erased_args{static_cast<const void *>(std::addressof(args))...};
        fire_event(*cached, object, erased_args.data());
    }

    /// Call-site implementation behind `SFT_REFLECT_INVOKE`.
    ///
    /// The `MethodInfo*` is resolved once per `(T, Member)` pair via a function-local `static`
    /// (thread-safe magic-static init, matches every other lazy-registration path in this
    /// package), never re-hashed per call. The unmodded path is then just one acquire-load
    /// compared against `nullptr` (`[[unlikely]]`-annotated so the compiler keeps the direct
    /// call as the predicted path) before falling through to `(object->*Member)(args...)` —
    /// the same call the caller would have written directly.
    ///
    /// @return Returns whatever `Member` returns (or nothing, for a `void` method).
    template <class T, auto Member, class... Args>
    auto invoke_reflected(T *object, std::string_view method_name, Args &&...args) {
        using Traits = MemberFunctionTraits<decltype(Member)>;
        using Return = typename Traits::Return;

        // Looked up by signature (name + `Member`'s real parameter types), not name alone, so
        // this resolves the exact overload `Member` refers to even when `T` declares more than
        // one method named `method_name` — see `Detail::compute_method_key`.
        static const MethodInfo *cached = [method_name]() -> const MethodInfo * {
            const TypeInfo &type = TypeRegistry::instance().type<T>();
            const std::vector<TypeId> param_types = collect_param_types<Traits>(std::make_index_sequence<Traits::arity>{});
            return type.find_method(method_name, param_types);
        }();

        if (cached != nullptr) [[likely]] {
            if (MethodInvokeFn override_fn = cached->override_fn.load(std::memory_order_acquire); override_fn != nullptr) [[unlikely]] {
                std::array<const void *, sizeof...(Args)> erased_args{static_cast<const void *>(std::addressof(args))...};
                if constexpr (std::is_void_v<Return>) {
                    override_fn(object, erased_args.data(), nullptr, cached->override_user_data);
                    return;
                } else {
                    alignas(Return) unsigned char storage[sizeof(Return)];
                    override_fn(object, erased_args.data(), storage, cached->override_user_data);
                    auto *result = std::launder(reinterpret_cast<Return *>(storage));
                    Return moved = std::move(*result);
                    result->~Return();
                    return moved;
                }
            }
        }
        return (object->*Member)(std::forward<Args>(args)...);
    }

} // namespace SFT::Reflection::Detail


/// Calls `METHOD` on `OBJECT_PTR` (an instance of a `SFT_REFLECT_TYPE`-annotated `TYPE`) through
/// the reflection override mechanism: if a mod has installed an override via
/// `TypeRegistry::set_method_override`/`sturdy_reflection_register_override`, that override runs
/// instead; otherwise this is a direct call, differing from `OBJECT_PTR->METHOD(...)` only by one
/// branch-predicted-away pointer comparison.
///
/// This is opt-in per call site, not global — only call sites written with `SFT_REFLECT_INVOKE`
/// are moddable; ordinary `obj.method(...)` calls are never intercepted. See
/// `STURDY_REFLECTION_ENABLE_MODDING` (above) to compile this down to a plain direct call
/// game-build-wide.
#define SFT_REFLECT_INVOKE(OBJECT_PTR, TYPE, METHOD, ...) \
    ::SFT::Reflection::Detail::invoke_reflected<TYPE, &TYPE::METHOD>((OBJECT_PTR), std::string_view{#METHOD} __VA_OPT__(, ) __VA_ARGS__)

/// Fires the `SFT_REFLECT_EVENT`-declared event named `EVENT_NAME` on `OBJECT_PTR` (an instance
/// of a `SFT_REFLECT_TYPE`-annotated `TYPE`), calling every mod-subscribed listener in
/// registration order. A no-op when nobody has subscribed — see `Multicast::fire` — so sprinkling
/// these through game logic costs one relaxed atomic load per call site until a mod actually
/// listens.
#define SFT_REFLECT_FIRE_EVENT(OBJECT_PTR, TYPE, EVENT_NAME, ...) \
    ::SFT::Reflection::Detail::fire_reflected_event<TYPE, ::SFT::Reflection::Detail::FixedString{EVENT_NAME}>((OBJECT_PTR) __VA_OPT__(, ) __VA_ARGS__)

#else // !STURDY_REFLECTION_ENABLE_MODDING

namespace SFT::Reflection::Detail {

    /// Evaluates (for side effects) and discards every argument. Used only to keep
    /// `SFT_REFLECT_FIRE_EVENT` semantically a "this call still runs its argument expressions"
    /// no-op when modding is compiled out, rather than silently dropping them — at `-O2`+ this
    /// disappears entirely for pure argument expressions.
    template <class... Args>
    constexpr void discard_reflected_event_args(Args &&...) noexcept {}

} // namespace SFT::Reflection::Detail

/// Modding compiled out (`STURDY_REFLECTION_ENABLE_MODDING` is `0`): this is a plain direct call
/// with no reflection machinery instantiated at all — no `TypeRegistry` lookup, no atomic load,
/// nothing beyond what `OBJECT_PTR->METHOD(...)` would have cost anyway.
#define SFT_REFLECT_INVOKE(OBJECT_PTR, TYPE, METHOD, ...) ((OBJECT_PTR)->METHOD(__VA_ARGS__))

/// Modding compiled out: no listener could possibly be subscribed, so this discards its
/// arguments (still evaluating them, for side effects) instead of calling into `Multicast`/
/// `TypeRegistry` at all.
#define SFT_REFLECT_FIRE_EVENT(OBJECT_PTR, TYPE, EVENT_NAME, ...) \
    ::SFT::Reflection::Detail::discard_reflected_event_args((OBJECT_PTR) __VA_OPT__(, ) __VA_ARGS__)

#endif // STURDY_REFLECTION_ENABLE_MODDING
