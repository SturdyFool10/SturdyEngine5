#!/usr/bin/env python3
"""Checks that CxxApi stays bindable by the Rust `cxx` crate. Nothing it writes is kept: the Rust repo authors and generates its own
`#[cxx::bridge]` with cxx / cxx-build; this only catches headers that would make that fail.

    check_bridge.py --repo . --compile-commands build/.../compile_commands.json [--work-dir DIR]

  1. Checks every function has a definition in the C++ sources (a declaration with no body only fails at link time).
  2. Builds a throwaway bridge from the headers and runs it through the real `cxx` toolchain: bindgen mirrors the plain-data types, `cargo check` type-checks the Rust, cxx-gen
     writes the C++ glue, and clang compiles that glue against the engine's real headers. This is what proves the signatures satisfy
     `cxx`'s own rules (trivially relocatable PODs, opaque types, `unique_ptr` of an abstract class, ...).

Step 3 needs cargo (with the `cxx`, `cxx-gen` and `proc-macro2` crates available offline or on the network) and bindgen; without them it
is skipped with a message, and steps 1-2 still run.
"""

import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CARGO_TOML = """[package]
name = "cxxapi_bridge_check"
version = "0.0.0"
edition = "2021"

[dependencies]
cxx = "1"
cxx-gen = "0.7"
proc-macro2 = "1"

[[bin]]
name = "generate"
path = "src/bin/generate.rs"

[workspace]
"""

GENERATE_RS = """// Runs cxx-gen over the bridge, writing bridge.h, bridge.cc and rust/cxx.h into the directory given as the second argument.
use std::{env, fs, str::FromStr};

fn main() {
    let args: Vec<String> = env::args().collect();
    let source = fs::read_to_string(&args[1]).expect("read bridge");
    // Only the bridge module matters to cxx-gen: wrap it so `crate::types` paths need not resolve.
    let tokens = proc_macro2::TokenStream::from_str(&source).expect("tokenize bridge");
    let generated = cxx_gen::generate_header_and_cc(tokens, &cxx_gen::Opt::default()).expect("cxx-gen");
    let out = std::path::Path::new(&args[2]);
    fs::create_dir_all(out.join("rust")).unwrap();
    fs::write(out.join("bridge.h"), &generated.header).unwrap();
    fs::write(out.join("bridge.cc"), &generated.implementation).unwrap();
    fs::write(out.join("rust/cxx.h"), cxx_gen::HEADER).unwrap();
}
"""


def run(command, cwd=None, env=None, check=True):
    result = subprocess.run(command, capture_output=True, text=True, cwd=cwd, env=env)
    if check and result.returncode != 0:
        print(f"command failed: {' '.join(map(str, command))}\n{result.stdout[-3000:]}\n{result.stderr[-6000:]}")
        sys.exit(1)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--compile-commands", required=True, type=Path)
    parser.add_argument("--work-dir", type=Path, default=None, help="persistent directory for the cargo target dir (speeds up reruns)")
    args = parser.parse_args()
    repo = args.repo.resolve()
    tools = repo / "CxxApi" / "tools"

    with tempfile.TemporaryDirectory(prefix="cxxapi_bridge_") as tmp_name:
        tmp = Path(tmp_name)
        generated = tmp / "cxxapi_bridge.rs"
        print(run([sys.executable, str(tools / "gen_bridge.py"), "--compile-commands", str(args.compile_commands), "--output", str(generated)]).stdout.strip())

        # ---- 1. every declared function is defined ----
        names = re.findall(r"\n\s+(?:unsafe )?fn (\w+)\(", generated.read_text(encoding="utf-8"))
        sources = "".join(p.read_text(encoding="utf-8", errors="replace") for p in (repo / "CxxApi" / "src" / "CxxApi").glob("*.cpp"))
        undefined = [n for n in names if not re.search(r"\b" + n + r"\s*\([^;{]*\)[^;{]*\{", sources)]
        if undefined:
            print(f"declared in CxxApi but defined nowhere (a link error for any consumer): {undefined}")
            return 1
        print(f"all {len(names)} functions have a definition")

        # ---- 2. the real cxx toolchain ----
        cargo, bindgen = shutil.which("cargo"), shutil.which("bindgen")
        clang = shutil.which(os.environ.get("CLANGXX", "clang++"))
        if not (cargo and bindgen and clang):
            print("cxx toolchain check SKIPPED: needs cargo, bindgen and clang++")
            return 0
        crate = tmp / "crate"
        (crate / "src" / "bin").mkdir(parents=True)
        (crate / "Cargo.toml").write_text(CARGO_TOML)
        (crate / "src" / "bin" / "generate.rs").write_text(GENERATE_RS)

        sys.path.insert(0, str(tools))
        import gen_bridge  # noqa: E402  (same directory)
        flags = gen_bridge.compile_flags(args.compile_commands)
        include_flags = [f for f in flags if f.startswith("-I")]

        types_rs = crate / "src" / "types.rs"
        run([bindgen, str(repo / "CxxApi" / "src" / "CxxApi" / "Types.hpp"), "-o", str(types_rs), "--no-layout-tests", "--rustified-enum", ".*",
             "--no-doc-comments", "--allowlist-type", "SFT::.*", "--enable-cxx-namespaces", "--", "-x", "c++", "-std=c++23",
             f"-I{repo / 'CxxApi' / 'src'}", f"-I{repo / 'RenderSettings' / 'src'}"])
        (crate / "src" / "lib.rs").write_text("#[allow(warnings)]\npub mod types;\n" + generated.read_text(encoding="utf-8"))

        work = args.work_dir or (tmp / "cargo-target")
        work.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, CARGO_TARGET_DIR=str(work), CARGO_NET_OFFLINE="true")
        check = run([cargo, "check", "--offline", "--quiet"], cwd=crate, env=env, check=False)
        if check.returncode != 0 and "no matching package" in check.stderr + check.stdout:
            print("cxx toolchain check SKIPPED: the cxx crates are not available offline")
            return 0
        if check.returncode != 0:
            print(check.stdout[-3000:], check.stderr[-6000:])
            print("FAILED: cargo check of the bridge")
            return 1
        print("cargo check: the Rust side of the bridge type-checks")

        glue = tmp / "glue"
        run([cargo, "run", "--offline", "--quiet", "--bin", "generate", "--", str(generated), str(glue)], cwd=crate, env=env)
        compile_result = run([clang, "-x", "c++", "-fsyntax-only", *flags, f"-I{glue}", "-Wno-unused-parameter", str(glue / "bridge.cc")], check=False)
        errors = [line for line in compile_result.stderr.splitlines() if "error:" in line]
        if compile_result.returncode != 0:
            print("\n".join(errors[:20]) or compile_result.stderr[-4000:])
            print("FAILED: the C++ glue cxx generates does not compile against the engine's headers")
            return 1
        print("clang: the generated C++ glue compiles against the real headers")
        print("cxx bridge verified")
        return 0


if __name__ == "__main__":
    sys.exit(main())
