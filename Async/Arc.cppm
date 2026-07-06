module;

#pragma region Imports
#include <memory>
#include <utility>
#pragma endregion

export module Sturdy.Async:Arc;

using std::make_shared;
using std::shared_ptr;

export namespace SFT::Async {

    // An atomically reference-counted accessor to a shared `T`, built on `std::shared_ptr`'s control
    // block (its refcount increment/decrement is already required by the standard to be a thread-safe
    // atomic operation, so there's no benefit to hand-rolling that part again). Copying an `Arc<T>`
    // doesn't copy `T` — it hands back a new accessor to the *same* `T`, safely, from any thread,
    // concurrently with other threads copying or dropping their own accessors to it; the underlying
    // `T` is destroyed exactly once, when the last `Arc<T>` referencing it goes away.
    //
    // Unlike `shared_ptr<T>`, an `Arc<T>` is never null: there is no default constructor, only
    // `Arc<T>::make(...)` (or the free `make_arc<T>(...)` below), so a valid `Arc<T>` always has a
    // `T` to reach — no null-check ever needed at a use site. A moved-from `Arc<T>` is still in a
    // valid-but-unspecified state (same rule as `shared_ptr`/`unique_ptr`): don't dereference one
    // after moving from it.
    //
    // `Arc<T>` only makes *reaching* `T` thread-safe, not `T` itself: concurrent reads of an
    // unsynchronized `T` through two accessors are fine, but concurrent writes (or a write racing a
    // read) still need their own synchronization — wrap the payload as `Arc<Mutex<T>>` (see :Mutex)
    // when multiple threads need to mutate the shared value, not just observe it.
    template <typename T>
    class Arc {
      public:
        template <typename... Args>
        [[nodiscard]] static Arc make(Args &&...args) {
            return Arc(make_shared<T>(std::forward<Args>(args)...));
        }

        Arc(const Arc &) noexcept = default;
        Arc &operator=(const Arc &) noexcept = default;
        Arc(Arc &&) noexcept = default;
        Arc &operator=(Arc &&) noexcept = default;
        ~Arc() noexcept = default;

        [[nodiscard]] T &operator*() const noexcept {
            return *value_;
        }

        [[nodiscard]] T *operator->() const noexcept {
            return value_.get();
        }

        [[nodiscard]] T *get() const noexcept {
            return value_.get();
        }

        // Number of `Arc<T>` accessors currently sharing this `T` — a snapshot; another thread may
        // have already copied or dropped an accessor by the time the caller reads the result, so use
        // it for diagnostics/logging, not as the basis for a correctness decision.
        [[nodiscard]] long use_count() const noexcept {
            return value_.use_count();
        }

        // Identity comparison: whether two accessors refer to the same `T`, not whether the pointed-to
        // values compare equal.
        [[nodiscard]] bool operator==(const Arc &other) const noexcept {
            return value_ == other.value_;
        }

      private:
        explicit Arc(shared_ptr<T> value) noexcept
            : value_(std::move(value)) {
        }

        shared_ptr<T> value_;
    };

    // `Arc<T>::make(args...)`, deducing `T` from the call site instead of naming it twice.
    template <typename T, typename... Args>
    [[nodiscard]] Arc<T> make_arc(Args &&...args) {
        return Arc<T>::make(std::forward<Args>(args)...);
    }

} // namespace SFT::Async
