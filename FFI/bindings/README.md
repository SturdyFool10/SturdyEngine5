# Language bindings for the C ABI

`FFI/src/FFI/Sturdy.h` is the one source of truth. Everything here is generated from it, so a function added to the header appears in
every language and no binding can disagree with another about a type.

| Language | File | Mechanism | Verified in CI |
|---|---|---|---|
| Python 3 | `python/sturdy.py` | `ctypes` | struct layout vs the real header, every function called through a stub |
| Java 22+ | `java/dev/sturdy/Sturdy.java` | Panama (`java.lang.foreign`) | same, including every method-handle descriptor |
| C# | `csharp/Sturdy.cs` | P/Invoke + function pointers (`unsafe`) | **generated only**: there is no .NET toolchain on the build machines |
| Rust | `rust/sturdy_ffi.rs` | raw `extern "C"` declarations + `#[repr(C)]` structs | compiled with `rustc`; every struct carries const size/alignment/offset assertions |
| any | `../api/sturdy_abi.json` | machine-readable description | regenerated and diffed |

The first-party Rust crates (`Rust/sturdy`) use `cxx` over `CxxApi/` (see `plans/cxx-api.md`). The Rust binding here exists for **native
plugins** (`Rust/sturdy-plugin`, a safe layer over it): a plugin must call the engine that loaded it, not link a second copy, so it
declares the C functions and leaves them undefined for the loader to resolve.

## How it works

```
Sturdy.h --clang AST--> api/sturdy_abi.json --gen_bindings.py--> python / java / csharp / rust
```

* `tools/describe_abi.py` asks clang for the header's AST and writes every enum, struct, callback type and function with its
  documentation as JSON. It never parses C by hand.
* `tools/gen_bindings.py` turns that JSON into each language. Struct layout is computed from the C rules (natural alignment; the header
  has no packing or bit-fields) and cross-checked against the real header, see below.
* `tests/bindings/check_bindings.py` (CTest name `FfiBindingsTest`) regenerates everything and **fails if a committed file differs**, then
  compares every struct's size, alignment and field offsets with a C program built from the real header, and calls every function in
  each binding through a stub library. Java is skipped when no JDK 22+ is installed.

## Verified against the real engine

Beyond the CI checks above, the Python and Java bindings were run against a real `libFFI.so` (configure with
`-DSTURDY_BUILD_SHARED_LIBS=ON`; needs no other flags):

* **Python:** `python/examples/two_modules.py` runs a whole game made of two Python modules on the real engine under a virtual display
  (`STURDY_LIBRARY=.../libFFI.so xvfb-run python3 two_modules.py`): a scene module and an overlay-only HUD module that draws UI, started
  in dependency order, 90 frames each, closed by a window-close request, both shut down, runtime result `STURDY_OK`.
* **Java:** version negotiation, a pure function, an error message, and a struct filled in by the engine, with the same derived resource id
  as Python (`0xe607f7eb08030967`) — two languages agreeing on a hash the engine computes.

That run needs a GPU (or a software Vulkan driver) and a display, so it is an example rather than a CTest.

## Changing the ABI

Edit `Sturdy.h`, then regenerate and commit the result:

```
python3 FFI/tests/bindings/check_bindings.py --repo . --update
```

The test fails on a stale file, so a header change without regeneration cannot land.

## Stack traces across the language boundary

A failed call returns a `SturdyResult`; `sturdy_last_error_message()` says what went wrong and the **trace** says where, in both
languages. A trace is a list of frames, innermost first:

| kind | what it is | needs symbols? |
|---|---|---|
| `FOREIGN` | the line of *your* code you announced with `sturdy_trace_set_foreign_call_site` | no |
| `SCOPE` | a named engine scope (`Renderer::render_frame`, a render feature, a game-logic module) | no, and it works on WebAssembly |
| `UNWOUND` | an engine scope a C++ exception travelled through: the logical stack at the throw | no |
| `NATIVE` | a machine frame: module + offset (+ symbol, file, line when the platform has them) | optional; the offset symbolizes offline |

Announce where you are, make calls, and on a failure read the trace (all of it is per thread, so read it on the thread that failed,
straight after the call):

```python
import sturdy
lib = sturdy.load()
try:
    sturdy.check(lib, lib.sturdy_ui_begin_element(engine, ctypes.byref(element)))
except sturdy.SturdyError as error:
    print(error)            # message, the Python line that made the call, then the engine's frames innermost first
    error.trace             # the same frames as dicts: kind, function, file, module, line, column, address, module_offset
```

The Python binding does this for you in `check()`; `sturdy.announce_call_site(lib, __file__, line, "function")` attributes failures in calls
you do not wrap in `check()`, and `sturdy.last_error_trace(lib)` reads the current thread's trace on demand. From C, Java or C#, call
`sturdy_last_error_trace_frame_count` then `sturdy_last_error_trace_frame` for each index, or take it as ready-made text with
`sturdy_last_error_trace_format`. `sturdy_trace_capture` takes a trace on demand (a panic hook, a watchdog),
and `sturdy_trace_install_crash_handler` prints the engine's scopes and native stack when the process dies from a fatal signal.

How much is captured is `sturdy_trace_set_detail` (`OFF`, `SCOPES`, `NATIVE`), or `STURDY_TRACE=off|scopes|native` in the environment.
Debug builds default to `NATIVE`, release builds to `SCOPES`, which costs a pointer push per instrumented stage. Failures that are part
of ordinary use (a size query, the end of an enumeration, a dead entity) record only the cheap layers so they stay cheap.

To symbolize a native frame offline: `addr2line -f -C -e <module> 0x<module_offset>` (or `llvm-symbolizer`).

The Rust crate wraps all of this in `sturdy::trace` (`announce_call_site`, `trace_here!`, `last_error_trace`, `install_crash_handler`,
`install_panic_hook`), going through its own bridge rather than this C ABI.

## Using them

All three need the engine built as a shared library (`STURDY_BUILD_SHARED_LIBS`), named `libsturdy.so` / `sturdy.dll` /
`libsturdy.dylib`, found on the loader path or via `$STURDY_LIBRARY`.

```python
import ctypes, sturdy
lib = sturdy.load()                                    # checks the ABI version
out = ctypes.c_uint32()                                # SturdyComponentId is a plain uint32_t
desc = sturdy.SturdyComponentDesc(struct_size=ctypes.sizeof(sturdy.SturdyComponentDesc), name=b"game.health", size=8, align=4)
sturdy.check(lib, lib.sturdy_ecs_register_component_ex(engine, ctypes.byref(desc), ctypes.byref(out)))
```

```java
Sturdy.load(null);                                     // or a Path; checks the ABI version
int result = Sturdy.sturdy_ecs_find_component(engine, name, outId);   // pointers and by-value structs are MemorySegments
```

Things that are the same in every language:

* **Results.** Nearly every function returns a `SturdyResult`; zero is success. The detail text is `sturdy_last_error_message()` and is
  thread-local, so read it on the thread that failed. Python's `check()` does this for you.
* **Struct sizes.** Structs you fill in carry a `struct_size` field; set it to the struct's size (the engine uses it for versioning).
* **Callbacks** may run on threads the engine created. See `plans/threading-contract.md` before touching a managed runtime from one.
* **Handles** (`SturdyEngine`, `SturdyFrame`, ...) are only valid inside the callback that received them.

Language specifics:

* **Java** has no unsigned types: C `uint32_t` is `int` and `uint64_t`/`size_t` is `long`. Pass `--enable-native-access` to the JVM.
  Callback descriptors are `Sturdy.DESCRIPTOR_<name>` for `Linker.upcallStub`.
* **Python** callback parameters are `c_void_p`, so you can pass `None` where the ABI allows a null callback or a callback instance
  (`sturdy.SturdyTaskFn(my_function)`). Keep a reference to the instance for as long as the engine may call it.
* **C#** needs `AllowUnsafeBlocks`. Callbacks are `delegate* unmanaged[Cdecl]` obtained from `[UnmanagedCallersOnly]` methods. The
  ABI's one-byte bool is `byte`.
