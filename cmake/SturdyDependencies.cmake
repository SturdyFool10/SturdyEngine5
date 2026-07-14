include_guard(GLOBAL)

include(FetchContent)

set(STURDY_DEPS_FOLDER "ThirdParty")
set(STURDY_GLM_TAG "1.0.3" CACHE STRING "glm git tag to fetch.")
set(STURDY_VMA_TAG "v3.4.0" CACHE STRING "Vulkan Memory Allocator git tag to fetch.")
set(STURDY_VOLK_TAG "1.4.350" CACHE STRING "volk git tag to fetch.")
set(STURDY_SDL3_TAG "release-3.4.10" CACHE STRING "SDL3 git tag to fetch.")
set(STURDY_GLFW_TAG "3.4" CACHE STRING "GLFW git tag to fetch.")
# {fmt} is fetched as a standalone library and spdlog is pointed at it via SPDLOG_FMT_EXTERNAL
# (see sturdy_fetch_spdlog), so the engine's direct <fmt/...> includes and spdlog share one fmt
# instead of spdlog's private bundled copy (two copies would collide in the fmt:: namespace).
# Keep this tag in sync with the fmt version spdlog bundles at STURDY_SPDLOG_TAG (v1.17.0 -> 12.1.0).
set(STURDY_FMT_TAG "12.1.0" CACHE STRING "{fmt} git tag to fetch; shared with spdlog via SPDLOG_FMT_EXTERNAL.")
set(STURDY_SPDLOG_TAG "v1.17.0" CACHE STRING "spdlog git tag to fetch.")
set(STURDY_MIMALLOC_TAG "v2.1.7" CACHE STRING "mimalloc git tag to fetch.")
set(STURDY_HARFBUZZ_TAG "14.2.1" CACHE STRING "HarfBuzz git tag to fetch.")
set(STURDY_MINIAUDIO_TAG "0.11.25" CACHE STRING "miniaudio git tag to fetch.")
# Slang is built from source so we get a static library with SPIRV-Tools baked in.
# The first configure is slow because Slang's CMake fetches and builds spirv-tools.
# Python3 must be available on the build machine for that step.
#
# KNOWN GAP (Web/wasm32): Slang's build compiles host-side code generators (slang-generate,
# slang-embed, ...) that must run natively mid-build to produce C++ sources, then links the
# result into the target library. Its CMake has no host/target split for cross-compiling those
# generators separately from the target library, so under the Emscripten toolchain they get
# built as .js/.wasm too and CMake tries to execute them as native tools, which fails. Core (which
# links Sturdy::Slang, and whose Renderer.cppm bakes Slang::UnCompiledShader into the
# backend-agnostic RendererCreateInfo) is therefore not buildable for STURDY_OS=Web yet — fixing
# this needs either a proper Slang host/target build split or decoupling shader compilation from
# Core's interface for Web (precompiled/offline shaders instead of in-engine compilation).
# Foundation and Platform are unaffected and build/link cleanly for Web today.
set(STURDY_SLANG_TAG "v2026.11" CACHE STRING "Slang git tag to fetch and build from source.")
set(STURDY_BOX3D_TAG "v0.1.0" CACHE STRING "Box3D git tag to fetch.")
set(STURDY_CLAY_TAG "v0.14" CACHE STRING "Clay git tag to fetch.")
# msdfgen-core has no dependency on FreeType/PNG/Skia — those only gate msdfgen-ext (font loading)
# and the msdfgen-standalone CLI, neither of which this engine uses: HarfBuzz's hb-draw already
# supplies glyph outlines directly (see Text/Outline.cppm), so msdfgen only ever consumes shapes
# built from those, never loads a font itself.
set(STURDY_MSDFGEN_TAG "v1.13" CACHE STRING "msdfgen git tag to fetch.")
# stb (github.com/nothings/stb) ships no releases/tags — pinning a known-good commit is the
# standard way to consume it. Only stb_image.h is used (decoding the PNG blobs HarfBuzz's
# hb-ot-color API returns for bitmap-format color emoji glyphs — CBDT/sbix).
set(STURDY_STB_TAG "31c1ad37456438565541f4919958214b6e762fb4" CACHE STRING "stb git commit to fetch.")
# Microsoft's official, MIT-licensed native D3D12/DXGI headers (no NuGet/.NET tooling involved —
# plain C/C++ headers only). Only fetched when STURDY_OS is Windows.
set(STURDY_DIRECTX_HEADERS_TAG "v1.721.2" CACHE STRING "DirectX-Headers git tag to fetch.")
# Apple's official, Apache-2.0-licensed C++ bindings for Metal (headers only; Apple ships no
# prebuilt binary, this just wraps the Objective-C API). Tags track macOS/iOS SDK releases. Only
# fetched when STURDY_OS is MacOS.
set(STURDY_METALCPP_TAG "release/metal-cpp_macOS27_iOS27" CACHE STRING "apple/metal-cpp git tag to fetch.")

set(STURDY_VULKAN_LIBRARY "" CACHE FILEPATH "Optional explicit Vulkan loader library. Set this to a static loader library when available.")
set(STURDY_SLANG_ROOT "" CACHE PATH "Root of a Slang SDK/install containing include/ and lib/ or a SlangConfig.cmake package.")
set(STURDY_SLANG_LIBRARY "" CACHE FILEPATH "Optional explicit Slang library. Overrides automatic fetch when set together with STURDY_SLANG_INCLUDE_DIR.")
set(STURDY_SLANG_INCLUDE_DIR "" CACHE PATH "Optional explicit Slang include directory containing slang.h. Overrides automatic fetch when set together with STURDY_SLANG_LIBRARY.")
option(STURDY_PREFER_SYSTEM_DEPENDENCIES "Try find_package before downloading FetchContent dependencies." ON)

# Every build/<arch>/<os>/<profile> tree normally gets its own _deps, so each configuration
# re-clones every third-party source. With this on, the git checkouts (and their download
# subbuilds) live in one shared cache instead, so a source is fetched once and reused by all
# trees. Per-configuration build artifacts stay isolated (each tree still builds the deps into
# its own _deps/<name>-build), so Debug/Release/arch outputs never collide. Turn OFF for fully
# independent trees — e.g. CI matrix jobs that configure the same source concurrently and could
# race on the shared checkout.
option(STURDY_SHARED_DEPS_CACHE "Share downloaded third-party sources across all build trees so each configuration does not re-download them." ON)
set(STURDY_DEPS_CACHE_DIR "${CMAKE_SOURCE_DIR}/.cache/deps" CACHE PATH "Directory holding the shared FetchContent source cache when STURDY_SHARED_DEPS_CACHE is ON.")

function(sturdy_fetchcontent_declare name)
    set(options)
    set(one_value_args GIT_REPOSITORY GIT_TAG)
    set(multi_value_args FIND_PACKAGE_ARGS)
    cmake_parse_arguments(STURDY_FETCH "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(_find_package_args)
    if(STURDY_PREFER_SYSTEM_DEPENDENCIES AND STURDY_FETCH_FIND_PACKAGE_ARGS)
        set(_find_package_args FIND_PACKAGE_ARGS ${STURDY_FETCH_FIND_PACKAGE_ARGS})
    endif()

    # Stashed so sturdy_register_license() can still fetch just this dependency's license later
    # even when find_package() satisfies it and FetchContent never populates its source — it
    # can't reuse this Declare to do that (FetchContent refuses to re-populate a name already
    # resolved via find_package()), so it needs the repo/tag to do its own independent fetch.
    # Keyed by lowercased name to match FetchContent's own <lowercaseName>_SOURCE_DIR convention
    # (and thus the name sturdy_register_license() is actually called with, e.g. "sdl3" for a
    # Declare of "SDL3").
    string(TOLOWER "${name}" _sturdy_dep_key)
    set_property(GLOBAL PROPERTY STURDY_DEP_GIT_REPOSITORY_${_sturdy_dep_key} "${STURDY_FETCH_GIT_REPOSITORY}")
    set_property(GLOBAL PROPERTY STURDY_DEP_GIT_TAG_${_sturdy_dep_key} "${STURDY_FETCH_GIT_TAG}")

    # Redirect only the source checkout and its download subbuild into the shared cache; the
    # build dir is left to FetchContent's default (this tree's _deps/<name>-build), keeping
    # per-configuration artifacts isolated.
    set(_cache_dirs)
    if(STURDY_SHARED_DEPS_CACHE)
        set(_cache_dirs
            SOURCE_DIR "${STURDY_DEPS_CACHE_DIR}/${name}-src"
            SUBBUILD_DIR "${STURDY_DEPS_CACHE_DIR}/${name}-subbuild"
        )
    endif()

    FetchContent_Declare(${name}
        GIT_REPOSITORY ${STURDY_FETCH_GIT_REPOSITORY}
        GIT_TAG ${STURDY_FETCH_GIT_TAG}
        GIT_SHALLOW TRUE
        GIT_PROGRESS FALSE
        EXCLUDE_FROM_ALL
        SYSTEM
        ${_cache_dirs}
        ${_find_package_args}
    )
endfunction()

function(sturdy_mark_dependency_targets_exclude_from_all)
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET "${_target}")
            continue()
        endif()

        get_target_property(_aliased_target "${_target}" ALIASED_TARGET)
        if(_aliased_target)
            set(_real_target "${_aliased_target}")
        else()
            set(_real_target "${_target}")
        endif()

        get_target_property(_imported "${_real_target}" IMPORTED)
        if(NOT _imported)
            set_target_properties("${_real_target}" PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()
endfunction()

function(sturdy_configure_dependencies)
    # Vulkan is the desktop (Windows/MacOS/Linux/FreeBSD) graphics API. Web gets WebGPU only —
    # see sturdy_configure_webgpu() below — so it never finds/links Vulkan at all.
    if(NOT STURDY_OS STREQUAL "Web")
        sturdy_find_vulkan()
    endif()

    if(STURDY_FETCH_DEPENDENCIES)
        sturdy_fetch_glm()
        sturdy_fetch_vma()
        sturdy_fetch_volk()
        sturdy_fetch_sdl3()
        # GLFW has no official Emscripten/wasm support (unlike SDL3, which is actively maintained
        # there), so it is not part of the Web build at all — see Platform/CMakeLists.txt, which
        # only links Sturdy::SDL3 for STURDY_OS=Web.
        if(NOT STURDY_OS STREQUAL "Web")
            sturdy_fetch_glfw()
        endif()
        sturdy_fetch_fmt()
        sturdy_fetch_spdlog()
        sturdy_fetch_mimalloc()
        sturdy_fetch_harfbuzz()
        sturdy_fetch_miniaudio()
        sturdy_fetch_slang()
        sturdy_fetch_box3d()
        sturdy_fetch_clay()
        sturdy_fetch_msdfgen()
        sturdy_fetch_stb_image()

        if(STURDY_OS STREQUAL "Windows")
            sturdy_fetch_directx_headers()
        endif()
        if(STURDY_OS STREQUAL "MacOS")
            sturdy_fetch_metalcpp()
        endif()
    else()
        find_package(glm CONFIG REQUIRED)
        find_package(VulkanMemoryAllocator CONFIG REQUIRED)
        find_package(volk CONFIG REQUIRED)
        find_package(SDL3 CONFIG REQUIRED)
        if(NOT STURDY_OS STREQUAL "Web")
            find_package(glfw3 CONFIG REQUIRED)
        endif()
        find_package(fmt CONFIG REQUIRED)
        find_package(spdlog CONFIG REQUIRED)
        find_package(mimalloc CONFIG REQUIRED)
        find_package(harfbuzz CONFIG REQUIRED)
        find_package(miniaudio CONFIG REQUIRED)
        sturdy_find_slang()
        find_package(box3d CONFIG REQUIRED)
        sturdy_find_clay()
        find_package(msdfgen CONFIG REQUIRED)
        sturdy_find_stb_image()

        if(STURDY_OS STREQUAL "Windows")
            sturdy_find_directx()
        endif()
        if(STURDY_OS STREQUAL "MacOS")
            sturdy_find_metalcpp()
        endif()
    endif()

    if(STURDY_OS STREQUAL "Web")
        sturdy_configure_webgpu()
    endif()

    sturdy_normalize_dependency_targets()
    sturdy_finalize_licenses()
endfunction()

function(sturdy_find_vulkan)
    find_package(Vulkan 1.4 REQUIRED)

    if(NOT TARGET Sturdy::Vulkan)
        add_library(Sturdy::Vulkan INTERFACE IMPORTED GLOBAL)

        if(STURDY_VULKAN_LIBRARY)
            add_library(SturdyVulkanImported UNKNOWN IMPORTED GLOBAL)
            set_target_properties(SturdyVulkanImported PROPERTIES
                IMPORTED_LOCATION "${STURDY_VULKAN_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIRS}"
            )
            target_link_libraries(Sturdy::Vulkan INTERFACE SturdyVulkanImported)
        else()
            target_link_libraries(Sturdy::Vulkan INTERFACE Vulkan::Vulkan)
        endif()
    endif()
endfunction()

# Fetches Microsoft's official, MIT-licensed DirectX-Headers (native C/C++ headers + import-lib
# GUIDs; no NuGet/.NET tooling involved) and links the Windows SDK's own d3d12/dxgi/dxguid import
# libraries against them, so Sturdy::DirectX gets the latest D3D12 declarations rather than
# whatever ships in the installed Windows SDK.
function(sturdy_fetch_directx_headers)
    sturdy_fetchcontent_declare(directx_headers
        GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
        GIT_TAG ${STURDY_DIRECTX_HEADERS_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET NAMES DirectX-Headers directx-headers
    )
    FetchContent_MakeAvailable(directx_headers)
    sturdy_mark_dependency_targets_exclude_from_all(DirectX-Headers DirectX-Guids Microsoft::DirectX-Headers Microsoft::DirectX-Guids)
    sturdy_register_license(directx_headers "${directx_headers_SOURCE_DIR}")

    if(NOT TARGET Sturdy::DirectX)
        add_library(Sturdy::DirectX INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sturdy::DirectX INTERFACE
            Microsoft::DirectX-Headers
            Microsoft::DirectX-Guids
            d3d12
            dxgi
            dxguid
        )
    endif()
endfunction()

# STURDY_FETCH_DEPENDENCIES=OFF fallback: the Windows SDK already ships d3d12.h/dxgi1_x.h on the
# default include path, just older than Microsoft's open-source DirectX-Headers releases.
function(sturdy_find_directx)
    if(NOT TARGET Sturdy::DirectX)
        add_library(Sturdy::DirectX INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sturdy::DirectX INTERFACE d3d12 dxgi dxguid)
    endif()
endfunction()

# Apple's official, Apache-2.0-licensed C++ bindings for Metal (apple/metal-cpp). It ships no
# CMakeLists.txt (headers only, same shape as Clay below), so this populates the source directly
# (the non-declarative FetchContent_Populate() form — FetchContent_MakeAvailable() would try to
# add_subdirectory() a tree with no build system) and wires up an include path + the actual
# system frameworks by hand.
function(sturdy_fetch_metalcpp)
    set(_cache_dirs)
    if(STURDY_SHARED_DEPS_CACHE)
        set(_cache_dirs
            SOURCE_DIR "${STURDY_DEPS_CACHE_DIR}/metalcpp-src"
            SUBBUILD_DIR "${STURDY_DEPS_CACHE_DIR}/metalcpp-subbuild"
        )
    endif()
    FetchContent_Populate(metalcpp
        QUIET
        GIT_REPOSITORY https://github.com/apple/metal-cpp.git
        GIT_TAG ${STURDY_METALCPP_TAG}
        GIT_SHALLOW TRUE
        GIT_PROGRESS FALSE
        ${_cache_dirs}
    )
    sturdy_register_license(metalcpp "${metalcpp_SOURCE_DIR}")

    if(NOT TARGET Sturdy::Metal)
        add_library(SturdyMetalCpp INTERFACE)
        target_include_directories(SturdyMetalCpp SYSTEM INTERFACE "${metalcpp_SOURCE_DIR}")
        sturdy_mark_dependency_targets_exclude_from_all(SturdyMetalCpp)

        add_library(Sturdy::Metal INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sturdy::Metal INTERFACE SturdyMetalCpp
            "-framework Metal"
            "-framework MetalKit"
            "-framework QuartzCore"
            "-framework Foundation"
        )
    endif()
endfunction()

function(sturdy_find_metalcpp)
    find_path(STURDY_METALCPP_INCLUDE_DIR NAMES Metal/Metal.hpp)
    if(NOT STURDY_METALCPP_INCLUDE_DIR)
        message(FATAL_ERROR "Could not find metal-cpp (Metal/Metal.hpp). Install apple/metal-cpp or enable STURDY_FETCH_DEPENDENCIES.")
    endif()

    if(NOT TARGET Sturdy::Metal)
        add_library(SturdyMetalCpp INTERFACE)
        target_include_directories(SturdyMetalCpp SYSTEM INTERFACE "${STURDY_METALCPP_INCLUDE_DIR}")

        add_library(Sturdy::Metal INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sturdy::Metal INTERFACE SturdyMetalCpp
            "-framework Metal"
            "-framework MetalKit"
            "-framework QuartzCore"
            "-framework Foundation"
        )
    endif()
endfunction()

# Web's only graphics API. Unlike every other dependency in this file, WebGPU is not a FetchContent
# checkout we control: `--use-port=emdawnwebgpu` tells em++ itself to download and build Dawn's
# pinned webgpu.h/webgpu_cpp.h implementation (see
# https://dawn.googlesource.com/dawn/+/HEAD/src/emdawnwebgpu/pkg/README.md), cached inside the
# Emscripten toolchain's own ports cache. That means there is no source directory here for
# sturdy_register_license() to point at — collecting Dawn's license text is left as a follow-up
# once a Web/WebGPU renderer backend actually exists to consume it.
function(sturdy_configure_webgpu)
    if(NOT TARGET Sturdy::WebGPU)
        add_library(Sturdy::WebGPU INTERFACE IMPORTED GLOBAL)
        target_compile_options(Sturdy::WebGPU INTERFACE "--use-port=emdawnwebgpu")
        target_link_options(Sturdy::WebGPU INTERFACE "--use-port=emdawnwebgpu")
    endif()
endfunction()

function(sturdy_find_slang)
    if(STURDY_SLANG_LIBRARY AND STURDY_SLANG_INCLUDE_DIR)
        add_library(SturdySlangImported UNKNOWN IMPORTED GLOBAL)
        set_target_properties(SturdySlangImported PROPERTIES
            IMPORTED_LOCATION "${STURDY_SLANG_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${STURDY_SLANG_INCLUDE_DIR}"
        )
        set(_sturdy_slang_target SturdySlangImported)
    else()
        find_package(Slang CONFIG QUIET
            HINTS
                "${STURDY_SLANG_ROOT}"
                "$ENV{SLANG_SDK}"
                "$ENV{SLANG_ROOT}"
        )

        if(TARGET Slang::slang)
            set(_sturdy_slang_target Slang::slang)
        elseif(TARGET slang::slang)
            set(_sturdy_slang_target slang::slang)
        elseif(TARGET slang)
            set(_sturdy_slang_target slang)
        else()
            find_path(STURDY_SLANG_INCLUDE_DIR
                NAMES slang.h
                HINTS
                    "${STURDY_SLANG_ROOT}"
                    "$ENV{SLANG_SDK}"
                    "$ENV{SLANG_ROOT}"
                PATH_SUFFIXES include
            )

            find_library(STURDY_SLANG_LIBRARY
                NAMES slang slang-compiler
                HINTS
                    "${STURDY_SLANG_ROOT}"
                    "$ENV{SLANG_SDK}"
                    "$ENV{SLANG_ROOT}"
                PATH_SUFFIXES lib lib64 bin
            )

            if(NOT STURDY_SLANG_INCLUDE_DIR OR NOT STURDY_SLANG_LIBRARY)
                message(FATAL_ERROR
                    "Could not find the Slang SDK. Install Slang or set STURDY_SLANG_ROOT, "
                    "SLANG_SDK, SLANG_ROOT, STURDY_SLANG_LIBRARY, or STURDY_SLANG_INCLUDE_DIR."
                )
            endif()

            add_library(SturdySlangImported UNKNOWN IMPORTED GLOBAL)
            set_target_properties(SturdySlangImported PROPERTIES
                IMPORTED_LOCATION "${STURDY_SLANG_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${STURDY_SLANG_INCLUDE_DIR}"
            )
            set(_sturdy_slang_target SturdySlangImported)
        endif()
    endif()

    if(NOT TARGET Sturdy::Slang)
        add_library(Sturdy::Slang INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sturdy::Slang INTERFACE "${_sturdy_slang_target}")
    endif()
endfunction()

# Clay ships no installed CMake package (its own CMakeLists.txt only builds examples), so unlike
# the other STURDY_PREFER_SYSTEM_DEPENDENCIES=OFF paths, this just looks for a system-installed
# clay.h rather than a find_package() config.
function(sturdy_find_clay)
    find_path(STURDY_CLAY_INCLUDE_DIR NAMES clay.h)
    if(NOT STURDY_CLAY_INCLUDE_DIR)
        message(FATAL_ERROR "Could not find clay.h. Install Clay or enable STURDY_FETCH_DEPENDENCIES.")
    endif()

    if(NOT TARGET clay)
        add_library(clay INTERFACE)
        target_include_directories(clay INTERFACE "${STURDY_CLAY_INCLUDE_DIR}")
    endif()
endfunction()

function(sturdy_fetch_slang)
    # Honor manual overrides even in fetch mode.
    if(STURDY_SLANG_LIBRARY AND STURDY_SLANG_INCLUDE_DIR)
        sturdy_find_slang()
        return()
    endif()

    # BUILD_SHARED_LIBS=OFF (forced globally) makes the Slang build produce a static library.
    set(SLANG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_REPLAYER OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGC OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_DXIL OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_GFX OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGD OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SPIRV_TOOLS_MIMALLOC OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    # slang-rhi is Slang's own example/test graphics-abstraction harness (unrelated to this
    # engine's renderer) and its test target does not configure cleanly when cross-compiling to
    # Emscripten, so it stays off everywhere rather than only on Web.
    set(SLANG_ENABLE_SLANG_RHI OFF CACHE BOOL "" FORCE)

    sturdy_fetchcontent_declare(slang
        GIT_REPOSITORY https://github.com/shader-slang/slang.git
        GIT_TAG ${STURDY_SLANG_TAG}
    )
    FetchContent_MakeAvailable(slang)
    sturdy_mark_dependency_targets_exclude_from_all(slang slang::slang Slang::slang slang-compiler)
    sturdy_register_license(slang "${slang_SOURCE_DIR}")

    # v2026.11 bug: slang-parser.cpp uses INT_MIN without including <climits>.
    # Patch the file after download but before the build compiles it.
    set(_slang_parser "${slang_SOURCE_DIR}/source/slang/slang-parser.cpp")
    if(EXISTS "${_slang_parser}")
        file(READ "${_slang_parser}" _slang_parser_content)
        if(NOT _slang_parser_content MATCHES "#include <climits>")
            string(REPLACE "#include <optional>"
                           "#include <optional>\n#include <climits>"
                           _slang_parser_content "${_slang_parser_content}")
            file(WRITE "${_slang_parser}" "${_slang_parser_content}")
        endif()
    endif()

    if(TARGET slang::slang)
        set(_target slang::slang)
    elseif(TARGET Slang::slang)
        set(_target Slang::slang)
    elseif(TARGET slang)
        set(_target slang)
    else()
        message(FATAL_ERROR
            "Slang source build completed but no slang target was exported. "
            "Set STURDY_SLANG_LIBRARY and STURDY_SLANG_INCLUDE_DIR to provide a prebuilt library manually."
        )
    endif()

    if(NOT TARGET Sturdy::Slang)
        add_library(Sturdy::Slang INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sturdy::Slang INTERFACE "${_target}")
    endif()
endfunction()

function(sturdy_fetch_glm)
    set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)
    set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    sturdy_fetchcontent_declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG ${STURDY_GLM_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(glm)
    sturdy_mark_dependency_targets_exclude_from_all(glm glm::glm)
    sturdy_register_license(glm "${glm_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_vma)
    sturdy_fetchcontent_declare(vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG ${STURDY_VMA_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET NAMES VulkanMemoryAllocator
    )
    FetchContent_MakeAvailable(vma)
    sturdy_mark_dependency_targets_exclude_from_all(VulkanMemoryAllocator GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator::VulkanMemoryAllocator)
    sturdy_register_license(vma "${vma_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_volk)
    set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
    set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
    sturdy_fetchcontent_declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG ${STURDY_VOLK_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(volk)
    sturdy_mark_dependency_targets_exclude_from_all(volk volk::volk)
    sturdy_register_license(volk "${volk_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_sdl3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
    set(SDL_PIPEWIRE_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL_DOCS ON CACHE BOOL "" FORCE)
    if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
        set(LIBC_HAS_ITOA "" CACHE INTERNAL "Have symbol itoa" FORCE)
    endif()
    sturdy_preseed_sdl3_linux_checks()

    sturdy_fetchcontent_declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG ${STURDY_SDL3_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(SDL3)
    sturdy_mark_dependency_targets_exclude_from_all(SDL3 SDL3::SDL3 SDL3::SDL3-static SDL3-static)
    sturdy_register_license(sdl3 "${sdl3_SOURCE_DIR}")
endfunction()

function(sturdy_preseed_sdl3_linux_checks)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
        return()
    endif()

    set(_sdl_true_checks
        LIBC_IS_GLIBC
        HAVE_ALLOCA_H
        HAVE_LIBM
        HAVE_FDATASYNC
        HAVE_GETAUXVAL
        HAVE_GETHOSTNAME
        HAVE_GETPAGESIZE
        HAVE_GETRESGID
        HAVE_GETRESUID
        HAVE_GMTIME_R
        HAVE_LOCALTIME_R
        HAVE_MEMFD_CREATE
        HAVE_NANOSLEEP
        HAVE_NL_LANGINFO
        HAVE_POSIX_FALLOCATE
        HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR_NP
        HAVE_PPOLL
        HAVE_SA_SIGACTION
        HAVE_SETJMP
        HAVE_SIGACTION
        HAVE_SIGTIMEDWAIT
        HAVE_ST_MTIM
        HAVE_SYSCONF
        ICONV_IN_LIBC
        LIBC_HAS_ABS
        LIBC_HAS_ACOS
        LIBC_HAS_ACOSF
        LIBC_HAS_ASIN
        LIBC_HAS_ASINF
        LIBC_HAS_ATAN
        LIBC_HAS_ATAN2
        LIBC_HAS_ATAN2F
        LIBC_HAS_ATANF
        LIBC_HAS_ATOF
        LIBC_HAS_ATOI
        LIBC_HAS_BCOPY
        LIBC_HAS_CEIL
        LIBC_HAS_CEILF
        LIBC_HAS_COPYSIGN
        LIBC_HAS_COPYSIGNF
        LIBC_HAS_COS
        LIBC_HAS_COSF
        LIBC_HAS_EXP
        LIBC_HAS_EXPF
        LIBC_HAS_FABS
        LIBC_HAS_FABSF
        LIBC_HAS_FLOAT_H
        LIBC_HAS_FLOOR
        LIBC_HAS_FLOORF
        LIBC_HAS_FMOD
        LIBC_HAS_FMODF
        LIBC_HAS_FOPEN64
        LIBC_HAS_FSEEKO
        LIBC_HAS_FSEEKO64
        LIBC_HAS_GETENV
        LIBC_HAS_ICONV_H
        LIBC_HAS_INDEX
        LIBC_HAS_INTTYPES_H
        LIBC_HAS_ISINF
        LIBC_HAS_ISINFF
        LIBC_HAS_ISNAN
        LIBC_HAS_ISNANF
        LIBC_HAS_LIMITS_H
        LIBC_HAS_LOG
        LIBC_HAS_LOG10
        LIBC_HAS_LOG10F
        LIBC_HAS_LOGF
        LIBC_HAS_LROUND
        LIBC_HAS_LROUNDF
        LIBC_HAS_MALLOC
        LIBC_HAS_MALLOC_H
        LIBC_HAS_MATH_H
        LIBC_HAS_MEMCMP
        LIBC_HAS_MEMCPY
        LIBC_HAS_MEMMOVE
        LIBC_HAS_MEMORY_H
        LIBC_HAS_MEMSET
        LIBC_HAS_MODF
        LIBC_HAS_MODFF
        LIBC_HAS_POW
        LIBC_HAS_POWF
        LIBC_HAS_PUTENV
        LIBC_HAS_RINDEX
        LIBC_HAS_ROUND
        LIBC_HAS_ROUNDF
        LIBC_HAS_SCALBN
        LIBC_HAS_SCALBNF
        LIBC_HAS_SETENV
        LIBC_HAS_SIGNAL_H
        LIBC_HAS_SIN
        LIBC_HAS_SINF
        LIBC_HAS_SQRT
        LIBC_HAS_SQRTF
        LIBC_HAS_SSCANF
        LIBC_HAS_STDARG_H
        LIBC_HAS_STDBOOL_H
        LIBC_HAS_STDDEF_H
        LIBC_HAS_STDINT_H
        LIBC_HAS_STDIO_H
        LIBC_HAS_STDLIB_H
        LIBC_HAS_STRCASESTR
        LIBC_HAS_STRCHR
        LIBC_HAS_STRCMP
        LIBC_HAS_STRINGS_H
        LIBC_HAS_STRING_H
        LIBC_HAS_STRLCAT
        LIBC_HAS_STRLCPY
        LIBC_HAS_STRLEN
        LIBC_HAS_STRNCMP
        LIBC_HAS_STRNLEN
        LIBC_HAS_STRPBRK
        LIBC_HAS_STRRCHR
        LIBC_HAS_STRSTR
        LIBC_HAS_STRTOD
        LIBC_HAS_STRTOK_R
        LIBC_HAS_STRTOL
        LIBC_HAS_STRTOLL
        LIBC_HAS_STRTOUL
        LIBC_HAS_STRTOULL
        LIBC_HAS_SYS_TYPES_H
        LIBC_HAS_TAN
        LIBC_HAS_TANF
        LIBC_HAS_TIME_H
        LIBC_HAS_TRUNC
        LIBC_HAS_TRUNCF
        LIBC_HAS_UNSETENV
        LIBC_HAS_VSNPRINTF
        LIBC_HAS_VSSCANF
        LIBC_HAS_WCHAR_H
        LIBC_HAS_WCSCMP
        LIBC_HAS_WCSDUP
        LIBC_HAS_WCSLCAT
        LIBC_HAS_WCSLCPY
        LIBC_HAS_WCSLEN
        LIBC_HAS_WCSNCMP
        LIBC_HAS_WCSNLEN
        LIBC_HAS_WCSSTR
        LIBC_HAS_WCSTOL
        LIBC_HAS__EXIT
    )

    set(_sdl_false_checks
        HAVE_ELF_AUX_INFO
        HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR
        HAVE_SYSCTLBYNAME
        ICONV_IN_LIBICONV
        LIBC_HAS_ITOA
        LIBC_HAS_SQR
        LIBC_HAS_STRNSTR
        LIBC_HAS__I64TOA
        LIBC_HAS__LTOA
    )

    foreach(_check IN LISTS _sdl_true_checks)
        set(${_check} "1" CACHE INTERNAL "Preseeded SDL3 Linux configure check")
    endforeach()

    foreach(_check IN LISTS _sdl_false_checks)
        set(${_check} "" CACHE INTERNAL "Preseeded SDL3 Linux configure check")
    endforeach()
endfunction()

function(sturdy_fetch_glfw)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    sturdy_fetchcontent_declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG ${STURDY_GLFW_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET NAMES glfw3 glfw
    )
    FetchContent_MakeAvailable(glfw)
    sturdy_mark_dependency_targets_exclude_from_all(glfw glfw3 glfw::glfw glfw3::glfw)
    sturdy_register_license(glfw "${glfw_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_fmt)
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    set(FMT_DOC OFF CACHE BOOL "" FORCE)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    sturdy_fetchcontent_declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG ${STURDY_FMT_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(fmt)
    sturdy_mark_dependency_targets_exclude_from_all(fmt fmt::fmt fmt-header-only fmt::fmt-header-only)
    sturdy_register_license(fmt "${fmt_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_spdlog)
    # Share the standalone {fmt} fetched by sturdy_fetch_fmt (which must run first) instead of
    # spdlog's bundled copy. spdlog reuses an existing fmt::fmt target when one is defined
    # (its CMake only find_package(fmt)s when the target is missing), so this just flips the mode.
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_WARNINGS OFF CACHE BOOL "" FORCE)
    sturdy_fetchcontent_declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG ${STURDY_SPDLOG_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(spdlog)
    sturdy_mark_dependency_targets_exclude_from_all(spdlog spdlog::spdlog spdlog::spdlog_header_only)
    sturdy_register_license(spdlog "${spdlog_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_mimalloc)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    # Keep engine C++ new/delete and Foundation::Memory on mimalloc, but do not export
    # malloc/free from the executable. Interposing debug mimalloc into system shared
    # libraries such as SDL3 corrupts allocator ownership during their startup paths.
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    sturdy_fetchcontent_declare(mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG ${STURDY_MIMALLOC_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(mimalloc)
    sturdy_mark_dependency_targets_exclude_from_all(mimalloc mimalloc-static mimalloc::mimalloc-static mimalloc::mimalloc)
    sturdy_register_license(mimalloc "${mimalloc_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_harfbuzz)
    set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
    sturdy_fetchcontent_declare(harfbuzz
        GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG ${STURDY_HARFBUZZ_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(harfbuzz)
    sturdy_mark_dependency_targets_exclude_from_all(harfbuzz harfbuzz::harfbuzz)
    sturdy_register_license(harfbuzz "${harfbuzz_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_miniaudio)
    # miniaudio is the base audio engine: device output (up to 7.1), capture with low latency,
    # and built-in lossless decoding (FLAC/WAV) via dr_flac. It is public-domain / MIT-0, so it
    # static-links into proprietary software with no copyleft obligation. It honors
    # BUILD_SHARED_LIBS (forced OFF globally), producing a static library. Steam Audio's
    # ray-traced spatialization is layered on top as a DSP node in miniaudio's node graph.
    set(MINIAUDIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MINIAUDIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MINIAUDIO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    # We do not need OGG/Vorbis/Opus; disabling them keeps miniaudio from pulling the
    # external/ogg, external/vorbis, and external/opus subprojects into the build.
    set(MINIAUDIO_NO_LIBVORBIS ON CACHE BOOL "" FORCE)
    set(MINIAUDIO_NO_LIBOPUS OFF CACHE BOOL "" FORCE)
    sturdy_fetchcontent_declare(miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG ${STURDY_MINIAUDIO_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(miniaudio)
    sturdy_mark_dependency_targets_exclude_from_all(miniaudio miniaudio::miniaudio)
    sturdy_register_license(miniaudio "${miniaudio_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_box3d)
    # Box3D is Erin Catto's 3D physics engine (the Box2D author's successor project). Its
    # add_library() call carries no STATIC/SHARED keyword, so it follows BUILD_SHARED_LIBS
    # (forced OFF globally) and links statically. Its samples/tests/docs subdirectories are
    # gated behind PROJECT_IS_TOP_LEVEL in its own CMakeLists, which is false when it's pulled
    # in via FetchContent, so they're skipped automatically.
    sturdy_fetchcontent_declare(box3d
        GIT_REPOSITORY https://github.com/erincatto/box3d.git
        GIT_TAG ${STURDY_BOX3D_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET NAMES box3d
    )
    FetchContent_MakeAvailable(box3d)
    sturdy_mark_dependency_targets_exclude_from_all(box3d box3d::box3d)
    sturdy_register_license(box3d "${box3d_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_clay)
    # Clay (clay.h) is a single-header UI layout library with no library target of its own — its
    # CMakeLists.txt only builds examples, and CLAY_INCLUDE_ALL_EXAMPLES defaults ON, which would
    # drag in raylib/SDL2/SDL3/sokol/etc. Disable every example option so add_subdirectory is a
    # no-op, then wire up the include directory ourselves.
    set(CLAY_INCLUDE_ALL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLAY_INCLUDE_DEMOS OFF CACHE BOOL "" FORCE)
    set(CLAY_INCLUDE_CPP_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(CLAY_INCLUDE_RAYLIB_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLAY_INCLUDE_SDL2_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLAY_INCLUDE_SDL3_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLAY_INCLUDE_WIN32_GDI_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLAY_INCLUDE_SOKOL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLAY_INCLUDE_PLAYDATE_EXAMPLES OFF CACHE BOOL "" FORCE)

    sturdy_fetchcontent_declare(clay
        GIT_REPOSITORY https://github.com/nicbarker/clay.git
        GIT_TAG ${STURDY_CLAY_TAG}
    )
    FetchContent_MakeAvailable(clay)

    if(NOT TARGET clay)
        add_library(clay INTERFACE)
        target_include_directories(clay INTERFACE "${clay_SOURCE_DIR}")
    endif()
    sturdy_mark_dependency_targets_exclude_from_all(clay)
    sturdy_register_license(clay "${clay_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_msdfgen)
    # msdfgen is Chlumsky's multi-channel signed distance field generator — the reference
    # implementation the Text package's SDF/MSDF glyph rasterizer is built on
    # (plans/text-rendering-sdf-atlas.md). Only msdfgen-core is needed: it turns a vector shape
    # (contours of line/quadratic/cubic segments, exactly what HarfBuzz's hb-draw outline
    # extraction produces) into a distance field with no external dependencies of its own.
    # msdfgen-ext (font loading via FreeType, PNG/Skia support) and the msdfgen-standalone CLI are
    # both unnecessary and disabled — this engine never asks msdfgen to load a font itself.
    set(MSDFGEN_CORE_ONLY ON CACHE BOOL "" FORCE)
    set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_OPENMP OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_INSTALL OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_DYNAMIC_RUNTIME OFF CACHE BOOL "" FORCE)

    sturdy_fetchcontent_declare(msdfgen
        GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
        GIT_TAG ${STURDY_MSDFGEN_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET
    )
    FetchContent_MakeAvailable(msdfgen)
    sturdy_mark_dependency_targets_exclude_from_all(msdfgen msdfgen::msdfgen-core msdfgen-core)
    sturdy_register_license(msdfgen "${msdfgen_SOURCE_DIR}")
endfunction()

function(sturdy_fetch_stb_image)
    # stb_image.h is a single header with no CMakeLists.txt of its own — wire up the include
    # directory by hand, same shape as sturdy_fetch_clay().
    sturdy_fetchcontent_declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG ${STURDY_STB_TAG}
    )
    FetchContent_MakeAvailable(stb)

    if(NOT TARGET stb_image)
        add_library(stb_image INTERFACE)
        target_include_directories(stb_image INTERFACE "${stb_SOURCE_DIR}")
    endif()
    sturdy_mark_dependency_targets_exclude_from_all(stb_image)
    sturdy_register_license(stb "${stb_SOURCE_DIR}")
endfunction()

function(sturdy_find_stb_image)
    find_path(STURDY_STB_INCLUDE_DIR NAMES stb_image.h)
    if(NOT STURDY_STB_INCLUDE_DIR)
        message(FATAL_ERROR "Could not find stb_image.h. Install stb or enable STURDY_FETCH_DEPENDENCIES.")
    endif()

    if(NOT TARGET stb_image)
        add_library(stb_image INTERFACE)
        target_include_directories(stb_image INTERFACE "${STURDY_STB_INCLUDE_DIR}")
    endif()
endfunction()

function(sturdy_normalize_dependency_targets)
    sturdy_alias_existing_target(Sturdy::GLM
        glm::glm
        glm
    )

    sturdy_alias_existing_target(Sturdy::VMA
        GPUOpen::VulkanMemoryAllocator
        VulkanMemoryAllocator
        VulkanMemoryAllocator::VulkanMemoryAllocator
    )

    sturdy_alias_existing_target(Sturdy::Volk
        volk::volk
        volk
    )

    sturdy_alias_existing_target(Sturdy::SDL3
        SDL3::SDL3-static
        SDL3::SDL3
        SDL3-static
        SDL3
    )

    # No GLFW on Web — see the STURDY_OS STREQUAL "Web" guards in sturdy_configure_dependencies().
    if(NOT STURDY_OS STREQUAL "Web")
        sturdy_alias_existing_target(Sturdy::GLFW
            glfw
            glfw3
            glfw::glfw
            glfw3::glfw
        )
    endif()

    sturdy_alias_existing_target(Sturdy::fmt
        fmt::fmt
        fmt
    )

    sturdy_alias_existing_target(Sturdy::spdlog
        spdlog::spdlog
        spdlog
        spdlog::spdlog_header_only
    )

    sturdy_alias_existing_target(Sturdy::mimalloc
        mimalloc-static
        mimalloc
        mimalloc::mimalloc-static
        mimalloc::mimalloc
    )

    sturdy_alias_existing_target(Sturdy::HarfBuzz
        harfbuzz::harfbuzz
        harfbuzz
    )

    sturdy_alias_existing_target(Sturdy::miniaudio
        miniaudio::miniaudio
        miniaudio
    )

    sturdy_alias_existing_target(Sturdy::box3d
        box3d::box3d
        box3d
    )

    sturdy_alias_existing_target(Sturdy::clay
        clay
    )

    sturdy_alias_existing_target(Sturdy::MsdfGen
        msdfgen::msdfgen-core
        msdfgen-core
        msdfgen::msdfgen
        msdfgen
    )

    sturdy_alias_existing_target(Sturdy::StbImage
        stb_image
    )
endfunction()

function(sturdy_alias_existing_target alias_name)
    if(TARGET "${alias_name}")
        return()
    endif()

    foreach(candidate IN LISTS ARGN)
        if(TARGET "${candidate}")
            add_library("${alias_name}" INTERFACE IMPORTED GLOBAL)
            target_link_libraries("${alias_name}" INTERFACE "${candidate}")
            return()
        endif()
    endforeach()

    message(FATAL_ERROR "None of these targets exist for ${alias_name}: ${ARGN}")
endfunction()
