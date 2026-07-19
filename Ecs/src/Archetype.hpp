#pragma once

#include <Ecs/src/Component.hpp>
#include <Ecs/src/Entity.hpp>
#include <Ecs/src/Signature.hpp>

#include <cstddef>
#include <vector>

namespace SFT::Ecs {

    // Owns one contiguous, growable column per component in its Signature (SoA storage — the whole
    // point of the archetype model: a system iterating one component type across every entity that
    // has it gets cache-friendly linear access, not a pointer-chase per entity). Rows across all
    // columns move in lockstep, indexed 0..size()-1; `entities_[row]` gives the Entity owning that row.
    class Archetype {
      public:
        Archetype(Signature signature, const ComponentRegistry &registry);
        ~Archetype();

        Archetype(const Archetype &) = delete;
        Archetype &operator=(const Archetype &) = delete;
        Archetype(Archetype &&) noexcept = default;
        Archetype &operator=(Archetype &&) noexcept = default;

        [[nodiscard]] const Signature &signature() const noexcept { return signature_; }

        [[nodiscard]] usize size() const noexcept { return entities_.size(); }

        [[nodiscard]] Entity entity_at(u32 row) const noexcept { return entities_[row]; }

        // Index into this archetype's columns for `id`, or ~0u if this archetype doesn't have it.
        [[nodiscard]] u32 column_index_of(ComponentId id) const noexcept;

        [[nodiscard]] void *row_pointer(u32 column_index, u32 row) noexcept;
        [[nodiscard]] const void *row_pointer(u32 column_index, u32 row) const noexcept;

        // Grows column storage if needed and records `entity` at the new row. Every column's slot at
        // the returned row is raw, uninitialized memory — the caller (World::spawn) must
        // placement-construct every column before the row is considered live.
        [[nodiscard]] u32 add_row(Entity entity);

        // Destroys `row`'s live components and swap-removes it (moves the last row into `row`'s slot
        // to keep storage dense). Returns the Entity that got moved into `row` (a default-constructed,
        // invalid Entity{} if `row` was already the last row, i.e. nothing needed moving) — the caller
        // (World::destroy) must update that entity's recorded row.
        Entity remove_row(u32 row);

        // Archetype-transition primitive backing World::add_component/remove_component. Moves every
        // column `this` shares with `destination` from `row` into `destination`'s already-reserved
        // `destination_row` (via destination.add_row()), then destroys whichever of `row`'s columns
        // `destination` does not have, and swap-removes `row` here exactly like remove_row. Columns
        // `destination` has that `this` doesn't (the newly added component, for an add_component call)
        // are left uninitialized — the caller must placement-construct them into `destination` itself.
        // Returns the Entity swap-moved into `row` here, with the same semantics as remove_row.
        Entity move_row_into(u32 row, Archetype &destination, u32 destination_row);

      private:
        struct Column {
            ComponentId id{};
            // Copied by value so archetype storage can invoke lifecycle operations without taking a
            // registry lock per row or retaining a descriptor borrow across plugin/type activity.
            ComponentInfo info{};
            std::byte *data = nullptr; // capacity_ * info.size bytes, aligned to info.align
        };

        void grow(usize new_capacity);

        // Shared tail of remove_row/move_row_into: assumes `row`'s components have already been
        // destroyed (or moved out) by the caller, and only needs to fill the resulting hole by
        // swap-moving the last row into it (or just popping, if `row` already was the last row).
        Entity compact_removed_row(u32 row) noexcept;

        Signature signature_;
        std::vector<Column> columns_;
        std::vector<Entity> entities_;
        usize capacity_ = 0;
    };

} // namespace SFT::Ecs
