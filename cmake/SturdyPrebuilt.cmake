include_guard(GLOBAL)

# Prebuilt static dependencies: for the few third-party libraries whose upstream publishes official static binaries that are compatible with
# how this engine links, download those instead of cloning and compiling the source. Everything else is untouched and still built from source.
#
# What counts as acceptable (a candidate is rejected, and left on source, if any of these fails):
#   * published by the project itself (its release page or its own download host), not a third-party repackaging;
#   * a real static library (an import library for a DLL is not one);
#   * compatible with the engine's link model: position-independent code (so it also links into a shared build), and on Windows the dynamic
#     C runtime the engine uses (a library built against the static CRT, /MT, clashes with it);
#   * the exact version the engine pins (a different STURDY_*_TAG falls back to source);
#   * pinned by SHA-256 here, and verified on every use. A hash mismatch discards the file and falls back to source; it never links the file.
#
# Each provider fills the same imported targets the source build would, so the rest of the build cannot tell the difference. Whenever a
# provider declines (unsupported platform, version override, offline and not cached, download or hash failure) the caller continues with the
# normal FetchContent source build. See "Prebuilt dependencies" in plans/prebuilt-dependencies.md for what was assessed and rejected, and why.

option(STURDY_PREFER_PREBUILT_DEPENDENCIES
    "Download upstream's official prebuilt static binaries instead of building a dependency from source, where upstream publishes a compatible one (see cmake/SturdyPrebuilt.cmake)." ON)
option(STURDY_PREBUILT_WEBP "Allow the prebuilt libwebp (needs STURDY_PREFER_PREBUILT_DEPENDENCIES)." ON)
option(STURDY_PREBUILT_OPENJPEG "Allow the prebuilt OpenJPEG (needs STURDY_PREFER_PREBUILT_DEPENDENCIES)." ON)
option(STURDY_PREBUILT_GLFW "Allow the prebuilt GLFW (needs STURDY_PREFER_PREBUILT_DEPENDENCIES)." ON)
option(STURDY_PREBUILT_DAWN "Allow the prebuilt Dawn (needs STURDY_PREFER_PREBUILT_DEPENDENCIES)." ON)
set(STURDY_DAWN_PREBUILT_RELEASE "v20261103.174204" CACHE STRING
    "google/dawn release whose official static binaries are used. Only the release pinned in cmake/SturdyPrebuilt.cmake has hashes; any other value builds Dawn from source.")

# Platform key of the TARGET (not the host) in the vocabulary the provider tables use, or empty when no prebuilt can apply: Web, FreeBSD and
# architectures upstream builds nothing for. `windows-*` keys exist so a provider can add one, but no current provider has any (see the plan doc).
function(sturdy_prebuilt_platform out_var)
    set(_key "")
    if(STURDY_OS STREQUAL "Linux")
        if(STURDY_ARCH STREQUAL "x86-64")
            set(_key "linux-x86_64")
        elseif(STURDY_ARCH STREQUAL "Arm64")
            set(_key "linux-aarch64")
        endif()
    elseif(STURDY_OS STREQUAL "MacOS")
        if(STURDY_ARCH STREQUAL "x86-64")
            set(_key "macos-x86_64")
        elseif(STURDY_ARCH STREQUAL "Arm64")
            set(_key "macos-arm64")
        endif()
    elseif(STURDY_OS STREQUAL "Windows")
        if(STURDY_ARCH STREQUAL "x86-64")
            set(_key "windows-x64")
        elseif(STURDY_ARCH STREQUAL "Arm64")
            set(_key "windows-arm64")
        endif()
    endif()
    set(${out_var} "${_key}" PARENT_SCOPE)
endfunction()

# Downloads `url` (verified against `sha256`) into the prebuilt cache and extracts it. Sets `out_dir` to the extraction root, or to "" on any
# failure (the caller then builds from source). A cached archive is re-verified; a corrupt or tampered one is deleted.
function(sturdy_prebuilt_acquire name url sha256 out_dir)
    set(${out_dir} "" PARENT_SCOPE)
    if(NOT DEFINED STURDY_DEPS_CACHE_DIR)
        return()
    endif()
    set(_cache "${STURDY_DEPS_CACHE_DIR}/prebuilt")
    get_filename_component(_file "${url}" NAME)
    string(SUBSTRING "${sha256}" 0 12 _short)
    set(_root "${_cache}/${name}-${_short}")
    if(EXISTS "${_root}/.sturdy-extracted")
        set(${out_dir} "${_root}" PARENT_SCOPE)
        return()
    endif()

    set(_archive "${_cache}/${_short}-${_file}")
    if(EXISTS "${_archive}")
        file(SHA256 "${_archive}" _actual)
        if(NOT _actual STREQUAL sha256)
            message(WARNING "Prebuilt ${name}: the cached ${_file} does not match its pinned SHA-256; discarding it.")
            file(REMOVE "${_archive}")
        endif()
    endif()
    if(NOT EXISTS "${_archive}")
        if(FETCHCONTENT_FULLY_DISCONNECTED)
            message(STATUS "Prebuilt ${name}: offline and not cached; building from source.")
            return()
        endif()
        message(STATUS "Prebuilt ${name}: downloading ${url}")
        file(MAKE_DIRECTORY "${_cache}")
        # Verified by hand below rather than with EXPECTED_HASH: a mismatch there is a hard configure error, and the contract here is that
        # a bad file falls back to the source build.
        file(DOWNLOAD "${url}" "${_archive}" STATUS _status TLS_VERIFY ON INACTIVITY_TIMEOUT 60)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _message)
            message(WARNING "Prebuilt ${name}: could not fetch ${_file} (${_message}); building from source.")
            file(REMOVE "${_archive}")
            return()
        endif()
        file(SHA256 "${_archive}" _actual)
        if(NOT _actual STREQUAL sha256)
            message(WARNING "Prebuilt ${name}: ${_file} does not match its pinned SHA-256 (got ${_actual}); not using it, building from source.")
            file(REMOVE "${_archive}")
            return()
        endif()
    endif()

    file(REMOVE_RECURSE "${_root}")
    file(MAKE_DIRECTORY "${_root}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_root}")
    file(WRITE "${_root}/.sturdy-extracted" "${url}\n${sha256}\n")
    set(${out_dir} "${_root}" PARENT_SCOPE)
endfunction()

# Downloads one small file to `dest` and keeps it only if it matches `sha256`; `out_ok` says whether `dest` now holds the verified file. An
# existing verified file is reused, so a cached prebuilt directory works offline.
function(sturdy_prebuilt_verified_file dest url sha256 out_ok)
    set(${out_ok} FALSE PARENT_SCOPE)
    if(EXISTS "${dest}")
        file(SHA256 "${dest}" _actual)
        if(_actual STREQUAL sha256)
            set(${out_ok} TRUE PARENT_SCOPE)
            return()
        endif()
        file(REMOVE "${dest}")
    endif()
    if(FETCHCONTENT_FULLY_DISCONNECTED)
        return()
    endif()
    get_filename_component(_dir "${dest}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    file(DOWNLOAD "${url}" "${dest}" STATUS _status TLS_VERIFY ON INACTIVITY_TIMEOUT 60)
    list(GET _status 0 _code)
    if(_code EQUAL 0)
        file(SHA256 "${dest}" _actual)
        if(_actual STREQUAL sha256)
            set(${out_ok} TRUE PARENT_SCOPE)
            return()
        endif()
    endif()
    file(REMOVE "${dest}")
endfunction()

# Fetches an upstream license text (the archives carry none) next to the binaries so the engine's license collection still ships attribution.
# Failure is a warning only: the binaries are still the verified ones.
function(sturdy_prebuilt_license name root url sha256)
    get_filename_component(_file "${url}" NAME)
    sturdy_prebuilt_verified_file("${root}/licenses/${name}/${_file}" "${url}" "${sha256}" _ok)
    if(NOT _ok)
        message(WARNING "Prebuilt ${name}: could not fetch a verified license text (${url}); it will be missing from the collected licenses.")
    endif()
endfunction()

# True when a prebuilt library built against the dynamic, non-debug C runtime can be linked here. Always true off Windows. On Windows the
# engine uses the dynamic runtime, and a Debug configuration links the debug flavour of it (and a different iterator-debug level), which a
# release-built C++ library does not match; so Debug builds (single- or multi-config) stay on source there.
function(sturdy_prebuilt_crt_compatible out_var)
    set(_ok TRUE)
    if(STURDY_OS STREQUAL "Windows")
        if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
            set(_ok FALSE)
        endif()
        if(CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
            set(_ok FALSE)
        endif()
    endif()
    set(${out_var} ${_ok} PARENT_SCOPE)
endfunction()

# Common gate: the global switch, the per-dependency switch, a supported platform, and a table entry for it. On success `out_entry` is the
# provider's "url;sha256;subdir" list and `out_platform` the key; otherwise both are empty.
function(sturdy_prebuilt_select name switch_var pinned_tag actual_tag out_entry)
    set(${out_entry} "" PARENT_SCOPE)
    if(NOT STURDY_PREFER_PREBUILT_DEPENDENCIES OR NOT ${switch_var})
        return()
    endif()
    if(NOT "${actual_tag}" STREQUAL "${pinned_tag}")
        message(STATUS "Prebuilt ${name}: STURDY tag is ${actual_tag}, binaries are pinned to ${pinned_tag}; building from source.")
        return()
    endif()
    sturdy_prebuilt_platform(_platform)
    if(NOT _platform)
        return()
    endif()
    set(_entry "${_STURDY_PREBUILT_${name}_${_platform}}")
    if(NOT _entry)
        return()
    endif()
    set(${out_entry} "${_entry}" PARENT_SCOPE)
endfunction()

# ---- libwebp -------------------------------------------------------------------------------------------------------------------------
# Official archives from the WebM project's download host (the location libwebp's own release notes point to). Static libwebp, libwebpdemux and
# libsharpyuv; position-independent. Hashes taken from the files at pin time. No Windows entry: the official Windows libwebp.lib is built against
# the static C runtime (LIBCMT), which the engine's dynamic-runtime link cannot take.
set(_STURDY_WEBP_BASE "https://storage.googleapis.com/downloads.webmproject.org/releases/webp")
set(_STURDY_PREBUILT_webp_linux-x86_64
    "${_STURDY_WEBP_BASE}/libwebp-1.6.0-linux-x86-64.tar.gz;1c5ffab71efecefa0e3c23516c3a3a1dccb45cc310ae1095c6f14ae268e38067;libwebp-1.6.0-linux-x86-64")
set(_STURDY_PREBUILT_webp_linux-aarch64
    "${_STURDY_WEBP_BASE}/libwebp-1.6.0-linux-aarch64.tar.gz;69f5eebe203e0f3942fe37986209a1725741be19c152950a4283b376c95ec798;libwebp-1.6.0-linux-aarch64")
set(_STURDY_PREBUILT_webp_macos-arm64
    "${_STURDY_WEBP_BASE}/libwebp-1.6.0-mac-arm64.tar.gz;bc6bf84cc70f3f8574fba797d1e4a7dea4feebe9fa4be919f202413ea2b3b8f2;libwebp-1.6.0-mac-arm64")
set(_STURDY_PREBUILT_webp_macos-x86_64
    "${_STURDY_WEBP_BASE}/libwebp-1.6.0-mac-x86-64.tar.gz;f112dd83b420ab2a4b27d46610d9827ddf4200216023281de378647ecca31c2a;libwebp-1.6.0-mac-x86-64")

# Sets `out_used` to TRUE when libwebp's imported targets (WebP::webp, WebP::webpdemux, WebP::sharpyuv) now exist; FALSE means "build from source".
function(sturdy_prebuilt_webp out_used)
    set(${out_used} FALSE PARENT_SCOPE)
    sturdy_prebuilt_select(webp STURDY_PREBUILT_WEBP "v1.6.0" "${STURDY_WEBP_TAG}" _entry)
    if(NOT _entry)
        return()
    endif()
    list(GET _entry 0 _url)
    list(GET _entry 1 _sha)
    list(GET _entry 2 _subdir)
    sturdy_prebuilt_acquire(webp "${_url}" "${_sha}" _root)
    if(NOT _root)
        return()
    endif()
    set(_prefix "${_root}/${_subdir}")
    foreach(_required lib/libwebp.a lib/libwebpdemux.a lib/libsharpyuv.a include/webp/decode.h)
        if(NOT EXISTS "${_prefix}/${_required}")
            message(WARNING "Prebuilt webp: ${_required} is missing from the archive; building from source.")
            return()
        endif()
    endforeach()

    find_package(Threads REQUIRED)
    if(NOT TARGET WebP::sharpyuv)
        add_library(WebP::sharpyuv STATIC IMPORTED GLOBAL)
        set_target_properties(WebP::sharpyuv PROPERTIES
            IMPORTED_LOCATION "${_prefix}/lib/libsharpyuv.a"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_prefix}/include"
            INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include")
        add_library(WebP::webp STATIC IMPORTED GLOBAL)
        set_target_properties(WebP::webp PROPERTIES
            IMPORTED_LOCATION "${_prefix}/lib/libwebp.a"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_prefix}/include"
            INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include"
            INTERFACE_LINK_LIBRARIES "WebP::sharpyuv;Threads::Threads")
        add_library(WebP::webpdemux STATIC IMPORTED GLOBAL)
        set_target_properties(WebP::webpdemux PROPERTIES
            IMPORTED_LOCATION "${_prefix}/lib/libwebpdemux.a"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_prefix}/include"
            INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include"
            INTERFACE_LINK_LIBRARIES "WebP::webp")
    endif()

    sturdy_prebuilt_license(webp "${_root}" "https://raw.githubusercontent.com/webmproject/libwebp/v1.6.0/COPYING"
        5aec868f669e384a22372a4e8a1a6cd7d44c64cd451f960ca69cc170d1e13acf)
    sturdy_register_license(webp "${_root}/licenses/webp")
    message(STATUS "libwebp: using the official prebuilt static libraries (${_subdir})")
    set(${out_used} TRUE PARENT_SCOPE)
endfunction()

# ---- OpenJPEG ------------------------------------------------------------------------------------------------------------------------
# Official release assets from the OpenJPEG project's GitHub releases (hashes equal the digests GitHub publishes for the assets). Static
# libopenjp2.a, position-independent. No Windows entry: the official Windows archive ships only a DLL and its import library. No Linux aarch64
# entry: upstream publishes none.
set(_STURDY_OPJ_BASE "https://github.com/uclouvain/openjpeg/releases/download/v2.5.4")
set(_STURDY_PREBUILT_openjpeg_linux-x86_64
    "${_STURDY_OPJ_BASE}/openjpeg-v2.5.4-linux-x86_64.tar.gz;77915284c4823bbb5f75053d2b6ec8af11378c9fc5f3d1742a17e1bed984277d;openjpeg-v2.5.4-linux-x86_64")
set(_STURDY_PREBUILT_openjpeg_macos-arm64
    "${_STURDY_OPJ_BASE}/openjpeg-v2.5.4-osx-arm64.zip;214b8b32cf42d32d79698ad766eea75366d1fb214a7ffe5ae3c563a7e7418981;openjpeg-v2.5.4-osx-arm64")
set(_STURDY_PREBUILT_openjpeg_macos-x86_64
    "${_STURDY_OPJ_BASE}/openjpeg-v2.5.4-osx-x86_64.zip;4bf5e64efb682293898ef5e3adcaf9dc0e6769212b9a65fc8191e98926ede595;openjpeg-v2.5.4-osx-x86_64")

# Sets `out_used` to TRUE when the `openjp2` target now exists; FALSE means "build from source". The archive's own CMake package is not used: it
# declares a shared `openjp2` and a separate `openjp2_static`, whereas the engine links one static target by the name the source build gives it.
function(sturdy_prebuilt_openjpeg out_used)
    set(${out_used} FALSE PARENT_SCOPE)
    sturdy_prebuilt_select(openjpeg STURDY_PREBUILT_OPENJPEG "v2.5.4" "${STURDY_OPENJPEG_TAG}" _entry)
    if(NOT _entry)
        return()
    endif()
    list(GET _entry 0 _url)
    list(GET _entry 1 _sha)
    list(GET _entry 2 _subdir)
    sturdy_prebuilt_acquire(openjpeg "${_url}" "${_sha}" _root)
    if(NOT _root)
        return()
    endif()
    set(_prefix "${_root}/${_subdir}")
    foreach(_required lib/libopenjp2.a include/openjpeg-2.5/openjpeg.h include/openjpeg-2.5/opj_config.h)
        if(NOT EXISTS "${_prefix}/${_required}")
            message(WARNING "Prebuilt openjpeg: ${_required} is missing from the archive; building from source.")
            return()
        endif()
    endforeach()

    if(NOT TARGET openjp2)
        add_library(openjp2 STATIC IMPORTED GLOBAL)
        set_target_properties(openjp2 PROPERTIES
            IMPORTED_LOCATION "${_prefix}/lib/libopenjp2.a"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_prefix}/include/openjpeg-2.5"
            INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include/openjpeg-2.5"
            INTERFACE_LINK_LIBRARIES "m")
    endif()

    sturdy_prebuilt_license(openjpeg "${_root}" "https://raw.githubusercontent.com/uclouvain/openjpeg/v2.5.4/LICENSE"
        a6af136f3e15038a666b61f376612a07d9a4e48cb7c01adbf3e33b3f14ab49b6)
    sturdy_register_license(openjpeg "${_root}/licenses/openjpeg")
    message(STATUS "OpenJPEG: using the official prebuilt static library (${_subdir})")
    set(${out_used} TRUE PARENT_SCOPE)
endfunction()

# ---- GLFW ----------------------------------------------------------------------------------------------------------------------------
# Official release archives from the GLFW project (github.com/glfw/glfw releases; GitHub publishes no digest for these assets, so the hashes are
# of the files at pin time). Static libraries: Windows `lib-vc2022/glfw3.lib` (dynamic C runtime; the `_mt` variant next to it is the static-CRT
# one and is not used) and macOS `lib-<arch>/libglfw3.a`. No Linux entry: GLFW publishes no Linux binaries. Windows applies to non-Debug
# builds only (see sturdy_prebuilt_crt_compatible).
set(_STURDY_GLFW_BASE "https://github.com/glfw/glfw/releases/download/3.4")
set(_STURDY_PREBUILT_glfw_windows-x64
    "${_STURDY_GLFW_BASE}/glfw-3.4.bin.WIN64.zip;54efa829400f2a0537f742b2b3bdd74e437bb4f2f048e4b7d3c5557d11a611e6;glfw-3.4.bin.WIN64")
set(_STURDY_PREBUILT_glfw_macos-arm64
    "${_STURDY_GLFW_BASE}/glfw-3.4.bin.MACOS.zip;6775085bdae60312a3002bff2e39779a83bc72a7e1c810bd806fddb00cb35fd0;glfw-3.4.bin.MACOS")
set(_STURDY_PREBUILT_glfw_macos-x86_64
    "${_STURDY_GLFW_BASE}/glfw-3.4.bin.MACOS.zip;6775085bdae60312a3002bff2e39779a83bc72a7e1c810bd806fddb00cb35fd0;glfw-3.4.bin.MACOS")

# Sets `out_used` to TRUE when the `glfw` target now exists; FALSE means "build from source".
function(sturdy_prebuilt_glfw out_used)
    set(${out_used} FALSE PARENT_SCOPE)
    sturdy_prebuilt_crt_compatible(_crt_ok)
    if(NOT _crt_ok)
        return()
    endif()
    sturdy_prebuilt_select(glfw STURDY_PREBUILT_GLFW "3.4" "${STURDY_GLFW_TAG}" _entry)
    if(NOT _entry)
        return()
    endif()
    list(GET _entry 0 _url)
    list(GET _entry 1 _sha)
    list(GET _entry 2 _subdir)
    sturdy_prebuilt_acquire(glfw "${_url}" "${_sha}" _root)
    if(NOT _root)
        return()
    endif()
    set(_prefix "${_root}/${_subdir}")
    if(STURDY_OS STREQUAL "Windows")
        set(_library "${_prefix}/lib-vc2022/glfw3.lib")
        set(_system_libs gdi32 user32 shell32)
    else()
        if(STURDY_ARCH STREQUAL "Arm64")
            set(_library "${_prefix}/lib-arm64/libglfw3.a")
        else()
            set(_library "${_prefix}/lib-x86_64/libglfw3.a")
        endif()
        set(_system_libs "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation")
    endif()
    foreach(_required "${_library}" "${_prefix}/include/GLFW/glfw3.h" "${_prefix}/include/GLFW/glfw3native.h")
        if(NOT EXISTS "${_required}")
            message(WARNING "Prebuilt glfw: ${_required} is missing from the archive; building from source.")
            return()
        endif()
    endforeach()

    if(NOT TARGET glfw)
        add_library(glfw STATIC IMPORTED GLOBAL)
        set_target_properties(glfw PROPERTIES
            IMPORTED_LOCATION "${_library}"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_prefix}/include"
            INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include"
            INTERFACE_LINK_LIBRARIES "${_system_libs}")
    endif()
    sturdy_register_license(glfw "${_prefix}")
    message(STATUS "GLFW: using the official prebuilt static library (${_subdir})")
    set(${out_used} TRUE PARENT_SCOPE)
endfunction()

# ---- Dawn ----------------------------------------------------------------------------------------------------------------------------
# Official Release archives from google/dawn's GitHub releases (one per working day, cut from main; the hashes equal the digests GitHub publishes).
# Each is one monolithic static libwebgpu_dawn plus Dawn's own exported CMake package, which is used as is (it carries the platform system
# libraries and frameworks). Pinned to a release whose Vulkan swapchain already honours the surface's alpha mode natively, so the engine's
# STURDY_PATCH_COMPOSITE_ALPHA source patch is not needed (it applies to the source build only).
#
# Differences from the source build the engine otherwise gets, all handled or checked:
#   * The archive also contains Dawn's OpenGL and Null backends, which the engine's source build leaves out on purpose. The engine now names its
#     native backend when it requests an adapter, so it can never land on one of them (Core/WebGPU/RHI/WebGpuAdapter.cpp).
#   * The archive omits dawn/platform/DawnPlatform.h and dawn_platform_export.h, which Core needs; the two files are fetched, hash-pinned, from
#     the same commit.
#   * Release builds only (the Debug archives are 250-840 MB). A Debug engine build can link a release Dawn on Linux and macOS; on Windows it
#     cannot (debug C runtime), so Windows Debug stays on source. Windows x64 only; Linux x86_64 only (no aarch64 build exists).
set(_STURDY_DAWN_COMMIT "dc5320f3ebebe9d42023c24d0ee76828412b66b4")
set(_STURDY_DAWN_BASE "https://github.com/google/dawn/releases/download/v20261103.174204")
set(_STURDY_PREBUILT_dawn_linux-x86_64
    "${_STURDY_DAWN_BASE}/Dawn-${_STURDY_DAWN_COMMIT}-ubuntu-latest-Release.tar.gz;02cacf29a28cdf450901142edcda7fe874d739103094ffb37034ed8e153945a0;Dawn-${_STURDY_DAWN_COMMIT}-ubuntu-latest-Release")
set(_STURDY_PREBUILT_dawn_macos-arm64
    "${_STURDY_DAWN_BASE}/Dawn-${_STURDY_DAWN_COMMIT}-macos-latest-Release.tar.gz;3c89a2937cf9049afb701a3ff8413572fd3ae15a9eb5bc6af453af7af8b8d9b4;Dawn-${_STURDY_DAWN_COMMIT}-macos-latest-Release")
set(_STURDY_PREBUILT_dawn_macos-x86_64
    "${_STURDY_DAWN_BASE}/Dawn-${_STURDY_DAWN_COMMIT}-macos-15-intel-Release.tar.gz;0ea943692ef4f1ac425cbf160d6cefdca63b4764b439963fbfa1fd82a740c1ea;Dawn-${_STURDY_DAWN_COMMIT}-macos-15-intel-Release")
set(_STURDY_PREBUILT_dawn_windows-x64
    "${_STURDY_DAWN_BASE}/Dawn-${_STURDY_DAWN_COMMIT}-windows-latest-Release.tar.gz;37942a2c5ff7289ae31506fff9063b96aeb4cfbecb4f881491f85764dc54817d;Dawn-${_STURDY_DAWN_COMMIT}-windows-latest-Release")

# Sets `out_used` to TRUE when Sturdy::WebGPU now exists; FALSE means "build from source".
function(sturdy_prebuilt_dawn out_used)
    set(${out_used} FALSE PARENT_SCOPE)
    sturdy_prebuilt_crt_compatible(_crt_ok)
    if(NOT _crt_ok)
        return()
    endif()
    sturdy_prebuilt_select(dawn STURDY_PREBUILT_DAWN "v20261103.174204" "${STURDY_DAWN_PREBUILT_RELEASE}" _entry)
    if(NOT _entry)
        return()
    endif()
    list(GET _entry 0 _url)
    list(GET _entry 1 _sha)
    list(GET _entry 2 _subdir)
    sturdy_prebuilt_acquire(dawn "${_url}" "${_sha}" _root)
    if(NOT _root)
        return()
    endif()
    set(_prefix "${_root}/${_subdir}")

    # The two headers Core derives from; the archive's include/ lacks them.
    set(_raw "https://raw.githubusercontent.com/google/dawn/${_STURDY_DAWN_COMMIT}")
    set(_extra "${_root}/extra-include")
    sturdy_prebuilt_verified_file("${_extra}/dawn/platform/DawnPlatform.h" "${_raw}/include/dawn/platform/DawnPlatform.h"
        e40a208fde1c27520d15515655ad48a0fc7f84e6cd344644b9debf0cb32277e7 _platform_header)
    sturdy_prebuilt_verified_file("${_extra}/dawn/platform/dawn_platform_export.h" "${_raw}/include/dawn/platform/dawn_platform_export.h"
        8a4572e603a646777c230d25ba4141bc2112ed80ab37cf5f00c5b48b370a8f53 _export_header)
    if(NOT _platform_header OR NOT _export_header)
        message(WARNING "Prebuilt dawn: could not fetch the verified dawn/platform headers; building Dawn from source.")
        return()
    endif()

    file(GLOB_RECURSE _config "${_prefix}/*/cmake/Dawn/DawnConfig.cmake")
    list(LENGTH _config _config_count)
    if(NOT _config_count EQUAL 1 OR NOT EXISTS "${_prefix}/include/webgpu/webgpu.h")
        message(WARNING "Prebuilt dawn: the archive does not have the expected layout; building from source.")
        return()
    endif()
    get_filename_component(_config_dir "${_config}" DIRECTORY)
    find_package(Threads REQUIRED)
    find_package(Dawn CONFIG QUIET NO_DEFAULT_PATH PATHS "${_config_dir}")
    if(NOT TARGET dawn::webgpu_dawn)
        message(WARNING "Prebuilt dawn: its CMake package did not define dawn::webgpu_dawn; building from source.")
        return()
    endif()

    if(NOT TARGET Sturdy::WebGPU)
        add_library(Sturdy::WebGPU INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sturdy::WebGPU INTERFACE dawn::webgpu_dawn)
        target_include_directories(Sturdy::WebGPU SYSTEM INTERFACE "${_extra}")
    endif()

    sturdy_prebuilt_license(dawn "${_root}" "${_raw}/LICENSE" 1c43dfd13b306e5bac47839ea90cd1882249a75c92963ade400d4c1165e42228)
    sturdy_register_license(dawn "${_root}/licenses/dawn")
    message(STATUS "Dawn: using the official prebuilt static library (${_subdir})")
    set(${out_used} TRUE PARENT_SCOPE)
endfunction()
