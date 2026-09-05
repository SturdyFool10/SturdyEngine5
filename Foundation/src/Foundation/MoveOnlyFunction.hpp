#pragma once

/// A portable stand-in for std::move_only_function (C++23) on toolchains that do not yet ship one --
/// Emscripten's bundled libc++ (as of the 6.0.9 packaging this was found against) is one, despite
/// otherwise supporting most of C++23's other library additions.
///
/// `SFT::Foundation::move_only_function<Sig>` is the real std::move_only_function everywhere the
/// standard library provides it, and this minimal polyfill only where it does not, so every caller
/// writes `Foundation::move_only_function<...>` once and gets the platform-appropriate type without
/// its own #ifdef.

#include <functional>

#pragma region Imports
#include <memory>
#include <type_traits>
#include <utility>
#pragma endregion

#ifdef __cpp_lib_move_only_function

namespace SFT::Foundation {

    template <typename Signature>
    using move_only_function = std::move_only_function<Signature>;

} // namespace SFT::Foundation

#else

namespace SFT::Foundation {

    namespace Detail {

        /// Type-erased call target. A move-only function has exactly one owner, so this is a plain
        /// unique_ptr to an abstract base rather than the small-buffer-optimized, ref-counted
        /// machinery std::function's copyable contract would need -- there is no copy to optimize
        /// for here.
        template <typename Ret, bool IsNoexcept, typename... Args>
        struct MoveOnlyFunctionBase {
            virtual ~MoveOnlyFunctionBase() = default;
            virtual Ret call(Args...) noexcept(IsNoexcept) = 0;
        };

        template <typename Fn, typename Ret, bool IsNoexcept, typename... Args>
        struct MoveOnlyFunctionImpl final : MoveOnlyFunctionBase<Ret, IsNoexcept, Args...> {
            Fn fn;

            explicit MoveOnlyFunctionImpl(Fn &&fn) noexcept(std::is_nothrow_move_constructible_v<Fn>)
                : fn(std::move(fn)) {}

            Ret call(Args... args) noexcept(IsNoexcept) override {
                return std::invoke(fn, std::forward<Args>(args)...);
            }
        };

        /// Shared body for the `Ret(Args...)` and `Ret(Args...) noexcept` partial specializations
        /// below -- the two differ only in whether `call` (and therefore `operator()`) is
        /// noexcept, which has to be spelled out at the type level rather than passed as a runtime
        /// bool, so the actual specializations are the thin wrappers underneath this.
        template <typename Ret, bool IsNoexcept, typename... Args>
        class MoveOnlyFunctionStorage {
          public:
            MoveOnlyFunctionStorage() noexcept = default;

            template <typename Fn>
                requires(!std::is_same_v<std::decay_t<Fn>, MoveOnlyFunctionStorage> &&
                        std::is_invocable_r_v<Ret, Fn &, Args...>)
            MoveOnlyFunctionStorage(Fn fn) // NOLINT(*-explicit-constructor) -- matches std::move_only_function's own implicit conversion.
                : storage_(std::make_unique<MoveOnlyFunctionImpl<Fn, Ret, IsNoexcept, Args...>>(std::move(fn))) {}

            MoveOnlyFunctionStorage(const MoveOnlyFunctionStorage &) = delete;
            MoveOnlyFunctionStorage &operator=(const MoveOnlyFunctionStorage &) = delete;
            MoveOnlyFunctionStorage(MoveOnlyFunctionStorage &&) noexcept = default;
            MoveOnlyFunctionStorage &operator=(MoveOnlyFunctionStorage &&) noexcept = default;
            ~MoveOnlyFunctionStorage() = default;

            [[nodiscard]] explicit operator bool() const noexcept { return storage_ != nullptr; }

          protected:
            [[nodiscard]] Ret invoke(Args... args) noexcept(IsNoexcept) {
                return storage_->call(std::forward<Args>(args)...);
            }

          private:
            std::unique_ptr<MoveOnlyFunctionBase<Ret, IsNoexcept, Args...>> storage_;
        };

    } // namespace Detail

    template <typename Signature>
    class move_only_function;

    template <typename Ret, typename... Args>
    class move_only_function<Ret(Args...)> : public Detail::MoveOnlyFunctionStorage<Ret, false, Args...> {
      public:
        using Detail::MoveOnlyFunctionStorage<Ret, false, Args...>::MoveOnlyFunctionStorage;

        Ret operator()(Args... args) { return this->invoke(std::forward<Args>(args)...); }
    };

    template <typename Ret, typename... Args>
    class move_only_function<Ret(Args...) noexcept> : public Detail::MoveOnlyFunctionStorage<Ret, true, Args...> {
      public:
        using Detail::MoveOnlyFunctionStorage<Ret, true, Args...>::MoveOnlyFunctionStorage;

        Ret operator()(Args... args) noexcept { return this->invoke(std::forward<Args>(args)...); }
    };

} // namespace SFT::Foundation

#endif
