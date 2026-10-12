#!/usr/bin/env python3
"""Verifies the generated language bindings (FFI/bindings/) against the C ABI header they were generated from.

    check_bindings.py --repo <repo root>             # the CTest entry: fails if anything is stale or wrong
    check_bindings.py --repo <repo root> --update    # regenerate the committed files, then verify

Checks, in order:
  1. Staleness. FFI/api/sturdy_abi.json and every generated binding are regenerated into a temporary directory and compared with the
     committed files. A change to Sturdy.h without re-running the tools fails here, which is how the bindings are kept from drifting.
  2. Layout. A C program built from the real header prints sizeof/alignof/offsetof for every struct; the Python (ctypes) and Java (Panama)
     bindings must agree on every size, alignment and field offset. This is where a binding is wrong in a way that corrupts memory.
  3. Calls. A stub shared library exports every function name; each binding calls every function through it, which proves the Python
     argtypes and the Java method-handle descriptors are consistent with their declared parameter types (Java's invokeExact checks this
     only at run time).
  4. Java is skipped (with a message) when no JDK 22+ is installed. C# is generated but not built here: there is no .NET toolchain.
"""

import argparse
import ctypes
import filecmp
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        print(f"command failed: {' '.join(map(str, command))}\n{result.stdout}\n{result.stderr}")
        sys.exit(1)
    return result.stdout


def find_clang():
    for name in (os.environ.get("CLANG"), "clang", "clang-20", "clang-19", "clang-18"):
        if name and shutil.which(name):
            return shutil.which(name)
    return None


def java_major(java):
    try:
        out = subprocess.run([java, "-version"], capture_output=True, text=True).stderr
        digits = out.split('"')[1].split(".")[0]
        return int(digits)
    except Exception:
        return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--update", action="store_true", help="rewrite the committed generated files first")
    args = parser.parse_args()
    repo = args.repo.resolve()
    tools, ffi = repo / "FFI" / "tools", repo / "FFI"
    clang = find_clang()
    if clang is None:
        print("SKIP: clang is needed to read the header")
        return 0

    committed = {
        "api": ffi / "api" / "sturdy_abi.json",
        "python": ffi / "bindings" / "python" / "sturdy.py",
        "java": ffi / "bindings" / "java" / "dev" / "sturdy" / "Sturdy.java",
        "csharp": ffi / "bindings" / "csharp" / "Sturdy.cs",
        "rust": ffi / "bindings" / "rust" / "sturdy_ffi.rs",
    }

    with tempfile.TemporaryDirectory(prefix="sturdy_bindings_") as tmp_name:
        tmp = Path(tmp_name)
        generated = {
            "api": tmp / "sturdy_abi.json",
            "python": tmp / "python" / "sturdy.py",
            "java": tmp / "java" / "dev" / "sturdy" / "Sturdy.java",
            "csharp": tmp / "csharp" / "Sturdy.cs",
            "rust": tmp / "rust" / "sturdy_ffi.rs",
        }
        env = dict(os.environ, PATH=os.environ.get("PATH", ""))
        # `clang` must be the one on PATH for the describer; point it at the one we found.
        describe = [sys.executable, str(tools / "describe_abi.py"), "--header", str(ffi / "src" / "FFI" / "Sturdy.h"), "--output", str(generated["api"])]
        print(run(describe, env=dict(env, PATH=str(Path(clang).parent) + os.pathsep + env["PATH"])).strip())
        run([sys.executable, str(tools / "gen_bindings.py"), "--api", str(generated["api"]), "--python", str(generated["python"]),
             "--java", str(generated["java"]), "--csharp", str(generated["csharp"]), "--rust", str(generated["rust"]), "--layout-check", str(tmp / "layout_check.c")])

        # ---- 1. staleness ----
        if args.update:
            for key, path in committed.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(generated[key], path)
            print("updated the committed generated files")
        stale = [str(committed[k].relative_to(repo)) for k in committed if not committed[k].exists() or not filecmp.cmp(generated[k], committed[k], shallow=False)]
        if stale:
            print("STALE generated files (Sturdy.h changed, or the generators did, without regenerating):")
            for s in stale:
                print("   ", s)
            print("run: python3 FFI/tests/bindings/check_bindings.py --repo . --update")
            return 1
        print("generated files are up to date")

        # ---- 2. layout reference from the real header ----
        run([clang, "-std=c11", f"-I{ffi / 'src'}", str(tmp / "layout_check.c"), "-o", str(tmp / "layout_check")])
        table = {}
        for line in run([str(tmp / "layout_check")]).splitlines():
            parts = line.split()
            table[parts[0]] = (int(parts[1]), int(parts[2]), {p.split("=")[0]: int(p.split("=")[1]) for p in parts[3:]})
        (tmp / "layout.txt").write_text(run([str(tmp / "layout_check")]))

        spec = importlib.util.spec_from_file_location("sturdy", committed["python"])
        sturdy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sturdy)
        failures = fields = 0
        for name, (size, align, offsets) in table.items():
            cls = getattr(sturdy, name)
            if ctypes.sizeof(cls) != size or ctypes.alignment(cls) != align:
                print(f"python layout mismatch: {name} size/align {ctypes.sizeof(cls)}/{ctypes.alignment(cls)} vs {size}/{align}")
                failures += 1
                continue
            for field, offset in offsets.items():
                fields += 1
                if getattr(cls, field).offset != offset:
                    print(f"python offset mismatch: {name}.{field} {getattr(cls, field).offset} vs {offset}")
                    failures += 1
        print(f"python: {len(table)} structs, {fields} fields match the real header" if not failures else f"python: {failures} layout mismatches")

        # ---- 3. every function through a stub library ----
        stub = None
        if sys.platform != "win32":
            stub_source = ["#include <stdint.h>"]
            import json
            description = json.loads(generated["api"].read_text(encoding="utf-8"))
            for f in description["functions"]:
                if f["name"] == "sturdy_abi_version_major":
                    stub_source.append(f"uint32_t sturdy_abi_version_major(void) {{ return {description['abi_version']['major']}; }}")
                elif f["name"] == "sturdy_abi_version_minor":
                    stub_source.append(f"uint32_t sturdy_abi_version_minor(void) {{ return {description['abi_version']['minor']}; }}")
                else:
                    stub_source.append(f"long {f['name']}(void) {{ return 0; }}")
            (tmp / "stub.c").write_text("\n".join(stub_source) + "\n")
            suffix = ".dylib" if sys.platform == "darwin" else ".so"
            stub = tmp / f"libsturdy{suffix}"
            run([clang, "-shared", "-fPIC", str(tmp / "stub.c"), "-o", str(stub)])

            library = sturdy.load(stub)
            calls = 0
            for name, (_, argtypes) in sturdy._FUNCTIONS.items():
                values = []
                for a in argtypes:
                    if isinstance(a, type) and issubclass(a, ctypes.Structure):
                        values.append(a())
                    elif a in (ctypes.c_void_p, ctypes.c_char_p) or a.__name__.startswith("LP_"):
                        values.append(None)
                    elif a in (ctypes.c_float, ctypes.c_double):
                        values.append(0.0)
                    elif a is ctypes.c_bool:
                        values.append(False)
                    else:
                        values.append(0)
                try:
                    getattr(library, name)(*values)
                    calls += 1
                except Exception as error:  # noqa: BLE001 - report which binding is wrong, whatever the exception
                    print(f"python call failed: {name}: {error}")
                    failures += 1
            print(f"python: {calls} functions called through the stub")
            try:
                sturdy.load(tmp / "does_not_exist.so")
                print("python: loading a missing library should have raised")
                failures += 1
            except OSError:
                pass

        # ---- 4. Java ----
        java, javac = shutil.which("java"), shutil.which("javac")
        if java and javac and java_major(java) >= 22 and stub is not None:
            classes = tmp / "classes"
            classes.mkdir()
            run([javac, "-d", str(classes), str(committed["java"]), str(ffi / "tests" / "bindings" / "BindingCheck.java")])
            result = subprocess.run([java, "--enable-native-access=ALL-UNNAMED", "-cp", str(classes), "BindingCheck", str(tmp / "layout.txt"), str(stub)],
                                    capture_output=True, text=True)
            print(result.stdout.strip())
            if result.returncode != 0:
                print(result.stderr)
                failures += 1
        else:
            print("java: SKIPPED (needs JDK 22+ and a POSIX stub library)")

        # ---- 5. Rust ----
        # Compiling the file is the check: every struct carries const assertions of its size, alignment and field offsets, so rustc
        # fails when its repr(C) layout differs from the C rules the generator applied (which the Python and Java checks tie to the header).
        rustc = shutil.which("rustc")
        if rustc:
            result = subprocess.run([rustc, "--edition", "2024", "--crate-type", "lib", "--emit=metadata", "-o", str(tmp / "sturdy_ffi.rmeta"), str(committed["rust"])],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                print("rust: the generated bindings do not compile:\n" + result.stderr[-3000:])
                failures += 1
            else:
                print("rust: compiles, every struct layout assertion holds")
        else:
            print("rust: SKIPPED (rustc not found)")

        if failures:
            print(f"FAILED: {failures} problem(s)")
            return 1
        print("bindings verified")
        return 0


if __name__ == "__main__":
    sys.exit(main())
