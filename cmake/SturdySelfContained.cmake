# Self-contained Linux executables: the finished binary needs nothing from the machine it runs on except what every
# Linux system of that kind guarantees -- the C library (glibc: libc, libm, libdl, libpthread, librt, libutil and the
# dynamic loader). Everything else (libstdc++, libgcc, SDL3, HarfBuzz, spdlog, fmt, zlib, ...) is linked into the executable.
# Libraries the engine opens at run time because the machine may legitimately lack them (the Vulkan loader, X11/Wayland/
# audio libraries inside SDL3) stay dlopen()ed; they are part of the GPU driver / desktop, not of this program.
#
# How the list of offenders was found (lld, RuntimeDemo, Dist):
#   * `llvm-readelf -d <exe>`            NEEDED entries: harfbuzz, spdlog, fmt, SDL3, wayland-client, zlib, libdeflate, libjpeg,
#                                        libjbig, liblzma, libzstd, libopenjph, libstdc++, libgcc_s.
#   * the link line (`ninja -t commands`) showed those as /usr/lib/*.so inputs: STURDY_PREFER_SYSTEM_DEPENDENCIES made
#                                        find_package() win over the pinned FetchContent sources, and libtiff/OpenEXR picked up
#                                        system codec libraries on their own.
#   * `llvm-nm -u` of each static archive attributed each shared library to the archive that needs it (libtiff.a -> zlib/jpeg/
#                                        jbig/lzma/zstd, OpenEXRCore -> openjph/deflate, WindowManager -> SDL3 + wayland-client).
#   * `llvm-readelf -V` / `llvm-nm -D --undefined-only` showed the binary demanding GLIBC_2.43 (new libm symbol versions) and
#                                        GLIBCXX_3.4.36: it would not start on any distribution older than a few months.
#   * lld honours `.symver` on undefined references and libm still exports the old versions, which is what glibc_compat.c uses.
#
# Other findings, in the order they surfaced (details in the matching sections below):
#   * The toolchain's own static runtime is not portable: on CachyOS' znver4 repositories libstdc++.a / libgcc.a / libc++.a contain
#     AVX-512 in code that runs unconditionally (_Unwind_RaiseException, std::ostringstream). LLVM's libc++ is built from source instead.
#   * Static OpenSSL and the HTTP/2, HTTP/3, QUIC and io_uring libraries are built once into the shared dependency cache.
#   * libwayland-client is opened at run time by WindowManager/Linux/WaylandClientLoader.cpp (symbols defined as trampolines).
#
# Remaining known limits (the post-link check reports them):
#   * glibc floor 2.34 (the pthread merge into libc; __libc_start_main). Going lower needs an older glibc sysroot (STURDY_LINUX_SYSROOT).
#   * The optional GLFW window provider links libX11/libwayland-client directly, so it is left out of a self-contained build.
#   * Linux x86-64 is the verified target; the glibc pinning is x86-64 only and AArch64 is untested.

include_guard(GLOBAL)
include(CheckSymbolExists)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_sturdy_self_contained_default ON)
else()
    set(_sturdy_self_contained_default OFF)
endif()

option(STURDY_SELF_CONTAINED
    "Link Linux executables so they need only the C library: no system third-party libraries, a statically linked C++ runtime, no -march=native, glibc symbol versions pinned. Overrides STURDY_PREFER_SYSTEM_DEPENDENCIES."
    ${_sturdy_self_contained_default})
option(STURDY_SELF_CONTAINED_CHECK
    "Inspect every executable after linking and fail the build if it needs a shared library outside the C-library allow-list or a glibc newer than STURDY_GLIBC_FLOOR."
    ON)
set(STURDY_GLIBC_FLOOR "2.34" CACHE STRING
    "Newest glibc symbol version a self-contained executable may require (post-link check). Empty reports the version without failing.")
set(STURDY_SELF_CONTAINED_TOLERATED_LIBS "" CACHE STRING
    "Shared libraries the post-link check reports as a warning instead of an error (for a known gap; empty by default, so every unexpected library fails the build).")
set(STURDY_LINUX_SYSROOT "" CACHE PATH
    "Optional sysroot with an older glibc (for example a manylinux/Debian-oldstable root). Passed as --sysroot so the binary records the older symbol versions.")

set(STURDY_SELF_CONTAINED_RUNTIME "LIBCXX" CACHE STRING
    "C++ runtime of a self-contained build: LIBCXX (LLVM's, built from source at baseline ISA; Clang only) or SYSTEM (the toolchain's static libstdc++).")
set_property(CACHE STURDY_SELF_CONTAINED_RUNTIME PROPERTY STRINGS LIBCXX SYSTEM)
set(STURDY_LIBCXX_TAG "llvmorg-${CMAKE_CXX_COMPILER_VERSION}" CACHE STRING
    "LLVM release tag whose libc++/libc++abi/libunwind/compiler-rt are built for LIBCXX. Defaults to the compiler's own version.")

# Builds (once, into the shared dependency cache) and wires in LLVM's C++ runtime. Everything compiled afterwards -- the engine and
# every FetchContent dependency -- uses libc++'s headers, and every C++ link ends with the static archives.
macro(_sturdy_use_built_libcxx)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-target-triple OUTPUT_VARIABLE _triple OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(MD5 _key "${STURDY_LIBCXX_TAG}|${CMAKE_CXX_COMPILER}|${CMAKE_CXX_COMPILER_VERSION}|${_triple}")
    string(SUBSTRING "${_key}" 0 8 _key)
    set(_root "${STURDY_DEPS_CACHE_DIR}")
    if(NOT _root)
        set(_root "${CMAKE_SOURCE_DIR}/.cache/deps")
    endif()
    set(_install "${_root}/libcxx-${STURDY_LIBCXX_TAG}-${_key}-install")
    if(NOT EXISTS "${_install}/.sturdy-complete")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" "-DTAG=${STURDY_LIBCXX_TAG}" "-DSOURCE_DIR=${_root}/llvm-runtimes-${STURDY_LIBCXX_TAG}-src"
                "-DBUILD_DIR=${_root}/libcxx-${STURDY_LIBCXX_TAG}-${_key}-build" "-DINSTALL_DIR=${_install}"
                "-DCC=${CMAKE_C_COMPILER}" "-DCXX=${CMAKE_CXX_COMPILER}" "-DTRIPLE=${_triple}"
                -P "${CMAKE_SOURCE_DIR}/cmake/BuildLlvmRuntimes.cmake"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "Building LLVM's C++ runtime failed (see above). Set -DSTURDY_SELF_CONTAINED_RUNTIME=SYSTEM to use the toolchain's own.")
        endif()
    endif()
    file(GLOB_RECURSE _builtins "${_install}/lib/libclang_rt.builtins*.a")
    list(GET _builtins 0 _builtins)

    # Static third-party libraries (spdlog, fmt, ...) end up inside shared objects too -- the hot-reload and plugin fixtures. Code compiled
    # for an executable only (-fPIE) cannot be linked there (a thread_local of spdlog fails with R_X86_64_TPOFF32), so everything is -fPIC.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    # Nothing here uses C++ modules. CMake would otherwise synthesize an `import std` target from the *toolchain's* standard-library module
    # sources (libstdc++'s bits/std.cc), which do not exist under -nostdinc++ and are not libc++'s.
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

    # Compile every C++ file against libc++'s headers (the install's __config_site matches the archives).
    set(_include "${_install}/include/c++/v1")
    string(APPEND CMAKE_CXX_FLAGS " -nostdinc++ -isystem ${_include}")

    # Link: no implicit C++ library or unwinder from the driver; the archives go on every C++ link line (CMAKE_CXX_STANDARD_LIBRARIES),
    # ahead of the driver's own -lgcc, so their definitions win and libgcc.a members are pulled only for symbols they lack.
    foreach(_kind EXE SHARED MODULE)
        # --exclude-libs keeps the runtime's symbols out of the dynamic symbol table even with --export-dynamic (STURDY_EXPORT_SYMBOLS): a
        # dlopen'd plugin -- a Rust cdylib on libgcc_s, a C++ module with its own C++ runtime -- must not bind its _Unwind_*/__cxa_* calls
        # to the host's copies (a Rust panic then fails with "failed to initiate panic, error 3").
        string(APPEND CMAKE_${_kind}_LINKER_FLAGS " -nostdlib++ --unwindlib=none -Xlinker --exclude-libs=libc++.a,libc++abi.a,libunwind.a")
    endforeach()
    set(CMAKE_CXX_STANDARD_LIBRARIES "${_install}/lib/libc++.a ${_install}/lib/libc++abi.a ${_install}/lib/libunwind.a ${_builtins} -lpthread -ldl")
    set(STURDY_LIBCXX_ROOT "${_install}" CACHE INTERNAL "")
    message(STATUS "Self-contained C++ runtime: LLVM ${STURDY_LIBCXX_TAG} libc++ (${_install})")
endmacro()

# Called once from the root CMakeLists, after the build matrix and before any dependency is configured.
function(sturdy_self_contained_configure)
    if(NOT STURDY_SELF_CONTAINED)
        return()
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(STATUS "Self-contained: only implemented for Linux; ignored on ${CMAKE_SYSTEM_NAME}.")
        return()
    endif()

    # 0. SDL3 is the window provider that opens X11, Wayland and the rest at run time. The optional GLFW provider links libX11 and
    #    libwayland-client directly (and repeats SDL3's generated protocol symbols), so a program carrying it needs both libraries on every
    #    machine. Build it with -DSTURDY_SELF_CONTAINED=OFF.
    if(STURDY_BUILD_GLFW_WINDOW_PROVIDER)
        message(STATUS "Self-contained: the GLFW window provider is left out (it links libX11/libwayland-client directly); SDL3 is used.")
        set(STURDY_BUILD_GLFW_WINDOW_PROVIDER OFF CACHE BOOL "Build the optional GLFW window provider." FORCE)
    endif()

    # 1. Third-party code comes from the pinned FetchContent sources, never from whatever the build machine has installed.
    set(STURDY_PREFER_SYSTEM_DEPENDENCIES OFF CACHE BOOL "Try find_package before downloading FetchContent dependencies." FORCE)

    # 2. -march=native (usually injected by CFLAGS/CXXFLAGS) bakes the build machine's instruction set into the program:
    #    it dies with SIGILL on any older CPU. Runtime-dispatched SIMD is the supported route (plans/capability-roadmap).
    foreach(_flags_var CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_ASM_FLAGS)
        if(DEFINED ${_flags_var} AND "${${_flags_var}}" MATCHES "-m(arch|tune|cpu)=native")
            string(REGEX REPLACE "(^| )-m(arch|tune|cpu)=native" "" _cleaned "${${_flags_var}}")
            set(${_flags_var} "${_cleaned}")
            message(STATUS "Self-contained: removed -m*=native from ${_flags_var} (it would make the binary CPU-specific).")
        endif()
    endforeach()

    # 3. The C++ runtime and the compiler runtime live inside the executable (libstdc++ is not on every system, and the system
    #    one is usually older than the one the program was compiled against). With Clang they are LLVM's libc++/libc++abi/
    #    libunwind/compiler-rt, built from source with the baseline instruction set: the toolchain's own static archives are
    #    compiled for the distribution's CPU level (see cmake/BuildLlvmRuntimes.cmake) and crash older CPUs.
    if(STURDY_SELF_CONTAINED_RUNTIME STREQUAL "LIBCXX" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        _sturdy_use_built_libcxx()
    else()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            message(WARNING "Self-contained: linking the toolchain's own static libstdc++/libgcc. They are built for the distribution's CPU "
                "level (check with `llvm-readelf -n libstdc++.a | rg 'x86-64-v'`); use Clang with STURDY_SELF_CONTAINED_RUNTIME=LIBCXX for a "
                "runtime that runs on every CPU.")
        endif()
        add_link_options("$<$<LINK_LANGUAGE:CXX>:-static-libstdc++>" "-static-libgcc")
    endif()

    # 4. Optional older glibc.
    if(STURDY_LINUX_SYSROOT)
        add_compile_options("--sysroot=${STURDY_LINUX_SYSROOT}")
        add_link_options("--sysroot=${STURDY_LINUX_SYSROOT}")
    endif()

    # 5. glibc symbol-version pinning (x86-64 glibc only; the version names inside are architecture specific).
    check_symbol_exists(__GLIBC__ features.h STURDY_HAVE_GLIBC)
    if(STURDY_HAVE_GLIBC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$" AND NOT STURDY_LINUX_SYSROOT)
        add_library(sturdy_glibc_compat OBJECT "${CMAKE_SOURCE_DIR}/cmake/linux/glibc_compat.c")
        set_target_properties(sturdy_glibc_compat PROPERTIES
            C_STANDARD 17
            C_EXTENSIONS ON
            POSITION_INDEPENDENT_CODE ON
            INTERPROCEDURAL_OPTIMIZATION OFF)
        # -fno-builtin: these files define sqrtf & co, which the compiler must not treat as intrinsics.
        # -fno-lto: native code, so the .symver directives are assembled as written instead of surviving as module asm.
        target_compile_options(sturdy_glibc_compat PRIVATE -O2 -fno-builtin -fno-lto -fvisibility=hidden -fno-stack-protector)
        set(STURDY_GLIBC_COMPAT_ENABLED ON CACHE INTERNAL "")
    else()
        set(STURDY_GLIBC_COMPAT_ENABLED OFF CACHE INTERNAL "")
        message(STATUS "Self-contained: glibc symbol pinning skipped (needs x86-64 glibc without STURDY_LINUX_SYSROOT).")
    endif()

    # The runtime macro above and the flag clean-up both edit this function's copies of the variables; hand them to the root.
    foreach(_var CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_ASM_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS CMAKE_CXX_STANDARD_LIBRARIES CMAKE_CXX_SCAN_FOR_MODULES CMAKE_POSITION_INDEPENDENT_CODE)
        set(${_var} "${${_var}}" PARENT_SCOPE)
    endforeach()

    find_program(STURDY_READELF NAMES llvm-readelf readelf)
    message(STATUS "Self-contained Linux build: system libraries off, static libstdc++/libgcc, glibc floor ${STURDY_GLIBC_FLOOR}")
endfunction()

# Applied to every executable (not to tests, which never leave the build machine).
function(sturdy_apply_self_contained target)
    if(NOT STURDY_SELF_CONTAINED OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    if(STURDY_GLIBC_COMPAT_ENABLED)
        target_sources("${target}" PRIVATE $<TARGET_OBJECTS:sturdy_glibc_compat>)
    endif()
    if(STURDY_SELF_CONTAINED_CHECK AND STURDY_READELF)
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DBINARY=$<TARGET_FILE:${target}>"
                "-DREADELF=${STURDY_READELF}"
                "-DGLIBC_FLOOR=${STURDY_GLIBC_FLOOR}"
                "-DTOLERATED=${STURDY_SELF_CONTAINED_TOLERATED_LIBS}"
                -P "${CMAKE_SOURCE_DIR}/cmake/SturdyCheckLinkage.cmake"
            COMMENT "Checking ${target} is self-contained"
            VERBATIM)
    endif()
endfunction()

# ---- optional system libraries -------------------------------------------------------------------------------------------
# Some features link a library found through pkg-config (Async: nghttp2, ngtcp2, nghttp3, liburing; zlib through FindZLIB).
# A self-contained build may use them only as static archives; a shared copy would put a new NEEDED entry into every
# executable. Probe like this:
#
#     sturdy_self_contained_static_probe_begin()
#     pkg_check_modules(FOO IMPORTED_TARGET libfoo)
#     sturdy_self_contained_static_probe_end()
#     sturdy_self_contained_accepts(FOO _foo_ok)       # TRUE outside self-contained builds
#     if(FOO_FOUND AND _foo_ok) ...
macro(sturdy_self_contained_static_probe_begin)
    if(STURDY_SELF_CONTAINED AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_sturdy_saved_find_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    endif()
endmacro()

macro(sturdy_self_contained_static_probe_end)
    if(DEFINED _sturdy_saved_find_suffixes)
        set(CMAKE_FIND_LIBRARY_SUFFIXES "${_sturdy_saved_find_suffixes}")
        unset(_sturdy_saved_find_suffixes)
    endif()
endmacro()

# pkg_check_modules() keeps a bare `-lfoo` when it cannot resolve a library to a file, and the linker would then pick the shared
# copy. Accept the module only when everything it links resolved to a static archive (or to the C library).
function(sturdy_self_contained_accepts prefix out_var)
    if(NOT STURDY_SELF_CONTAINED OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(${out_var} TRUE PARENT_SCOPE)
        return()
    endif()
    set(_accepted TRUE)
    foreach(_lib IN LISTS ${prefix}_LINK_LIBRARIES)
        if(_lib MATCHES "^-l(c|m|dl|pthread|rt|util|atomic)$" OR _lib MATCHES "\\.a$")
            continue()
        endif()
        set(_accepted FALSE)
        message(STATUS "Self-contained: ${prefix} needs '${_lib}', which is not a static archive; feature left off.")
        break()
    endforeach()
    set(${out_var} ${_accepted} PARENT_SCOPE)
endfunction()

# Called from the root CMakeLists right after sturdy_self_contained_configure(): the flag clean-up there crosses a function boundary, and a
# slip in that plumbing (it happened once) silently puts -march=native back into every compile -- a binary that dies with SIGILL on older CPUs.
function(sturdy_self_contained_verify)
    if(NOT STURDY_SELF_CONTAINED OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    foreach(_flags_var CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_ASM_FLAGS)
        if("${${_flags_var}}" MATCHES "-m(arch|tune|cpu)=native")
            message(FATAL_ERROR "Self-contained build: ${_flags_var} still contains a -m*=native flag (${${_flags_var}}). Executables built with it "
                "only run on the CPU model that built them.")
        endif()
    endforeach()
endfunction()
