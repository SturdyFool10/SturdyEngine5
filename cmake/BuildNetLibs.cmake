# Builds zlib, nghttp2, nghttp3, ngtcp2 (core, no TLS backend) and liburing from source as static libraries into one install prefix.
# Run in script mode by cmake/SturdyNetLibs.cmake at configure time, once per (tags, compiler) pair, into the shared dependency cache:
#   cmake -DROOT=... -DINSTALL_DIR=... -DCC=clang -DTAGS="zlib=v1.3.2;nghttp2=v1.71.0;..." [-DWITH_LIBURING=ON] -P BuildNetLibs.cmake
#
# Every library here is C and builds with its own CMake project, in its own build tree: the upstream projects define clashing global
# targets (`check`) and options, so they cannot share one CMake tree, and being C they do not depend on the C++ runtime choice. Baseline
# ISA and -fPIC; the user's CFLAGS (typically -march=native) are removed from the environment.
cmake_minimum_required(VERSION 3.20)

foreach(_required ROOT INSTALL_DIR CC TAGS)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "BuildNetLibs: -D${_required}=... is required")
    endif()
endforeach()

set(_clean_env ${CMAKE_COMMAND} -E env --unset=CFLAGS --unset=CXXFLAGS --unset=LDFLAGS --unset=CPPFLAGS --unset=CC --unset=CXX)

foreach(_pair IN LISTS TAGS)
    string(REPLACE "=" ";" _parts "${_pair}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _tag_${_name})
endforeach()

function(_net_clone name repo)
    set(_src "${ROOT}/net-src/${name}-${_tag_${name}}")
    if(NOT EXISTS "${_src}/.git")
        message(STATUS "net libs: fetching ${name} ${_tag_${name}}")
        file(REMOVE_RECURSE "${_src}")
        execute_process(
            COMMAND git clone --quiet --depth 1 --branch "${_tag_${name}}" --recurse-submodules --shallow-submodules "${repo}" "${_src}"
            RESULT_VARIABLE _rc ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "net libs: cloning ${name} ${_tag_${name}} failed: ${_err}")
        endif()
    endif()
    set(_net_src "${_src}" PARENT_SCOPE)
endfunction()

function(_net_cmake name repo)
    _net_clone("${name}" "${repo}")
    set(_build "${ROOT}/net-build/${name}-${_tag_${name}}")
    file(REMOVE_RECURSE "${_build}")
    message(STATUS "net libs: building ${name}")
    execute_process(
        COMMAND ${_clean_env} ${CMAKE_COMMAND} -S "${_net_src}" -B "${_build}" -G Ninja
            -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${CC}" "-DCMAKE_C_FLAGS=-O2 -fPIC -fno-lto"
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON "-DCMAKE_INSTALL_PREFIX=${INSTALL_DIR}" -DCMAKE_INSTALL_LIBDIR=lib
            -DBUILD_TESTING=OFF ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "net libs: configuring ${name} failed:\n${_out}\n${_err}")
    endif()
    execute_process(COMMAND ${_clean_env} ${CMAKE_COMMAND} --build "${_build}" --target install
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "net libs: building ${name} failed:\n${_out}\n${_err}")
    endif()
endfunction()

# OpenSSL: the dominant third-party build of the engine by far, and identical for every build tree, so it lives in the shared prefix too.
# Static, position independent, no apps/tests/docs; the assembly it carries dispatches on the CPU at run time, so the result is baseline.
# --openssldir=/etc/ssl makes a bare SSL_CTX_set_default_verify_paths() look where most distributions keep their trust store.
if(DEFINED _tag_openssl)
    set(_ossl_name "openssl-${_tag_openssl}")
    set(_ossl_src "${ROOT}/net-src/${_ossl_name}")
    if(NOT EXISTS "${_ossl_src}/Configure")
        message(STATUS "net libs: fetching OpenSSL ${_tag_openssl}")
        file(MAKE_DIRECTORY "${ROOT}/net-src")
        set(_tarball "${ROOT}/net-src/${_ossl_name}.tar.gz")
        file(DOWNLOAD "https://github.com/openssl/openssl/releases/download/${_ossl_name}/${_ossl_name}.tar.gz" "${_tarball}"
            STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            message(FATAL_ERROR "net libs: downloading OpenSSL ${_tag_openssl} failed: ${_status}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_tarball}" WORKING_DIRECTORY "${ROOT}/net-src" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0 OR NOT EXISTS "${_ossl_src}/Configure")
            message(FATAL_ERROR "net libs: unpacking ${_tarball} failed")
        endif()
        file(REMOVE "${_tarball}")
    endif()
    set(_ossl_build "${ROOT}/net-build/${_ossl_name}")
    file(REMOVE_RECURSE "${_ossl_build}")
    file(MAKE_DIRECTORY "${_ossl_build}")
    cmake_host_system_information(RESULT _jobs QUERY NUMBER_OF_LOGICAL_CORES)
    message(STATUS "net libs: building OpenSSL ${_tag_openssl} (-j${_jobs})")
    execute_process(
        COMMAND ${_clean_env} "${CMAKE_COMMAND}" -E env "CC=${CC}" perl "${_ossl_src}/Configure" no-shared no-tests no-docs no-apps no-module
            "--prefix=${INSTALL_DIR}" --libdir=lib --openssldir=/etc/ssl -fPIC -O2
        WORKING_DIRECTORY "${_ossl_build}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "net libs: configuring OpenSSL failed:\n${_out}\n${_err}")
    endif()
    execute_process(COMMAND ${_clean_env} make -j${_jobs} build_libs
        WORKING_DIRECTORY "${_ossl_build}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(_rc EQUAL 0)
        execute_process(COMMAND ${_clean_env} make install_dev
            WORKING_DIRECTORY "${_ossl_build}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    endif()
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "net libs: building OpenSSL failed:\n${_out}\n${_err}")
    endif()
endif()

_net_cmake(zlib https://github.com/madler/zlib.git
    -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_EXAMPLES=OFF -DZLIB_INSTALL=ON)

_net_cmake(nghttp2 https://github.com/nghttp2/nghttp2.git
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DENABLE_LIB_ONLY=ON -DENABLE_DOC=OFF -DENABLE_FAILMALLOC=OFF
    -DENABLE_WERROR=OFF -DENABLE_EXAMPLES=OFF -DENABLE_APP=OFF -DENABLE_HPACK_TOOLS=OFF)

_net_cmake(nghttp3 https://github.com/ngtcp2/nghttp3.git
    -DENABLE_LIB_ONLY=ON -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=OFF -DENABLE_WERROR=OFF -DENABLE_ASAN=OFF)

# ngtcp2: the core library only. Its OpenSSL backend probes QUIC symbols by linking against libssl, which does not exist yet at
# configure time (OpenSSL is built by the main build); the small OpenSSL glue is compiled by cmake/SturdyNetLibs.cmake instead.
_net_cmake(ngtcp2 https://github.com/ngtcp2/ngtcp2.git
    -DENABLE_LIB_ONLY=ON -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=OFF -DENABLE_OPENSSL=OFF -DENABLE_GNUTLS=OFF
    -DENABLE_BORINGSSL=OFF -DENABLE_PICOTLS=OFF -DENABLE_WOLFSSL=OFF -DENABLE_JEMALLOC=OFF -DENABLE_WERROR=OFF -DENABLE_ASAN=OFF)

if(WITH_LIBURING)
    # liburing has no CMake build: configure writes config-host.h and the kernel-compat header, then six C files make the library (nolibc.c supplies __uring_memset & co. when configure picks liburing's no-libc mode, as it does on x86-64).
    _net_clone(liburing https://github.com/axboe/liburing.git)
    set(_uring "${_net_src}")
    if(NOT EXISTS "${_uring}/config-host.h")
        execute_process(COMMAND ${_clean_env} "${CMAKE_COMMAND}" -E env "CC=${CC}" ./configure WORKING_DIRECTORY "${_uring}"
            RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "net libs: liburing ./configure failed: ${_err}")
        endif()
    endif()
    set(_uring_build "${ROOT}/net-build/liburing-${_tag_liburing}")
    file(REMOVE_RECURSE "${_uring_build}")
    file(MAKE_DIRECTORY "${_uring_build}")
    file(WRITE "${_uring_build}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.20)
project(liburing_static C)
add_library(uring STATIC \"${_uring}/src/setup.c\" \"${_uring}/src/queue.c\" \"${_uring}/src/register.c\"
    \"${_uring}/src/syscall.c\" \"${_uring}/src/version.c\" \"${_uring}/src/nolibc.c\")
target_include_directories(uring PRIVATE \"${_uring}/src/include\" \"${_uring}/src\")
target_compile_definitions(uring PRIVATE _GNU_SOURCE LIBURING_INTERNAL _LARGEFILE_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(uring PRIVATE -include \"${_uring}/config-host.h\" -fno-stack-protector -w)
install(TARGETS uring ARCHIVE DESTINATION lib)
install(DIRECTORY \"${_uring}/src/include/\" DESTINATION include FILES_MATCHING PATTERN \"*.h\")
")
    message(STATUS "net libs: building liburing")
    execute_process(
        COMMAND ${_clean_env} ${CMAKE_COMMAND} -S "${_uring_build}" -B "${_uring_build}/b" -G Ninja -DCMAKE_BUILD_TYPE=Release
            "-DCMAKE_C_COMPILER=${CC}" "-DCMAKE_C_FLAGS=-O2 -fPIC -fno-lto" -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            "-DCMAKE_INSTALL_PREFIX=${INSTALL_DIR}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(_rc EQUAL 0)
        execute_process(COMMAND ${_clean_env} ${CMAKE_COMMAND} --build "${_uring_build}/b" --target install
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    endif()
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "net libs: building liburing failed:\n${_out}\n${_err}")
    endif()
endif()

file(WRITE "${INSTALL_DIR}/.sturdy-complete" "${TAGS}\n")
message(STATUS "net libs: installed to ${INSTALL_DIR}")
