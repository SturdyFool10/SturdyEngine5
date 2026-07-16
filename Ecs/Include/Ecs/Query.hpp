#pragma once

#include <Ecs/Archetype.hpp>
#include <Ecs/Component.hpp>
#include <Ecs/Entity.hpp>

#include <array>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    // A read-only view over every entity whose archetype has (at least) every component in `Ts...`.
    // Built by World::query<Ts...>() — never constructed directly (it only takes a raw archetype
    // vector + match list, both World-owned). Each `Ts` in the pack is either `Component` (write
    // access) or `const Component` (read access); that const-qualification *is* the access
    // declaration Ecs/System.hpp's Schedule derives conflict detection from, so a system's data
    // dependencies never need declaring twice. Deliberately depends only on Archetype.hpp, not
    // World.hpp — World.hpp includes this header, not the other way around.
    template <class... Ts>
    class Query {
      public:
        static constexpr usize ComponentCount = sizeof...(Ts);

        Query(std::vector<Archetype> *archetypes, std::vector<u32> archetype_indices) noexcept
            : archetypes_(archetypes), archetype_indices_(std::move(archetype_indices)),
              ids_{component_id<std::remove_const_t<Ts>>()...} {}

        class Iterator {
          public:
            Iterator() noexcept = default;

            [[nodiscard]] bool operator!=(const Iterator &other) const noexcept {
                return archetype_position_ != other.archetype_position_ || row_ != other.row_;
            }

            Iterator &operator++() noexcept {
                ++row_;
                advance_to_valid();
                return *this;
            }

            [[nodiscard]] std::tuple<Entity, Ts &...> operator*() const noexcept {
                return dereference(std::make_index_sequence<ComponentCount>{});
            }

          private:
            friend class Query;

            Iterator(std::vector<Archetype> *archetypes, const std::vector<u32> *archetype_indices,
                     const std::array<ComponentId, ComponentCount> *ids, usize archetype_position) noexcept
                : archetypes_(archetypes), archetype_indices_(archetype_indices), ids_(ids),
                  archetype_position_(archetype_position) {
                resolve_columns();
                advance_to_valid();
            }

            void resolve_columns() noexcept {
                if (archetype_position_ >= archetype_indices_->size()) {
                    return;
                }
                const Archetype &archetype = (*archetypes_)[(*archetype_indices_)[archetype_position_]];
                for (usize i = 0; i < ComponentCount; ++i) {
                    columns_[i] = archetype.column_index_of((*ids_)[i]);
                }
            }

            // Skips archetypes with no rows left, resetting row_ to 0 each time archetype_position_
            // advances, until either a non-empty archetype is found or every matching archetype is
            // exhausted. The exhausted state (archetype_position_ == archetype_indices_->size(),
            // row_ == 0) is exactly what end() constructs by hand, so the two compare equal.
            void advance_to_valid() noexcept {
                while (archetype_position_ < archetype_indices_->size() &&
                       row_ >= (*archetypes_)[(*archetype_indices_)[archetype_position_]].size()) {
                    ++archetype_position_;
                    row_ = 0;
                    resolve_columns();
                }
            }

            template <usize... Is>
            [[nodiscard]] std::tuple<Entity, Ts &...> dereference(std::index_sequence<Is...>) const noexcept {
                Archetype &archetype = (*archetypes_)[(*archetype_indices_)[archetype_position_]];
                return std::tuple<Entity, Ts &...>(
                    archetype.entity_at(static_cast<u32>(row_)),
                    *static_cast<std::remove_reference_t<Ts> *>(
                        archetype.row_pointer(columns_[Is], static_cast<u32>(row_)))...);
            }

            std::vector<Archetype> *archetypes_ = nullptr;
            const std::vector<u32> *archetype_indices_ = nullptr;
            const std::array<ComponentId, ComponentCount> *ids_ = nullptr;
            usize archetype_position_ = 0;
            usize row_ = 0;
            std::array<u32, ComponentCount> columns_{};
        };

        [[nodiscard]] Iterator begin() const noexcept {
            return Iterator(archetypes_, &archetype_indices_, &ids_, 0);
        }

        [[nodiscard]] Iterator end() const noexcept {
            Iterator it;
            it.archetype_position_ = archetype_indices_.size();
            return it;
        }

      private:
        std::vector<Archetype> *archetypes_;
        std::vector<u32> archetype_indices_;
        std::array<ComponentId, ComponentCount> ids_;
    };

} // namespace SFT::Ecs
