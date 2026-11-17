# Static copies of the libraries behind Async's optional network features, for builds that must not depend on what the machine has
# installed (cmake/SturdySelfContained.cmake): HTTP/2 (nghttp2), HTTP/3 (nghttp3), QUIC (ngtcp2 + its OpenSSL glue), io_uring (liburing)
# and gzip/deflate decoding in HttpClient (zlib).
#
# They are built once per (tags, compiler) pair by cmake/BuildNetLibs.cmake into the shared dependency cache, not in every build tree:
# a clean build of a new tree reuses them and spends no time compiling them. Called from sturdy_fetch_dependencies() after OpenSSL, because
# ngtcp2's TLS glue (compiled here, from the same checkout) needs OpenSSL 3.5's QUIC API.
include_guard(GLOBAL)

set(STURDY_NGHTTP2_TAG "v1.71.0" CACHE STRING "nghttp2 git tag to build.")
set(STURDY_NGHTTP3_TAG "v1.19.0" CACHE STRING "nghttp3 git tag to build.")
set(STURDY_NGTCP2_TAG "v1.26.0" CACHE STRING "ngtcp2 git tag to build.")
set(STURDY_LIBURING_TAG "liburing-2.15" CACHE STRING "liburing git tag to build.")
set(STURDY_ZLIB_TAG "v1.3.2" CACHE STRING "zlib git tag to build.")
# The OpenSSL release of the shared prefix reuses the project-wide pin (cmake/SturdyDependencies.cmake, STURDY_OPENSSL_VERSION).

function(sturdy_fetch_net_libs)
    set(_root "${STURDY_DEPS_CACHE_DIR}")
    set(_with_uring OFF)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_with_uring ON)
    endif()
    set(_tags "openssl=${STURDY_OPENSSL_VERSION};zlib=${STURDY_ZLIB_TAG};nghttp2=${STURDY_NGHTTP2_TAG};nghttp3=${STURDY_NGHTTP3_TAG};ngtcp2=${STURDY_NGTCP2_TAG};liburing=${STURDY_LIBURING_TAG}")
    string(MD5 _key "${_tags}|${_with_uring}|${CMAKE_C_COMPILER}|${CMAKE_C_COMPILER_VERSION}")
    string(SUBSTRING "${_key}" 0 8 _key)
    set(_install "${_root}/net-${_key}-install")
    if(NOT EXISTS "${_install}/.sturdy-complete")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" "-DROOT=${_root}" "-DINSTALL_DIR=${_install}" "-DCC=${CMAKE_C_COMPILER}" "-DTAGS=${_tags}"
                "-DWITH_LIBURING=${_with_uring}" -P "${CMAKE_SOURCE_DIR}/cmake/BuildNetLibs.cmake"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "Building the static network libraries failed (see above). "
                "Configure with -DSTURDY_SELF_CONTAINED=OFF to use the system's, or -DSTURDY_ASYNC_WITH_HTTP2=OFF -DSTURDY_ASYNC_WITH_QUIC=OFF "
                "-DSTURDY_ASYNC_WITH_IO_URING=OFF to leave them out.")
        endif()
    endif()

    function(_sturdy_net_imported target archive)
        add_library(${target} STATIC IMPORTED GLOBAL)
        set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${_install}/lib/${archive}" INTERFACE_INCLUDE_DIRECTORIES "${_install}/include")
    endfunction()
    _sturdy_net_imported(sturdy_zlib libz.a)
    _sturdy_net_imported(sturdy_nghttp2 libnghttp2.a)
    target_compile_definitions(sturdy_nghttp2 INTERFACE NGHTTP2_STATICLIB)
    _sturdy_net_imported(sturdy_nghttp3 libnghttp3.a)
    target_compile_definitions(sturdy_nghttp3 INTERFACE NGHTTP3_STATICLIB)
    _sturdy_net_imported(sturdy_ngtcp2_core libngtcp2.a)
    target_compile_definitions(sturdy_ngtcp2_core INTERFACE NGTCP2_STATICLIB)
    if(_with_uring)
        _sturdy_net_imported(sturdy_liburing liburing.a)
    endif()

    # OpenSSL from the prefix, under the names find_package(OpenSSL) would have made (Async links these the same way on every platform).
    add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::Crypto PROPERTIES IMPORTED_LOCATION "${_install}/lib/libcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_install}/include" INTERFACE_LINK_LIBRARIES "pthread;dl")
    add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::SSL PROPERTIES IMPORTED_LOCATION "${_install}/lib/libssl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_install}/include" INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
    set(OPENSSL_VERSION "${STURDY_OPENSSL_VERSION}" CACHE STRING "" FORCE)
    set(OPENSSL_FOUND TRUE CACHE BOOL "" FORCE)
    sturdy_register_license(openssl "${_root}/net-src/openssl-${STURDY_OPENSSL_VERSION}")

    # The OpenSSL glue of ngtcp2 (two C files), built against this build's OpenSSL.
    set(_ngtcp2_src "${_root}/net-src/ngtcp2-${STURDY_NGTCP2_TAG}")
    add_library(sturdy_ngtcp2_crypto_ossl STATIC "${_ngtcp2_src}/crypto/ossl/ossl.c" "${_ngtcp2_src}/crypto/shared.c")
    target_include_directories(sturdy_ngtcp2_crypto_ossl PUBLIC "${_ngtcp2_src}/crypto/includes")
    target_include_directories(sturdy_ngtcp2_crypto_ossl PRIVATE "${_ngtcp2_src}/crypto" "${_ngtcp2_src}/lib")
    target_link_libraries(sturdy_ngtcp2_crypto_ossl PUBLIC sturdy_ngtcp2_core OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(sturdy_ngtcp2_crypto_ossl PROPERTIES POSITION_INDEPENDENT_CODE ON EXCLUDE_FROM_ALL ON)

    sturdy_register_license(zlib "${_root}/net-src/zlib-${STURDY_ZLIB_TAG}")
    sturdy_register_license(nghttp2 "${_root}/net-src/nghttp2-${STURDY_NGHTTP2_TAG}")
    sturdy_register_license(nghttp3 "${_root}/net-src/nghttp3-${STURDY_NGHTTP3_TAG}")
    sturdy_register_license(ngtcp2 "${_ngtcp2_src}")
    if(_with_uring)
        sturdy_register_license(liburing "${_root}/net-src/liburing-${STURDY_LIBURING_TAG}")
    endif()
endfunction()
