#!/usr/bin/env python3
"""A game whose modules live in native plugins written in two other languages, hosted from Python.

    STURDY_LIBRARY=/path/to/libFFI.so python3 two_plugins.py <rust-plugin.so> <c-plugin.so>

  * The Rust plugin (Rust/examples/plugin_demo, built with the `sturdy-plugin` crate) supplies the scene and asks the window to close
    after 90 frames.
  * The C plugin (FFI/examples/plugins/hud_plugin.c) is an overlay-only HUD that starts after "gameplay".

The host (this script) never touches a frame: it loads both libraries, hands their modules to `sturdy_runtime_run_modules`, and releases
the plugins afterwards. Both plugins call the engine's C functions directly (resolved against the engine library this script loaded), so
they share one engine with each other. Needs a GPU (or a software Vulkan driver) and a display (xvfb works).
"""

import ctypes
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import sturdy  # noqa: E402

# ctypes opens the engine library privately; `sturdy_plugin_load` promotes it to the global symbol scope itself, so the plugins'
# undefined engine functions resolve to this same copy.
lib = sturdy.load()


def load_plugin(path, shadow=True):
    desc = sturdy.SturdyPluginLoadDesc()
    desc.struct_size = ctypes.sizeof(desc)
    desc.flags = sturdy.SturdyPluginLoadFlags.STURDY_PLUGIN_SHADOW_COPY if shadow else 0
    desc.path = os.fsencode(path)
    plugin = sturdy.SturdyPlugin()
    sturdy.check(lib, lib.sturdy_plugin_load(ctypes.byref(desc), ctypes.byref(plugin)))
    return plugin


def text(getter, plugin):
    buffer = ctypes.create_string_buffer(128)
    length = ctypes.c_size_t()
    sturdy.check(lib, getter(plugin, buffer, ctypes.sizeof(buffer), ctypes.byref(length)))
    return buffer.value.decode()


def main(rust_path, c_path):
    plugins = [load_plugin(rust_path), load_plugin(c_path)]
    for plugin in plugins:
        count = ctypes.c_uint32()
        sturdy.check(lib, lib.sturdy_plugin_module_count(plugin, ctypes.byref(count)))
        print(f"plugin {text(lib.sturdy_plugin_name, plugin)!r} ({text(lib.sturdy_plugin_language, plugin)}): {count.value} module(s)")

    modules = (sturdy.SturdyGameModule * 2)()
    offset = 0
    for plugin in plugins:
        count = ctypes.c_uint32()
        view = (sturdy.SturdyGameModule * (2 - offset)).from_buffer(modules, offset * ctypes.sizeof(sturdy.SturdyGameModule))
        sturdy.check(lib, lib.sturdy_plugin_take_modules(plugin, view, 2 - offset, ctypes.byref(count)))
        offset += count.value
    assert offset == 2, offset

    config = sturdy.SturdyRuntimeConfig()
    sturdy.check(lib, lib.sturdy_runtime_config_init(ctypes.byref(config)))
    config.app_name = b"two plugins"
    config.window_title = b"two plugins"
    config.window_width, config.window_height = 960, 540
    exit_code = ctypes.c_int32(-1)
    result = lib.sturdy_runtime_run_modules(ctypes.byref(config), modules, offset, 0, None, ctypes.byref(exit_code))
    print(f"runtime returned {sturdy.SturdyResult(result).name}, exit code {exit_code.value}")

    # The modules are destroyed by now; releasing the handles lets each library close.
    for plugin in plugins:
        sturdy.check(lib, lib.sturdy_plugin_release(plugin))
    return 0 if result == sturdy.SturdyResult.STURDY_OK else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
