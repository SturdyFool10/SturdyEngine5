#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <Ecs/ComponentSchema.hpp>
#include <Ecs/World.hpp>
#include <Foundation/Foundation.hpp>

namespace SFT::Engine {

    /// Resources and event channels created on behalf of code that cannot own C++ storage (Rust, C#, Java, scripts), shared by
    /// every binding so they behave identically: the engine allocates and owns the bytes, binds them into the world under a
    /// canonical name, and frees them on destruction (or at process exit).
    ///
    /// Two languages sharing a resource or an event channel must agree on what is in it, not just how big it is: re-creating one
    /// under the same name must give the same size and, when both calls describe their fields (`Ecs::ComponentSchema`), the same
    /// layout (kinds, offsets, counts; names are ignored). A mismatch names the first field that differs. Alignment is not part
    /// of the layout because the engine allocates the storage.
    enum class ForeignResourceErrorKind {
        /// A bad argument or a conflicting re-creation.
        InvalidArgument,
        /// Nothing (of the right sort) is bound under that key.
        NotAvailable,
        /// An index past the end.
        OutOfRange,
    };

    struct ForeignResourceError {
        ForeignResourceErrorKind kind = ForeignResourceErrorKind::InvalidArgument;
        std::string message;
    };

    template <class Value>
    using ForeignResourceResult = std::expected<Value, ForeignResourceError>;

    /// The argument checks of `create_foreign_resource`, available separately so a binding can reject bad arguments before it
    /// touches any engine state. @return Why they are invalid, or empty when fine.
    [[nodiscard]] std::optional<std::string> validate_foreign_resource(std::string_view name, u32 size, const Ecs::ComponentSchema *schema);
    /// As above for `create_foreign_event_channel` (`event_size` is the size of one event).
    [[nodiscard]] std::optional<std::string> validate_foreign_event_channel(std::string_view name, u32 event_size,
                                                                            const Ecs::ComponentSchema *schema);

    /// Creates a resource of `size` bytes (zeroed, or copied from `initial`) and binds it into `world`. Creating it again with the
    /// same size and layout succeeds and changes nothing.
    [[nodiscard]] ForeignResourceResult<Ecs::ResourceKey> create_foreign_resource(Ecs::World &world, std::string_view name, u32 size,
                                                                                  const void *initial,
                                                                                  const Ecs::ComponentSchema *schema);
    /// Creates an event channel of fixed-size records the scheduler drains every frame: events sent during a frame are readable
    /// for the rest of it and gone by the next.
    [[nodiscard]] ForeignResourceResult<Ecs::ResourceKey> create_foreign_event_channel(Ecs::World &world, std::string_view name,
                                                                                       u32 event_size,
                                                                                       const Ecs::ComponentSchema *schema);
    /// Destroys a resource or channel created here. NotAvailable for anything else (the engine's own C++ resources are not ours
    /// to free).
    [[nodiscard]] ForeignResourceResult<void> destroy_foreign_resource(Ecs::World &world, Ecs::ResourceKey key);

    /// Copies a bound resource's bytes out / overwrites them. Works for any bound resource, the engine's own included; `size` must
    /// equal the resource's size.
    [[nodiscard]] ForeignResourceResult<void> read_resource(Ecs::World &world, Ecs::ResourceKey key, void *out, u32 size);
    [[nodiscard]] ForeignResourceResult<void> write_resource(Ecs::World &world, Ecs::ResourceKey key, const void *data, u32 size);

    /// Appends one event; `size` must equal the channel's event size.
    [[nodiscard]] ForeignResourceResult<void> send_foreign_event(Ecs::World &world, Ecs::ResourceKey channel, const void *event, u32 size);
    [[nodiscard]] ForeignResourceResult<u32> foreign_event_count(Ecs::World &world, Ecs::ResourceKey channel);
    /// Copies event `index` out; OutOfRange when there is no such event.
    [[nodiscard]] ForeignResourceResult<void> read_foreign_event(Ecs::World &world, Ecs::ResourceKey channel, u32 index, void *out, u32 size);
    [[nodiscard]] ForeignResourceResult<void> clear_foreign_events(Ecs::World &world, Ecs::ResourceKey channel);

} // namespace SFT::Engine
