include_guard(GLOBAL)
include(FetchContent)
include(CheckCXXSourceCompiles)

# Aggregates upstream LICENSE/COPYING/NOTICE files from every FetchContent dependency into
# thirdparty/licenses/<name>/ so a shipped build carries attribution regardless of whether the
# dependency was actually vendored or resolved to a system package via
# STURDY_PREFER_SYSTEM_DEPENDENCIES (linking a system copy doesn't remove the obligation to
# ship its license). thirdparty/licenses/ is gitignored (it's regenerated content, not something
# to track), so it's populated by a build-time custom command rather than only at configure time
# — that way an incremental build that never reconfigures still repopulates it before any of our
# own C++ gets compiled.
set(STURDY_LICENSES_DIR "${CMAKE_SOURCE_DIR}/thirdparty/licenses" CACHE PATH
    "Directory upstream dependency licenses are copied into at build time."
)

# Registers `name`'s license file (found under its FetchContent source dir) to be copied into
# STURDY_LICENSES_DIR/<name>/ as part of the sturdy_collect_licenses target. `source_dir` is
# normally `${name}_SOURCE_DIR` — but when STURDY_PREFER_SYSTEM_DEPENDENCIES resolved `name` to
# a system package (via FIND_PACKAGE_ARGS), FetchContent_MakeAvailable() never populates it and
# that variable is empty. FetchContent also refuses to populate `name` directly at that point
# ("already populated by find_package()"), so in that case this does its own small, independent
# shallow clone under a distinct name — purely to read the license, decoupled from whichever
# copy (vendored or system) actually gets linked — using the GIT_REPOSITORY/GIT_TAG stashed by
# sturdy_fetchcontent_declare(name ...).
function(sturdy_register_license name source_dir)
    if(NOT source_dir OR NOT EXISTS "${source_dir}")
        get_property(_sturdy_dep_repo GLOBAL PROPERTY STURDY_DEP_GIT_REPOSITORY_${name})
        get_property(_sturdy_dep_tag GLOBAL PROPERTY STURDY_DEP_GIT_TAG_${name})
        if(_sturdy_dep_repo AND _sturdy_dep_tag)
            FetchContent_Populate("${name}_license_fetch"
                QUIET
                GIT_REPOSITORY "${_sturdy_dep_repo}"
                GIT_TAG "${_sturdy_dep_tag}"
                GIT_SHALLOW TRUE
                GIT_PROGRESS FALSE
                SOURCE_DIR "${STURDY_DEPS_CACHE_DIR}/${name}-license-src"
                SUBBUILD_DIR "${STURDY_DEPS_CACHE_DIR}/${name}-license-subbuild"
            )
            set(source_dir "${${name}_license_fetch_SOURCE_DIR}")
        endif()
    endif()

    if(NOT source_dir OR NOT EXISTS "${source_dir}")
        message(WARNING "sturdy_register_license: could not obtain source for '${name}' to collect its license")
        return()
    endif()

    file(GLOB _sturdy_license_candidates
        LIST_DIRECTORIES FALSE
        "${source_dir}/LICENSE*"
        "${source_dir}/License*"
        "${source_dir}/license*"
        "${source_dir}/COPYING*"
        "${source_dir}/Copying*"
        "${source_dir}/copying*"
        "${source_dir}/NOTICE*"
    )
    if(NOT _sturdy_license_candidates)
        message(WARNING "sturdy_register_license: no LICENSE/COPYING file found for '${name}' in ${source_dir}")
        return()
    endif()

    list(GET _sturdy_license_candidates 0 _sturdy_license_file)
    get_filename_component(_sturdy_license_filename "${_sturdy_license_file}" NAME)
    set(_sturdy_dest_file "${STURDY_LICENSES_DIR}/${name}/${_sturdy_license_filename}")

    add_custom_command(
        OUTPUT "${_sturdy_dest_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${STURDY_LICENSES_DIR}/${name}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_sturdy_license_file}" "${_sturdy_dest_file}"
        DEPENDS "${_sturdy_license_file}"
        COMMENT "Collecting ${name} license"
        VERBATIM
    )
    add_custom_target(sturdy_license_${name} DEPENDS "${_sturdy_dest_file}")
    set_property(GLOBAL APPEND PROPERTY STURDY_LICENSE_TARGETS "sturdy_license_${name}")

    # Recorded (as parallel lists) for sturdy_generate_license_embeds() to #embed directly from
    # the original source path — independent of the build-time copy into STURDY_LICENSES_DIR above.
    set_property(GLOBAL APPEND PROPERTY STURDY_LICENSE_NAMES "${name}")
    set_property(GLOBAL APPEND PROPERTY STURDY_LICENSE_FILES "${_sturdy_license_file}")
    set_property(GLOBAL APPEND PROPERTY STURDY_LICENSE_FILENAMES "${_sturdy_license_filename}")
endfunction()

# Probes whether the active C++ compiler accepts #embed (standard in C++26, offered as an
# extension in C++23 mode by recent Clang/GCC — but not by every compiler this engine might be
# built with, e.g. MSVC as of this writing). Cached so the probe only runs once per build tree.
function(sturdy_check_embed_support)
    if(DEFINED CACHE{STURDY_HAVE_CXX_EMBED})
        return()
    endif()

    set(_sturdy_probe_file "${CMAKE_BINARY_DIR}/CMakeFiles/sturdy_embed_probe.txt")
    if(NOT EXISTS "${_sturdy_probe_file}")
        file(WRITE "${_sturdy_probe_file}" "x")
    endif()

    set(CMAKE_REQUIRED_QUIET TRUE)
    check_cxx_source_compiles("
        int main() {
            static const unsigned char data[] = {
#embed \"${_sturdy_probe_file}\"
            };
            return sizeof(data) > 0 ? 0 : 1;
        }
    " STURDY_HAVE_CXX_EMBED)
endfunction()

# Turns a file's raw bytes into a comma-separated C++ integer-literal list (e.g. "0x1f,0x8b,...")
# suitable for initializing an `unsigned char[]` — the fallback used when the compiler lacks
# #embed. Slower than #embed (the whole point of #embed is letting the compiler skip tokenizing
# one integer literal per byte), but these are small license files, and this path only runs at
# all on a compiler that can't do better.
function(sturdy_file_to_hex_literal file_path out_var)
    file(READ "${file_path}" _sturdy_hex HEX)
    if(_sturdy_hex STREQUAL "")
        set(${out_var} "0" PARENT_SCOPE)
        return()
    endif()
    string(REGEX REPLACE "(..)" "0x\\1," _sturdy_literal "${_sturdy_hex}")
    string(REGEX REPLACE ",$" "" _sturdy_literal "${_sturdy_literal}")
    set(${out_var} "${_sturdy_literal}" PARENT_SCOPE)
endfunction()

# Generates the Sturdy.Core :Licenses partition's implementation unit (see Core/Licenses.cppm)
# embedding every dependency license registered via sturdy_register_license() directly into a
# single generated .cpp, plus a metadata table pairing each with its project name. Runs at
# configure time, since the dependency graph — and therefore which licenses exist to embed — is
# only known once every sturdy_fetch_*() call has resolved. The generated file is not tracked in
# source control; Core/CMakeLists.txt adds it to the Core target via STURDY_LICENSES_EMBED_CPP.
function(sturdy_generate_license_embeds)
    sturdy_check_embed_support()

    get_property(_sturdy_names GLOBAL PROPERTY STURDY_LICENSE_NAMES)
    get_property(_sturdy_files GLOBAL PROPERTY STURDY_LICENSE_FILES)
    get_property(_sturdy_filenames GLOBAL PROPERTY STURDY_LICENSE_FILENAMES)

    set(_sturdy_byte_arrays "")
    set(_sturdy_table_entries "")

    list(LENGTH _sturdy_names _sturdy_count)
    if(_sturdy_count GREATER 0)
        math(EXPR _sturdy_last_index "${_sturdy_count} - 1")
        foreach(_sturdy_i RANGE 0 ${_sturdy_last_index})
            list(GET _sturdy_names ${_sturdy_i} _sturdy_name)
            list(GET _sturdy_files ${_sturdy_i} _sturdy_file)
            list(GET _sturdy_filenames ${_sturdy_i} _sturdy_filename)

            string(MAKE_C_IDENTIFIER "${_sturdy_name}" _sturdy_ident)

            if(STURDY_HAVE_CXX_EMBED)
                set(_sturdy_bytes_body "#embed \"${_sturdy_file}\" if_empty(0)")
            else()
                sturdy_file_to_hex_literal("${_sturdy_file}" _sturdy_bytes_body)
            endif()

            # `unsigned char` (not `char`) so every byte value 0-255 fits without a narrowing
            # conversion — matters for the hex-literal fallback, since #embed's own values get a
            # standard-mandated exemption from that check but plain integer literals don't.
            # `const` (not `constexpr`): these are read at runtime only, and reinterpret_cast
            # (needed below to view the bytes as `char` for EmbeddedText) isn't usable inside a
            # constant expression, so making the array `constexpr` would just fail to compile.
            string(APPEND _sturdy_byte_arrays
                "        inline const unsigned char ${_sturdy_ident}_license_bytes[] = {\n${_sturdy_bytes_body}\n        };\n\n"
            )
            string(APPEND _sturdy_table_entries
                "            {\"${_sturdy_name}\", \"${_sturdy_filename}\", Foundation::EmbeddedText{reinterpret_cast<const char *>(${_sturdy_ident}_license_bytes), sizeof(${_sturdy_ident}_license_bytes)}},\n"
            )
        endforeach()
    endif()

    set(_sturdy_generated_file "${CMAKE_BINARY_DIR}/generated/Core/LicensesEmbed.cpp")

    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/Core")
    file(WRITE "${_sturdy_generated_file}" "\
// GENERATED by cmake/SturdyLicenses.cmake (sturdy_generate_license_embeds) at configure time,
// from every dependency license registered via sturdy_register_license(). Do not hand-edit —
// re-run cmake to regenerate.
#if defined(__clang__)
#pragma clang diagnostic ignored \"-Wc23-extensions\"
#endif
#include <span>
#include <string_view>
#include <Foundation/Foundation.hpp>

#include <Core/Licenses.hpp>

namespace SFT::Core {
    namespace {
${_sturdy_byte_arrays}        inline const ThirdPartyLicense all_licenses_table[] = {
${_sturdy_table_entries}        };
    } // namespace

    std::span<const ThirdPartyLicense> third_party_licenses() noexcept {
        return all_licenses_table;
    }
} // namespace SFT::Core
")

    set(STURDY_LICENSES_EMBED_CPP "${_sturdy_generated_file}" CACHE INTERNAL
        "Generated Sturdy.Core :Licenses implementation embedding every dependency's license."
    )
endfunction()

# Creates the aggregate sturdy_collect_licenses target every compiled Sturdy package depends on
# (see sturdy_add_package in SturdyPackage.cmake). Call once, after every
# sturdy_register_license() call has run.
function(sturdy_finalize_licenses)
    get_property(_sturdy_license_targets GLOBAL PROPERTY STURDY_LICENSE_TARGETS)
    add_custom_target(sturdy_collect_licenses ALL DEPENDS ${_sturdy_license_targets})

    sturdy_generate_license_embeds()
endfunction()
