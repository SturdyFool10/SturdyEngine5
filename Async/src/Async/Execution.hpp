#pragma once

#include <Async/Runtime.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace SFT::Async::Execution {

    // P2300-shaped vocabulary backed by Foundation::Async.  This is deliberately independent
    // of any vendor std::execution implementation, so the engine can select either backend.

    template <class R, class... Values>
    concept Receiver = requires(R receiver, std::exception_ptr error, Values &&...values) {
        receiver.set_value(std::forward<Values>(values)...);
        receiver.set_error(error);
        receiver.set_done();
    };

    template <class State>
    class Operation {
        State state_;
      public:
        explicit Operation(State state) : state_(std::move(state)) {}
        void start() { std::invoke(state_); }
    };

    template <class Rt, class F>
    class TaskSender {
        F fn_;
      public:
        explicit TaskSender(F fn) : fn_(std::move(fn)) {}

        [[nodiscard]] auto submit() && { return Rt::spawn(std::move(fn_)); }

        template <class R>
        auto connect(R receiver) && {
            return Operation{[fn = std::move(fn_), receiver = std::move(receiver)]() mutable {
                Rt::spawn([fn = std::move(fn), receiver = std::move(receiver)]() mutable {
                    try {
                        if constexpr (std::is_void_v<std::invoke_result_t<F &>>) {
                            std::invoke(fn);
                            receiver.set_value();
                        } else {
                            receiver.set_value(std::invoke(fn));
                        }
                    } catch (...) {
                        receiver.set_error(std::current_exception());
                    }
                });
            }};
        }
    };

    template <class R>
    struct SyncReceiver {
        std::optional<R> value;
        std::exception_ptr error;
        bool done = false;
        void set_value(R v) { value.emplace(std::move(v)); }
        void set_done() noexcept { done = true; }
        void set_error(std::exception_ptr ep) noexcept { error = ep; done = true; }
    };

    struct VoidSyncReceiver {
        bool value = false;
        std::exception_ptr error;
        void set_value() noexcept { value = true; }
        void set_done() noexcept {}
        void set_error(std::exception_ptr ep) noexcept { error = ep; }
    };

    template <class Rt, class F>
    [[nodiscard]] auto schedule(F fn) { return TaskSender<Rt, F>{std::move(fn)}; }

    template <class Rt = DefaultRuntime, class F>
    [[nodiscard]] auto async(F fn) { return schedule<Rt>(std::move(fn)); }

    template <class Rt, class S, class F>
    [[nodiscard]] auto then(S sender, F fn) {
        auto continuation = [sender = std::move(sender), fn = std::move(fn)]() mutable {
            auto task = std::move(sender).submit();
            if constexpr (std::is_void_v<decltype(task.wait())>) {
                task.wait();
                std::invoke(fn);
            } else {
                auto value = task.wait();
                return std::invoke(fn, std::move(value));
            }
        };
        return TaskSender<Rt, decltype(continuation)>{std::move(continuation)};
    }

    template <class Rt, class F>
    [[nodiscard]] auto just(F fn) { return TaskSender<Rt, F>{std::move(fn)}; }

    // Direct engine-runtime submission for callers that do not need sender composition.
    template <class Rt = DefaultRuntime, class F>
    [[nodiscard]] auto submit(F fn) { return Rt::spawn(std::move(fn)); }

    template <class Rt = DefaultRuntime, class F>
    [[nodiscard]] auto sync_wait(F fn) {
        auto task = Rt::spawn(std::move(fn));
        return task.wait();
    }

} // namespace SFT::Async::Execution
