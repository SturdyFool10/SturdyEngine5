#pragma once

#include <Ecs/Component.hpp>
#include <Ecs/Entity.hpp>
#include <Ecs/Signature.hpp>

#include <cstddef>
#include <vector>

namespace SFT::Ecs {

    // Owns one contiguous, growable column per component in its Signature (SoA storage — the whole
    // point of the archetype model: a system iterating one component type across every entity that
    // has it gets cache-friendly linear access, not a pointer-chase per entity). Rows across all
    // columns move in lockstep, indexed 0..size()-1; `entities_[row]` gives the Entity owning that row.
    //
    // First-cut scope: entities are only ever added at spawn (with their full, final component set)
    // and removed at destroy — there is no add_component/remove_component archetype-transition support
    // yet (that needs a type-erased column-by-column move from one archetype's row into another's,
    // plus the archetype graph the design doc describes; deferred to a later session rather than
    // shipped half-working). Nothing here forecloses adding that later: the column layout and
    // move_construct/destroy plumbing below is exactly what a transition path would reuse.
    class Archetype {
      public:
        explicit Archetype(Signature signature);
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

      private:
        struct Column {
            ComponentId id{};
            // Copied by value, not a `const ComponentInfo *` into component_info()'s registry: that
            // registry is a std::vector<ComponentInfo> that keeps growing as new component types are
            // first seen (via component_id<T>()), and any such growth reallocates and invalidates
            // pointers/references into it. A pointer captured here at Archetype-construction time
            // would dangle the moment any *other* archetype later registers a brand new component
            // type — copying the (small, POD) struct once avoids that entirely.
            ComponentInfo info{};
            std::byte *data = nullptr; // capacity_ * info.size bytes, aligned to info.align
        };

        void grow(usize new_capacity);

        Signature signature_;
        std::vector<Column> columns_;
        std::vector<Entity> entities_;
        usize capacity_ = 0;
    };

} // namespace SFT::Ecs
