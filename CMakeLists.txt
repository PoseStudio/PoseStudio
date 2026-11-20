# ==============================================================================
# PoseStudio Open Source Project
# File: CMakeLists.txt
# Description: The primary CMake build script: the project, the C++ compiler and the Qt6
#              framework, the libraries the application shares with its test tools, and
#              the application target itself. The rest is in modules, included below in
#              the order they depend on each other:
#                cmake/Dependencies.cmake   the Vulkan SDK and every fetched library
#                cmake/Shaders.cmake        GLSL -> SPIR-V, mirrored next to the executable
#                cmake/Tools.cmake          the test tools under tools/ and their CTest tests
#                cmake/CyclesEngine.cmake   the render engine plugin (opt-in)
#              Each is an include(), not a subdirectory: it runs in this file's scope, so
#              every path in it is relative to the repository root.
# ==============================================================================

cmake_minimum_required(VERSION 3.21)
# C is enabled alongside C++ because a couple of third-party dependencies ship C implementation
# files we compile ourselves (miniz's sources, as the library posestudio_miniz; ufbx.c, into the
# application). Without C in the language list, CMake classifies .c files as non-buildable and
# silently skips them.
project(PoseStudio VERSION 0.4.4 LANGUAGES C CXX)

# ==============================================================================
# [ COMPILER CONFIGURATION ]
# ==============================================================================

# Qt 6 mandates C++17 or higher. Enforce this standard across all platforms.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# [ QT6 FRAMEWORK INTEGRATION ]
# ==============================================================================

# CONTRIBUTOR NOTICE: 
# Do not hardcode your local Qt path here before submitting a Pull Request!
# If CMake cannot find Qt, pass your path via the command line when building:
# e.g., cmake -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/msvc2022_64" ..
if(NOT DEFINED CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64" CACHE PATH "Fallback Qt Path")
endif()

# Locate required Qt modules. The build will fatally abort if these are missing.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql Concurrent Network)

# Automatically invoke Qt's pre-processors during the build:
set(CMAKE_AUTOMOC ON) # Meta-Object Compiler (Handles Signals & Slots)
set(CMAKE_AUTOUIC ON) # User Interface Compiler (Handles .ui XML files)
set(CMAKE_AUTORCC ON) # Resource Compiler (Handles .qrc binary embedding)

# ==============================================================================
# [ DEPENDENCIES ]
# ==============================================================================

# The Vulkan SDK (which must be installed) and every third-party library (fetched at pinned
# versions, so a fresh clone needs nothing but Qt and the SDK). It leaves behind what the
# targets below name: the Vulkan:: targets, each library's <name>_SOURCE_DIR, the static
# libraries lightusd_static and ${POSESTUDIO_OPENXR_TARGET}.
include(cmake/Dependencies.cmake)

# ==============================================================================
# [ SHARED LIBRARIES (what the application and the test tools both link) ]
# ==============================================================================

# The Qt-free parts of the engine that a test tool exercises — the IK core, the native figure
# importer, miniz — are each compiled ONCE, as a library, and linked into the application and
# into the tools: one source list each, and a tool runs the very object code the application
# ships. A source belongs in one of these lists, and not in the application's, exactly when a
# tool under tools/ needs it too; it must then stay free of Qt and of the Vulkan device, and
# depend on no source outside its library.

# GLM, with the conventions every translation unit must share: angles in radians and a [0,1]
# clip-space depth range (Vulkan's; OpenGL's is [-1,1]). They are stated here and nowhere else —
# whatever includes GLM links this target and gets them with the include path. Mismatched GLM
# flags across files are a classic silent-corruption bug.
add_library(posestudio_glm INTERFACE)
target_include_directories(posestudio_glm INTERFACE
    ${glm_SOURCE_DIR}                        # so <glm/glm.hpp> resolves
)
target_compile_definitions(posestudio_glm INTERFACE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

# posestudio_engine_library(<name> <STATIC|OBJECT> <sources...>): one of the shared libraries.
#
# Its sources have no Qt in them, but they are the application's: they must mean to the compiler
# exactly what the application's other sources mean. Everything that links Qt is compiled the way
# Qt's platform target (Qt6::Platform) dictates — on MSVC in conforming mode (-permissive-), with
# the real __cplusplus and UTF-8 sources, and with its platform definitions (UNICODE among them) —
# and conforming mode alone changes the object code the compiler emits for these files. A library
# therefore takes the same settings, read off Qt's target rather than typed here (so they follow
# whichever Qt version is in use), and keeps them PRIVATE: a tool that links the library runs the
# application's object code, yet compiles its own sources with its own settings.
#
# It is also compiled position independent: Qt asks that of the code that links it where the
# platform has the notion (not with MSVC, where the property adds nothing), and what is linked
# into such an executable must be position independent too.
function(posestudio_engine_library name kind)
    add_library(${name} ${kind} ${ARGN})
    set_target_properties(${name} PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF   # (no Qt: nothing to generate)
        POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(${name} PRIVATE $<TARGET_PROPERTY:Qt6::Platform,INTERFACE_COMPILE_DEFINITIONS>)
    target_compile_options(${name} PRIVATE $<TARGET_PROPERTY:Qt6::Platform,INTERFACE_COMPILE_OPTIONS>)
endfunction()

# miniz's C implementation (its split source layout: core + inflate + deflate + zip). The figure
# importer inflates gzip-compressed scene files with it, tinyexr inflates through it, and the
# asset packages read and write .zip files; compiling the whole small set is link-safe and avoids
# feature-macro fiddling. (Its own CMake is skipped: cmake/miniz-shim supplies the miniz_export.h
# that it would have generated.)
#
# An OBJECT library, not a static one: LightUSD's static library carries a miniz of its own under
# the same symbol names. Handed to the linker as object files, ours is the one every reference
# binds to; as members of an archive the two copies would compete by link order. The price is
# CMake's rule for object libraries — their files reach only the programs that link them
# DIRECTLY — so every program that needs miniz, also through posestudio_figureimport, names
# posestudio_miniz itself.
posestudio_engine_library(posestudio_miniz OBJECT
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_zip.c
)
target_include_directories(posestudio_miniz PUBLIC
    ${miniz_SOURCE_DIR}                      # so <miniz.h> resolves
    ${CMAKE_SOURCE_DIR}/cmake/miniz-shim     # supplies miniz_export.h (miniz's CMake is skipped)
)

# THE IK CORE: the Armature (the runtime skeleton, its pose, and the whole full-body-IK
# integration) and scene/ik (the rig's policy and the solver). std + GLM only — no Qt, no Vulkan.
# Linked by the application and by the IK harness (tools/ikharness), which drives the real drag
# loop through it. A new scene/armature*.cpp or scene/ik/*.cpp is listed HERE, and nowhere else.
# (scene/ik/rigconformer.cpp is the one exception: it reads the importers' ModelData, and is in
# the application's list.) See the FBIK section in CLAUDE.md.
posestudio_engine_library(posestudio_ik STATIC
    # The Armature (scene/armature*): the runtime skeleton, its pose, the IK integration, the soft body.
    src/viewport/scene/armature.cpp   # the runtime skeleton + pose + FK + pins (Vulkan-free)
    src/viewport/scene/armature.h
    src/viewport/scene/armaturetypes.h    # ... its nested state types (a runtime bone, a hanging arm, the landing bounce, a hanging foot)
    src/viewport/scene/armaturesoftbody.h # ... and the soft body's (a breast's mass, a boneless breast's field, a cheek, the acceleration reader)
    src/viewport/scene/armatureik.cpp # the Armature's FBIK drag lifecycle (rig build, begin/drag/settle/end, pins)
    src/viewport/scene/armatureiksolve.cpp # the per-tick whole-body solve: the tick, the stage map, solve + apply
    src/viewport/scene/armatureiksolvedrag.cpp    # ... its stages: what the drag IS (unknowns, trunk policy, foot/knee/elbow drags)
    src/viewport/scene/armatureiksolveposture.cpp # ... the posture model (stiffness + references, the spine's coupling)
    src/viewport/scene/armatureiksolvepins.cpp    # ... the pins' rows (user pins, the foot contact model, live contacts, the knee-drag foot)
    src/viewport/scene/armatureiksolvecursor.cpp  # ... the dragged joint's rows (the cursor, the hand slide and lay, the slid foot's sole)
    src/viewport/scene/armatureiksolverows.cpp    # ... the world's rows (balance, the floor's and the body volumes' planes)
    src/viewport/scene/armatureiksolvestate.h     # ... the per-tick scratch the stages share
    src/viewport/scene/armatureiksolvetuning.h    # ... every tuning constant, with its reasoning
    src/viewport/scene/armatureiksolvegeometry.h  # ... the rotation geometry the row stages share
    src/viewport/scene/armatureikpost.cpp   # the drag tick's post-steps: the idle arms' hang, the head's righting
    src/viewport/scene/armatureboneclass.cpp # the bone classes (body / face rig / digit / carpal): what a selection selects and lights
    src/viewport/scene/armatureikdigit.cpp  # the digit drag: a finger or a toe moves its own digit alone
    src/viewport/scene/armaturelanding.cpp  # the landing bounce: a figure dropped onto her feet absorbs the landing
    src/viewport/scene/armaturehangfeet.cpp # the hanging feet come down flat: a lifted figure's feet, back on the floor
    src/viewport/scene/armaturesoftbody.cpp    # THE SOFT BODY's common entry points: the tick, the landing's kick, the settle, the delta fields' weights
    src/viewport/scene/armaturesoftbodyshared.h # ... what its translation units share: the env levers' reader, the trace switches
    src/viewport/scene/armaturebreasts.cpp     # THE SOFT BREASTS: the pectorals hung by gravity, by their size; their bounce
    src/viewport/scene/armaturebreastfield.cpp # THE BONELESS BREAST: on a rig with no breast bone, the chest's own skin moved as a delta field
    src/viewport/scene/armaturebuttocks.cpp    # THE SOFT BUTTOCKS: the glutes' skin as a delta field the GPU blends, jiggling with the hips' movement
    src/viewport/scene/softbodysettings.h      # ... and their process-wide switches (off unless the Soft Body Physics tab sets them)
    src/viewport/scene/armatureikfigure.cpp # the figure move: Ctrl + the body itself moves the figure as she is posed
    src/viewport/scene/armaturedials.cpp    # the transform dials: a joint's rotation as Bend Forward / Bend Sideways / Twist, 0-100
    src/viewport/scene/jointtransform.h     # ... and what the Transform tab shows of them (plain data)
    # scene/ik/: the rig's policy, the solver, the balance model, the body volumes, the rig standard's roles.
    src/viewport/scene/ik/ikmath.cpp          # the Euler composition and its inverse, rotation vectors, segment distances
    src/viewport/scene/ik/ikmath.h
    src/viewport/scene/ik/skeletongraph.cpp   # the body graph rooted at the pelvis: marks a drag's active paths
    src/viewport/scene/ik/bonealiases.cpp     # the bone-alias table the two posing test tools share
    src/viewport/scene/ik/bonealiases.h
    src/viewport/scene/ik/rigroles.cpp        # the rig standard's roles: what each bone is, by structure
    src/viewport/scene/ik/rigroles.h
    src/viewport/scene/ik/namerules.cpp       # the one reading of a bone name: its side token, a digit's token, the twist and segment words
    src/viewport/scene/ik/namerules.h
    src/viewport/scene/ik/skeletongraph.h
    src/viewport/scene/ik/balancecontroller.cpp # the balance model: segment masses, centre of mass, support polygon
    src/viewport/scene/ik/balancecontroller.h
    src/viewport/scene/ik/ikrig.cpp           # the drag's POLICY per figure: the rig built from the bind skeleton (body tree, masses, floor clearances)
    src/viewport/scene/ik/ikrig.h
    src/viewport/scene/ik/ikrig_constants.h   # the rig's tuning constants (private to the ikrig*.cpp)
    src/viewport/scene/ik/ikrigstructure.cpp  # what each joint IS to the rig (girdle, spine, leg, limb tip, real mass) + the tree metrics
    src/viewport/scene/ik/ikrigdrag.cpp       # beginDrag: contacts -> pins, user pins, grab promotion
    src/viewport/scene/ik/ikrigpolicy.cpp     # per-tick policy: intents, the lift-off test, the step policy's entry
    src/viewport/scene/ik/ikrigstepping.cpp   # balance-driven foot re-planting
    src/viewport/scene/ik/ikrigcontacts.cpp   # live contact re-detection mid-drag, landings
    src/viewport/scene/ik/ikrigvolumes.cpp    # the self-collision body volumes: the mesh fit, the drag-start mask, the target clamp
    src/viewport/scene/ik/bodymesh.cpp        # the body mesh sample the volumes are fitted to (+ its dump sidecar)
    src/viewport/scene/ik/jointsolver.cpp     # THE IK solver: whole-body regularized least squares on the Euler channels
    src/viewport/scene/ik/jointsolver.h
    src/viewport/scene/ik/bodyvolume.h        # the self-collision capsules + the one reading of their axes
    src/viewport/scene/ik/ikeffector.h        # one pin of a drag
    src/viewport/scene/ik/bodymesh.h
    src/viewport/scene/ik/cursorfilter.h      # the drag's adaptive cursor low-pass (shared with the harness)
)
target_include_directories(posestudio_ik PUBLIC
    src/viewport/scene
    src/viewport/scene/ik
)
target_link_libraries(posestudio_ik PUBLIC posestudio_glm)

# THE NATIVE FIGURE IMPORTER (import/figure/: rigged, morphed characters read from their JSON
# scene format, via nlohmann + miniz) — the whole Qt-free reader; its Qt-facing service,
# viewport/figureimportservice.cpp, is the application's. Linked by the application, by the
# figure check (tools/figurecheck) and by the script-link check (tools/scriptlink), which uses
# its document sniff. A new import/figure/*.cpp is listed HERE, and nowhere else.
posestudio_engine_library(posestudio_figureimport STATIC
    src/viewport/import/figure/figuredocument.cpp
    src/viewport/import/figure/figuredocument.h
    src/viewport/import/figure/uriresolver.cpp
    src/viewport/import/figure/uriresolver.h
    src/viewport/import/figure/figuredata.h
    src/viewport/import/figure/figureutils.cpp     # shared JSON/URI/path helpers of the figure parsers
    src/viewport/import/figure/figureutils.h
    src/viewport/import/figure/geometryparser.cpp
    src/viewport/import/figure/geometryparser.h
    src/viewport/import/figure/uvparser.cpp
    src/viewport/import/figure/uvparser.h
    src/viewport/import/figure/materialparser.cpp
    src/viewport/import/figure/materialparser.h
    src/viewport/import/figure/nodeparser.cpp
    src/viewport/import/figure/nodeparser.h
    src/viewport/import/figure/skinparser.cpp
    src/viewport/import/figure/skinparser.h
    src/viewport/import/figure/morphparser.cpp
    src/viewport/import/figure/morphparser.h
    src/viewport/import/figure/morphresolver.cpp
    src/viewport/import/figure/morphresolver.h
    src/viewport/import/figure/formulaops.cpp      # the formula operations' one reader (morph walk + corrective translator)
    src/viewport/import/figure/formulaops.h
    src/viewport/import/figure/correctiveparser.cpp
    src/viewport/import/figure/correctiveparser.h
    src/viewport/import/figure/subdivision.cpp
    src/viewport/import/figure/subdivision.h
    src/viewport/import/figure/correctivescan.cpp  # on-disk discovery of pose correctives
    src/viewport/import/figure/correctivescan.h
    src/viewport/import/figure/bonefixups.cpp      # morph-driven joint centres + legacy twist lock
    src/viewport/import/figure/bonefixups.h
    src/viewport/import/figure/scenematerials.cpp  # applies a preset's materials (both forms) onto zones
    src/viewport/import/figure/scenematerials.h
    src/viewport/import/figure/followeraddons.cpp  # post-load follower addons: find / fit / merge
    src/viewport/import/figure/followeraddons.h
    src/viewport/import/figure/figureimporter.cpp
    src/viewport/import/figure/figureimporter.h
)
target_include_directories(posestudio_figureimport
    PUBLIC
        src/viewport/import
        src/viewport/import/figure
        src/viewport/util
        ${nlohmann_json_SOURCE_DIR}/single_include # so <nlohmann/json.hpp> resolves
    PRIVATE
        # miniz.h for figuredocument.cpp. Only the include path: miniz's object files are linked by
        # each program itself (see posestudio_miniz).
        $<TARGET_PROPERTY:posestudio_miniz,INTERFACE_INCLUDE_DIRECTORIES>
)
# (Its headers name the import's shared types — modeldata.h, vertex.h: the Vulkan vertex-input
# structs — hence the Vulkan HEADERS for whoever includes them. No device, no loader.)
target_link_libraries(posestudio_figureimport PUBLIC posestudio_glm Vulkan::Headers)

# ==============================================================================
# [ TARGET DEFINITION & SOURCE FILES ]
# ==============================================================================

# Define the final executable output and map all required files.
# Grouped logically to make finding files easier for contributors.
add_executable(
    PoseStudio 
    
    # --- Core Application Bootstrap ---
    src/main.cpp 
    src/constants.h  
    
    # --- Application Shell (menu bar, splash/About overlay, Fusion theming) ---
    src/shell/menumanager.cpp
    src/shell/menumanager.h
    src/shell/documentactions.cpp         # the document's life: New / Open / Save, the close gate, the title, the scene-keeping restart
    src/shell/documentactions.h
    src/shell/splashoverlay.cpp
    src/shell/splashoverlay.h
    src/shell/apptheme.cpp                # AppTheme::install: icon, Fusion proxy style, QSS modules, menu shadows
    src/shell/apptheme.h
    src/shell/appproxystyle.h
    src/help/helpwindow.cpp               # Help -> User Manual (F1): contents tree + page browser + search
    src/help/helpwindow.h
    src/help/helpmanual.cpp               # the manual's manifest, embedded Markdown pages and page styling
    src/help/helpmanual.h
    src/help/whatsnewdialog.cpp           # "What's New in x.y.z": the version's release notes, once per version at startup
    src/help/whatsnewdialog.h

    # --- Asset Manager (and the Materials Library: the same widget, built twice) ---
    src/assets/assetmanagerwidget.cpp
    src/assets/assetmanagerwidget.h
    src/assets/assetmanagerwidget_tree.cpp  # the same class, a second TU: the tree (build/refresh, search, navigation)
    src/assets/assetmanagerwidget_menus.cpp # … a third: the tree/grid context menus
    src/assets/assetmanagerwidget_files.cpp # … a fourth: an asset's file operations and packages (assetstore.h)
    src/assets/assetmanagerscope.cpp      # what one instance browses: the Asset Manager / the Materials Library
    src/assets/assetmanagerscope.h
    src/assets/assetnodeids.h             # node-id / grid-role vocabulary (header-only)
    src/assets/assetfolderproxymodel.cpp  # tree proxy: folder hit icons + collection drag-reparent
    src/assets/assetfolderproxymodel.h
    src/assets/assettreeview.cpp          # the tree view: QSS separator color, drop highlight, spring-loading
    src/assets/assettreeview.h
    src/assets/assettreedelegate.cpp      # tree row painting
    src/assets/assettreedelegate.h
    src/assets/assetgriddelegate.cpp      # thumbnail grid cell painting
    src/assets/assetgriddelegate.h
    src/assets/customtooltip.cpp          # the grid's floating rich-text tooltip
    src/assets/customtooltip.h
    src/assets/assetpaths.h               # the module's path, filter and picture rules (header-only)
    src/assets/assetscan.cpp              # filesystem scanning: asset/thumbnail pairing, folder hit classification
    src/assets/assetscan.h
    src/assets/assetstore.cpp             # where a saved asset's files live: the texture store beside the asset
    src/assets/assetstore.h
    src/assets/assetstore_detail.h        # (private to the store's three implementation files)
    src/assets/assetfileops.cpp           # … its file operations: rename, duplicate, move, delete
    src/assets/assetfileops.h
    src/assets/assetpackage.cpp           # … and packages: assets and folders as a .zip
    src/assets/assetpackage.h
    src/assets/assetscriptlink.cpp        # a content-library script (.dsa) read as the folder / file / page it links to
    src/assets/assetscriptlink.h
    src/assets/assetdb.cpp                # every Collections/Favorites SQL statement
    src/assets/assetdb.h
    src/assets/assetthumbnailloader.cpp   # off-thread thumbnail decode + the gradient canvas
    src/assets/assetthumbnailloader.h
    src/assets/assetgriddragcontroller.cpp # the hand-rolled grid drag (reorder / drop onto the tree)
    src/assets/assetgriddragcontroller.h
    src/assets/assetbreadcrumblabel.cpp   # the grid title: clickable breadcrumb / plain virtual title
    src/assets/assetbreadcrumblabel.h
    src/assets/assetlibraryprompt.cpp     # "Add Asset Folder": the one dialog flow that registers a library
    src/assets/assetlibraryprompt.h

    # --- Shared custom widgets (app-standard controls, reusable across panels) ---
    src/widgets/dragnumberbox.cpp         # scrub field: drag to change, click to type
    src/widgets/dragnumberbox.h
    src/widgets/menupickerbutton.cpp      # push button + exclusive QMenu (the app's "dropdown")
    src/widgets/menupickerbutton.h

    # --- Properties panels (right-docker tabs) ---
    src/properties/environmentpanel.cpp
    src/properties/environmentpanel.h
    src/properties/hdriselector.cpp      # the HDRI picker (button + thumbnail menu) the panel hosts
    src/properties/hdriselector.h
    src/properties/transformpanel.cpp    # the Transform tab: the selected joint's three rotation dials
    src/properties/transformpanel.h
    src/properties/materialpanel.cpp     # the Materials tab: the selected object's material zones as sliders (the core: mirror + edits)
    src/properties/materialpanel.h
    src/properties/materialpanel_build.cpp   # ... its construction: sections, rows, the texture slots' names and places
    src/properties/materialpanel_slots.cpp   # ... its texture slots: thumbnails, their menu and file dialog
    src/properties/materialpanel_files.cpp   # ... its material files: Save (with the icon), Import, apply
    src/properties/materialthumbbutton.h # the tab's map thumbnail button (image + frame + hover preview)
    src/properties/overlaystack.cpp      # the tab's Overlays section: the zone's overlay stack as cards
    src/properties/overlaystack.h
    src/properties/propertyrows.cpp      # the builders every properties tab shares (scroll column, groups, scrub row, restore button)
    src/properties/propertyrows.h
    src/properties/colorswatch.cpp       # ... and their color row: the swatch button and the live color dialog
    src/properties/colorswatch.h
    src/properties/softbodypanel.cpp     # the Soft Body Physics tab: the breasts' and the buttocks' switches and dials
    src/properties/softbodypanel.h
    src/properties/lightspanel.cpp       # the Lights tab: the scene's light objects (point / spot / area / sun)
    src/properties/lightspanel.h

    # --- Production rendering (the Render window; the engine itself is a plugin: engines/cycles) ---
    src/render/renderengineapi.h          # THE ENGINE INTERFACE: the plain-data contract with an engine library
    src/render/renderscene.h              # a scene description with what it points at, held for a render
    src/render/renderenginelibrary.cpp    # finds and loads the engine library (QLibrary)
    src/render/renderenginelibrary.h
    src/render/rendersession.cpp          # one render: start, poll, the image, save
    src/render/rendersession.h
    src/render/splatcomposite.cpp         # a render with Gaussian splats in it: the scene laid into the capture's layers
    src/render/splatcomposite.h
    src/render/renderwindow.cpp           # Render -> Render Image (F12): the window
    src/render/renderwindow.h

    # --- 3D Viewport (Vulkan) ---
    # Qt-facing layer (the only viewport files that touch Qt):
    src/viewport/viewportwidget.cpp
    src/viewport/viewportwidget.h
    src/viewport/viewportwidget_testhooks.cpp # its side of the scripted in-app test: `ui`, `uishot`, `app`, `windowsize`
    src/viewport/viewportstrip.cpp        # the floating top-right control strip (pickers, buttons, badge)
    src/viewport/viewportstrip.h
    src/viewport/jointmodebadge.cpp       # the strip's "the mouse is turning Bend / Side-Side / Twist" badge
    src/viewport/jointmodebadge.h
    src/viewport/jointmodalinput.cpp      # the joint mouse mode's input wherever in the app it lands
    src/viewport/jointmodalinput.h
    src/viewport/vulkanwindow.cpp         # VulkanWindow lifecycle (one class, many TUs - see vulkanwindow.h for the map)
    src/viewport/vulkanwindow.h
    src/viewport/projectstate.cpp      # the .pss document's runtime state (path + dirty, change signal)
    src/viewport/projectstate.h
    src/viewport/vulkanwindow_environment.cpp # the HDRI: default path, off-thread IBL bake, lighting dials
    src/viewport/vulkanwindow_input.cpp   # mouse/wheel/key routing + the object context menu
    src/viewport/vulkanwindow_camera.cpp  # named views, Blender-convention view hotkeys
    src/viewport/vulkanwindow_ik.cpp      # the FBIK drag loop, the ground fall
    src/viewport/vulkanwindow_pose.cpp    # pose utilities, pins, pose files, delete, the dials + mouse mode, undo/redo
    src/viewport/vulkanwindow_project.cpp # the document: File -> New, the .pss project saved and opened
    src/viewport/vulkanwindow_materials.cpp # the Materials tab's side: the view, the edit bracket, the texture slots (decode + bake + upload)
    src/viewport/vulkanwindow_render.cpp  # the scene captured for a render engine (captureRenderScene)
    src/viewport/vulkanwindow_materialfile.cpp # a material saved (.psm), its ball icon rendered, a saved one applied
    src/viewport/vulkanwindow_lights.cpp  # the light objects: the list as an undoable edit, a marker's drag
    src/viewport/vulkanwindow_traced.cpp  # the Rendered mode: the render engine's picture kept in step with the scene
    src/viewport/rendersnapshot.h         # that capture: the description and the storage behind it
    src/viewport/vulkanwindow_bench.cpp   # POSESTUDIO_IK_PERF / _BENCH diagnostics
    src/viewport/vulkanwindow_vr.cpp      # VR Mode: the OpenXR session's lifecycle, wait thread, stereo frame
    # The OpenXR layer (Qt-free: OpenXR + Vulkan + GLM): the headset found at startup, the session
    # and its swapchains, the eye/placement math. rendering/ never includes these.
    src/viewport/vr/openxrsystem.cpp
    src/viewport/vr/openxrsystem.h
    src/viewport/vr/openxrsession.cpp
    src/viewport/vr/openxrsession.h
    src/viewport/vr/openxrinput.cpp     # the controllers: actions, bindings, sticks/trigger/buttons, aim poses, haptics
    src/viewport/vr/openxrinput.h
    src/viewport/vr/vrmath.cpp
    src/viewport/vr/vrmath.h
    src/viewport/ikdiagnostics.h          # the diagnostics state (null unless enabled)
    src/viewport/vulkanwindow_script.cpp  # POSESTUDIO_IK_SCRIPT: the scripted in-app posing test (the loader, the step driver)
    src/viewport/vulkanwindow_script_commands.cpp # ... what each script command does, by family
    src/viewport/vulkanwindow_script_metrics.cpp  # ... the metrics `expect` asserts on, and `report`
    src/viewport/vulkanwindow_script_detail.h     # ... what those three share (private to them)
    src/viewport/ikscript.h               # its state (null unless enabled)
    src/viewport/viewpreset.h             # the named camera views (strip <-> window vocabulary)
    src/viewport/modelimportservice.cpp   # an import's orchestration: parse -> texture decode -> bakes -> GPU upload
    src/viewport/modelimportservice.h
    src/viewport/texturedecode.cpp        # the texture decode layer: QImage decodes, the derived maps, the gutter fill
    src/viewport/texturedecode.h
    src/viewport/importprogress.cpp       # an import's modal progress dialog and phase clock (the four import services)
    src/viewport/importprogress.h
    src/viewport/figureimportservice.cpp  # figure analogue: figure -> ModelData -> decode -> upload
    src/viewport/figureimportservice.h
    src/viewport/environmentsource.cpp    # decodes .hdr (stb) and .exr (tinyexr) panoramas -> EnvironmentImage (codec layer)
    src/viewport/environmentsource.h
    src/viewport/import/texturegutters.cpp # UV-island gutter dilation (kills mip-bleed seam lines)
    src/viewport/import/texturegutters.h
    src/viewport/import/aobaker.cpp        # per-vertex ambient-occlusion bake (BVH hemisphere rays)
    src/viewport/import/aobaker.h
    src/viewport/import/tangentgen.cpp     # per-vertex UV tangent generation (Lengyel)
    src/viewport/import/tangentgen.h
    src/viewport/import/smoothnormals.cpp  # THE SMOOTHING BY ANGLE: shading normals computed from the faces
    src/viewport/import/smoothnormals.h
    src/viewport/stb_impl.cpp             # single TU that compiles stb_image's implementation
    src/viewport/tinyexr_impl.cpp         # single TU that compiles tinyexr's implementation
    # Layer-free utilities (pure std) shared by rendering/, scene/, import/ and the services:
    src/viewport/util/parallelfor.h       # index-parallel loop for CPU bakes (IBL prefilter, corrective subdivision)
    src/viewport/util/colorcurves.h       # the sRGB curve both ways and the ACES tonemap fit, once
    src/viewport/util/asciilower.h        # ASCII lower-casing (file extensions and names)
    # Rendering core (pure Vulkan + std + GLM, no Qt):
    src/viewport/rendering/vulkancommon.h
    src/viewport/rendering/vulkanhandles.h  # move-only RAII owners for raw device handles
    src/viewport/rendering/descriptorutil.h # one-call descriptor layout/pool/set/write helpers
    src/viewport/rendering/attachmentimage.cpp # RAII 2D attachment image + view, layout-barrier helpers
    src/viewport/rendering/attachmentimage.h
    src/viewport/rendering/renderpassbuilder.cpp # single-subpass render-pass builder + the sync contracts
    src/viewport/rendering/renderpassbuilder.h
    src/viewport/rendering/samplercache.cpp # shared device-lifetime samplers keyed by parameters
    src/viewport/rendering/samplercache.h
    src/viewport/rendering/shaderlibrary.cpp # SPIR-V loading by name + the one in-code shader list
    src/viewport/rendering/shaderlibrary.h
    src/viewport/rendering/ibldata.h        # the CPU-baked IBL products the GPU layer uploads
    src/viewport/rendering/vreyeview.h      # a VR eye's view + projection — the one VR type the core knows
    src/viewport/rendering/vulkancontext.cpp
    src/viewport/rendering/vulkancontext.h
    src/viewport/rendering/vulkanswapchain.cpp
    src/viewport/rendering/vulkanswapchain.h
    src/viewport/rendering/vulkanpipeline.cpp
    src/viewport/rendering/vulkanpipeline.h
    src/viewport/rendering/vulkanbuffer.cpp
    src/viewport/rendering/vulkanbuffer.h
    src/viewport/rendering/vulkanimage.cpp
    src/viewport/rendering/vulkanimage.h
    src/viewport/rendering/iblmaps.cpp    # GPU prefiltered-specular cubemap + BRDF LUT (IBL set 3)
    src/viewport/rendering/iblmaps.h
    src/viewport/rendering/shadowmap.cpp  # key-light depth-only shadow map (set 3 binding 2)
    src/viewport/rendering/shadowmap.h
    src/viewport/rendering/hdrtarget.cpp  # offscreen HDR scene target (linear RGBA16F + MSAA)
    src/viewport/rendering/hdrtarget.h
    src/viewport/rendering/outlinemask.cpp # selection-outline coverage mask (the selected model's silhouette)
    src/viewport/rendering/outlinemask.h
    src/viewport/rendering/postprocess.cpp # bloom chain + the tonemapping composite (+ the selection outline)
    src/viewport/rendering/postprocess.h
    src/viewport/rendering/tracedview.cpp   # the Rendered mode's path-traced picture as a texture + its draw
    src/viewport/rendering/tracedview.h
    src/viewport/rendering/gpuframetimer.cpp # GPU time of a frame and of its splat draws (timestamp queries)
    src/viewport/rendering/gpuframetimer.h
    src/viewport/rendering/selectionstyle.h # the selection outline's and the joint highlight's color + opacity (Preferences → Selection)
    src/viewport/rendering/viewportcolors.h # the viewport's background color, shared with the render export
    src/viewport/rendering/vulkancommands.cpp
    src/viewport/rendering/vulkancommands.h
    src/viewport/rendering/vulkanrenderer.cpp
    src/viewport/rendering/vulkanrenderer.h
    src/viewport/rendering/vma_impl.cpp
    # Model import (format-neutral IR + one importer per format; pure std + Vulkan + GLM, no Qt).
    # Add a format by dropping import/<fmt>/ and registering it in importerregistry.cpp.
    src/viewport/import/vertex.h
    src/viewport/import/modeldata.h
    src/viewport/import/meshimporter.h
    src/viewport/import/meshrepair.cpp     # what any reader does to an incomplete mesh (missing normals)
    src/viewport/import/meshrepair.h
    # The rigged-character readers (FBX, glTF / GLB, USD): each a file -> the skinned ModelData +
    # its raw skeleton; what they share (the facing / centre / floor step, the texture finder, the
    # import summary) lives in import/rigged/; the conformer gives the skeleton its anatomy.
    src/viewport/import/rigged/riggedimport.cpp
    src/viewport/import/rigged/riggedimport.h
    src/viewport/import/fbx/fbxfigureimporter.cpp   # the FBX figure reader (ufbx)
    src/viewport/import/fbx/fbxfigureimporter.h
    src/viewport/import/gltf/gltffigureimporter.cpp # the glTF / GLB figure reader (cgltf; the single CGLTF_IMPLEMENTATION TU)
    src/viewport/import/gltf/gltffigureimporter.h
    src/viewport/import/usd/usdfigureimporter.cpp   # the USD / USDZ figure reader (LightUSD + its render-scene layer)
    src/viewport/import/usd/usdfigureimporter.h
    src/viewport/import/splat/splatimporter.cpp     # GAUSSIAN SPLATS: the .ply / .splat / .spz reader
    src/viewport/import/splat/splatimporter.h
    src/viewport/splatimportservice.cpp             # the Qt-facing splat import (reader, progress, upload)
    src/viewport/splatimportservice.h
    src/viewport/scene/splatcloud.cpp               # the splats on the GPU: buffers, the far-first sort, the draw
    src/viewport/scene/splatcloud.h
    src/viewport/scene/ik/rigconformer.cpp          # the rig standard's conformer: frames, orders and limits per role
    src/viewport/scene/ik/rigconformer.h
    src/viewport/riggedimportservice.cpp            # the Qt-facing rigged-character import (reader by extension, conform, upload)
    src/viewport/riggedimportservice.h
    src/viewport/import/importerregistry.cpp
    src/viewport/import/importerregistry.h
    src/viewport/import/obj/objimporter.cpp   # single TINYOBJLOADER_IMPLEMENTATION TU
    src/viewport/import/obj/objimporter.h
    src/viewport/import/specgloss.cpp         # the specular / glossiness -> metallic / roughness conversion every importer applies
    src/viewport/import/specgloss.h
    # (The native figure importer, import/figure/, is the library posestudio_figureimport: see
    # [ SHARED LIBRARIES ] above.)
    # Scene:
    src/viewport/scene/ray.h             # the world-space picking ray (camera + models share it)
    src/viewport/scene/camera.cpp
    src/viewport/scene/camera.h
    src/viewport/scene/cameraubo.cpp     # the set-0 camera/lighting UBO block + its per-frame fill
    src/viewport/scene/cameraubo.h
    src/viewport/scene/environment.cpp   # procedural studio env + IBL bakes (SH, prefilter, BRDF LUT)
    src/viewport/scene/environment.h
    src/viewport/scene/lightingsettings.h # live lighting dials shared with the Environment panel
    src/viewport/scene/grid.cpp
    src/viewport/scene/grid.h
    src/viewport/scene/textureuploadcache.cpp # per-model dedup of texture uploads (shared atlases)
    src/viewport/scene/textureuploadcache.h
    src/viewport/scene/mesh.cpp          # one material group: buffers, material set, draw
    src/viewport/scene/mesh.h
    src/viewport/scene/correctiveset.cpp # a model's GPU-blended pose correctives (delta table + weights)
    src/viewport/scene/correctiveset.h
    src/viewport/scene/groundsampler.cpp # the ground button's posed lowest-point scan
    src/viewport/scene/groundsampler.h
    src/viewport/scene/model.cpp         # one imported model: meshes + armature + per-frame pose data
    src/viewport/scene/model.h
    src/viewport/scene/modelbake.cpp     # Model::bakePosedMesh: the vertex stage on the CPU, for a render engine
    src/viewport/scene/posedmesh.h       # … and what it fills
    src/viewport/scene/meshdisplace.cpp  # DISPLACEMENT: a posed mesh subdivided and moved by its displacement map, for a render
    src/viewport/scene/meshdisplace.h
    src/viewport/scene/scenelight.h      # a light object (point / spot / area / sun) and its shared rules
    src/viewport/scene/renderexport.cpp  # material / camera / lighting translated for a render engine
    src/viewport/scene/renderexport.h
    src/viewport/scene/bonepicker.cpp    # screen-space joint / bone-body picking across figures
    src/viewport/scene/bonepicker.h
    src/viewport/scene/posefile.cpp      # the plain-text .pose file reader/writer
    src/viewport/scene/posefile.h
    src/viewport/scene/projectfile.cpp   # the versioned-JSON .pss project file reader/writer (nlohmann)
    src/viewport/scene/projectfile.h
    src/viewport/scene/materialjson.cpp  # a material as JSON: the one codec of the project's zones and the .psm file
    src/viewport/scene/materialjson.h
    src/viewport/scene/materialfile.cpp  # the .psm material file (a saved material: values + maps), as text
    src/viewport/scene/materialfile.h
    src/viewport/scene/materialxfile.cpp # a MaterialX document (.mtlx) read as materials: values + image files
    src/viewport/scene/materialxfile.h
    src/viewport/scene/shaderpresetfile.cpp # a content library's shader preset (.duf) read as a material: values + map URIs
    src/viewport/scene/shaderpresetfile.h
    src/viewport/scene/materialball.cpp  # the ball a saved material is pictured on: its mesh, and the built-in ball shader
    src/viewport/scene/materialball.h
    # (The Armature, scene/armature*.cpp, and scene/ik/ are the library posestudio_ik: see
    # [ SHARED LIBRARIES ] above. Only the rig conformer, listed with the importers, is here.)
    src/viewport/scene/materialparams.h     # the Materials tab's per-zone values and view (plain data)
    src/viewport/scene/materialparams.cpp   # ... its name tables, field setters and comparisons (and the overlay's)
    src/viewport/scene/materialoverlay.h    # a zone's overlays: tattoos, bruises, makeup layered over its maps (plain data)
    src/viewport/scene/overlaycompositor.cpp # ... composited into the images the zone's slots bind
    src/viewport/scene/overlaycompositor.h
    src/viewport/scene/shadowpass.cpp    # the key light's depth-only pass: map, pipeline, frustum fit
    src/viewport/scene/shadowpass.h
    src/viewport/scene/outlinepass.cpp   # the selection-outline mask pass
    src/viewport/scene/outlinepass.h
    src/viewport/scene/lineoverlay.cpp   # skeleton / pin markers / ortho floor line (one line draw)
    src/viewport/scene/lineoverlay.h
    src/viewport/scene/scenepipelines.cpp # the shared layouts, camera/IBL sets, fallbacks, mesh pipelines
    src/viewport/scene/scenepipelines.h
    src/viewport/scene/scene.cpp         # orchestration: construction, models, the record sequence
    src/viewport/scene/scenepicking.cpp  # Scene's CPU picking: lights, models, joints, and the selection a pick makes
    src/viewport/scene/sceneposing.cpp   # Scene's posing forwarders to the active figure
    src/viewport/scene/scenematerials.cpp # the Materials tab's side: the selected object's zones and their materials
    src/viewport/scene/scene.h
    src/viewport/scene/shademode.h # the shader picker's mode table (names + how each mode draws)

    # --- Preferences ---
    src/preferences/preferencesdialog.cpp
    src/preferences/preferencesdialog.h
    src/preferences/preferencespanel.cpp
    src/preferences/preferencespanel.h
    src/preferences/generalpreferencespanel.cpp
    src/preferences/generalpreferencespanel.h
    src/preferences/vrpreferencespanel.cpp  # VR support toggle, headset status, render scale
    src/preferences/vrpreferencespanel.h
    src/preferences/selectionpreferencespanel.cpp # the selection outline's and the joint highlight's color + opacity
    src/preferences/selectionpreferencespanel.h
    src/preferences/assetspreferencespanel.cpp
    src/preferences/assetspreferencespanel.h
    src/preferences/placeholderpreferencespanel.cpp # one class for every not-yet-built tab (Interface/Input/Navigation/System)
    src/preferences/placeholderpreferencespanel.h
    src/preferences/factoryresetpreferencespanel.cpp
    src/preferences/factoryresetpreferencespanel.h

    # --- Data Layer & Storage ---
    src/core/database.cpp
    src/core/database.h
    src/core/assetlibraries.cpp           # the one reader/writer of the AssetLibraries table
    src/core/assetlibraries.h
    src/core/librarypaths.cpp             # resolves the per-user "My PoseStudio Library" (+ hdri/)
    src/core/librarypaths.h
    src/core/hiddennames.h                  # THE rule for what a library browser does not show: a name that begins with a dot
    src/core/importformats.h                # which extensions take which import route (menu, Asset Manager, command line, viewport)
    src/core/runmode.h                      # what kind of run this is: a person's, a scripted one, a probing one
    src/core/preferencesmanager.cpp
    src/core/preferencesmanager.h
    src/core/installping.cpp              # per-launch anonymous install ping (signed; official builds only)
    src/core/installping.h
    src/core/updatecheck.cpp              # startup "newer release on GitHub?" check + Help → Check for Updates
    src/core/updatecheck.h
    src/core/vrsettings.cpp                # VR Mode's preferences + the startup headset report
    src/core/vrsettings.h
    src/core/selectionsettings.cpp         # Preferences → Selection, stored: the two marks' colors and opacities
    src/core/selectionsettings.h
    
    # --- Embedded Resources ---
    resources/resources.qrc   # Injects images, UI styles, and SQL templates
)

# ---------------------------------------------------------------------------------------------
# THE USER MANUAL (Help -> User Manual). The pages are plain Markdown in docs/manual/ (readable on
# GitHub as they are) with a manifest, manual.json, for the table of contents; they are embedded
# under qrc:/manual/ here. The folder is GLOBBED, so a new page needs only its .md file and a line
# in manual.json (CONFIGURE_DEPENDS re-checks the glob at build time). CHANGELOG.md rides along as
# changelog.md - the manual's "What's New" page - so the release notes are never a second copy.
# tools/manual/check_manual.py validates the manifest and the links (see the manual-check test).
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE POSESTUDIO_MANUAL_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/docs/manual/*.md"
    "${CMAKE_SOURCE_DIR}/docs/manual/*.json"
    "${CMAKE_SOURCE_DIR}/docs/manual/images/*")
list(FILTER POSESTUDIO_MANUAL_FILES EXCLUDE REGEX "/README\.md$") # (the folder's own how-to: not a page)
set_source_files_properties("${CMAKE_SOURCE_DIR}/CHANGELOG.md" PROPERTIES QT_RESOURCE_ALIAS "changelog.md")
qt_add_resources(PoseStudio "manual"
    PREFIX "/manual"
    BASE "${CMAKE_SOURCE_DIR}/docs/manual"
    FILES ${POSESTUDIO_MANUAL_FILES} "${CMAKE_SOURCE_DIR}/CHANGELOG.md")

# The Windows executable icon resource (.rc) is understood only by the Windows resource
# compiler. On macOS/Linux (Clang/GCC) CMake has no RC language enabled, so listing it
# unconditionally breaks configuration there — add it only on Windows. The .manifest forces
# the process ANSI code page to UTF-8 (see the comment inside the file): without it, model/
# figure/HDRI imports from folders with non-ASCII characters fail through every narrow-char
# file API. CMake merges it into the executable via mt.exe automatically. The VERSIONINFO
# resource (Explorer's Properties -> Details, Task Manager, AV/SmartScreen heuristics) is a
# configure_file() template so the number is still stamped from project(VERSION) alone: it
# is generated into the build tree and compiled as a second .rc beside the icon's.
if(WIN32)
    string(TIMESTAMP POSESTUDIO_BUILD_YEAR "%Y")
    configure_file(resources/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
    target_sources(PoseStudio PRIVATE
        resources/PoseStudio.rc                   # Injects the .exe icon
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"  # VERSIONINFO, stamped from project(VERSION)
        resources/PoseStudio.manifest             # UTF-8 process code page (non-ASCII import paths)
    )
endif()

# ==============================================================================
# [ INCLUDES & LINKING ]
# ==============================================================================

# Tell the compiler where to look for #include "header.h" files. Every source sub-tree is
# on the path so files can include each other by bare name (e.g. "database.h") regardless of
# which folder either file lives in — keeping the component split a pure organizational move
# that needs no #include rewrites. Add a new src/ subfolder here when you create one.
target_include_directories(PoseStudio PRIVATE
    src
    src/core
    src/shell
    src/help
    src/assets
    src/widgets
    src/properties
    src/render
    src/preferences
    src/viewport
    src/viewport/util
    src/viewport/rendering
    src/viewport/scene
    src/viewport/scene/ik
    src/viewport/import
    src/viewport/import/obj
    src/viewport/import/figure
    src/viewport/import/fbx
    src/viewport/import/gltf
    src/viewport/import/usd
    src/viewport/import/splat
    src/viewport/import/rigged
    src/viewport/vr
    ${ufbx_SOURCE_DIR}                       # so <ufbx.h> resolves
    ${cgltf_SOURCE_DIR}                      # so <cgltf.h> resolves
    ${lightusd_SOURCE_DIR}/src               # so <lightusd.hh> and <tydra/render-data.hh> resolve
    ${vma_SOURCE_DIR}/include                # so <vk_mem_alloc.h> resolves
    ${tinyobjloader_SOURCE_DIR}             # so <tiny_obj_loader.h> resolves
    ${nlohmann_json_SOURCE_DIR}/single_include # so <nlohmann/json.hpp> resolves
    ${stb_SOURCE_DIR}                        # so <stb_image.h> resolves
    ${tinyexr_SOURCE_DIR}                    # so <tinyexr.h> resolves
    ${openxr_SOURCE_DIR}/include             # so <openxr/openxr.h> resolves (the loader target exports it too)
    # (<glm/...> and <miniz.h> come with posestudio_glm and posestudio_miniz, linked below.)
)

# ufbx's single C source, compiled directly into the target (its own CMake is not run). Marked
# as C by its extension, so MSVC uses the C compiler.
target_sources(PoseStudio PRIVATE
    ${ufbx_SOURCE_DIR}/ufbx.c
)

# (GLM's Vulkan conventions are not here: they come with posestudio_glm, for every target alike.)
target_compile_definitions(PoseStudio PRIVATE
    # The app's version string (Constants::APP_VERSION, the title bar, the About overlay, the
    # install ping) is stamped from project(... VERSION) above, so a release bumps one number.
    POSESTUDIO_VERSION="${PROJECT_VERSION}"
    # OpenXR's headers only declare the Vulkan graphics binding structs (XrGraphicsBindingVulkanKHR,
    # the KHR_vulkan_enable entry points) under this define — set project-wide so every VR TU
    # agrees, like GLM's conventions.
    XR_USE_GRAPHICS_API_VULKAN
)

# ------------------------------------------------------------------------------
# Anonymous install ping (src/core/installping.h): on every launch the app tells
# posestudio.io that an installation exists (random install ID + version + OS).
# The HMAC key that signs each ping is NOT in the repository: the release
# workflow passes it from a GitHub Actions secret as -DPOSESTUDIO_PING_KEY=...,
# and a build without one (any local build, any fork) compiles an EMPTY key and
# never pings — the server rejects unsigned pings, so there is nothing to send.
# Keep the key to hex characters: it travels through a compile definition.
# The URL is a cache variable too, so a staging endpoint can be built in.
# ------------------------------------------------------------------------------
set(POSESTUDIO_PING_KEY "" CACHE STRING
    "HMAC key that signs the anonymous install ping (official builds only; empty = never pings)")
set(POSESTUDIO_PING_URL "https://www.posestudio.io/ping" CACHE STRING
    "Endpoint the anonymous install ping is sent to")
mark_as_advanced(POSESTUDIO_PING_KEY POSESTUDIO_PING_URL)
# Scoped to the one translation unit that reads them (not target-wide): the key then reaches
# exactly one compiler invocation, and changing it rebuilds one file rather than the whole app.
set_property(SOURCE src/core/installping.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    POSESTUDIO_PING_KEY="${POSESTUDIO_PING_KEY}"
    POSESTUDIO_PING_URL="${POSESTUDIO_PING_URL}"
)

# Link Qt (Gui is now explicit: QWindow/QVulkanInstance live there) and the Vulkan
# loader. Vulkan::Vulkan also carries the SDK's include path for <vulkan/vulkan.h>.
target_link_libraries(PoseStudio PRIVATE
    posestudio_ik               # the IK core: the Armature + scene/ik (shared with the IK harness)
    posestudio_figureimport     # the native figure importer (shared with the figure and script-link checks)
    posestudio_miniz            # miniz: the figure importer's inflate, tinyexr's, the asset packages' .zip
    posestudio_glm              # GLM with the Vulkan conventions
    Qt6::Widgets
    Qt6::Gui
    Qt6::Sql
    Qt6::Concurrent  # off-thread IBL bake on HDRI switches (QtConcurrent::run + QFutureWatcher)
    Qt6::Network     # the anonymous install ping (src/core/installping.*) and the update check (src/core/updatecheck.*)
    Vulkan::Vulkan
    ${POSESTUDIO_OPENXR_TARGET} # the static OpenXR loader (VR Mode, src/viewport/vr/)
    lightusd_static             # the static USD reader (the USD character importer, src/viewport/import/usd/)
)

# ==============================================================================
# [ SHIPPED ASSET CONTENT ]
# ==============================================================================

# The "Maquettes" library is real, on-disk content (not embedded in the binary via
# resources.qrc) because the Asset Manager browses libraries through QDir/QDirIterator,
# not Qt's resource filesystem. Mirror it next to the executable after every build so
# it's always present at the path database.cpp registers as the built-in library.
#
# We remove the destination first so this is a true MIRROR, not just an overlay: plain
# copy_directory never deletes files that were removed/renamed in source, which would leave
# stale assets (e.g. old thumbnail names) lingering in the deployed library.
add_custom_command(
    TARGET PoseStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:PoseStudio>/Maquettes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/Maquettes"
            "$<TARGET_FILE_DIR:PoseStudio>/Maquettes"
    COMMENT "Mirroring built-in Maquettes library next to the executable"
)

# NOTE: HDRI environment panoramas are user content, not build output. They live in the user's own
# asset library ("My PoseStudio Library/hdri" — see src/core/librarypaths.h), where users add their
# own; packaging/installers are responsible for placing the stock set there. Nothing is mirrored
# next to the executable.

# ==============================================================================
# [ SHADERS, TEST TOOLS, THE RENDER ENGINE ]
# ==============================================================================

# Shader compilation (GLSL -> SPIR-V) and the mirror of the compiled shaders next to the
# executable. After the PoseStudio target, which it attaches to.
include(cmake/Shaders.cmake)

# The test tools under tools/ and their tests. CTest collects the tests of the directory
# enable_testing() is called in, and `ctest` is run from the top of the build tree: so it is
# called here, before the first test is added.
enable_testing()
include(cmake/Tools.cmake)

# THE CYCLES RENDER ENGINE, opt-in (-DPOSESTUDIO_WITH_CYCLES=ON; cmake/CyclesEngine.cmake has the
# whole story): fetched, built as the plugin PoseStudioCycles and mirrored to
# <app folder>/engines/cycles. Off, nothing is fetched and the application is built without a
# render engine — which it says where a render is asked for, and is otherwise complete.
# (After the tools: it makes the render engine's check wait for the engine.)
include(cmake/CyclesEngine.cmake)
if(POSESTUDIO_WITH_CYCLES)
    add_dependencies(PoseStudioRenderCheck PoseStudioCyclesEngine)
    # (The check renders with the engine as built, wherever the tool's own folder is.)
    set_tests_properties(render-engine-check PROPERTIES ENVIRONMENT "POSESTUDIO_CYCLES_DIR=${POSESTUDIO_CYCLES_ENGINE_DIR}")
endif()
