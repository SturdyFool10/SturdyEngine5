/* A template for bringing an interpreter (Lua, QuickJS, V8, Python ...) into the engine as a script language.
 *
 * The engine ships no interpreter. It ships `SturdyScriptLanguage`: you register one, and its `load` turns source text into a
 * `SturdyScriptInstance` whose callbacks drive your interpreter. Everything between "INTERPRETER:" markers is where your runtime goes; the rest
 * is the contract and is complete. Read the "Scripts" section of FFI/src/FFI/Sturdy.h and plans/scripting.md first.
 *
 * Rules an adapter must keep:
 *  - Every callback runs with the engine's script lock held, on one thread at a time: the interpreter needs no locking of its own. The thread may
 *    differ between calls (`request_render_frame` can be a render thread), so use an interpreter that tolerates a change of thread, or
 *    re-enter it with its own "attach thread" call.
 *  - Callbacks must not unwind (longjmp / C++ exceptions / panics). Turn an interpreter error into STURDY_FALSE plus a reason in `error`
 *    (`sturdy_bytes_sink_set`) or log it and return; an exception escaping a callback marks the script unhealthy.
 *  - `update` runs at the update boundary: the whole engine API is safe to call from it.
 *  - A script that fails to parse must fail in `load`, not in `start`, so hot reload can keep the old version running.
 */

#include <stdlib.h>
#include <string.h>

#include <FFI/Sturdy.h>

typedef struct AdapterContext {
    /* INTERPRETER: a shared runtime / library handle, created once when the language is registered (e.g. a JSRuntime*). */
    int placeholder;
} AdapterContext;

typedef struct ScriptContext {
    AdapterContext *adapter;
    /* INTERPRETER: this script's own state (a lua_State*, a JSContext*) and the references to its entry points. */
    char *name;
} ScriptContext;

static void fail(SturdyBytesSink *error, const char *reason) { (void)sturdy_bytes_sink_set(error, (const uint8_t *)reason, strlen(reason)); }

static SturdyBool STURDY_ABI_CALL script_start(SturdyEngine engine, void *user_data, SturdyBytesSink *error) {
    ScriptContext *script = (ScriptContext *)user_data;
    (void)engine;
    (void)script;
    /* INTERPRETER: run the script's top level / call its `start()` (it may use the engine API with `engine`). */
    (void)error; /* on failure: fail(error, "reason"); return STURDY_FALSE; */
    return STURDY_TRUE;
}

static void STURDY_ABI_CALL script_update(SturdyEngine engine, const SturdyScriptTick *tick, void *user_data) {
    (void)engine;
    (void)tick;
    (void)user_data;
    /* INTERPRETER: call the script's `update(delta_seconds, elapsed_seconds, tick)`. */
}

static void STURDY_ABI_CALL script_stop(SturdyEngine engine, void *user_data) {
    (void)engine;
    (void)user_data;
    /* INTERPRETER: call the script's `stop()`. Do not free here: `destroy` does. */
}

static SturdyBool STURDY_ABI_CALL script_save(void *user_data, SturdyBytesSink *out) {
    (void)user_data;
    (void)out;
    /* INTERPRETER: if the script defines `save_state()`, serialize its result with `sturdy_bytes_sink_set(out, bytes, size)` and return TRUE. */
    return STURDY_FALSE; /* no state */
}

static SturdyBool STURDY_ABI_CALL script_restore(void *user_data, const uint8_t *state, size_t size) {
    (void)user_data;
    (void)state;
    (void)size;
    /* INTERPRETER: hand the bytes to the script's `restore_state()`. Return FALSE to refuse (the reload then fails). */
    return STURDY_TRUE;
}

static SturdyBool STURDY_ABI_CALL script_call(void *user_data, const char *function, const uint8_t *args, size_t args_size, SturdyBytesSink *result,
                                              SturdyBytesSink *error) {
    (void)user_data;
    (void)args;
    (void)args_size;
    (void)result;
    /* INTERPRETER: look `function` up among the script's exported functions, call it with `args` as a string/buffer, write the return value to
     * `result` with `sturdy_bytes_sink_set`. */
    fail(error, function != NULL ? "no such function" : "no function name");
    return STURDY_FALSE;
}

static void STURDY_ABI_CALL script_message(void *user_data, const SturdyScriptMessage *message) {
    (void)user_data;
    (void)message;
    /* INTERPRETER: call the script's `on_message(from, topic, payload)`. */
}

static void STURDY_ABI_CALL script_destroy(void *user_data) {
    ScriptContext *script = (ScriptContext *)user_data;
    /* INTERPRETER: close the script's state (lua_close, JS_FreeContext). */
    free(script->name);
    free(script);
}

static SturdyBool STURDY_ABI_CALL language_load(void *user_data, const SturdyScriptSource *source, SturdyScriptInstance *out_instance, SturdyBytesSink *error) {
    ScriptContext *script = (ScriptContext *)calloc(1, sizeof(ScriptContext));
    if (script == NULL) {
        fail(error, "out of memory");
        return STURDY_FALSE;
    }
    script->adapter = (AdapterContext *)user_data;
    script->name = strdup(source->name);
    /* INTERPRETER: create the script's state, compile `source->code` (or read `source->path` when code is empty) and look up its entry points.
     * Anything that can be wrong with the script should fail HERE: on a parse error call fail(error, message), free everything, return FALSE. */
    memset(out_instance, 0, sizeof(*out_instance));
    out_instance->struct_size = sizeof(SturdyScriptInstance);
    out_instance->user_data = script;
    out_instance->start = script_start;
    out_instance->update = script_update;
    out_instance->stop = script_stop;
    out_instance->save_state = script_save;
    out_instance->restore_state = script_restore;
    out_instance->call = script_call;
    out_instance->on_message = script_message;
    out_instance->destroy = script_destroy;
    return STURDY_TRUE;
}

static void STURDY_ABI_CALL language_destroy(void *user_data) {
    /* INTERPRETER: shut the shared runtime down. */
    free(user_data);
}

/* Call once at startup, before the game runs. Afterwards `sturdy_script_load` / `sturdy_script_module_create` accept `language == "mylang"` or a
 * path ending in ".my". */
SturdyResult register_my_language(void) {
    static const char *const extensions[] = {".my"};
    AdapterContext *context = (AdapterContext *)calloc(1, sizeof(AdapterContext));
    SturdyScriptLanguage language;
    SturdyResult result;
    if (context == NULL) return STURDY_ERROR_OUT_OF_MEMORY;
    /* INTERPRETER: create the shared runtime here. */
    memset(&language, 0, sizeof(language));
    language.struct_size = sizeof(SturdyScriptLanguage);
    language.name = "mylang";
    language.extensions = extensions;
    language.extension_count = 1;
    language.user_data = context;
    language.load = language_load;
    language.destroy = language_destroy;
    result = sturdy_script_language_register(&language);
    if (result != STURDY_OK) free(context); /* a refused registration took nothing */
    return result;
}
