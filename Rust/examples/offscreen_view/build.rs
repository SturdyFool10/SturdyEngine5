//! Applies the `export-symbols` link flag so engine stack traces name functions (see the sturdy-build crate).
fn main() {
    sturdy_build::emit();
}
