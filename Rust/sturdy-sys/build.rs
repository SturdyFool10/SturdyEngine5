//! Builds the SturdyEngine 5 (this repository) C++ engine as static archives and links them into the Rust
//! artifact, so the final executable needs nothing on the target machine but a GPU driver.
//!
//! Pipeline:
//!   1. `cxx-gen` turns the `#[cxx::bridge]` in `src/lib.rs` into `bridge.h` / `bridge.cc`.
//!   2. CMake configures the *unmodified* engine with `cmake/hook.cmake` injected, which adds the
//!      shim (+ generated bridge) as a target inside the engine's own build, so it inherits the
//!      engine's exact compiler flags, definitions and include paths.
//!   3. Ninja prints the authoritative link line of a probe executable; we mirror its archives (in
//!      order) and system libraries as Cargo link directives.
//!
//! Every `src/*.rs` file containing a `#[cxx::bridge]` module is its own bridge (see
//! `bridge_modules`); every `cpp/sturdy_rs/*.cpp` file is compiled into the shim automatically, so
//! adding a subsystem is "add the two files", not "edit build.rs".
//!
//! Environment knobs:
//!   STURDY_RS_TARGET_CPU   `-march` value for the C++ side (default: `x86-64-v2` on x86_64, never
//!                          `native`, which would make the binary crash on other machines)
//!   STURDY_RS_CMAKE_ARGS   extra whitespace-separated arguments for the CMake configure step
//!   STURDY_RS_BUILD_TYPE   CMake build type for the engine (default: Release)
//!   STURDY_RS_DEPS_DIR     directory for the engine's third-party sources (default: a per-build `OUT_DIR/deps`);
//!                          share one between profiles, or prepare it with `Rust/tools/prefetch-deps.sh`
//!   STURDY_RS_OFFLINE      `1` forbids network access during the build (needs a populated STURDY_RS_DEPS_DIR)
//!
//! Cargo feature flags reach the engine's own `STURDY_*` CMake options (see `feature_cmake_args`
//! and `sturdy-sys/Cargo.toml`'s `[features]` doc comments) — this is how a downstream Rust
//! application chooses engine build configuration without ever touching CMake itself.

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Prevents system shared libraries from leaking into the otherwise fully static link.
const HERMETIC_CMAKE_ARGS: &[&str] = &[
    "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_JBIG=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_LibLZMA=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_WebP=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_LERC=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_libdeflate=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_Imath=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_openjph=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_liblzma=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_libjpeg-turbo=ON",
    // libtiff decides its optional codecs from what it finds; pin them off so the result does
    // not depend on the build machine (the codecs left are the ones that need no external lib).
    "-Djpeg=OFF",
    "-Djpeg12=OFF",
    "-Dold-jpeg=OFF",
    "-Djbig=OFF",
    "-Dlzma=OFF",
    "-Dzstd=OFF",
    "-Dwebp=OFF",
    "-Dlerc=OFF",
    "-DOPENEXR_FORCE_INTERNAL_IMATH=ON",
    "-DOPENEXR_FORCE_INTERNAL_OPENJPH=ON",
];

/// System shared libraries the final binary is allowed to depend on: they are part of the OS
/// graphics stack (like the GPU driver itself) and cannot be bundled.
const ALLOWED_SYSTEM_LIBS: &[&str] = &["wayland-client"];
/// Linked by the engine's CMake but unnecessary: loaded at runtime through volk instead.
const DROPPED_SYSTEM_LIBS: &[&str] = &["vulkan"];

/// The platform being built *for*. A build script is compiled for the build machine, so `cfg!(windows)` and friends
/// describe the host; cross builds (Windows from Linux through cargo-xwin, macOS through cargo-zigbuild, ...) need
/// Cargo's view of the target instead.
fn target_os() -> String {
    env::var("CARGO_CFG_TARGET_OS").unwrap_or_default()
}

fn target_is_msvc() -> bool {
    target_os() == "windows" && env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc")
}

/// A CMake toolchain file for the target, when one is provided: `CMAKE_TOOLCHAIN_FILE_<target triple>` (what
/// cargo-xwin exports, `-` or `_` separated), `TARGET_CMAKE_TOOLCHAIN_FILE`, or plain `CMAKE_TOOLCHAIN_FILE` (the same
/// lookup order as the `cmake` crate). Only consulted when cross compiling.
fn cross_toolchain_file() -> Option<PathBuf> {
    let target = env::var("TARGET").ok()?;
    let host = env::var("HOST").ok()?;
    if target == host {
        return None;
    }
    let keys = [
        format!("CMAKE_TOOLCHAIN_FILE_{target}"),
        format!("CMAKE_TOOLCHAIN_FILE_{}", target.replace('-', "_")),
        "TARGET_CMAKE_TOOLCHAIN_FILE".to_owned(),
        "CMAKE_TOOLCHAIN_FILE".to_owned(),
    ];
    keys.iter().find_map(|key| {
        println!("cargo:rerun-if-env-changed={key}");
        env::var_os(key).map(PathBuf::from)
    })
}

fn main() {
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    // This crate lives in the engine's own repository (`<repo>/Rust/sturdy-sys`), so the engine source is
    // the repository root. STURDY_RS_ENGINE_DIR overrides it to build against a different checkout.
    let engine_src = env::var_os("STURDY_RS_ENGINE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("../.."));
    assert!(
        engine_src.join("CMakeLists.txt").exists(),
        "{} is not a SturdyEngine5 checkout (no CMakeLists.txt); set STURDY_RS_ENGINE_DIR",
        engine_src.display()
    );

    for path in ["build.rs", "src", "cpp", "cmake"] {
        println!("cargo:rerun-if-changed={path}");
    }
    println!("cargo:rerun-if-changed={}", engine_src.join("CMakeLists.txt").display());
    for dir in [
        "Foundation", "Reflection", "Async", "Ecs", "Core", "RHI", "Renderer", "WindowManager",
        "Engine", "ApplicationHost", "Runtime", "CxxApi", "RenderSettings", "Audio", "AudioGpu", "Animation", "Physics",
        "EngineKit", "cmake", "Shaders", "Fonts", "thirdparty",
    ] {
        println!("cargo:rerun-if-changed={}", engine_src.join(dir).display());
    }
    for var in [
        "STURDY_RS_TARGET_CPU",
        "STURDY_RS_CMAKE_ARGS",
        "STURDY_RS_BUILD_TYPE",
        "STURDY_RS_ENGINE_DIR",
        "STURDY_RS_DEPS_DIR",
        "STURDY_RS_OFFLINE",
    ] {
        println!("cargo:rerun-if-env-changed={var}");
    }

    let bridge_cc = generate_bridges(&manifest, &out);
    let build_dir = out.join("engine-build");
    configure(&manifest, &out, &engine_src, &build_dir, &out.join("gen"), &bridge_cc);
    // Compile with one job per hardware thread rather than the generator's own default.
    let jobs = std::thread::available_parallelism().map_or(1, |n| n.get()).to_string();
    run(Command::new("cmake").args([
        "--build".as_ref(),
        build_dir.as_os_str(),
        "--parallel".as_ref(),
        jobs.as_ref(),
        "--target".as_ref(),
        "sturdy_rs_shim".as_ref(),
        "sturdy_rs_link_probe".as_ref(),
        // Ninja deletes linker response files after a successful link by default. The link
        // probe's response file is parsed below to mirror its archive list into Cargo, so keep it.
        "--".as_ref(),
        "-d".as_ref(),
        "keeprsp".as_ref(),
    ]));
    emit_link_directives(&build_dir, &out);
}

/// Runs `cxx-gen` over every `src/*.rs` file that contains a `#[cxx::bridge]` module — one engine
/// subsystem per file (`src/lib.rs` -> `sturdy_rs/bridge.h`, `src/ecs.rs` -> `sturdy_rs/ecs_bridge.h`,
/// ...) — and returns the generated `.cc` paths for the shim to compile.
///
/// Adding a subsystem is adding a file here, in `cpp/sturdy_rs/`, and one `mod` line in
/// `src/lib.rs`; nothing else in this build script changes.
fn generate_bridges(manifest: &Path, out: &Path) -> Vec<PathBuf> {
    let gen_dir = out.join("gen");
    fs::create_dir_all(gen_dir.join("rust")).unwrap();
    fs::create_dir_all(gen_dir.join("sturdy_rs")).unwrap();
    fs::write(gen_dir.join("rust/cxx.h"), cxx_gen::HEADER).unwrap();

    let src_dir = manifest.join("src");
    let mut generated = Vec::new();
    let mut entries: Vec<_> = fs::read_dir(&src_dir)
        .unwrap_or_else(|e| panic!("read {}: {e}", src_dir.display()))
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|ext| ext == "rs"))
        .collect();
    entries.sort();

    for path in entries {
        let source = fs::read_to_string(&path).unwrap_or_else(|e| panic!("read {}: {e}", path.display()));
        if !source.contains("#[cxx::bridge") {
            continue;
        }
        let stem = path.file_stem().unwrap().to_str().unwrap();
        // src/lib.rs keeps the original "bridge.h"/"bridge.cc" names the existing shim includes.
        let base = if stem == "lib" { "bridge".to_owned() } else { format!("{stem}_bridge") };

        let tokens: proc_macro2::TokenStream =
            source.parse().unwrap_or_else(|e| panic!("{} is not valid Rust tokens: {e}", path.display()));
        let code = cxx_gen::generate_header_and_cc(tokens, &cxx_gen::Opt::default())
            .unwrap_or_else(|e| panic!("cxx-gen failed on the bridge in {}: {e}", path.display()));

        let header = gen_dir.join(format!("sturdy_rs/{base}.h"));
        let cc = gen_dir.join(format!("sturdy_rs/{base}.cc"));
        fs::write(&header, &code.header).unwrap();
        fs::write(&cc, &code.implementation).unwrap();
        generated.push(cc);
    }
    generated
}

/// Every `.cpp` file under `cpp/sturdy_rs/` (the hand-written shim, one subsystem per file) plus
/// every generated bridge `.cc` (one per `src/*.rs` bridge module).
fn shim_sources(manifest: &Path, bridge_cc: &[PathBuf]) -> Vec<PathBuf> {
    let cpp_dir = manifest.join("cpp/sturdy_rs");
    let mut sources: Vec<_> = fs::read_dir(&cpp_dir)
        .unwrap_or_else(|e| panic!("read {}: {e}", cpp_dir.display()))
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|ext| ext == "cpp"))
        .collect();
    sources.sort();
    sources.extend(bridge_cc.iter().cloned());
    sources
}

/// Cargo feature -> the `STURDY_*` CMake option it forwards to. Kept in one place so the mapping
/// in `sturdy-sys/Cargo.toml`'s `[features]` doc comments has a single source of truth to match.
fn feature_cmake_args() -> Vec<&'static str> {
    let mut args = Vec::new();
    if env::var_os("CARGO_FEATURE_WEBGPU").is_some() {
        args.push("-DSTURDY_ENABLE_WEBGPU=ON");
    }
    if env::var_os("CARGO_FEATURE_GLFW").is_some() {
        args.push("-DSTURDY_BUILD_GLFW_WINDOW_PROVIDER=ON");
    }
    if env::var_os("CARGO_FEATURE_TRACY").is_some() {
        args.push("-DSTURDY_ENABLE_TRACY=ON");
    }
    if env::var_os("CARGO_FEATURE_DEBUG_SECTIONS").is_some() {
        args.push("-DSTURDY_ENABLE_STATIC_DEAD_STRIPPING=OFF");
    }
    args
}

fn configure(
    manifest: &Path,
    out: &Path,
    engine_src: &Path,
    build_dir: &Path,
    gen_dir: &Path,
    bridge_cc: &[PathBuf],
) {
    let cpu = env::var("STURDY_RS_TARGET_CPU").ok().unwrap_or_else(|| {
        match env::var("CARGO_CFG_TARGET_ARCH").as_deref() {
            Ok("x86_64") => "x86-64-v2".into(),
            _ => String::new(),
        }
    });
    // `native-cpu` opts out of the portable baseline; STURDY_RS_TARGET_CPU still wins if both are
    // set, since it's the more specific ask.
    let cpu = if env::var_os("CARGO_FEATURE_NATIVE_CPU").is_some() && env::var_os("STURDY_RS_TARGET_CPU").is_none() {
        "native".to_owned()
    } else {
        cpu
    };
    let build_type = env::var("STURDY_RS_BUILD_TYPE").unwrap_or_else(|_| "Release".into());
    // Deliberately explicit: a builder's CFLAGS/CXXFLAGS (commonly `-march=native`) must not leak
    // into an artifact meant to run on other machines.
    // `-mxsave` on x86: Foundation's CPUID probe reads XCR0 via `xgetbv` (after checking OSXSAVE),
    // which clang only accepts with that feature enabled; `-march=native` used to imply it.
    let toolchain = cross_toolchain_file();
    // clang-cl (the MSVC-style driver a Windows toolchain file selects) only takes GNU-style options behind `/clang:`.
    let clang_cl = toolchain.is_some() && target_is_msvc();
    let gnu = |flag: &str| if clang_cl { format!("/clang:{flag}") } else { flag.to_owned() };
    // clang-cl puts `/clang:` options after everything else, so a `-march` there would override (and drop) the AVX a
    // source file's own `/arch:AVX2` asks for. Express the portable baseline as added features instead, which only
    // ever add to what each file enables.
    let mut cxx_flags = match cpu.as_str() {
        "" => String::new(),
        "x86-64-v2" if clang_cl => ["-msse4.2", "-mpopcnt", "-mcx16"].map(gnu).join(" "),
        _ => gnu(&format!("-march={cpu}")),
    };
    if env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("x86_64") {
        cxx_flags.push(' ');
        cxx_flags.push_str(&gnu("-mxsave"));
    }

    let shim_sources = shim_sources(manifest, bridge_cc)
        .into_iter()
        .map(|p| p.display().to_string())
        .collect::<Vec<_>>()
        .join(";");
    let include_dirs = format!("{};{}", manifest.join("cpp").display(), gen_dir.display());

    let mut cmd = Command::new("cmake");
    cmd.arg("-S").arg(engine_src).arg("-B").arg(build_dir).args(["-G", "Ninja"]);
    // The engine's build matrix (cmake/SturdyMatrix.cmake) defaults to the build machine; say what the target is, so a
    // cross build never configures for the host (and a cached value from an earlier configure cannot win either).
    let sturdy_os = match target_os().as_str() {
        "windows" => Some("Windows"),
        "macos" => Some("MacOS"),
        "linux" => Some("Linux"),
        "freebsd" => Some("FreeBSD"),
        _ => None,
    };
    let sturdy_arch = match env::var("CARGO_CFG_TARGET_ARCH").as_deref() {
        Ok("x86_64") => Some("x86-64"),
        Ok("aarch64") => Some("Arm64"),
        Ok("riscv64") => Some("RISCV"),
        _ => None,
    };
    if let Some(os) = sturdy_os {
        cmd.arg(format!("-DSTURDY_OS={os}"));
    }
    if let Some(arch) = sturdy_arch {
        cmd.arg(format!("-DSTURDY_ARCH={arch}"));
    }
    match &toolchain {
        // The toolchain file names the cross compilers, sysroot and target; it must not be overridden here.
        Some(file) => {
            let file = if clang_cl { msvc_toolchain_wrapper(out, file) } else { file.clone() };
            cmd.arg(format!("-DCMAKE_TOOLCHAIN_FILE={}", file.display()));
        }
        None => {
            cmd.arg("-DCMAKE_C_COMPILER=clang").arg("-DCMAKE_CXX_COMPILER=clang++");
        }
    }
    cmd.arg(format!("-DCMAKE_BUILD_TYPE={build_type}"))
        .arg(format!("-DCMAKE_C_FLAGS={cxx_flags}"))
        .arg(format!("-DCMAKE_CXX_FLAGS={cxx_flags}"))
        .arg("-DCMAKE_POSITION_INDEPENDENT_CODE=ON")
        // Rust's linker cannot consume clang's ThinLTO bitcode unless its LLVM matches exactly.
        .arg("-DSTURDY_ENABLE_LTO=OFF")
        .arg("-DSTURDY_BUILD_SHARED_LIBS=OFF")
        .arg("-DSTURDY_BUILD_RUNTIME_DEMO=OFF")
        .arg("-DSTURDY_USE_COMPILER_LAUNCHER=OFF")
        // Build every dependency from source: a system copy would defeat the static bundle (and
        // stray CMake packages, e.g. CUDA's harfbuzz config, break the configure outright).
        .arg("-DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER")
        // Third-party projects (libtiff, OpenEXR, ...) call find_package on their own and would
        // otherwise bind to whatever shared libraries the build machine has installed. Turn those
        // lookups off so each codec degrades cleanly or falls back to a vendored source build.
        .args(HERMETIC_CMAKE_ARGS)
        // Cargo feature flags -> engine CMake options; see `feature_cmake_args`.
        .args(feature_cmake_args())
        // Keep the source tree pristine: the dependency cache lives in OUT_DIR, unless STURDY_RS_DEPS_DIR points at a
        // shared (or pre-populated, for offline builds) directory.
        .arg(format!("-DSTURDY_DEPS_CACHE_DIR={}", deps_dir(out).display()))
        .arg(format!("-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES={}", manifest.join("cmake/hook.cmake").display()))
        .arg(format!("-DSTURDY_RS_SHIM_CMAKE={}", manifest.join("cmake/shim.cmake").display()))
        .arg(format!("-DSTURDY_RS_SHIM_SOURCES={shim_sources}"))
        .arg(format!("-DSTURDY_RS_SHIM_INCLUDE_DIRS={include_dirs}"));
    if env::var_os("STURDY_RS_OFFLINE").is_some_and(|v| v != "0") {
        // Never touch the network: every dependency must already be in the deps directory (see tools/prefetch-deps.sh).
        cmd.arg("-DFETCHCONTENT_FULLY_DISCONNECTED=ON");
    }
    if let Ok(extra) = env::var("STURDY_RS_CMAKE_ARGS") {
        cmd.args(extra.split_whitespace());
    }
    // Configure-time compiler flags come only from the -D arguments above.
    cmd.env_remove("CFLAGS").env_remove("CXXFLAGS").env_remove("LDFLAGS");
    run(&mut cmd);
}

/// It also means to select the dynamic CRT (`/MD`, what Rust links) but its `set(CMAKE_MSVC_RUNTIME_LIBRARY CACHE STRING
/// "MultiThreadedDLL")` sets the value to empty (the string is only the doc string), so clang-cl falls back to the static
/// CRT and the final link fails on a RuntimeLibrary mismatch; set it properly.
///
/// cargo-xwin's toolchain file empties CMake's default Windows libraries (kernel32, user32, shell32, ...), leaving every
/// project to name them. Third-party code inside the engine build that links an executable (Slang's tools, the link
/// probe) does not, so restore the MSVC defaults on top of it.
fn msvc_toolchain_wrapper(out: &Path, toolchain: &Path) -> PathBuf {
    let wrapper = out.join("msvc-cross-toolchain.cmake");
    let libraries = "kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib";
    // It also leaves CMake free to search the build machine's /usr for headers, libraries and packages, which a
    // Windows target can never use (and whose glibc headers then clash with the MSVC CRT). Root every search in an
    // empty directory instead; build-machine programs (python, git, ...) are still found.
    // The splatted Windows SDK has lower-case header names; on a case-sensitive file system a third-party
    // `#include <Intsafe.h>` (written against case-insensitive Windows) misses. Forwarding headers fix the ones in use.
    let aliases = out.join("msvc-header-case-aliases");
    fs::create_dir_all(&aliases).unwrap();
    for (alias, real) in [("Intsafe.h", "intsafe.h"), ("Windows.h", "windows.h"), ("WinSock2.h", "winsock2.h"), ("ShlObj.h", "shlobj.h"), ("Shlwapi.h", "shlwapi.h"), ("DbgHelp.h", "dbghelp.h"), ("Psapi.h", "psapi.h"), ("VersionHelpers.h", "versionhelpers.h")] {
        fs::write(aliases.join(alias), format!("#pragma once\n#include <{real}>\n")).unwrap();
    }
    let aliases = aliases.display().to_string().replace('\\', "/");
    let find_root = out.join("msvc-cross-find-root");
    fs::create_dir_all(&find_root).unwrap();
    let body = format!(
        "include(\"{}\")\n\
         set(CMAKE_C_STANDARD_LIBRARIES \"{libraries}\" CACHE STRING \"\" FORCE)\n\
         set(CMAKE_CXX_STANDARD_LIBRARIES \"{libraries}\" CACHE STRING \"\" FORCE)\n\
         set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreadedDLL\" CACHE STRING \"\" FORCE)\n\
         set(CMAKE_FIND_ROOT_PATH \"{}\")\n\
         set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n\
         set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n\
         set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n\
         set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n\
         if(NOT CMAKE_C_FLAGS MATCHES \"msvc-header-case-aliases\")\n\
           set(CMAKE_C_FLAGS \"${{CMAKE_C_FLAGS}} /imsvc {aliases}\" CACHE STRING \"\" FORCE)\n\
           set(CMAKE_CXX_FLAGS \"${{CMAKE_CXX_FLAGS}} /imsvc {aliases}\" CACHE STRING \"\" FORCE)\n\
         endif()\n",
        toolchain.display().to_string().replace('\\', "/"),
        find_root.display().to_string().replace('\\', "/")
    );
    fs::write(&wrapper, body).unwrap();
    wrapper
}

/// Where the engine's third-party sources live: `STURDY_RS_DEPS_DIR` when set (shared between profiles and
/// checkouts, or prepared ahead of an offline build), else a per-build `OUT_DIR/deps`.
fn deps_dir(out: &Path) -> PathBuf {
    env::var_os("STURDY_RS_DEPS_DIR").map(PathBuf::from).unwrap_or_else(|| out.join("deps"))
}

fn run(cmd: &mut Command) {
    cmd.env_remove("CFLAGS").env_remove("CXXFLAGS").env_remove("LDFLAGS");
    let status = cmd.status().unwrap_or_else(|e| panic!("failed to spawn {cmd:?}: {e}"));
    assert!(status.success(), "{cmd:?} failed with {status}");
}

/// Splits a link command on whitespace, expanding `@response-file` arguments.
fn tokens(command: &str, build_dir: &Path) -> Vec<String> {
    let mut result = Vec::new();
    for token in command.split_whitespace() {
        match token.strip_prefix('@') {
            Some(rsp) if build_dir.join(rsp).is_file() => {
                let body = fs::read_to_string(build_dir.join(rsp)).unwrap();
                result.extend(body.split_whitespace().map(str::to_owned));
            }
            _ => result.push(token.to_owned()),
        }
    }
    result
}

fn emit_link_directives(build_dir: &Path, out: &Path) {
    let output = Command::new("ninja")
        .args(["-C"])
        .arg(build_dir)
        .args(["-t", "commands", "sturdy_rs_link_probe"])
        .output()
        .expect("failed to run `ninja -t commands`");
    assert!(output.status.success(), "ninja -t commands failed");
    let commands = String::from_utf8_lossy(&output.stdout);
    let link_line = commands
        .lines()
        // GNU-style drivers name the output with `-o`; an MSVC-style link (lld-link / link.exe) with `/out:`.
        .rfind(|l| l.contains("sturdy_rs_link_probe") && (l.contains("-o ") || l.contains("/out:")))
        .expect("could not find the probe link command");

    let mut directives = Vec::new();
    let mut seen_names: HashMap<String, PathBuf> = HashMap::new();
    let mut searched = Vec::new();
    let mut system_libs = Vec::new();
    // macOS: `-framework Cocoa` arrives as two tokens. UNTESTED (no Mac available): written from the ld64 conventions.
    let mut frameworks: Vec<String> = Vec::new();
    let mut take_framework = false;

    for token in tokens(link_line, build_dir) {
        // MSVC-style linker options (`-libpath:"..."`, `/LIBPATH:...`, `/out:`, `/nologo`, ...) are not libraries; the
        // first would otherwise read as `-l ibpath:...`. Rust's own link gets the library paths from cargo-xwin/the SDK.
        let lower = token.to_ascii_lowercase();
        if lower.starts_with("-libpath:") || (token.starts_with('/') && !Path::new(&token).exists()) {
            continue;
        }
        // `-Xlinker <flag>` forwards one argument to the linker; the flag itself is handled below.
        if token == "-Xlinker" {
            continue;
        }
        if take_framework {
            take_framework = false;
            if !frameworks.contains(&token) {
                frameworks.push(token);
            }
            continue;
        }
        // `-weak_framework` (SDL3's CoreHaptics / UniformTypeIdentifiers) is linked as an ordinary framework: Rust has no
        // weak link kind, and both exist on every macOS the deployment target (11.0) allows.
        if token == "-framework" || token == "-weak_framework" {
            take_framework = true;
            continue;
        }
        // CMake/Clang may render a Windows import library as `-lfoo.lib`. Handle the `-l`
        // prefix first; treating it as an archive named `-lfoo` gives Cargo an invalid `-l` name.
        if let Some(name) = token
            .strip_prefix("-l")
            .and_then(|name| name.strip_suffix(".lib").or_else(|| name.strip_suffix(".LIB")))
        {
            if !system_libs.contains(&name.to_owned()) {
                system_libs.push(name.to_owned());
            }
            continue;
        }
        if let Some(name) = token.strip_prefix("-Wl,-framework,").or_else(|| token.strip_prefix("-Wl,-weak_framework,")) {
            if !frameworks.iter().any(|f| f == name) {
                frameworks.push(name.to_owned());
            }
            continue;
        }
        // Windows (MSVC ABI): archives are `Name.lib` (no `lib` prefix); a `.lib` that is not one of the engine's own
        // build outputs is a system import library (`kernel32.lib`, `dwmapi.lib`, ...). UNTESTED: written from the
        // MSVC conventions without a Windows machine; see `Rust/plans/vertex-launcher-gaps.md`.
        if let Some(stem) = token.strip_suffix(".lib").or_else(|| token.strip_suffix(".LIB")) {
            let file = Path::new(stem).file_name().unwrap().to_string_lossy().into_owned();
            let own_archive = build_dir.join(&token).is_file() || Path::new(&token).is_file();
            if own_archive {
                let path = if Path::new(&token).is_absolute() { PathBuf::from(&token) } else { build_dir.join(&token) };
                let mut dir = path.parent().unwrap().to_path_buf();
                let mut file = file;
                // Windows file names are case-insensitive and Cargo links by bare name, so the engine's
                // `Core.lib` and Slang's `core.lib` would resolve to the same archive. Link a renamed copy
                // of any archive whose name was already taken by one from a different directory.
                let key = file.to_ascii_lowercase();
                if seen_names.get(&key).is_some_and(|previous| *previous != dir) {
                    let unique = format!("{}_{}", dir.file_name().and_then(|n| n.to_str()).unwrap_or("dup"), file);
                    let renamed_dir = out.join("renamed-archives");
                    fs::create_dir_all(&renamed_dir).expect("create renamed archive directory");
                    fs::copy(&path, renamed_dir.join(format!("{unique}.lib"))).expect("copy colliding archive");
                    file = unique;
                    dir = renamed_dir;
                }
                if seen_names.insert(file.to_ascii_lowercase(), dir.clone()).is_none() {
                    if !searched.contains(&dir) {
                        directives.push(format!("cargo:rustc-link-search=native={}", dir.display()));
                        searched.push(dir);
                    }
                    directives.push(format!("cargo:rustc-link-lib=static={file}"));
                }
            } else if !system_libs.contains(&file) {
                system_libs.push(file);
            }
            continue;
        }
        if let Some(lib) = token.strip_suffix(".a") {
            let path = build_dir.join(&token);
            let file = Path::new(lib).file_name().unwrap().to_string_lossy().into_owned();
            let name = file.strip_prefix("lib").unwrap_or(&file).to_owned();
            if name == "sturdy_rs_link_probe" {
                continue;
            }
            let dir = path.parent().unwrap().to_path_buf();
            if let Some(previous) = seen_names.insert(name.clone(), dir.clone()) {
                assert_eq!(
                    previous, dir,
                    "two different static archives named lib{name}.a; Cargo link-lib names cannot disambiguate them"
                );
                continue;
            }
            if !searched.contains(&dir) {
                directives.push(format!("cargo:rustc-link-search=native={}", dir.display()));
                searched.push(dir);
            }
            directives.push(format!("cargo:rustc-link-lib=static={name}"));
        } else if let Some(name) = shared_library_name(&token) {
            if DROPPED_SYSTEM_LIBS.contains(&name.as_str()) {
                continue;
            }
            assert!(
                ALLOWED_SYSTEM_LIBS.contains(&name.as_str())
                    // macOS ships libc++/libSystem/... under /usr/lib; they are part of the OS.
                    || (target_os() == "macos" && token.starts_with("/usr/lib/")),
                "the engine link line pulls in system shared library `{token}`, which would make the \
                 executable depend on the build machine. Disable its find_package in \
                 HERMETIC_CMAKE_ARGS (build.rs) or add it to ALLOWED_SYSTEM_LIBS if it is part of the OS."
            );
            if !system_libs.contains(&name) {
                system_libs.push(name);
            }
        } else if let Some(lib) = token.strip_prefix("-l")
            && !system_libs.contains(&lib.to_owned())
        {
            system_libs.push(lib.to_owned());
        }
    }

    // Threading/dl are always needed by SDL3/volk/spdlog even when the link line omits them (not a Windows concept).
    let target_os = target_os();
    for lib in if target_os == "windows" { &[][..] } else { &["pthread", "dl", "m"][..] } {
        if !system_libs.iter().any(|l| l == lib) {
            system_libs.push((*lib).into());
        }
    }
    for lib in system_libs {
        if lib == "stdc++" {
            continue; // handled below
        }
        directives.push(format!("cargo:rustc-link-lib={lib}"));
    }

    for framework in frameworks {
        directives.push(format!("cargo:rustc-link-lib=framework={framework}"));
    }

    if target_os == "macos" {
        // The platform C++ runtime is libc++ (part of the OS); there is no libstdc++ to bundle.
        directives.push("cargo:rustc-link-lib=c++".to_owned());
    } else if target_os != "windows" {
        // The engine is C++26 and is built against a very recent libstdc++; a target machine's copy is
        // likely older, so the C++ runtime is bundled statically whenever the static archive exists.
        // Ask for the *target's* archive: a cross build (e.g. aarch64 from x86_64) must not bundle the host's.
        let mut probe = Command::new("clang++");
        if let (Ok(target), Ok(host)) = (env::var("TARGET"), env::var("HOST"))
            && target != host
        {
            probe.arg(format!("--target={target}"));
            // The cross C++ flags (cc-rs's CXXFLAGS_<target>) carry the target sysroot that holds its libstdc++.
            let key = format!("CXXFLAGS_{}", target.replace('-', "_"));
            println!("cargo:rerun-if-env-changed={key}");
            if let Ok(flags) = env::var(&key) {
                probe.args(flags.split_whitespace());
            }
        }
        let static_stdcxx = probe
            .arg("-print-file-name=libstdc++.a")
            .output()
            .ok()
            .map(|o| PathBuf::from(String::from_utf8_lossy(&o.stdout).trim()))
            .filter(|p| p.is_absolute() && p.exists());
        if let Some(archive) = static_stdcxx {
            directives.push(format!("cargo:rustc-link-search=native={}", archive.parent().unwrap().display()));
            directives.push("cargo:rustc-link-lib=static=stdc++".to_owned());
        } else {
            println!("cargo:warning=libstdc++.a not found; the binary will need a compatible libstdc++.so at runtime");
            directives.push("cargo:rustc-link-lib=stdc++".to_owned());
        }
    }

    // Keep a copy for a top-level binary that depends on this library through one or more Rust
    // wrapper crates. Cargo's rustc-link-lib instructions otherwise stop at this crate's library
    // target, while its native search paths are still propagated.
    let manifest = out.join("link-directives.txt");
    fs::write(&manifest, directives.join("\n")).expect("write native link directives");
    for directive in &directives {
        println!("{directive}");
    }
    println!("cargo:link_directives_file={}", manifest.display());
}

/// `/usr/lib/libfoo-bar.so.1.2` -> `foo-bar`.
fn shared_library_name(token: &str) -> Option<String> {
    let file = Path::new(token).file_name()?.to_str()?;
    let stem = file.strip_prefix("lib")?;
    // `libfoo.so.1`, or macOS `libfoo.1.dylib` / `libfoo.dylib`.
    let end = stem.find(".so").or_else(|| stem.ends_with(".dylib").then(|| stem.find('.')).flatten())?;
    Some(stem[..end].to_owned())
}
