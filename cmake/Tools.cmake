# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
# Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
# ==============================================================================
# PoseStudio Open Source Project
# File: cmake/Tools.cmake
# Description: The test tools under tools/ and the CTest suite: one small program and one
#              test for each part of the engine that is checked outside the application,
#              and the user manual's checker. Included by the root CMakeLists.txt after
#              the shared libraries (posestudio_ik, posestudio_figureimport,
#              posestudio_miniz, posestudio_glm) and after enable_testing().
#
#              To add a tool: its sources under tools/<name>/, and one
#              posestudio_check_tool() call here.
# ==============================================================================

# Each tool under tools/ that checks a part of the engine is a small program registered as ONE
# CTest test: it compiles its own .cpp with the few application sources it exercises — or links
# the shared library that holds them — prints its checks, and exits non-zero when one fails.
#
# posestudio_check_tool(<target> <test name>
#     SOURCES      <files...>     the tool's own sources, and the application sources it compiles
#     [INCLUDES    <dirs...>]     its include directories
#     [LIBRARIES   <targets...>]  what it links: a shared library of the application's sources,
#                                 posestudio_glm for anything that includes GLM, Vulkan::Headers,
#                                 Qt modules
#     [DEFINITIONS <defs...>])    compile definitions of its own
#
# No tool has a Q_OBJECT in it, so Qt's generators (moc, uic, rcc) are switched off for all of
# them: nothing generated is compiled into a tool, and no tool gets an _autogen project.
#
# A tool that links Qt needs Qt's DLLs when it runs, and nothing copies them next to it (that is
# done for a release package only): on Windows its test gets Qt's bin folder at the front of its
# PATH, so `ctest` works from any shell. (ENVIRONMENT_MODIFICATION came with CMake 3.22; under an
# older CMake those tests need Qt's bin folder on the PATH of the shell that runs them.)
function(posestudio_check_tool target test_name)
    cmake_parse_arguments(PARSE_ARGV 2 tool "" "" "SOURCES;INCLUDES;LIBRARIES;DEFINITIONS")
    add_executable(${target} ${tool_SOURCES})
    set_target_properties(${target} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    if(tool_INCLUDES)
        target_include_directories(${target} PRIVATE ${tool_INCLUDES})
    endif()
    if(tool_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${tool_DEFINITIONS})
    endif()
    if(tool_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${tool_LIBRARIES})
    endif()
    add_test(NAME ${test_name} COMMAND ${target})
    if(WIN32 AND "${tool_LIBRARIES}" MATCHES "Qt6::" AND NOT CMAKE_VERSION VERSION_LESS 3.22)
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endfunction()

# The full-body-IK harness: unit tests for the IK math/constraints plus a drag-loop simulator
# that runs the REAL engine loop (the Armature's own drag tick: the joint-space solve, applied in
# full, through the same cursor filter and tick order as the viewport) on a dumped figure
# skeleton and gates the measured behaviour. Built by default so it can't rot, and registered
# with CTest. It links the IK core the application links (posestudio_ik: no Qt, no Vulkan), so
# what it drives is the application's own object code. The real-figure phases need a skeleton
# dump (run the app once with POSESTUDIO_DUMP_SKELETON=<path>; the dump is a vendor's rig and
# stays out of the repo) — pass it on the command line or in POSESTUDIO_IK_SKELETON, which a test
# reads from the environment `ctest` is run in; without one those phases are skipped and the
# unit tests still run.
posestudio_check_tool(PoseStudioIkHarness ik-harness
    SOURCES
        tools/ikharness/ikharness.cpp   # argument parsing, --custom, main
        tools/ikharness/report.cpp      # gates + phase printing
        tools/ikharness/report.h
        tools/ikharness/simulator.cpp   # the drag-loop simulator over the real Armature
        tools/ikharness/simulator.h
        tools/ikharness/unitphases.cpp  # skeleton-free unit phases
        tools/ikharness/unitphases.h
        tools/ikharness/realphases.cpp  # the gated real-figure phases
        tools/ikharness/realphases.h
    INCLUDES
        tools/ikharness
    LIBRARIES
        posestudio_ik
)
# Without a dump the harness says that the real-figure phases did not run, and CTest then shows
# ik-harness as SKIPPED: a run that gated no figure is not a pass. A test skipped by its output
# is skipped whatever its exit code, so the unit phases — which need no dump — are a test of
# their own, which always runs and always counts.
set_tests_properties(ik-harness PROPERTIES
    SKIP_REGULAR_EXPRESSION "\\[SKIP\\] \\[real\\] figure phases \\(no skeleton dump")
add_test(NAME ik-harness-unit COMMAND PoseStudioIkHarness --only "[unit]")

# The .pss project codec's round-trip guard (tools/projectfile): writes a fully-populated
# ProjectDocument and reads it back field-by-field, then the reader's rejection cases and a
# forward-compatibility document. Compiles the codec alone — scene/projectfile.cpp, the material
# codec it shares with the .psm file (scene/materialjson.cpp) and the material's name tables
# (scene/materialparams.cpp) — no Qt, no Vulkan; its scratch files live in the OS temp dir, so it
# runs on every platform the app builds for.
posestudio_check_tool(PoseStudioProjectFileCheck project-file-check
    SOURCES
        tools/projectfile/roundtrip.cpp
        src/viewport/scene/projectfile.cpp
        src/viewport/scene/materialjson.cpp
        src/viewport/scene/materialparams.cpp
    INCLUDES
        src/viewport/scene
        ${nlohmann_json_SOURCE_DIR}/single_include
    LIBRARIES
        posestudio_glm
)

# The overlay compositor's guard (tools/overlay): the blend modes and bitmap adjustments on known
# colors, the crop of an overlay image, and whole composites — color over a skin map, gloss into
# the roughness map, a height added about mid grey, a normal blended, a stack's order, a
# mismatched overlay size, a zone without the map. scene/overlaycompositor.cpp alone; the Vulkan
# HEADERS only (modeldata.h's vertex layout names them) — no Qt, no device.
posestudio_check_tool(PoseStudioOverlayCheck overlay-check
    SOURCES
        tools/overlay/check_overlay.cpp
        src/viewport/scene/overlaycompositor.cpp
        src/viewport/scene/materialparams.cpp
    INCLUDES
        src/viewport/scene
        src/viewport/import
        src/viewport/util
    LIBRARIES
        posestudio_glm
        Vulkan::Headers
)

# The guard of THE SMOOTHING BY ANGLE (tools/smoothing): a faceted sphere made round, a cube's
# and a hexagon's edges kept, a surface smoothed across its material zones, what a vertex
# carries over, and the import's rule for which meshes get it. import/smoothnormals.cpp and the
# per-mesh tangents alone; the Vulkan HEADERS only — no Qt, no device.
posestudio_check_tool(PoseStudioSmoothingCheck smoothing-check
    SOURCES
        tools/smoothing/check_smoothing.cpp
        src/viewport/import/smoothnormals.cpp
        src/viewport/import/tangentgen.cpp
    INCLUDES
        src/viewport/import
    LIBRARIES
        posestudio_glm
        Vulkan::Headers
)

# The guard of DISPLACEMENT (tools/displace): scene/meshdisplace alone — a sphere grown by its
# strength, the mid level, the subdivision's counts and the triangle budget, a UV seam and a
# box's edges kept closed, a border shared with another zone held, the normals turned by the
# relief and left alone without any, the material's UV transform. std + GLM; no Qt, no device.
posestudio_check_tool(PoseStudioDisplaceCheck displace-check
    SOURCES
        tools/displace/check_displace.cpp
        src/viewport/scene/meshdisplace.cpp
    INCLUDES
        src/viewport/scene
        src/viewport/util
    LIBRARIES
        posestudio_glm
)

# The guard of import/texturegutters (tools/gutters): the import's gutter fill leaves every texel
# a UV triangle covers and fills the rest — a quad over the WHOLE UV square (corners on 0 and 1:
# a cube's or a plane's mapping) included, whose second triangle the duplicate test used to drop.
# That one file; the Vulkan HEADERS only — no Qt, no device.
posestudio_check_tool(PoseStudioGutterCheck gutter-check
    SOURCES
        tools/gutters/check_gutters.cpp
        src/viewport/import/texturegutters.cpp
    INCLUDES
        src/viewport/import
        src/viewport/util
    LIBRARIES
        posestudio_glm
        Vulkan::Headers
)

# The saved material's guard (tools/materialfile): the .psm codec as text — a round trip, the
# overlays left out, every way a text is not a material — and the ball a material is pictured
# on: the mesh a render engine traces (winding, normals, where the map's middle is) and the
# built-in shader a copy without an engine draws the icon with. No Qt, no device (the Vulkan
# HEADERS only: modeldata.h's vertex layout names them).
posestudio_check_tool(PoseStudioMaterialFileCheck material-file-check
    SOURCES
        tools/materialfile/check_materialfile.cpp
        src/viewport/scene/materialfile.cpp
        src/viewport/scene/materialjson.cpp
        src/viewport/scene/materialparams.cpp
        src/viewport/scene/materialball.cpp
        src/viewport/scene/environment.cpp
    INCLUDES
        src/viewport/scene
        src/viewport/rendering     # ibldata.h (environment.h's bake products)
        src/viewport/import
        src/viewport/util
        ${nlohmann_json_SOURCE_DIR}/single_include
    LIBRARIES
        posestudio_glm
        Vulkan::Headers
)

# The MaterialX material reader's guard and report tool (tools/materialx): the four shading
# models, the texture nodes (slot, channel, factor, placement), prefixes and color spaces, the
# older document form, includes, and what is not a material file — on documents written in the
# tool; with --report <file.mtlx> it prints what a real file's materials read as. No Qt, no
# device, no JSON: the reader, the material's values and the color curves (util) are all it needs.
posestudio_check_tool(PoseStudioMaterialXCheck materialx-check
    SOURCES
        tools/materialx/check_materialx.cpp
        src/viewport/scene/materialxfile.cpp
        src/viewport/scene/materialparams.cpp
    INCLUDES
        src/viewport/scene
        src/viewport/util
    LIBRARIES
        posestudio_glm
)

# The shader-preset reader's guard and report tool (tools/shaderpreset): a content library's
# shader preset (.duf, gzip-compressed or plain) read as a material — the three base mixing
# modes, the coat as a clear coat and merged, flakes, refraction, emission, the relief, partial
# presets onto a standing material, and what is refused — on presets written in the tool; with
# --report <file|folder> [--root <content root>] it prints what real presets read as and a tally.
# The reader with the figure importer's document helpers (gzip, URI decoding), the material's
# values, the spec/gloss solve, and stb_image for the report's map means. No Qt, no device.
posestudio_check_tool(PoseStudioShaderPresetCheck shader-preset-check
    SOURCES
        tools/shaderpreset/check_shaderpreset.cpp
        src/viewport/scene/shaderpresetfile.cpp
        src/viewport/scene/materialparams.cpp
        src/viewport/import/specgloss.cpp
        src/viewport/stb_impl.cpp
    INCLUDES
        src/viewport/scene
        src/viewport/util
        ${stb_SOURCE_DIR}
    LIBRARIES
        posestudio_figureimport
        posestudio_miniz
        posestudio_glm
)

# The guard of a render's Gaussian splat composite (tools/splatcomposite): the display transform
# and its inverse, the capture coming out of a render as it went in, the backdrop behind it, the
# scene laid over it in linear light (a figure's edge, the floor's shadow), the front of the
# capture over the scene, a transparent picture, the linear picture an OpenEXR file is saved
# from. render/splatcomposite.cpp alone — no Qt, no engine, no device.
posestudio_check_tool(PoseStudioSplatCompositeCheck splat-composite-check
    SOURCES
        tools/splatcomposite/check_composite.cpp
        src/render/splatcomposite.cpp
    INCLUDES
        src/render
        src/viewport/util
)

# The script-link reader's guard (tools/scriptlink): writes synthetic content-library scripts —
# folder links, file loaders, web links, chains, programs, an encrypted one — and checks where
# AssetScriptLink::follow() leads, and which folders loneFigureIn() opens. Qt6::Core plus the
# figure importer's document sniff (figureutils + figuredocument, out of posestudio_figureimport,
# and miniz); no widgets, no Vulkan device.
posestudio_check_tool(PoseStudioScriptLinkCheck script-link-check
    SOURCES
        tools/scriptlink/check_links.cpp
        src/assets/assetscriptlink.cpp
    INCLUDES
        src/assets
        src/core
    LIBRARIES
        posestudio_figureimport
        posestudio_miniz
        Qt6::Core
)

# The library browsers' data layer (tools/assetdb): the Asset Manager and the Materials Library
# are one widget over one database, and this checks what keeps them apart and what they share —
# the scope column's migration and the schema file's own, collections listed / named / created
# per browser, favorites shown in part and reordered in part. The real AssetDb statements on an
# in-memory SQLite database; Qt6::Core + Qt6::Sql, no widgets, nothing of the user's touched.
posestudio_check_tool(PoseStudioAssetDbCheck asset-db-check
    SOURCES
        tools/assetdb/check_assetdb.cpp
        src/assets/assetdb.cpp
        src/assets/assetscan.cpp
        src/core/database.cpp
    INCLUDES
        src
        src/assets
        src/core
    DEFINITIONS
        POSESTUDIO_VERSION="0.0.0"
        POSESTUDIO_SCHEMA_FILE="${CMAKE_SOURCE_DIR}/resources/database/initialize.sql"
    LIBRARIES
        Qt6::Core
        Qt6::Sql
)

# The asset store's guard (tools/assetstore): the standard for where a saved asset's files live
# (src/assets/assetstore.h) run on folders it makes in the OS temp folder — textures collected
# beside an asset (linked, copied, the same bytes found), found again after the folder was
# rearranged, an asset renamed / duplicated / moved / deleted with its icon and textures, and a
# package written and installed. QtCore + nlohmann + miniz; no widgets, no database, no Vulkan.
posestudio_check_tool(PoseStudioAssetStoreCheck asset-store-check
    SOURCES
        tools/assetstore/check_assetstore.cpp
        src/assets/assetstore.cpp
        src/assets/assetfileops.cpp
        src/assets/assetpackage.cpp
    INCLUDES
        src
        src/assets
        src/core
        ${nlohmann_json_SOURCE_DIR}/single_include
    LIBRARIES
        posestudio_miniz
        Qt6::Core
)

# The native figure importer's guard and report tool (tools/figurecheck): with no arguments it
# writes a synthetic content library — a base figure, shape morphs, controls, a two-figure scene —
# into the OS temp folder and checks what a character preset comes out as (the rules that turn "a
# preset dials a control" into "the cage has this shape" and "this zone has that material");
# with --report <preset> it prints what a REAL preset imports as, for a before/after diff across a
# content library. The whole Qt-free importer (posestudio_figureimport, with the Vulkan HEADERS
# its vertex layout names) + miniz — no Qt, no device.
posestudio_check_tool(PoseStudioFigureCheck figure-shape-check
    SOURCES
        tools/figurecheck/check_figure.cpp
    LIBRARIES
        posestudio_figureimport
        posestudio_miniz
)

# The render engine's guard (tools/rendercheck): loads the engine library the way the application
# does (src/render/renderenginelibrary) and renders a small scene made in the tool through the
# engine interface, checking the conventions the interface states and start / status / cancel.
# Qt6::Core + Gui (QLibrary, QImage for --out); no Vulkan, no figure. WITHOUT an engine — a build
# made without Cycles — it reports that and exits 0, the application being complete without one,
# and CTest shows the test as SKIPPED: nothing was rendered, so nothing passed.
posestudio_check_tool(PoseStudioRenderCheck render-engine-check
    SOURCES
        tools/rendercheck/check_render.cpp
        src/render/renderenginelibrary.cpp
    INCLUDES
        src/render
    LIBRARIES
        Qt6::Core
        Qt6::Gui
)
set_tests_properties(render-engine-check PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped \\(a build without Cycles\\)")

# The user manual's checker (docs/manual/README.md): the manifest, the pages, every link, and a
# warning per menu action the manual does not mention. Needs only Python.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME manual-check
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/manual/check_manual.py"
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
