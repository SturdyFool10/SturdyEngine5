//! Publishes the `export-symbols` choice to dependents and applies it to this crate's own tests.
//!
//! Cargo does not let a library carry linker flags to the executable that links it, so the flag itself is applied by the final package's
//! build script (`sturdy_build::emit()`), which reads the metadata published here. This crate's own test binaries link the engine too, so
//! they get the flag directly.

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    if std::env::var_os("CARGO_FEATURE_EXPORT_SYMBOLS").is_none() {
        return;
    }
    // Read by dependents' build scripts as `DEP_STURDY_RS_EXPORT_SYMBOLS` (this package's `links = "sturdy_rs"`).
    println!("cargo:export_symbols=1");

    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let argument = match target_os.as_str() {
        "linux" | "android" | "freebsd" | "netbsd" | "openbsd" | "dragonfly" | "illumos" | "solaris" => Some("-rdynamic"),
        "macos" | "ios" | "tvos" | "visionos" | "watchos" => Some("-Wl,-export_dynamic"),
        _ => None,
    };
    if let Some(argument) = argument {
        // This package is a library, so the only thing it links is its own unit-test binary; the plain form reaches that one (the
        // `-tests` form only names integration-test targets, which this package does not have, and cargo rejects it).
        println!("cargo:rustc-link-arg={argument}");
    }
}
