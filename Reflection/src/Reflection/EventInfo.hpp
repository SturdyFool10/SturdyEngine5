#pragma once

#include <Reflection/Attribute.hpp>
#include <Reflection/Multicast.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <vector>

namespace SFT::Reflection {


    /// A named hook point a reflected type declares (via `SFT_REFLECT_EVENT`) without any
    /// backing C++ member — game code fires it explicitly (`SFT_REFLECT_FIRE_EVENT`) at whatever
    /// point in its logic it chooses, and any number of mods can listen without needing to
    /// replace a method the way `MethodInfo::override_fn` does. This is the Java-`EventListener`
    /// analog: many independent subscribers, none of which can clobber another.
    struct EventInfo {
        TypeId key{};
        UString name;
        std::vector<TypeId> param_types;
        Multicast listeners;
        /// Arbitrary tooling/mod-facing metadata. See `find_attribute`.
        std::vector<Attribute> attributes;
    };

    /// Fires `event` on `object` with `args`, calling every subscribed listener in registration
    /// order. A no-op when nobody has subscribed — see `Multicast::fire`.
    inline void fire_event(const EventInfo &event, void *object, const void *const *args) noexcept {
        event.listeners.fire(object, args);
    }


} // namespace SFT::Reflection
