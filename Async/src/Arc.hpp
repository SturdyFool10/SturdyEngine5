#pragma once

#include <memory>
#include <utility>

namespace SFT::Async {

    template <typename T>
    class Arc {
      public:
        template <typename... Args>
        [[nodiscard]] static Arc make(Args &&...args) {
            return Arc(std::make_shared<T>(std::forward<Args>(args)...));
        }

        Arc(const Arc &) noexcept = default;
        Arc &operator=(const Arc &) noexcept = default;
        Arc(Arc &&) noexcept = default;
        Arc &operator=(Arc &&) noexcept = default;
        ~Arc() noexcept = default;

        [[nodiscard]] T &operator*() const noexcept { return *value_; }
        [[nodiscard]] T *operator->() const noexcept { return value_.get(); }
        [[nodiscard]] T *get() const noexcept { return value_.get(); }
        [[nodiscard]] long use_count() const noexcept { return value_.use_count(); }
        [[nodiscard]] bool operator==(const Arc &other) const noexcept { return value_ == other.value_; }

      private:
        explicit Arc(std::shared_ptr<T> value) noexcept
            : value_(std::move(value)) {}

        std::shared_ptr<T> value_;
    };

    template <typename T, typename... Args>
    [[nodiscard]] Arc<T> make_arc(Args &&...args) {
        return Arc<T>::make(std::forward<Args>(args)...);
    }

} // namespace SFT::Async
