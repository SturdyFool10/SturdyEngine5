# Builds LLVM's libc++, libc++abi, libunwind and compiler-rt builtins from source into a shared install prefix. Run in script mode by
# cmake/SturdySelfContained.cmake at configure time, once per (LLVM tag, compiler) pair:
#   cmake -DTAG=llvmorg-23.1.5 -DSOURCE_DIR=... -DBUILD_DIR=... -DINSTALL_DIR=... -DCC=clang -DCXX=clang++ -P BuildLlvmRuntimes.cmake
#
# Why not the toolchain's own: a distribution's libstdc++.a / libgcc.a / libc++.a is compiled for the distribution's CPU level. On
# CachyOS' znver4 repositories, for example, 53 libstdc++ objects and 35 libgcc objects are x86-64-v4 (AVX-512 in `_Unwind_RaiseException`
# and `std::ostringstream`), so a static link of them dies with SIGILL on any CPU without AVX-512. Built here with the default
# x86-64 baseline and no -march, the archives run on every CPU of the architecture.
cmake_minimum_required(VERSION 3.20)

foreach(_required TAG SOURCE_DIR BUILD_DIR INSTALL_DIR CC CXX TRIPLE)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "BuildLlvmRuntimes: -D${_required}=... is required")
    endif()
endforeach()

# Sparse, shallow checkout: only the runtimes and what their build files include (about 240 MB instead of the whole monorepo).
if(NOT EXISTS "${SOURCE_DIR}/runtimes/CMakeLists.txt")
    message(STATUS "LLVM runtimes: fetching ${TAG} (sparse)")
    file(REMOVE_RECURSE "${SOURCE_DIR}")
    execute_process(
        COMMAND git clone --quiet --depth 1 --filter=blob:none --sparse --branch "${TAG}"
                https://github.com/llvm/llvm-project.git "${SOURCE_DIR}"
        RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "LLVM runtimes: git clone of ${TAG} failed: ${_err}\n"
            "Set STURDY_LIBCXX_TAG to an existing llvmorg-* tag, or STURDY_SELF_CONTAINED_RUNTIME=SYSTEM to use the toolchain's own runtime.")
    endif()
    execute_process(
        COMMAND git sparse-checkout set runtimes libcxx libcxxabi libunwind compiler-rt cmake llvm/cmake llvm/utils third-party libc
        WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "LLVM runtimes: git sparse-checkout failed: ${_err}")
    endif()
endif()

# The build must not inherit the user's CFLAGS/CXXFLAGS/LDFLAGS (typically -march=native), or the result would not be baseline.
set(_clean_env
    ${CMAKE_COMMAND} -E env --unset=CFLAGS --unset=CXXFLAGS --unset=LDFLAGS --unset=CPPFLAGS
        --unset=CC --unset=CXX)

# A list-valued cache entry cannot be passed reliably on a command line; an initial-cache file keeps the semicolons intact.
file(REMOVE_RECURSE "${BUILD_DIR}")
file(MAKE_DIRECTORY "${BUILD_DIR}")
file(WRITE "${BUILD_DIR}/initial-cache.cmake"
    "set(LLVM_ENABLE_RUNTIMES \"compiler-rt;libunwind;libcxxabi;libcxx\" CACHE STRING \"\")\n")

message(STATUS "LLVM runtimes: configuring")
execute_process(
    COMMAND ${_clean_env} ${CMAKE_COMMAND} -C "${BUILD_DIR}/initial-cache.cmake" -S "${SOURCE_DIR}/runtimes" -B "${BUILD_DIR}" -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_C_COMPILER=${CC}" "-DCMAKE_CXX_COMPILER=${CXX}"
        "-DCMAKE_C_COMPILER_TARGET=${TRIPLE}" "-DCMAKE_CXX_COMPILER_TARGET=${TRIPLE}"
        "-DCMAKE_C_FLAGS=-fno-lto" "-DCMAKE_CXX_FLAGS=-fno-lto"
        -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF
        -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF
        -DCOMPILER_RT_BUILD_ORC=OFF -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF -DCOMPILER_RT_BUILD_CRT=OFF
        -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON
        -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON
        -DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON -DLIBCXXABI_STATICALLY_LINK_UNWINDER_IN_STATIC_LIBRARY=ON
        -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF
        -DLIBCXX_HARDENING_MODE=none -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        "-DCMAKE_INSTALL_PREFIX=${INSTALL_DIR}" -DLLVM_INCLUDE_TESTS=OFF
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "LLVM runtimes: configure failed:\n${_out}\n${_err}")
endif()

message(STATUS "LLVM runtimes: building (libc++, libc++abi, libunwind, compiler-rt builtins)")
execute_process(
    COMMAND ${_clean_env} ${CMAKE_COMMAND} --build "${BUILD_DIR}" --target install
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "LLVM runtimes: build failed:\n${_out}\n${_err}")
endif()

if(NOT EXISTS "${INSTALL_DIR}/lib/libc++.a" OR NOT EXISTS "${INSTALL_DIR}/lib/libc++abi.a" OR NOT EXISTS "${INSTALL_DIR}/lib/libunwind.a")
    message(FATAL_ERROR "LLVM runtimes: the build finished but ${INSTALL_DIR}/lib lacks libc++.a / libc++abi.a / libunwind.a")
endif()
file(GLOB_RECURSE _builtins "${INSTALL_DIR}/lib/libclang_rt.builtins*.a")
if(NOT _builtins)
    message(FATAL_ERROR "LLVM runtimes: no libclang_rt.builtins*.a was installed under ${INSTALL_DIR}/lib")
endif()
file(WRITE "${INSTALL_DIR}/.sturdy-complete" "${TAG}\n")
message(STATUS "LLVM runtimes: installed to ${INSTALL_DIR}")
