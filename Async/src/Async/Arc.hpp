#pragma once

#include <memory>
#include <utility>

namespace SFT::Async {

    template <typename T>
    class Arc {
      public:
        /// Returns the current or globally available make value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename... Args>
        [[nodiscard]] static Arc make(Args &&...args) {
            return Arc(std::make_shared<T>(std::forward<Args>(args)...));
        }

        /// Constructs a `Arc` from another instance.
        ///
        /// @note This function does not throw exceptions.
        Arc(const Arc &) noexcept = default;
        /// Assigns a new value to this `Arc`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Arc &operator=(const Arc &) noexcept = default;
        /// Constructs a `Arc` from another instance.
        ///
        /// @note This function does not throw exceptions.
        Arc(Arc &&) noexcept = default;
        /// Assigns a new value to this `Arc`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Arc &operator=(Arc &&) noexcept = default;
        /// Destroys the `Arc` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Arc() noexcept = default;

        /// Dereferences this iterator or handle.
        ///
        /// @return Returns the value or reference currently addressed by the iterator/handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T &operator*() const noexcept { return *value_; }
        /// Accesses the object referenced by this `Arc`.
        ///
        /// @return Returns a pointer through which the referenced object can be accessed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T *operator->() const noexcept { return value_.get(); }
        /// Returns the value or resource currently represented by `Arc`.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T *get() const noexcept { return value_.get(); }
        /// Returns the use count for this `Arc`.
        ///
        /// @return Returns the current use count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] long use_count() const noexcept { return value_.use_count(); }
        /// Compares the operands for equality.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool operator==(const Arc &other) const noexcept { return value_ == other.value_; }

      private:
        /// Constructs a `Arc` from the supplied initialization values.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit Arc(std::shared_ptr<T> value) noexcept
            : value_(std::move(value)) {}

        std::shared_ptr<T> value_;
    };

    /// Creates an arc value from the supplied arguments.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    template <typename T, typename... Args>
    [[nodiscard]] Arc<T> make_arc(Args &&...args) {
        return Arc<T>::make(std::forward<Args>(args)...);
    }

} // namespace SFT::Async
