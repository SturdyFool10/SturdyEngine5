#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <Ecs/Component.hpp>
#include <Ecs/ComponentSchema.hpp>
#include <Foundation/Foundation.hpp>

namespace SFT::Engine {

    /// The argument checks of `register_foreign_component`, available separately so a binding can reject bad arguments before it
    /// touches any engine state (the C ABI promises that ordering).
    ///
    /// @return Why the arguments are invalid, or an empty optional when they are fine.
    [[nodiscard]] std::optional<std::string> validate_foreign_component(std::string_view name, u32 size, u32 align,
                                                                        const Ecs::ComponentSchema *schema);

    /// Registers a plain-data component on behalf of code that cannot supply C++ construction and destruction (Rust, C#, Java,
    /// scripts), applying the rules every binding must share, so the C ABI and CxxApi accept and reject exactly the same things
    /// with the same messages:
    ///
    /// - the name is non-empty, the size nonzero, the alignment a power of two no greater than 64;
    /// - if given, the fields fit the size and do not overlap;
    /// - registering a name again returns its id when size and alignment match, and fails when they differ (live entities are
    ///   already laid out the old way);
    /// - when both registrations describe their fields they must agree on kinds, offsets and counts (names are ignored); a
    ///   mismatch names the first field that differs;
    /// - a registration without fields is accepted against any layout;
    /// - a C++ component with no shape yet gets one from a reflection type of the same name, if one is registered.
    ///
    /// The component is raw bytes: copied with memcpy, destroyed with no cleanup, zero-initialized by default.
    ///
    /// @param schema The fields, or null / empty when the caller did not describe them.
    ///
    /// @return The component's id, or why registration was refused.
    [[nodiscard]] std::expected<Ecs::ComponentId, std::string> register_foreign_component(Ecs::ComponentRegistry &registry,
                                                                                          std::string_view name,
                                                                                          u32 size,
                                                                                          u32 align,
                                                                                          const Ecs::ComponentSchema *schema);

} // namespace SFT::Engine
