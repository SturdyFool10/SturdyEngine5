#pragma once

// Clang doesn't apply the standard [[no_unique_address]] when targeting the MSVC ABI — MSVC's object
// layout historically couldn't support it, so Clang silently ignores it there (with a warning) rather
// than risk an ABI mismatch with MSVC-built objects. The fix is the vendor spelling
// [[msvc::no_unique_address]], which Clang does implement correctly on that target. Detect whichever
// spelling actually works via __has_cpp_attribute instead of hand-checking compiler/target macros, so
// this keeps working as toolchains change.
#if defined(__has_cpp_attribute)
    #if __has_cpp_attribute(msvc::no_unique_address)
        #define STURDY_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
    #elif __has_cpp_attribute(no_unique_address)
        #define STURDY_NO_UNIQUE_ADDRESS [[no_unique_address]]
    #else
        #define STURDY_NO_UNIQUE_ADDRESS
    #endif
#else
    #define STURDY_NO_UNIQUE_ADDRESS
#endif
