/* An overlay-only HUD module as a native plugin written in C, using only Sturdy.h.
 *
 *     cc -shared -fPIC -O2 -I FFI/src FFI/examples/plugins/hud_plugin.c -o libhud_plugin.so
 *
 * The engine functions it calls are left undefined and resolved at load time against the engine library already in the process, so
 * the plugin shares the host's engine (and its ECS, UI state and window) instead of carrying a copy. See two_plugins.py for the host. */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <FFI/Sturdy.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

static atomic_uint g_frames;

static SturdyBool STURDY_ABI_CALL hud_frame(SturdyEngine engine, SturdySurface surface, const SturdyFrameInput *input, SturdyFrame frame,
                                            void *user_data) {
    SturdyUiElement element;
    SturdyUiTextStyle style;
    char label[64];
    (void)surface;
    (void)user_data;
    if (sturdy_ui_begin(engine, input) != STURDY_OK) {
        return STURDY_FALSE;
    }
    sturdy_ui_element_init(&element);
    element.width_kind = STURDY_UI_SIZING_FIXED;
    element.width_value = 300;
    element.padding_left = element.padding_right = element.padding_top = element.padding_bottom = 8;
    element.background[0] = 0.05f;
    element.background[1] = 0.06f;
    element.background[2] = 0.08f;
    element.background[3] = 0.85f;
    element.id = "hud-panel";
    sturdy_ui_text_style_init(&style);
    snprintf(label, sizeof(label), "hud (C plugin)  frame %u", atomic_fetch_add(&g_frames, 1) + 1);
    if (sturdy_ui_begin_element(engine, &element) == STURDY_OK) {
        (void)sturdy_ui_text(engine, label, &style);
        (void)sturdy_ui_end_element(engine);
    }
    (void)sturdy_ui_end(engine, frame);
    return STURDY_FALSE; /* overlay only: contributes no scene */
}

static void STURDY_ABI_CALL hud_shutdown(SturdyEngine engine, void *user_data) {
    (void)engine;
    (void)user_data;
    fprintf(stderr, "hud (C plugin): shutdown after %u frames\n", atomic_load(&g_frames));
}

EXPORT SturdyResult STURDY_ABI_CALL sturdy_plugin_entry(const SturdyPluginHost *host, SturdyPluginReason reason, void *state) {
    SturdyGameModule module;
    const char *after[1] = {"gameplay"};
    (void)state;
    if (reason != STURDY_PLUGIN_LOAD) {
        return STURDY_OK;
    }
    host->set_info(host->context, "c", "hud", "1.0");
    memset(&module, 0, sizeof(module));
    module.struct_size = (uint32_t)sizeof(module);
    module.name = "hud";
    module.after = after;
    module.after_count = 1;
    module.contributes_scene = STURDY_FALSE;
    module.logic.struct_size = (uint32_t)sizeof(SturdyGameLogic);
    module.logic.request_render_frame = hud_frame;
    module.logic.on_shutdown = hud_shutdown;
    return host->add_module(host->context, &module);
}
