include_guard(GLOBAL)

# Aggregates upstream LICENSE/COPYING/NOTICE files from every vendored FetchContent dependency
# into thirdparty/licenses/<name>/ so a shipped build carries attribution without bundling any
# vendored source tree. thirdparty/licenses/ is gitignored (it's regenerated content, not
# something to track), so it's populated by a build-time custom command rather than only at
# configure time — that way an incremental build that never reconfigures still repopulates it
# before any of our own C++ gets compiled.
set(STURDY_LICENSES_DIR "${CMAKE_SOURCE_DIR}/thirdparty/licenses" CACHE PATH
    "Directory upstream dependency licenses are copied into at build time."
)

# Registers `name`'s license file (found under its FetchContent source dir) to be copied into
# STURDY_LICENSES_DIR/<name>/ as part of the sturdy_collect_licenses target. Silently does
# nothing if source_dir doesn't exist (e.g. STURDY_PREFER_SYSTEM_DEPENDENCIES resolved `name`
# to a system package instead of fetching source) or carries no recognizable license file.
function(sturdy_register_license name source_dir)
    if(NOT source_dir OR NOT EXISTS "${source_dir}")
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
endfunction()

# Creates the aggregate sturdy_collect_licenses target every compiled Sturdy package depends on
# (see sturdy_add_package in SturdyPackage.cmake). Call once, after every
# sturdy_register_license() call has run.
function(sturdy_finalize_licenses)
    get_property(_sturdy_license_targets GLOBAL PROPERTY STURDY_LICENSE_TARGETS)
    add_custom_target(sturdy_collect_licenses ALL DEPENDS ${_sturdy_license_targets})
endfunction()
