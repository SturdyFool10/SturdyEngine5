#pragma once

#include <Ecs/src/Archetype.hpp>
#include <Ecs/src/Component.hpp>
#include <Ecs/src/Entity.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    // A typed view over every entity whose archetype has (at least) every component in `Ts...`.
    // Built by World::query<Ts...>() — never constructed directly (it only takes a raw archetype
    // vector + match list, both World-owned). Each `Ts` in the pack is either `Component` (write
    // access) or `const Component` (read access); that const-qualification *is* the access
    // declaration Ecs/System.hpp's Schedule derives conflict detection from, so a system's data
    // dependencies never need declaring twice. Deliberately depends only on Archetype.hpp, not
    // World.hpp — World.hpp includes this header, not the other way around.
    template <class... Ts>
    class Query {
      private:
        struct DirectAccessToken {
            explicit DirectAccessToken(std::shared_lock<std::shared_mutex> access) noexcept
                : access(std::move(access)) {}

            std::shared_lock<std::shared_mutex> access;
        };

      public:
        static constexpr usize ComponentCount = sizeof...(Ts);

        Query(std::vector<Archetype> *archetypes,
              std::vector<u32> archetype_indices,
              std::array<ComponentId, ComponentCount> ids,
              std::shared_lock<std::shared_mutex> direct_access_lock = {})
            : archetypes_(archetypes), archetype_indices_(std::move(archetype_indices)),
              ids_(ids) {
            if (direct_access_lock.owns_lock()) {
                direct_access_token_ = std::make_shared<DirectAccessToken>(std::move(direct_access_lock));
            }
        }

        Query(const Query &) = delete;
        Query &operator=(const Query &) = delete;
        Query(Query &&) noexcept = default;
        Query &operator=(Query &&) noexcept = default;

        // A stable row range within one matching archetype. Direct chunks share their Query's World
        // borrow token; Schedule chunks rely on Schedule's exclusive World borrow. A chunk is
        // move-only so one mutable row range cannot accidentally be dispatched more than once.
        class Chunk {
          public:
            Chunk(const Chunk &) = delete;
            Chunk &operator=(const Chunk &) = delete;
            Chunk(Chunk &&) noexcept = default;
            Chunk &operator=(Chunk &&) noexcept = default;

            [[nodiscard]] usize size() const noexcept {
                return static_cast<usize>(end_row_ - begin_row_);
            }

            template <class F>
            void each(F &&fn) const noexcept {
                static_assert(std::is_nothrow_invocable_v<F &, Entity, Ts &...>,
                              "ECS chunk callbacks must be noexcept and accept (Entity, Components&...).");
                for (u32 row = begin_row_; row < end_row_; ++row) {
                    invoke_row(fn, row, std::make_index_sequence<ComponentCount>{});
                }
            }

          private:
            friend class Query;

            Chunk(Archetype *archetype,
                  std::array<u32, ComponentCount> columns,
                  u32 begin_row,
                  u32 end_row,
                  std::shared_ptr<DirectAccessToken> direct_access_token) noexcept
                : archetype_(archetype), columns_(columns), begin_row_(begin_row), end_row_(end_row),
                  direct_access_token_(std::move(direct_access_token)) {}

            template <class F, usize... Is>
            void invoke_row(F &fn, u32 row, std::index_sequence<Is...>) const noexcept {
                std::invoke(fn,
                            archetype_->entity_at(row),
                            *static_cast<Ts *>(archetype_->row_pointer(columns_[Is], row))...);
            }

            Archetype *archetype_ = nullptr;
            std::array<u32, ComponentCount> columns_{};
            u32 begin_row_ = 0;
            u32 end_row_ = 0;
            std::shared_ptr<DirectAccessToken> direct_access_token_;
        };

        [[nodiscard]] std::vector<Chunk> chunks(usize minimum_rows_per_chunk, usize target_parallelism) const {
            minimum_rows_per_chunk = std::max<usize>(1, minimum_rows_per_chunk);
            target_parallelism = std::max<usize>(1, target_parallelism);

            usize total_rows = 0;
            for (u32 archetype_index : archetype_indices_) {
                const usize row_count = (*archetypes_)[archetype_index].size();
                if (row_count > std::numeric_limits<u32>::max()) {
                    Detail::contract_violation(
                        "ECS query archetype contains {} rows, exceeding the 32-bit row index contract.",
                        row_count);
                }
                if (row_count > std::numeric_limits<usize>::max() - total_rows) {
                    Detail::contract_violation(
                        "ECS query row count overflow while partitioning {} matching archetypes.",
                        archetype_indices_.size());
                }
                total_rows += row_count;
            }

            std::vector<Chunk> result;
            if (total_rows == 0) {
                return result;
            }

            const usize parallel_rows = total_rows / target_parallelism +
                                        static_cast<usize>(total_rows % target_parallelism != 0);
            const usize rows_per_chunk = std::max(minimum_rows_per_chunk, parallel_rows);

            usize chunk_count = 0;
            for (u32 archetype_index : archetype_indices_) {
                const usize row_count = (*archetypes_)[archetype_index].size();
                chunk_count += row_count / rows_per_chunk +
                               static_cast<usize>(row_count % rows_per_chunk != 0);
            }
            result.reserve(chunk_count);

            for (u32 archetype_index : archetype_indices_) {
                Archetype &archetype = (*archetypes_)[archetype_index];
                const usize row_count = archetype.size();
                if (row_count == 0) {
                    continue;
                }

                std::array<u32, ComponentCount> columns{};
                for (usize component_index = 0; component_index < ComponentCount; ++component_index) {
                    columns[component_index] = archetype.column_index_of(ids_[component_index]);
                    if (columns[component_index] == ~0u) {
                        Detail::contract_violation(
                            "ECS query plan corruption: matching archetype is missing dense component ID {}.",
                            ids_[component_index]);
                    }
                }

                for (usize begin = 0; begin < row_count;) {
                    const usize end = begin + std::min(rows_per_chunk, row_count - begin);
                    result.push_back(Chunk{
                        &archetype,
                        columns,
                        static_cast<u32>(begin),
                        static_cast<u32>(end),
                        direct_access_token_});
                    begin = end;
                }
            }
            return result;
        }

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

            Iterator(std::vector<Archetype> *archetypes, const std::vector<u32> *archetype_indices, const std::array<ComponentId, ComponentCount> *ids, usize archetype_position) noexcept
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
                    if (columns_[i] == ~0u) {
                        Detail::contract_violation(
                            "ECS query plan corruption: matching archetype is missing dense component ID {}.",
                            (*ids_)[i]);
                    }
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
        // Direct queries and chunks share a World borrow. Scheduled queries leave this empty because
        // Schedule already owns the World's exclusive execution borrow.
        std::shared_ptr<DirectAccessToken> direct_access_token_;
    };

} // namespace SFT::Ecs
