#pragma once

#include <Reflection/TypeInfo.hpp>

#include <Foundation/Foundation.hpp>

#include <string_view>
#include <utility>
#include <vector>

namespace SFT::Reflection {


    /// Builds a `TypeInfo` at runtime, with no C++ type or `SFT_REFLECT_TYPE` behind it.
    ///
    /// `SFT_REFLECT_TYPE`/`FIELD`/`METHOD`/`EVENT` reflect an *existing* compiled C++ type — the
    /// right tool for a game's own code and for native (C++) mods, which have real types and
    /// member pointers to give the macros. A mod that defines a brand-new kind of thing entirely
    /// at its own runtime (a scripted mod adding an item type, say, with no corresponding C++
    /// struct anywhere) has no member pointers to offer, so it needs to describe fields/methods
    /// by hand instead — offsets, sizes, and type-erased accessor functions it supplies itself
    /// (e.g. into a buffer it allocates and owns). This builder is that path.
    ///
    /// Once `register_type`'d, a dynamically-built type is indistinguishable from a
    /// macro-reflected one to every other consumer of `TypeRegistry` — other mods can find it by
    /// name, read/write its fields, invoke its methods, subscribe to its events, or install
    /// overrides/hooks on it exactly as if it had been a first-party C++ type all along. This is
    /// what lets mods build on mods: the registry doesn't care where a `TypeInfo` came from.
    class TypeInfoBuilder {
      public:
        /// Starts building a type descriptor.
        ///
        /// @param canonical_name Stable name other code will look this type up by.
        /// @param size Size in bytes of one instance.
        /// @param align Required alignment of one instance.
        TypeInfoBuilder(std::string_view canonical_name, usize size, usize align) {
            info_.key = TypeId::from_name(canonical_name);
            info_.canonical_name = UString{canonical_name};
            info_.size = size;
            info_.align = align;
        }

        /// Sets the schema version (defaults to 1).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &schema_version(u32 version) noexcept {
            info_.schema_version = version;
            return *this;
        }

        /// Adds `flags` to this type's `TypeFlags` (combined with whatever was already set).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &flags(TypeFlags added_flags) noexcept {
            info_.flags = info_.flags | added_flags;
            return *this;
        }

        /// Records `base_type` as this type's reflected base, enabling
        /// `TypeRegistry::find_field`/`find_method`/`find_event` to fall through to it.
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &base(TypeId base_type) noexcept {
            info_.base_type = base_type;
            return *this;
        }

        /// Sets the construct/destroy function pointers and opaque `user_data` passed to each of
        /// them. `move_construct`/`destroy` are required (matches `Detail::make_type_info`'s
        /// invariant, enforced at `register_type`); `default_construct`/`copy_construct` may be
        /// null when the type does not support that operation.
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &constructors(TypeMoveConstructFn move_construct,
                                       TypeDestroyFn destroy,
                                       TypeDefaultConstructFn default_construct = nullptr,
                                       TypeCopyConstructFn copy_construct = nullptr,
                                       void *user_data = nullptr) noexcept {
            info_.move_construct = move_construct;
            info_.destroy = destroy;
            info_.default_construct = default_construct;
            info_.copy_construct = copy_construct;
            info_.user_data = user_data;
            return *this;
        }

        /// Declares one field.
        ///
        /// @param name Stable name other code will look this field up by.
        /// @param offset Byte offset from the start of an instance.
        /// @param size Size in bytes of the field's value.
        /// @param align Alignment required by the field's value.
        /// @param field_type Identifies the field's declared type (see `type_id_for`), used only
        /// for cross-checking by callers that care; never consulted by `copy_field_out`/`_in`.
        /// @param field_flags Set `FieldFlags::Trivial` when raw offset+memcpy access is safe;
        /// otherwise supply `copy_get`/`copy_set`.
        /// @param copy_get Required when `field_flags` omits `Trivial`.
        /// @param copy_set Required when `field_flags` omits `Trivial` and the field is writable.
        /// @param attributes Arbitrary tooling/mod-facing metadata (see `find_attribute`).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &field(std::string_view name,
                                usize offset,
                                usize size,
                                usize align,
                                TypeId field_type,
                                FieldFlags field_flags = FieldFlags::Trivial,
                                FieldCopyGetFn copy_get = nullptr,
                                FieldCopySetFn copy_set = nullptr,
                                std::vector<Attribute> attributes = {}) {
            info_.fields.push_back(FieldInfo{
                .key = TypeId::from_name(name),
                .name = UString{name},
                .field_type = field_type,
                .offset = offset,
                .size = size,
                .align = align,
                .flags = field_flags,
                .copy_get = copy_get,
                .copy_set = copy_set,
                .attributes = std::move(attributes),
            });
            return *this;
        }

        /// Declares one method.
        ///
        /// @param name Stable name other code will look this method up by.
        /// @param return_type Identifies the method's return type (`type_id_for<void>()` for none).
        /// @param param_types One `TypeId` per parameter, in call order.
        /// @param invoke The method's real implementation (see `MethodInvokeFn`).
        /// @param attributes Arbitrary tooling/mod-facing metadata (see `find_attribute`).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &method(std::string_view name,
                                 TypeId return_type,
                                 std::vector<TypeId> param_types,
                                 MethodInvokeFn invoke,
                                 std::vector<Attribute> attributes = {}) {
            MethodInfo method_info{};
            method_info.key = TypeId::from_name(name);
            method_info.name = UString{name};
            method_info.return_type = return_type;
            method_info.param_types = std::move(param_types);
            method_info.invoke = invoke;
            method_info.attributes = std::move(attributes);
            info_.methods.push_back(std::move(method_info));
            return *this;
        }

        /// Declares one event.
        ///
        /// @param name Stable name other code will look this event up by.
        /// @param param_types One `TypeId` per parameter `fire_event`'s `args` will carry, in order.
        /// @param attributes Arbitrary tooling/mod-facing metadata (see `find_attribute`).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &event(std::string_view name, std::vector<TypeId> param_types, std::vector<Attribute> attributes = {}) {
            info_.events.push_back(EventInfo{
                .key = TypeId::from_name(name),
                .name = UString{name},
                .param_types = std::move(param_types),
                .listeners = Multicast{},
                .attributes = std::move(attributes),
            });
            return *this;
        }

        /// Declares one static field. Same shape as `field`, except `offset` is always `0` and
        /// `address` supplies the field's one true address instead (see `FieldFlags::Static`).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &static_field(std::string_view name,
                                       void *address,
                                       usize size,
                                       usize align,
                                       TypeId field_type,
                                       FieldFlags field_flags = FieldFlags::Trivial,
                                       FieldCopyGetFn copy_get = nullptr,
                                       FieldCopySetFn copy_set = nullptr,
                                       std::vector<Attribute> attributes = {}) {
            info_.static_fields.push_back(FieldInfo{
                .key = TypeId::from_name(name),
                .name = UString{name},
                .field_type = field_type,
                .offset = 0,
                .size = size,
                .align = align,
                .flags = field_flags | FieldFlags::Static,
                .copy_get = copy_get,
                .copy_set = copy_set,
                .attributes = std::move(attributes),
                .static_address = address,
            });
            return *this;
        }

        /// Declares one static method. Same shape as `method`; `invoke`'s `object` parameter is
        /// ignored (call through `invoke_static_method`, which always passes `nullptr`).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &static_method(std::string_view name,
                                        TypeId return_type,
                                        std::vector<TypeId> param_types,
                                        MethodInvokeFn invoke,
                                        std::vector<Attribute> attributes = {}) {
            MethodInfo method_info{};
            method_info.key = TypeId::from_name(name);
            method_info.name = UString{name};
            method_info.return_type = return_type;
            method_info.param_types = std::move(param_types);
            method_info.invoke = invoke;
            method_info.attributes = std::move(attributes);
            method_info.is_static = true;
            info_.static_methods.push_back(std::move(method_info));
            return *this;
        }

        /// Declares one parameterized constructor.
        ///
        /// @param param_types One `TypeId` per parameter, in call order.
        /// @param invoke Placement-constructs into the destination (see `ConstructorInvokeFn`).
        /// @param attributes Arbitrary tooling/mod-facing metadata (see `find_attribute`).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &constructor(std::vector<TypeId> param_types, ConstructorInvokeFn invoke, std::vector<Attribute> attributes = {}) {
            info_.constructors.push_back(ConstructorInfo{
                .param_types = std::move(param_types),
                .invoke = invoke,
                .attributes = std::move(attributes),
            });
            return *this;
        }

        /// Adds one attribute to the type itself (as opposed to one of its fields/methods/events).
        ///
        /// @return Returns `*this` so calls can be chained.
        TypeInfoBuilder &type_attribute(Attribute attribute) {
            info_.attributes.push_back(std::move(attribute));
            return *this;
        }

        /// Finishes building. Pass the result straight to `TypeRegistry::register_type`; the
        /// builder is left holding a moved-from `TypeInfo` afterwards and should not be reused.
        ///
        /// @return Returns the finished descriptor.
        [[nodiscard]] TypeInfo build() noexcept {
            return std::move(info_);
        }

      private:
        TypeInfo info_{};
    };


} // namespace SFT::Reflection
