include_guard(GLOBAL)

include(FetchContent)

set(STURDY_DEPS_FOLDER "ThirdParty")
set(STURDY_GLM_TAG "1.0.3" CACHE STRING "glm git tag to fetch.")
set(STURDY_VMA_TAG "v3.4.0" CACHE STRING "Vulkan Memory Allocator git tag to fetch.")
set(STURDY_VOLK_TAG "1.4.350" CACHE STRING "volk git tag to fetch.")
set(STURDY_SDL3_TAG "release-3.4.10" CACHE STRING "SDL3 git tag to fetch.")
set(STURDY_GLFW_TAG "3.4" CACHE STRING "GLFW git tag to fetch.")
set(STURDY_SPDLOG_TAG "v1.17.0" CACHE STRING "spdlog git tag to fetch.")
set(STURDY_MIMALLOC_TAG "v2.1.7" CACHE STRING "mimalloc git tag to fetch.")
set(STURDY_HARFBUZZ_TAG "14.2.1" CACHE STRING "HarfBuzz git tag to fetch.")
# Slang is built from source so we get a static library with SPIRV-Tools baked in.
# The first configure is slow because Slang's CMake fetches and builds spirv-tools.
# Python3 must be available on the build machine for that step.
set(STURDY_SLANG_TAG "v2026.11" CACHE STRING "Slang git tag to fetch and build from source.")

set(STURDY_VULKAN_LIBRARY "" CACHE FILEPATH "Optional explicit Vulkan loader library. Set this to a static loader library when available.")
set(STURDY_SLANG_ROOT "" CACHE PATH "Root of a Slang SDK/install containing include/ and lib/ or a SlangConfig.cmake package.")
set(STURDY_SLANG_LIBRARY "" CACHE FILEPATH "Optional explicit Slang library. Overrides automatic fetch when set together with STURDY_SLANG_INCLUDE_DIR.")
set(STURDY_SLANG_INCLUDE_DIR "" CACHE PATH "Optional explicit Slang include directory containing slang.h. Overrides automatic fetch when set together with STURDY_SLANG_LIBRARY.")

function(sturdy_configure_dependencies)
    sturdy_find_vulkan()

    if(STURDY_FETCH_DEPENDENCIES)
        sturdy_fetch_glm()
        sturdy_fetch_vma()
        sturdy_fetch_volk()
        sturdy_fetch_sdl3()
        sturdy_fetch_glfw()
        sturdy_fetch_spdlog()
        sturdy_fetch_mimalloc()
        sturdy_fetch_harfbuzz()
        sturdy_fetch_slang()
    else()
        find_package(glm CONFIG REQUIRED)
        find_package(VulkanMemoryAllocator CONFIG REQUIRED)
        find_package(volk CONFIG REQUIRED)
        find_package(SDL3 CONFIG REQUIRED)
        find_package(glfw3 CONFIG REQUIRED)
        find_package(spdlog CONFIG REQUIRED)
        find_package(mimalloc CONFIG REQUIRED)
        find_package(harfbuzz CONFIG REQUIRED)
        sturdy_find_slang()
    endif()

    sturdy_normalize_dependency_targets()
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

function(sturdy_fetch_slang)
    # Honor manual overrides even in fetch mode.
    if(STURDY_SLANG_LIBRARY AND STURDY_SLANG_INCLUDE_DIR)
        sturdy_find_slang()
        return()
    endif()

    # BUILD_SHARED_LIBS=OFF (forced globally) makes the Slang build produce a static library.
    set(SLANG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_GFX OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGD OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(slang
        GIT_REPOSITORY https://github.com/shader-slang/slang.git
        GIT_TAG ${STURDY_SLANG_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(slang)

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
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG ${STURDY_GLM_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glm)
endfunction()

function(sturdy_fetch_vma)
    FetchContent_Declare(vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG ${STURDY_VMA_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(vma)
endfunction()

function(sturdy_fetch_volk)
    set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG ${STURDY_VOLK_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(volk)
endfunction()

function(sturdy_fetch_sdl3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
    set(SDL_PIPEWIRE_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG ${STURDY_SDL3_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endfunction()

function(sturdy_fetch_glfw)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG ${STURDY_GLFW_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)
endfunction()

function(sturdy_fetch_spdlog)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG ${STURDY_SPDLOG_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(spdlog)
endfunction()

function(sturdy_fetch_mimalloc)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG ${STURDY_MIMALLOC_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(mimalloc)
endfunction()

function(sturdy_fetch_harfbuzz)
    set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(harfbuzz
        GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG ${STURDY_HARFBUZZ_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(harfbuzz)
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

    sturdy_alias_existing_target(Sturdy::GLFW
        glfw
        glfw3
        glfw::glfw
        glfw3::glfw
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
