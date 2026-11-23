# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
# Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
# ==============================================================================
# PoseStudio Open Source Project
# File: cmake/Shaders.cmake
# Description: Shader compilation (GLSL -> SPIR-V) and the mirror of the compiled shaders
#              next to the executable. Included by the root CMakeLists.txt after the
#              PoseStudio target, which it attaches to; glslc comes from the Vulkan SDK
#              (Vulkan_GLSLC_EXECUTABLE, cmake/Dependencies.cmake).
# ==============================================================================

# Vulkan consumes SPIR-V, not GLSL, so every shader is compiled by glslc at build time.
# The compiled .spv files are written to the build tree, then mirrored next to the
# executable (same approach as Maquettes), where the renderer's shader library loads them at
# runtime by name (the viewport hands it <applicationDirPath>/shaders). Add new shaders to this
# list and they're picked up automatically; editing a .vert/.frag re-triggers just that compile
# via the DEPENDS.
#
# Shared GLSL lives in shaders/include/*.glsl and is pulled in with #include (glslc's
# GL_GOOGLE_include_directive, searched via -I). Every shader depends on every include file —
# a coarse but robust rule (no DEPFILE needed) that guarantees an include edit recompiles
# the shaders that use it. Add a new include file to SHADER_INCLUDES.
set(SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/viewport/shaders")
set(SHADER_INCLUDE_DIR "${SHADER_SOURCE_DIR}/include")
set(SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_BINARY_DIR}")

set(SHADER_INCLUDES
    "${SHADER_INCLUDE_DIR}/material.glsl"
    "${SHADER_INCLUDE_DIR}/camera_ubo.glsl"   # the set-0 camera/lighting UBO + rotateY
    "${SHADER_INCLUDE_DIR}/skinning.glsl"     # joint buffer + dual-quaternion skinning blend
    "${SHADER_INCLUDE_DIR}/correctives.glsl"  # pose-corrective buffers + per-vertex blend
    "${SHADER_INCLUDE_DIR}/color.glsl"        # the ACES tonemap
    "${SHADER_INCLUDE_DIR}/fullscreen.glsl"   # the fullscreen triangle
)

# The shader list mirrors kShaderNames in src/viewport/rendering/shaderlibrary.h — keep both in step.
set(SHADER_SOURCES
    grid.vert
    grid.frag
    mesh.vert
    mesh.frag
    skeleton.vert
    skeleton.frag
    shadow.vert
    shadow.frag
    background.vert
    background.frag
    fullscreen.vert
    bloombright.frag
    bloomblur.frag
    composite.frag
    sssblur.frag
    outlinemask.frag
    depthresolve.frag
    photoresolve.frag
    splat.vert
    splat.frag
    splatmask.frag
    traced.frag
    depthcutout.frag
)

set(COMPILED_SPV_FILES "")
foreach(shader ${SHADER_SOURCES})
    set(spv "${SHADER_BINARY_DIR}/${shader}.spv")
    add_custom_command(
        OUTPUT "${spv}"
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} -I "${SHADER_INCLUDE_DIR}" "${SHADER_SOURCE_DIR}/${shader}" -o "${spv}"
        DEPENDS "${SHADER_SOURCE_DIR}/${shader}" ${SHADER_INCLUDES}
        COMMENT "Compiling shader ${shader} -> ${shader}.spv"
        VERBATIM
    )
    list(APPEND COMPILED_SPV_FILES "${spv}")
endforeach()

add_custom_target(PoseStudioShaders DEPENDS ${COMPILED_SPV_FILES})
add_dependencies(PoseStudio PoseStudioShaders)

# Mirror the compiled shaders next to the executable on every build, through
# cmake/MirrorDirectory.cmake: it replaces the copy that is there, so a renamed or removed
# shader does not linger, and it leaves things alone when the executable's folder already is
# the build tree's shaders folder (a single-config generator), where a remove-then-copy would
# delete the compile output itself.
add_custom_command(
    TARGET PoseStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            "-DFROM=${SHADER_BINARY_DIR}"
            "-DTO=$<TARGET_FILE_DIR:PoseStudio>/shaders"
            -P "${CMAKE_CURRENT_LIST_DIR}/MirrorDirectory.cmake"
    COMMENT "Mirroring compiled SPIR-V shaders next to the executable"
    VERBATIM
)
