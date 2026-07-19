#include <Ecs/src/Archetype.hpp>

#include <cstring>
#include <new>

namespace SFT::Ecs {

Archetype::Archetype(Signature signature, const ComponentRegistry &registry) : signature_(std::move(signature)) {
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

Archetype::~Archetype() {
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

[[nodiscard]] u32 Archetype::column_index_of(ComponentId id) const noexcept {
    for (usize i = 0; i < columns_.size(); ++i) {
        if (columns_[i].id == id) {
            return static_cast<u32>(i);
        }
    }
    return ~0u;
}

[[nodiscard]] void *Archetype::row_pointer(u32 column_index, u32 row) noexcept {
    Column &column = columns_[column_index];
    return column.data + static_cast<usize>(row) * column.info.size;
}

[[nodiscard]] const void *Archetype::row_pointer(u32 column_index, u32 row) const noexcept {
    const Column &column = columns_[column_index];
    return column.data + static_cast<usize>(row) * column.info.size;
}

void Archetype::grow(usize new_capacity) {
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

[[nodiscard]] u32 Archetype::add_row(Entity entity) {
    const usize row = entities_.size();
    if (row >= capacity_) {
        grow(capacity_ == 0 ? 8 : capacity_ * 2);
    }
    entities_.push_back(entity);
    return static_cast<u32>(row);
}

Entity Archetype::remove_row(u32 row) {
    for (Column &column : columns_) {
        std::byte *row_ptr = column.data + static_cast<usize>(row) * column.info.size;
        if (column.info.destroy != nullptr) {
            column.info.destroy(row_ptr, column.info.user_data);
        }
    }
    return compact_removed_row(row);
}

Entity Archetype::move_row_into(u32 row, Archetype &destination, u32 destination_row) {
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

Entity Archetype::compact_removed_row(u32 row) noexcept {
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
