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
option(STURDY_PREFER_SYSTEM_DEPENDENCIES "Try find_package before downloading FetchContent dependencies." ON)

function(sturdy_fetchcontent_declare name)
    set(options)
    set(one_value_args GIT_REPOSITORY GIT_TAG)
    set(multi_value_args FIND_PACKAGE_ARGS)
    cmake_parse_arguments(STURDY_FETCH "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(_find_package_args)
    if(STURDY_PREFER_SYSTEM_DEPENDENCIES AND STURDY_FETCH_FIND_PACKAGE_ARGS)
        set(_find_package_args FIND_PACKAGE_ARGS ${STURDY_FETCH_FIND_PACKAGE_ARGS})
    endif()

    FetchContent_Declare(${name}
        GIT_REPOSITORY ${STURDY_FETCH_GIT_REPOSITORY}
        GIT_TAG ${STURDY_FETCH_GIT_TAG}
        GIT_SHALLOW TRUE
        GIT_PROGRESS FALSE
        EXCLUDE_FROM_ALL
        SYSTEM
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
    set(SLANG_ENABLE_REPLAYER OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGC OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_DXIL OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_GFX OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGD OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SPIRV_TOOLS_MIMALLOC OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

    sturdy_fetchcontent_declare(slang
        GIT_REPOSITORY https://github.com/shader-slang/slang.git
        GIT_TAG ${STURDY_SLANG_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET NAMES Slang slang
    )
    FetchContent_MakeAvailable(slang)
    sturdy_mark_dependency_targets_exclude_from_all(slang slang::slang Slang::slang slang-compiler)

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
endfunction()

function(sturdy_fetch_vma)
    sturdy_fetchcontent_declare(vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG ${STURDY_VMA_TAG}
        FIND_PACKAGE_ARGS CONFIG QUIET NAMES VulkanMemoryAllocator
    )
    FetchContent_MakeAvailable(vma)
    sturdy_mark_dependency_targets_exclude_from_all(VulkanMemoryAllocator GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator::VulkanMemoryAllocator)
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
endfunction()

function(sturdy_fetch_spdlog)
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
endfunction()

function(sturdy_fetch_mimalloc)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
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
