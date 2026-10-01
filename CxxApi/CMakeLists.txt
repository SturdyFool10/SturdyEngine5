# Sturdy.CxxApi — the engine's C++ API restated in the subset of C++ that the Rust `cxx` crate (and
# bindgen for the plain-data types) can bind directly. See plans/cxx-api.md for the rules every
# declaration follows.
#
# A leaf like FFI: nothing in the engine depends on it. It only translates; features live below.
sturdy_add_package(CxxApi
    PUBLIC_DEPS
        Sturdy::Runtime
)

sturdy_add_tests(CxxApi TIMEOUT 60)
