/* A hot-reloadable module written in plain C: it never sees the C++ TypeRegistry, only the function
 * table the host passes in. HotReloadTest loads it to prove a non-C++ module can register types. */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

/* Mirror of SturdyModuleReflectionApi (Engine/HotReloadableModule.hpp). */
typedef struct ModuleApi {
    uint32_t struct_size;
    uint32_t abi_version;
    void *registry;
    void *(*type_builder_create)(void *registry, const char *canonical_name, uint32_t size, uint32_t align);
    void (*type_builder_set_constructors)(void *builder, void (*move_construct)(void *, void *, void *),
                                          void (*destroy)(void *, void *), void (*default_construct)(void *, void *), void *user_data);
    void (*type_builder_add_field)(void *builder, const char *name, uint32_t offset, uint32_t size, uint32_t align, uint32_t primitive_kind);
    int (*type_builder_finish)(void *builder);
    void (*type_builder_discard)(void *builder);
    int (*unregister_type)(void *registry, const char *canonical_name);
} ModuleApi;

typedef struct CItem {
    int32_t hp;
    float weight;
} CItem;

static void move_construct(void *destination, void *source, void *user) {
    (void)user;
    *(CItem *)destination = *(CItem *)source;
}
static void destroy(void *object, void *user) {
    (void)object;
    (void)user;
}
static void default_construct(void *destination, void *user) {
    (void)user;
    ((CItem *)destination)->hp = 0;
    ((CItem *)destination)->weight = 0.0f;
}

EXPORT void sturdy_module_register_types_c(const ModuleApi *api) {
    void *builder = api->type_builder_create(api->registry, "hotreload.c_item", sizeof(CItem), _Alignof(CItem));
    api->type_builder_set_constructors(builder, move_construct, destroy, default_construct, NULL);
    api->type_builder_add_field(builder, "hp", offsetof(CItem, hp), sizeof(int32_t), _Alignof(int32_t), 2);
    api->type_builder_add_field(builder, "weight", offsetof(CItem, weight), sizeof(float), _Alignof(float), 4);
    (void)api->type_builder_finish(builder);
}

EXPORT void sturdy_module_unregister_types_c(const ModuleApi *api) {
    (void)api->unregister_type(api->registry, "hotreload.c_item");
}

EXPORT int32_t sturdy_module_version(void) { return 7; }
