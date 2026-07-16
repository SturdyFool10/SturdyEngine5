#include <Ecs/Archetype.hpp>

#include <new>

namespace SFT::Ecs {

Archetype::Archetype(Signature signature) : signature_(std::move(signature)) {
    columns_.reserve(signature_.size());
    for (ComponentId id : signature_) {
        columns_.push_back(Column{.id = id, .info = component_info(id), .data = nullptr});
    }
}

Archetype::~Archetype() {
    for (Column &column : columns_) {
        if (column.data == nullptr) {
            continue;
        }
        for (usize row = 0; row < entities_.size(); ++row) {
            column.info.destroy(column.data + row * column.info.size);
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
            column.info.move_construct(new_data + row * column.info.size, column.data + row * column.info.size);
            column.info.destroy(column.data + row * column.info.size);
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
    const usize last = entities_.size() - 1;
    Entity moved{};
    for (Column &column : columns_) {
        std::byte *row_ptr = column.data + static_cast<usize>(row) * column.info.size;
        column.info.destroy(row_ptr);
        if (row != last) {
            std::byte *last_ptr = column.data + last * column.info.size;
            column.info.move_construct(row_ptr, last_ptr);
            column.info.destroy(last_ptr);
        }
    }
    if (row != last) {
        entities_[row] = entities_[last];
        moved = entities_[row];
    }
    entities_.pop_back();
    return moved;
}

} // namespace SFT::Ecs
