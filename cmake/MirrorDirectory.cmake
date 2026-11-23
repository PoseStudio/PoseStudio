# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
# Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
# ==============================================================================
# PoseStudio Open Source Project
# File: cmake/MirrorDirectory.cmake
# Description: Build-time helper run in script mode (cmake -P). Makes TO an exact copy of
#              FROM — what TO held before is removed first, so a file deleted from FROM
#              does not linger — and does nothing when the two name the same folder.
#
# Usage:  cmake -DFROM=<source folder> -DTO=<destination folder> -P MirrorDirectory.cmake
#
# Why "the same folder" happens: the compiled shaders are written to <build>/shaders, and
# the executable lands in <build>/<Config>/ with a multi-config generator (Visual Studio,
# Xcode) but in <build>/ itself with a single-config one (Ninja, Makefiles). In the second
# case the mirror's destination IS its source, and removing TO would remove FROM.
# ==============================================================================

if(NOT DEFINED FROM OR NOT DEFINED TO)
    message(FATAL_ERROR "MirrorDirectory.cmake needs -DFROM=<folder> and -DTO=<folder>")
endif()
if(NOT IS_DIRECTORY "${FROM}")
    message(FATAL_ERROR "MirrorDirectory.cmake: the source folder does not exist: ${FROM}")
endif()

# Compare the two folders as the file system resolves them: symbolic links followed, and
# without regard to letter case where the host's file system ignores it.
file(REAL_PATH "${FROM}" _mirror_source)
get_filename_component(_mirror_target "${TO}" ABSOLUTE)
if(EXISTS "${_mirror_target}")
    file(REAL_PATH "${_mirror_target}" _mirror_target)
endif()
if(CMAKE_HOST_WIN32 OR CMAKE_HOST_APPLE)
    string(TOLOWER "${_mirror_source}" _mirror_source_key)
    string(TOLOWER "${_mirror_target}" _mirror_target_key)
else()
    set(_mirror_source_key "${_mirror_source}")
    set(_mirror_target_key "${_mirror_target}")
endif()
if(_mirror_source_key STREQUAL _mirror_target_key)
    return() # already in place: the executable sits next to the compile output
endif()

file(REMOVE_RECURSE "${_mirror_target}")
file(MAKE_DIRECTORY "${_mirror_target}")
file(COPY "${_mirror_source}/" DESTINATION "${_mirror_target}")
