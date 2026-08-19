#pragma once

#include <Ecs/Component.hpp>
#include <Ecs/Entity.hpp>
#include <Ecs/Signature.hpp>

#include <cstddef>
#include <vector>

namespace SFT::Ecs {


    class Archetype {
      public:
        /// Constructs a `Archetype` from the supplied initialization values.
        ///
        /// @param signature `signature` value used by the operation.
        /// @param registry `registry` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        Archetype(Signature signature, const ComponentRegistry &registry);
        /// Destroys the `Archetype` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Archetype();

        /// Disables this construction form for `Archetype`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Archetype(const Archetype &) = delete;
        /// Assigns a new value to this `Archetype`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Archetype &operator=(const Archetype &) = delete;
        /// Constructs a `Archetype` from another instance.
        ///
        /// @note This function does not throw exceptions.
        Archetype(Archetype &&) noexcept = default;
        /// Assigns a new value to this `Archetype`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Archetype &operator=(Archetype &&) noexcept = default;

        /// Returns the current or globally available signature value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Signature &signature() const noexcept;

        /// Returns the size for this `Archetype`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `Archetype`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept;

        /// Performs the entity at operation for `Archetype` using the supplied arguments.
        ///
        /// @param row `row` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Entity entity_at(u32 row) const noexcept;


        /// Performs the column index of operation for `Archetype` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 column_index_of(ComponentId id) const noexcept;

        /// Performs the row pointer operation for `Archetype` using the supplied arguments.
        ///
        /// @param column_index Zero-based index of the target element or entry.
        /// @param row `row` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] void *row_pointer(u32 column_index, u32 row) noexcept;
        /// Performs the row pointer operation for `Archetype` using the supplied arguments.
        ///
        /// @param column_index Zero-based index of the target element or entry.
        /// @param row `row` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const void *row_pointer(u32 column_index, u32 row) const noexcept;


        /// Adds row using the supplied arguments and current state.
        ///
        /// @param entity Entity used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u32 add_row(Entity entity);


        /// Removes the row from its owning collection or system.
        ///
        /// @param row `row` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        Entity remove_row(u32 row);


        /// Moves row into using the supplied arguments and current state.
        ///
        /// @param row `row` value used by the operation.
        /// @param destination Destination value or resource.
        /// @param destination_row `destination_row` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        Entity move_row_into(u32 row, Archetype &destination, u32 destination_row);

      private:
        struct Column {
            ComponentId id{};


            ComponentInfo info{};
            std::byte *data = nullptr;
        };

        /// Grows the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param new_capacity `new_capacity` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void grow(usize new_capacity);


        /// Performs the compact removed row operation for `Archetype` using the supplied arguments.
        ///
        /// @param row `row` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        Entity compact_removed_row(u32 row) noexcept;

        Signature signature_;
        std::vector<Column> columns_;
        std::vector<Entity> entities_;
        usize capacity_ = 0;
    };

} // namespace SFT::Ecs
