#!/usr/bin/env python3
"""A game made of two Python modules running on the real engine, through the generated binding.

    STURDY_LIBRARY=/path/to/libFFI.so python3 two_modules.py          # opens a window; closes itself after ~2 seconds

It shows the multi-module runtime (`sturdy_runtime_run_modules`) from a language other than C++:

  * "gameplay" supplies the scene (the engine's default frame: its camera and lighting), counts frames, and asks the window to close.
  * "hud" starts after "gameplay", contributes no scene of its own (`contributes_scene = False`: overlay only), and draws a panel with
    the frame count through the immediate-mode UI (`sturdy_ui_*`) on top of whatever the scene module produced.

Frame callbacks arrive on the engine's render thread, so a real game would keep per-module state thread-safe; here each module only
touches its own counter. Needs a GPU (or a software Vulkan driver) and a display (xvfb works).
"""

import ctypes
import os
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import sturdy  # noqa: E402  (the generated binding lives one directory up)

FRAMES_BEFORE_CLOSE = 90

lib = sturdy.load()
log = []
state = {"gameplay_frames": 0, "hud_frames": 0, "closed": False, "hud_ok": True, "init": []}
lock = threading.Lock()


# ---- gameplay: the scene module ----
def gameplay_init(engine, _user):
    state["init"].append("gameplay")
    return 1


def gameplay_frame(engine, surface, frame_input, frame, _user):
    with lock:
        state["gameplay_frames"] += 1
        count = state["gameplay_frames"]
    if count == FRAMES_BEFORE_CLOSE and not state["closed"]:
        state["closed"] = True
        # Ask the window to close; the runtime then shuts the modules down in reverse order.
        sturdy.check(lib, lib.sturdy_window_request_close(engine, surface, None))
    return 1  # render the engine's default frame


# ---- hud: an overlay-only module ----
def hud_init(engine, _user):
    state["init"].append("hud")
    return 1


def hud_frame(engine, surface, frame_input, frame, _user):
    ok = lib.sturdy_ui_begin(engine, frame_input) == sturdy.SturdyResult.STURDY_OK
    if ok:
        element = sturdy.SturdyUiElement()
        lib.sturdy_ui_element_init(ctypes.byref(element))
        element.width_kind, element.width_value = sturdy.SturdyUiSizing.STURDY_UI_SIZING_FIXED, 260
        element.padding_left = element.padding_right = element.padding_top = element.padding_bottom = 8
        element.background[:] = (0.05, 0.06, 0.08, 0.85)
        element.id = b"hud-panel"
        ok = lib.sturdy_ui_begin_element(engine, ctypes.byref(element)) == sturdy.SturdyResult.STURDY_OK
        style = sturdy.SturdyUiTextStyle()
        lib.sturdy_ui_text_style_init(ctypes.byref(style))
        with lock:
            state["hud_frames"] += 1
            label = f"hud (python)  frame {state['hud_frames']}".encode()
        ok = ok and lib.sturdy_ui_text(engine, label, ctypes.byref(style)) == sturdy.SturdyResult.STURDY_OK
        ok = ok and lib.sturdy_ui_end_element(engine) == sturdy.SturdyResult.STURDY_OK
        ok = ok and lib.sturdy_ui_end(engine, frame) == sturdy.SturdyResult.STURDY_OK
    if not ok:
        state["hud_ok"] = False
    return 0  # overlay-only: contributes no scene


def shutdown(_engine, _user):
    log.append("shutdown")


def make_logic(init, frame):
    table = sturdy.SturdyGameLogic()
    table.struct_size = ctypes.sizeof(table)
    # The callback slots have named types: wrap a Python function and keep a reference so it is not collected.
    keep = (
        sturdy.SturdyGameLogic_on_engine_initialized_fn(init),
        sturdy.SturdyGameLogic_request_render_frame_fn(frame),
        sturdy.SturdyGameLogic_on_shutdown_fn(shutdown),
    )
    table.on_engine_initialized, table.request_render_frame, table.on_shutdown = keep
    return table, keep


def main():
    config = sturdy.SturdyRuntimeConfig()
    sturdy.check(lib, lib.sturdy_runtime_config_init(ctypes.byref(config)))
    config.app_name = b"two python modules"
    config.window_title = b"two python modules"
    config.window_width, config.window_height = 960, 540

    gameplay_table, gameplay_keep = make_logic(gameplay_init, gameplay_frame)
    hud_table, hud_keep = make_logic(hud_init, hud_frame)
    after = (ctypes.c_char_p * 1)(b"gameplay")

    modules = (sturdy.SturdyGameModule * 2)()
    modules[0].struct_size = ctypes.sizeof(sturdy.SturdyGameModule)
    modules[0].name, modules[0].contributes_scene, modules[0].logic = b"gameplay", 1, gameplay_table
    modules[1].struct_size = ctypes.sizeof(sturdy.SturdyGameModule)
    modules[1].name, modules[1].contributes_scene, modules[1].logic = b"hud", 0, hud_table
    modules[1].after, modules[1].after_count = ctypes.cast(after, ctypes.POINTER(ctypes.c_char_p)), 1

    exit_code = ctypes.c_int32(-1)
    result = lib.sturdy_runtime_run_modules(ctypes.byref(config), modules, 2, 0, None, ctypes.byref(exit_code))
    print(f"runtime returned {sturdy.SturdyResult(result).name}, exit code {exit_code.value}")
    print(f"modules started in order: {state['init']}")
    print(f"gameplay frames: {state['gameplay_frames']}, hud frames: {state['hud_frames']}, hud drew without error: {state['hud_ok']}")
    print(f"shutdown callbacks: {len(log)}")
    del gameplay_keep, hud_keep
    return 0 if result == sturdy.SturdyResult.STURDY_OK and state["gameplay_frames"] >= FRAMES_BEFORE_CLOSE and state["hud_ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
