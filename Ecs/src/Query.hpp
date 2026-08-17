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

#include <tracy/Tracy.hpp>

namespace SFT::Ecs {


    template <class... Ts>
    class Query {
      private:
        struct DirectAccessToken {
            /// Constructs a `DirectAccessToken` from the supplied initialization values.
            ///
            /// @param access `access` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            explicit DirectAccessToken(std::shared_lock<std::shared_mutex> access) noexcept
                : access(std::move(access)) {}

            std::shared_lock<std::shared_mutex> access;
        };

      public:
        static constexpr usize ComponentCount = sizeof...(Ts);

        /// Constructs a `Query` from the supplied initialization values.
        ///
        /// @param archetypes `archetypes` value used by the operation.
        /// @param archetype_indices `archetype_indices` value used by the operation.
        /// @param ids `ids` value used by the operation.
        /// @param direct_access_lock `direct_access_lock` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Disables this construction form for `Query`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Query(const Query &) = delete;
        /// Assigns a new value to this `Query`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Query &operator=(const Query &) = delete;
        /// Constructs a `Query` from another instance.
        ///
        /// @note This function does not throw exceptions.
        Query(Query &&) noexcept = default;
        /// Assigns a new value to this `Query`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Query &operator=(Query &&) noexcept = default;


        class Chunk {
          public:
            /// Disables this construction form for `Chunk`.
            ///
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            Chunk(const Chunk &) = delete;
            /// Assigns a new value to this `Chunk`.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            Chunk &operator=(const Chunk &) = delete;
            /// Constructs a `Chunk` from another instance.
            ///
            /// @note This function does not throw exceptions.
            Chunk(Chunk &&) noexcept = default;
            /// Assigns a new value to this `Chunk`.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This function does not throw exceptions.
            Chunk &operator=(Chunk &&) noexcept = default;

            /// Returns the size for this `Chunk`.
            ///
            /// @return Returns the current size value.
            /// @note This function does not throw exceptions.
            /// Returns the size for this `Chunk`.
            ///
            /// @return Returns the current size value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] usize size() const noexcept {
                return static_cast<usize>(end_row_ - begin_row_);
            }

            /// Performs the each operation for `Chunk` using the supplied arguments.
            ///
            /// @note This function does not throw exceptions.
            template <class F>
            void each(F &&fn) const noexcept {
                ZoneScopedN("Query::Chunk::each");
                static_assert(std::is_nothrow_invocable_v<F &, Entity, Ts &...>,
                              "ECS chunk callbacks must be noexcept and accept (Entity, Components&...).");
                for (u32 row = begin_row_; row < end_row_; ++row) {
                    invoke_row(fn, row, std::make_index_sequence<ComponentCount>{});
                }
            }

          private:
            friend class Query;

            /// Constructs a `Chunk` from the supplied initialization values.
            ///
            /// @param archetype `archetype` value used by the operation.
            /// @param columns `columns` value used by the operation.
            /// @param begin_row `begin_row` value used by the operation.
            /// @param end_row `end_row` value used by the operation.
            /// @param direct_access_token `direct_access_token` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            Chunk(Archetype *archetype,
                  std::array<u32, ComponentCount> columns,
                  u32 begin_row,
                  u32 end_row,
                  std::shared_ptr<DirectAccessToken> direct_access_token) noexcept
                : archetype_(archetype), columns_(columns), begin_row_(begin_row), end_row_(end_row),
                  direct_access_token_(std::move(direct_access_token)) {}

            /// Performs the invoke row operation for `Chunk` using the supplied arguments.
            ///
            /// @note This function does not throw exceptions.
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

        /// Performs the chunks operation for `Query` using the supplied arguments.
        ///
        /// @param minimum_rows_per_chunk `minimum_rows_per_chunk` value used by the operation.
        /// @param target_parallelism `target_parallelism` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::vector<Chunk> chunks(usize minimum_rows_per_chunk, usize target_parallelism) const {
            ZoneScopedN("Query::chunks");
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
            /// Constructs a `Iterator` in its default state.
            ///
            /// @note This function does not throw exceptions.
            Iterator() noexcept = default;

            /// Compares the operands for inequality.
            ///
            /// @param other Other object used by the operation.
            ///
            /// @return Returns `true` when the operands differ; otherwise returns `false`.
            /// @note This function does not throw exceptions.
            [[nodiscard]] bool operator!=(const Iterator &other) const noexcept {
                return archetype_position_ != other.archetype_position_ || row_ != other.row_;
            }

            /// Advances the `Iterator` to its next value or element.
            ///
            /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function does not throw exceptions.
            Iterator &operator++() noexcept {
                ++row_;
                advance_to_valid();
                return *this;
            }

            /// Dereferences this iterator or handle.
            ///
            /// @return Returns the value or reference currently addressed by the iterator/handle.
            /// @note This function does not throw exceptions.
            [[nodiscard]] std::tuple<Entity, Ts &...> operator*() const noexcept {
                return dereference(std::make_index_sequence<ComponentCount>{});
            }

          private:
            friend class Query;

            /// Constructs a `Iterator` from the supplied initialization values.
            ///
            /// @param archetypes `archetypes` value used by the operation.
            /// @param archetype_indices `archetype_indices` value used by the operation.
            /// @param ids `ids` value used by the operation.
            /// @param archetype_position `archetype_position` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            Iterator(std::vector<Archetype> *archetypes, const std::vector<u32> *archetype_indices, const std::array<ComponentId, ComponentCount> *ids, usize archetype_position) noexcept
                : archetypes_(archetypes), archetype_indices_(archetype_indices), ids_(ids),
                  archetype_position_(archetype_position) {
                resolve_columns();
                advance_to_valid();
            }

            /// Resolves columns into the concrete value used by the engine.
            ///
            /// @note This function does not throw exceptions.
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


            /// Reports whether advance to valid is valid for the current operation.
            ///
            /// @note This function does not throw exceptions.
            void advance_to_valid() noexcept {
                while (archetype_position_ < archetype_indices_->size() &&
                       row_ >= (*archetypes_)[(*archetype_indices_)[archetype_position_]].size()) {
                    ++archetype_position_;
                    row_ = 0;
                    resolve_columns();
                }
            }

            /// Returns the current or globally available dereference value.
            ///
            /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function does not throw exceptions.
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

        /// Returns an iterator to the first element in the range.
        ///
        /// @return Returns an iterator referring to the first element.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Iterator begin() const noexcept {
            return Iterator(archetypes_, &archetype_indices_, &ids_, 0);
        }

        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @return Returns the one-past-the-end iterator.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Iterator end() const noexcept {
            Iterator it;
            it.archetype_position_ = archetype_indices_.size();
            return it;
        }

      private:
        std::vector<Archetype> *archetypes_;
        std::vector<u32> archetype_indices_;
        std::array<ComponentId, ComponentCount> ids_;


        std::shared_ptr<DirectAccessToken> direct_access_token_;
    };

} // namespace SFT::Ecs
