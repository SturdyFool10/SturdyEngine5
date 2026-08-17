#include <Ecs/src/Archetype.hpp>

#include <cstring>
#include <new>

#include <tracy/Tracy.hpp>

namespace SFT::Ecs {

/// Performs the archetype operation for `Ecs` using the supplied arguments.
///
/// @param signature `signature` value used by the operation.
/// @param registry `registry` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
Archetype::Archetype(Signature signature, const ComponentRegistry &registry) : signature_(std::move(signature)) {
    ZoneScopedN("Archetype::Archetype");
    columns_.reserve(signature_.size());
    for (ComponentId id : signature_) {
        const ComponentInfo *info = registry.info(id);
        if (info == nullptr) {
            Detail::contract_violation(
                "ECS archetype creation referenced unregistered dense component ID {}.",
                id);
        }
        columns_.push_back(Column{.id = id, .info = *info, .data = nullptr});
    }
}

/// Destroys the `Ecs` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
Archetype::~Archetype() {
    ZoneScopedN("Archetype::~Archetype");
    for (Column &column : columns_) {
        if (column.data == nullptr) {
            continue;
        }
        for (usize row = 0; row < entities_.size(); ++row) {
            if (column.info.destroy != nullptr) {
                column.info.destroy(column.data + row * column.info.size, column.info.user_data);
            }
        }
        ::operator delete(column.data, std::align_val_t(column.info.align));
    }
}

/// Performs the column index of operation for `Ecs` using the supplied arguments.
///
/// @param id Identifier of the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 Archetype::column_index_of(ComponentId id) const noexcept {
    ZoneScopedN("Archetype::column_index_of");
    for (usize i = 0; i < columns_.size(); ++i) {
        if (columns_[i].id == id) {
            return static_cast<u32>(i);
        }
    }
    return ~0u;
}

/// Performs the row pointer operation for `Ecs` using the supplied arguments.
///
/// @param column_index Zero-based index of the target element or entry.
/// @param row `row` value used by the operation.
///
/// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
/// @note This function does not throw exceptions.
[[nodiscard]] void *Archetype::row_pointer(u32 column_index, u32 row) noexcept {
    ZoneScopedN("Archetype::row_pointer");
    Column &column = columns_[column_index];
    return column.data + static_cast<usize>(row) * column.info.size;
}

/// Performs the row pointer operation for `Ecs` using the supplied arguments.
///
/// @param column_index Zero-based index of the target element or entry.
/// @param row `row` value used by the operation.
///
/// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
/// @note This function does not throw exceptions.
[[nodiscard]] const void *Archetype::row_pointer(u32 column_index, u32 row) const noexcept {
    ZoneScopedN("Archetype::row_pointer");
    const Column &column = columns_[column_index];
    return column.data + static_cast<usize>(row) * column.info.size;
}

/// Grows the supplied or associated value/state using the supplied arguments and current state.
///
/// @param new_capacity `new_capacity` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void Archetype::grow(usize new_capacity) {
    ZoneScopedN("Archetype::grow");
    for (Column &column : columns_) {
        std::byte *new_data = static_cast<std::byte *>(
            ::operator new(new_capacity * column.info.size, std::align_val_t(column.info.align)));
        for (usize row = 0; row < entities_.size(); ++row) {
            std::byte *destination = new_data + row * column.info.size;
            std::byte *source = column.data + row * column.info.size;
            if (column.info.move_construct != nullptr) {
                column.info.move_construct(destination, source, column.info.user_data);
            } else {
                std::memcpy(destination, source, column.info.size);
            }
            if (column.info.destroy != nullptr) {
                column.info.destroy(source, column.info.user_data);
            }
        }
        if (column.data != nullptr) {
            ::operator delete(column.data, std::align_val_t(column.info.align));
        }
        column.data = new_data;
    }
    capacity_ = new_capacity;
}

/// Adds row using the supplied arguments and current state.
///
/// @param entity Entity used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] u32 Archetype::add_row(Entity entity) {
    ZoneScopedN("Archetype::add_row");
    const usize row = entities_.size();
    if (row >= capacity_) {
        grow(capacity_ == 0 ? 8 : capacity_ * 2);
    }
    entities_.push_back(entity);
    return static_cast<u32>(row);
}

/// Removes the row from its owning collection or system.
///
/// @param row `row` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
Entity Archetype::remove_row(u32 row) {
    ZoneScopedN("Archetype::remove_row");
    for (Column &column : columns_) {
        std::byte *row_ptr = column.data + static_cast<usize>(row) * column.info.size;
        if (column.info.destroy != nullptr) {
            column.info.destroy(row_ptr, column.info.user_data);
        }
    }
    return compact_removed_row(row);
}

/// Moves row into using the supplied arguments and current state.
///
/// @param row `row` value used by the operation.
/// @param destination Destination value or resource.
/// @param destination_row `destination_row` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
Entity Archetype::move_row_into(u32 row, Archetype &destination, u32 destination_row) {
    ZoneScopedN("Archetype::move_row_into");
    for (Column &column : columns_) {
        std::byte *source = column.data + static_cast<usize>(row) * column.info.size;
        const u32 destination_column = destination.column_index_of(column.id);
        if (destination_column != ~0u) {
            std::byte *dest_ptr = static_cast<std::byte *>(destination.row_pointer(destination_column, destination_row));
            if (column.info.move_construct != nullptr) {
                column.info.move_construct(dest_ptr, source, column.info.user_data);
            } else {
                std::memcpy(dest_ptr, source, column.info.size);
            }
        }
        if (column.info.destroy != nullptr) {
            column.info.destroy(source, column.info.user_data);
        }
    }
    return compact_removed_row(row);
}

/// Performs the compact removed row operation for `Ecs` using the supplied arguments.
///
/// @param row `row` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
Entity Archetype::compact_removed_row(u32 row) noexcept {
    ZoneScopedN("Archetype::compact_removed_row");
    const usize last = entities_.size() - 1;
    Entity moved{};
    if (row != last) {
        for (Column &column : columns_) {
            std::byte *row_ptr = column.data + static_cast<usize>(row) * column.info.size;
            std::byte *last_ptr = column.data + last * column.info.size;
            if (column.info.move_construct != nullptr) {
                column.info.move_construct(row_ptr, last_ptr, column.info.user_data);
            } else {
                std::memcpy(row_ptr, last_ptr, column.info.size);
            }
            if (column.info.destroy != nullptr) {
                column.info.destroy(last_ptr, column.info.user_data);
            }
        }
        entities_[row] = entities_[last];
        moved = entities_[row];
    }
    entities_.pop_back();
    return moved;
}

} // namespace SFT::Ecs
