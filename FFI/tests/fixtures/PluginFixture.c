/* A native plugin written against nothing but Sturdy.h, the way a Rust cdylib, a Zig library or a NativeAOT assembly would be: it
 * exports `sturdy_plugin_entry`, describes itself, and adds two modules. It records what happens to it in the file named by the
 * STURDY_PLUGIN_TEST_LOG environment variable, because the test cannot read a closed library's memory after it unloads.
 *
 * STURDY_PLUGIN_TEST_REFUSE=1 makes the entry point add a module and then refuse to load. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FFI/Sturdy.h>

#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

static void record(const char *format, const char *text, long number) {
    const char *path = getenv("STURDY_PLUGIN_TEST_LOG");
    FILE *file = path != NULL ? fopen(path, "a") : NULL;
    if (file == NULL) {
        return;
    }
    fprintf(file, format, text, number);
    fputc('\n', file);
    fclose(file);
}

static SturdyBool STURDY_ABI_CALL on_initialized(SturdyEngine engine, void *user_data) {
    (void)engine;
    record("init:%s%.0ld", (const char *)user_data, 0);
    return STURDY_TRUE;
}

static void STURDY_ABI_CALL on_shutdown(SturdyEngine engine, void *user_data) {
    (void)engine;
    record("shutdown:%s%.0ld", (const char *)user_data, 0);
}

static void STURDY_ABI_CALL on_destroy(void *user_data) {
    record("destroy:%s%.0ld", (const char *)user_data, 0);
}

static int add(const SturdyPluginHost *host, const char *name, const char *dependency, int with_callbacks) {
    SturdyGameModule module;
    const char *after[1];
    memset(&module, 0, sizeof(module));
    module.struct_size = (uint32_t)sizeof(module);
    module.name = name;
    module.contributes_scene = STURDY_FALSE;
    if (dependency != NULL) {
        after[0] = dependency;
        module.after = after;
        module.after_count = 1;
    }
    module.logic.struct_size = (uint32_t)sizeof(SturdyGameLogic);
    module.logic.user_data = (void *)name;
    if (with_callbacks) {
        module.logic.on_engine_initialized = on_initialized;
        module.logic.on_shutdown = on_shutdown;
    }
    module.logic.destroy = on_destroy;
    return host->add_module(host->context, &module) == STURDY_OK;
}

FIXTURE_EXPORT SturdyResult STURDY_ABI_CALL sturdy_plugin_entry(const SturdyPluginHost *host, SturdyPluginReason reason, void *state) {
    if (reason == STURDY_PLUGIN_UNLOAD) {
        record("unload state=%s%.0ld", state != NULL ? "set" : "none", 0);
        if (state != NULL) {
            record("unload generation=%.0s%ld", "", *(int *)state);
            free(state);
        }
        return STURDY_OK;
    }

    record("load path=%s%.0ld", host->path, 0);
    if (host->abi_major != STURDY_ABI_VERSION_MAJOR) {
        return STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE;
    }
    host->set_info(host->context, "c", "fixture", "1.0");

    /* Generation counter carried across a reload through prior_state. */
    {
        int *generation = (int *)malloc(sizeof(int));
        *generation = host->prior_state != NULL ? *(int *)host->prior_state + 1 : 1;
        if (host->prior_state != NULL) {
            record("prior generation=%.0s%ld", "", *(int *)host->prior_state);
            free(host->prior_state);
        }
        host->set_state(host->context, generation);
    }

    if (!add(host, "fixture.a", NULL, 1) || !add(host, "fixture.b", "fixture.a", 0)) {
        return STURDY_ERROR_INTERNAL;
    }
    /* A null module must be refused by the host. */
    if (host->add_module(host->context, NULL) != STURDY_ERROR_INVALID_ARGUMENT) {
        return STURDY_ERROR_INTERNAL;
    }
    {
        const char *refuse = getenv("STURDY_PLUGIN_TEST_REFUSE");
        if (refuse != NULL && refuse[0] == '1') {
            return STURDY_ERROR_CALLBACK_FAILED;
        }
    }
    return STURDY_OK;
}
