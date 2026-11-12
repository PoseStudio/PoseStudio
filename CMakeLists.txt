# ==============================================================================
# PoseStudio Open Source Project
# File: CMakeLists.txt
# Description: The primary CMake build script.
#              Configures the C++ compiler, locates the Qt6 framework, handles 
#              Qt's meta-object compilation, and links all source files.
# ==============================================================================

cmake_minimum_required(VERSION 3.21)
# C is enabled alongside C++ because a couple of third-party dependencies ship C implementation
# files we compile directly into the target (e.g. miniz's inflate sources for the figure importer).
# Without C in the language list, CMake classifies .c files as non-buildable and silently skips them.
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
# [ 3D VIEWPORT & IMPORT DEPENDENCIES ]
#   Vulkan SDK (required), plus header-only/FetchContent libs: GLM, VMA,
#   tinyobjloader, nlohmann/json, miniz, stb_image, tinyexr
# ==============================================================================

# The viewport renders with Vulkan. This requires the LunarG Vulkan SDK to be
# installed (https://vulkan.lunarg.com): it supplies the headers, the loader, the
# validation layers, and the glslc shader compiler used below. find_package fails
# fast with a clear message if the SDK is missing.
#
# On Windows the installer sets VULKAN_SDK in the user environment, but IDE-launched
# CMake processes sometimes don't inherit it. Fall back to the canonical install root
# so developers don't have to pass -DVulkan_* manually.
if(WIN32 AND NOT DEFINED ENV{VULKAN_SDK})
    file(GLOB _vulkan_sdk_candidates "C:/VulkanSDK/*")
    if(_vulkan_sdk_candidates)
        list(SORT _vulkan_sdk_candidates ORDER DESCENDING)
        list(GET _vulkan_sdk_candidates 0 _vulkan_sdk_root)
        set(ENV{VULKAN_SDK} "${_vulkan_sdk_root}")
    endif()
endif()
find_package(Vulkan REQUIRED)

if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR
        "glslc (the GLSL->SPIR-V compiler) was not found. It ships with the Vulkan SDK; "
        "install the SDK from https://vulkan.lunarg.com and re-run CMake.")
endif()

# GLM (math) and AMD's Vulkan Memory Allocator (VMA) are both header-only. We fetch
# pinned versions at configure time so a fresh clone needs nothing but the Vulkan SDK +
# Qt. We only want their headers, not their own build/install rules — so SOURCE_SUBDIR
# points at a directory that doesn't exist, which makes FetchContent_MakeAvailable
# download the sources but skip add_subdirectory(). We then consume the headers via
# include paths below (version-robust: no dependence on their CMake target names).
include(FetchContent)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    SOURCE_SUBDIR  headers-only-do-not-configure
)
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# tinyobjloader: single-header Wavefront .obj/.mtl parser (used by the OBJ importer). Same
# headers-only treatment as above — we include tiny_obj_loader.h directly and compile its
# implementation in exactly one TU (import/obj/objimporter.cpp), like vma_impl.cpp does for VMA.
FetchContent_Declare(tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG        v2.0.0rc13
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# nlohmann/json: header-only JSON parser used by the native figure importer (import/figure/). The
# figure scene format is JSON, and keeping the parse in a header-only lib (rather than Qt's
# QJsonDocument) is what lets the importer stay Qt-free like the rest of the engine core. Same
# no-configure treatment; we include <nlohmann/json.hpp> from its single_include dir below.
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# miniz: small zlib-compatible (de)compressor. The figure importer needs it to inflate gzip-
# compressed scene files. Unlike the header-only libs above, miniz has C implementation files, so
# we still skip its CMake (no-configure) but compile its split C sources directly into the target
# (see target_sources below) and include <miniz.h> — the same "consume the sources, not their
# build" approach, one level up from header-only.
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.1.2
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# stb: single-header image loaders. We use stb_image's HDR decoder (stbi_loadf) to load the .hdr
# environment panoramas for image-based lighting. Header-only, so compiled in one TU (stb_impl.cpp),
# same treatment as VMA/tinyobjloader. Pinned to a commit (the repo has no release tags).
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        013ac3beddff3dbffafd5177e7972067cd2b5083
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# tinyexr: OpenEXR (.exr) decoder for environment panoramas — the float-image companion to
# stb_image's .hdr path (environmentsource.cpp dispatches on file extension). Header-only for us,
# implementation compiled in one TU (tinyexr_impl.cpp). Its ZIP inflate goes through the same miniz
# sources the figure importer already compiles into the target (TINYEXR_USE_MINIZ, the default) —
# no zlib dependency.
FetchContent_Declare(tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG        v1.0.13
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# ufbx: the FBX reader (binary and ASCII FBX from version 3000, skinning, blend shapes, the whole
# node transform stack, axis and unit conversion, embedded textures). Single-source C (ufbx.c +
# ufbx.h), MIT or public domain; compiled into the target like miniz. Pinned to a release tag.
FetchContent_Declare(ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG        v0.23.1
    GIT_SHALLOW    TRUE
)
# cgltf: the glTF 2.0 reader behind the GLB / glTF character importer (import/gltf/) — a single
# C header (cgltf.h, MIT) that parses the JSON and the GLB container, loads the buffers (external
# .bin files and base64 data URIs alike) and reads accessors of every component type; images stay
# encoded (the Qt layer decodes them, as for every format). Implementation compiled in the ONE TU
# gltffigureimporter.cpp (CGLTF_IMPLEMENTATION), the tinyobjloader treatment. Pinned to a release tag.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  headers-only-do-not-configure
)
FetchContent_MakeAvailable(glm vma tinyobjloader nlohmann_json miniz stb tinyexr ufbx cgltf)

# LightUSD (the library formerly named TinyUSDZ): the USD reader behind the USD character importer
# (import/usd/) — USDA, USDC (crate) and USDZ packages, UsdSkel skeletons and skin weights,
# UsdPreviewSurface materials, no dependency outside its own tree (Apache 2.0; the full OpenUSD
# needs TBB and more, and a clone of this project must build with Qt and the Vulkan SDK alone).
# Like OpenXR below it IS configured and built, as a STATIC library (lightusd_static); its reader
# layer ("Tydra") turns a stage into render-ready meshes, skeletons and materials. Every optional
# part is off: tools, tests, examples, MaterialX, audio, scripting, the glTF/OBJ/FBX bridges.
set(LIGHTUSD_BUILD_TESTS OFF CACHE BOOL "LightUSD: no tests" FORCE)
set(LIGHTUSD_BUILD_EXAMPLES OFF CACHE BOOL "LightUSD: no examples" FORCE)
set(LIGHTUSD_BUILD_TOOLS OFF CACHE BOOL "LightUSD: no tools" FORCE)
set(LIGHTUSD_BUILD_SHARED_LIBS OFF CACHE BOOL "LightUSD: static" FORCE)
set(LIGHTUSD_WITH_TYDRA ON CACHE BOOL "LightUSD: the render-scene layer is what we read" FORCE)
set(LIGHTUSD_WITH_USDMTLX OFF CACHE BOOL "LightUSD: no MaterialX" FORCE)
set(LIGHTUSD_WITH_AUDIO OFF CACHE BOOL "LightUSD: no audio" FORCE)
set(LIGHTUSD_WITH_USD_TO_GLTF OFF CACHE BOOL "LightUSD: no glTF bridge" FORCE)
set(LIGHTUSD_WITH_USDOBJ OFF CACHE BOOL "LightUSD: no OBJ bridge" FORCE)
set(LIGHTUSD_WITH_USDFBX OFF CACHE BOOL "LightUSD: no FBX bridge" FORCE)
set(LIGHTUSD_WITH_USDVOX OFF CACHE BOOL "LightUSD: no voxel bridge" FORCE)
set(LIGHTUSD_WITH_USDVOL ON CACHE BOOL "LightUSD: volumes stay on (its render-scene layer links the VDB reader even when they are off)" FORCE)
set(LIGHTUSD_WITH_MCP_SERVER OFF CACHE BOOL "LightUSD: no MCP server" FORCE)
set(LIGHTUSD_WITH_QJS OFF CACHE BOOL "LightUSD: no JavaScript" FORCE)
set(LIGHTUSD_WITH_WAMR OFF CACHE BOOL "LightUSD: no WebAssembly" FORCE)
set(LIGHTUSD_WITH_GEOGRAM OFF CACHE BOOL "LightUSD: no geogram" FORCE)
set(LIGHTUSD_WITH_PYTHON OFF CACHE BOOL "LightUSD: no Python binding" FORCE)
set(LIGHTUSD_WITH_PXR_COMPAT_API OFF CACHE BOOL "LightUSD: no pxr-compatible API" FORCE)
set(LIGHTUSD_WITH_C_API OFF CACHE BOOL "LightUSD: no C API" FORCE)
set(LIGHTUSD_WITH_EXR OFF CACHE BOOL "LightUSD: no EXR textures (the Qt layer decodes images)" FORCE)
set(LIGHTUSD_WITH_TIFF OFF CACHE BOOL "LightUSD: no TIFF textures" FORCE)
set(LIGHTUSD_USE_CCACHE OFF CACHE BOOL "LightUSD: no ccache" FORCE)
set(LIGHTUSD_ENABLE_THREAD ON CACHE BOOL "LightUSD: its crate reader may use threads (off is for browser builds)" FORCE)
set(LIGHTUSD_NO_WERROR ON CACHE BOOL "LightUSD: a warning in the library must not fail this build" FORCE)
FetchContent_Declare(lightusd
    GIT_REPOSITORY https://github.com/lighttransport/tinyusdz.git
    GIT_TAG        v1.0.0-rc4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lightusd)
# (CMAKE_AUTOMOC is on project-wide and would scan the library's .hh headers for Q_OBJECT — a
# warning per header, nothing more; the library has no Qt in it.)
if(TARGET lightusd_static)
    set_target_properties(lightusd_static PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()

# OpenXR: the loader behind VR Mode (src/viewport/vr/). Unlike the header-only libraries above
# this one IS configured and built — the Khronos OpenXR-SDK repository (the PRE-GENERATED loader
# sources: no Python at configure time, unlike OpenXR-SDK-Source) builds a STATIC openxr_loader
# that links into the executable, so the package carries no extra DLL. The loader finds the
# active runtime (Meta Quest Link, SteamVR, Virtual Desktop, Monado) through the registry /
# ~/.config at xrCreateInstance time; the runtime's own Vulkan requirements are negotiated at
# startup (OpenXrSystem) when VR support is enabled in Preferences.
set(DYNAMIC_LOADER OFF CACHE BOOL "OpenXR: link the loader statically" FORCE)
set(BUILD_LOADER ON CACHE BOOL "OpenXR: the loader is what we need" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "OpenXR: no tests" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "OpenXR: no API layers" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "OpenXR: no conformance tests" FORCE)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.65
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(openxr)
if(TARGET OpenXR::openxr_loader)
    set(POSESTUDIO_OPENXR_TARGET OpenXR::openxr_loader)
else()
    set(POSESTUDIO_OPENXR_TARGET openxr_loader)
endif()

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

    # --- Asset Manager ---
    src/assets/assetmanagerwidget.cpp
    src/assets/assetmanagerwidget.h
    src/assets/assetmanagerwidget_menus.cpp # the same class, second TU: the tree/grid context menus
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
    src/assets/assetscan.cpp              # filesystem scanning: asset/thumbnail pairing, folder hit classification
    src/assets/assetscan.h
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
    src/properties/materialpanel.cpp     # the Materials tab: the selected object's material zones as sliders
    src/properties/materialpanel.h
    src/properties/materialthumbbutton.h # the tab's map thumbnail button (image + frame + hover preview)
    src/properties/overlaystack.cpp      # the tab's Overlays section: the zone's overlay stack as cards
    src/properties/overlaystack.h
    src/properties/propertyrows.cpp      # the row builders every properties tab shares (group, restore button)
    src/properties/propertyrows.h
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
    src/viewport/viewportstrip.cpp        # the floating top-right control strip (pickers, buttons, badge)
    src/viewport/viewportstrip.h
    src/viewport/jointmodebadge.cpp       # the strip's "the mouse is turning Bend / Side-Side / Twist" badge
    src/viewport/jointmodebadge.h
    src/viewport/jointmodalinput.cpp      # the joint mouse mode's input wherever in the app it lands
    src/viewport/jointmodalinput.h
    src/viewport/vulkanwindow.cpp         # VulkanWindow lifecycle (one class, seven TUs - see vulkanwindow.h)
    src/viewport/vulkanwindow.h
    src/viewport/projectstate.cpp      # the .pss document's runtime state (path + dirty, change signal)
    src/viewport/projectstate.h
    src/viewport/vulkanwindow_environment.cpp # the HDRI: default path, off-thread IBL bake, lighting dials
    src/viewport/vulkanwindow_input.cpp   # mouse/wheel/key routing + the object context menu
    src/viewport/vulkanwindow_camera.cpp  # named views, Blender-convention view hotkeys
    src/viewport/vulkanwindow_ik.cpp      # the FBIK drag loop, ground fall, X/Y/Z wheel hold
    src/viewport/vulkanwindow_pose.cpp    # pose utilities, pins, pose files, delete, undo/redo
    src/viewport/vulkanwindow_materials.cpp # the Materials tab's side: the view, the edit bracket, the texture slots (decode + bake + upload)
    src/viewport/vulkanwindow_render.cpp  # the scene captured for a render engine (captureRenderScene)
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
    src/viewport/vulkanwindow_script.cpp  # POSESTUDIO_IK_SCRIPT: the scripted in-app posing test
    src/viewport/ikscript.h               # its state (null unless enabled)
    src/viewport/viewpreset.h             # the named camera views (strip <-> window vocabulary)
    src/viewport/modelimportservice.cpp   # orchestrates parse -> texture decode -> GPU upload
    src/viewport/modelimportservice.h
    src/viewport/figureimportservice.cpp  # figure analogue: figure -> ModelData -> decode -> upload
    src/viewport/figureimportservice.h
    src/viewport/environmentsource.cpp    # decodes .hdr panoramas (stb) -> EnvironmentImage (codec layer)
    src/viewport/environmentsource.h
    src/viewport/import/texturegutters.cpp # UV-island gutter dilation (kills mip-bleed seam lines)
    src/viewport/import/texturegutters.h
    src/viewport/import/aobaker.cpp        # per-vertex ambient-occlusion bake (BVH hemisphere rays)
    src/viewport/import/aobaker.h
    src/viewport/import/tangentgen.cpp     # per-vertex UV tangent generation (Lengyel)
    src/viewport/import/tangentgen.h
    src/viewport/stb_impl.cpp             # single TU that compiles stb_image's implementation
    src/viewport/tinyexr_impl.cpp         # single TU that compiles tinyexr's implementation
    # Layer-free utilities (pure std) shared by rendering/, scene/, import/ and the services:
    src/viewport/util/parallelfor.h       # index-parallel loop for CPU bakes (IBL prefilter, corrective subdivision)
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
    src/viewport/rendering/tracedview.cpp   # the Rendered mode's path-traced picture as a texture + its draw
    src/viewport/rendering/tracedview.h
    src/viewport/rendering/postprocess.h
    src/viewport/rendering/selectionstyle.h # the selection outline's and the joint highlight's color + opacity (Preferences → Selection)
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
    src/viewport/import/splat/splatimporter.cpp     # GAUSSIAN SPLATS: the .ply / .splat reader
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
    # Native figure importer (rigged/morphed characters; JSON scene format via nlohmann + miniz):
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
    src/viewport/scene/scenelight.h      # a light object (point / spot / area / sun) and its shared rules
    src/viewport/scene/renderexport.cpp  # material / camera / lighting translated for a render engine
    src/viewport/scene/renderexport.h
    src/viewport/scene/bonepicker.cpp    # screen-space joint / bone-body picking across figures
    src/viewport/scene/bonepicker.h
    src/viewport/scene/posefile.cpp      # the plain-text .pose file reader/writer
    src/viewport/scene/posefile.h
    src/viewport/scene/projectfile.cpp   # the versioned-JSON .pss project file reader/writer (nlohmann)
    src/viewport/scene/projectfile.h
    src/viewport/scene/armature.cpp   # the runtime skeleton + pose + FK + pins (Vulkan-free)
    src/viewport/scene/armature.h
    src/viewport/scene/armatureik.cpp # the Armature's FBIK drag lifecycle (rig build, begin/drag/settle/end, pins)
    src/viewport/scene/armatureiksolve.cpp # the per-tick whole-body solve: the tick, the stage map, solve + apply
    src/viewport/scene/armatureiksolvedrag.cpp    # ... its stages: what the drag IS (unknowns, trunk policy, foot/knee/elbow drags)
    src/viewport/scene/armatureiksolveposture.cpp # ... the posture model (stiffness + references, the spine's coupling)
    src/viewport/scene/armatureiksolverows.cpp    # ... the rows (pins, the foot contact model, the cursor, balance, floor + volumes)
    src/viewport/scene/armatureiksolvestate.h     # ... the per-tick scratch the stages share
    src/viewport/scene/armatureiksolvetuning.h    # ... every tuning constant, with its reasoning
    src/viewport/scene/armatureikpost.cpp   # the drag tick's post-steps: the idle arms' hang, the head's righting
    src/viewport/scene/armatureikdigit.cpp  # the bone classes (body / face rig / digit) and the digit drag
    src/viewport/scene/armaturelanding.cpp  # the landing bounce: a figure dropped onto her feet absorbs the landing
    src/viewport/scene/armaturehangfeet.cpp # the hanging feet come down flat: a lifted figure's feet, back on the floor
    src/viewport/scene/armaturebreasts.cpp     # THE SOFT BREASTS: the pectorals hung by gravity, by their size; their bounce
    src/viewport/scene/armaturebreastfield.cpp # THE BONELESS BREAST: on a rig with no breast bone, the chest's own skin moved as a delta field
    src/viewport/scene/armaturebuttocks.cpp    # THE SOFT BUTTOCKS: the glutes' skin as a delta field the GPU blends, jiggling with the hips' movement
    src/viewport/scene/softbodysettings.h
    src/viewport/scene/armatureikfigure.cpp # the figure move: Ctrl + the body itself moves the figure as she is posed
    src/viewport/scene/armaturedials.cpp    # the transform dials: a joint's rotation as Bend Forward / Bend Sideways / Twist, 0-100
    src/viewport/scene/jointtransform.h     # ... and what the Transform tab shows of them (plain data)
    src/viewport/scene/materialparams.h     # the Materials tab's per-zone values and view (plain data)
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
    src/viewport/scene/sceneposing.cpp   # Scene's posing forwarders to the active figure
    src/viewport/scene/scenematerials.cpp # the Materials tab's side: the selected object's zones and their materials
    src/viewport/scene/scene.h
    src/viewport/scene/shademode.h # the shader picker's mode table (names + how each mode draws)

    # Full-body IK (Qt-free: std + GLM). See the FBIK section in CLAUDE.md.
    src/viewport/scene/ik/ikmath.cpp          # shortest-arc / signed-angle / Euler decomposition
    src/viewport/scene/ik/ikmath.h
    src/viewport/scene/ik/skeletongraph.cpp   # undirected joint graph, dynamically re-rootable
    src/viewport/scene/ik/bonealiases.cpp     # the bone-alias table the two posing test tools share
    src/viewport/scene/ik/bonealiases.h
    src/viewport/scene/ik/rigroles.cpp        # the rig standard's roles: what each bone is, by structure
    src/viewport/scene/ik/rigroles.h
    src/viewport/scene/ik/namerules.cpp       # the one reading of a bone name: its side token, a digit's token, the twist and segment words
    src/viewport/scene/ik/namerules.h
    src/viewport/scene/ik/skeletongraph.h
    src/viewport/scene/ik/balancecontroller.cpp # CoM + support polygon + pelvis correction
    src/viewport/scene/ik/balancecontroller.h
    src/viewport/scene/ik/ikrig.cpp           # the drag's POLICY per figure: rig build, body volumes, shared helpers
    src/viewport/scene/ik/ikrig.h
    src/viewport/scene/ik/ikrig_constants.h   # the rig's tuning constants (private to the ikrig*.cpp)
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
    src/core/importformats.h                # which extensions take which import route (menu, Asset Manager, command line, viewport)
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
    ${glm_SOURCE_DIR}                        # so <glm/glm.hpp> resolves
    ${vma_SOURCE_DIR}/include                # so <vk_mem_alloc.h> resolves
    ${tinyobjloader_SOURCE_DIR}             # so <tiny_obj_loader.h> resolves
    ${nlohmann_json_SOURCE_DIR}/single_include # so <nlohmann/json.hpp> resolves
    ${miniz_SOURCE_DIR}                      # so <miniz.h> resolves
    ${CMAKE_SOURCE_DIR}/cmake/miniz-shim     # supplies miniz_export.h (miniz's CMake is skipped)
    ${stb_SOURCE_DIR}                        # so <stb_image.h> resolves
    ${tinyexr_SOURCE_DIR}                    # so <tinyexr.h> resolves
    ${openxr_SOURCE_DIR}/include             # so <openxr/openxr.h> resolves (the loader target exports it too)
)

# miniz's C implementation, compiled directly into the target (its split source layout: core +
# inflate + deflate + zip). We only need inflate for the figure importer, but compiling the whole
# small set is link-safe and avoids feature-macro fiddling. Marked as C so MSVC uses the C compiler.
target_sources(PoseStudio PRIVATE
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_zip.c
    ${ufbx_SOURCE_DIR}/ufbx.c
)

# GLM must be told, project-wide, to match Vulkan conventions: angles in radians and a
# [0,1] clip-space depth range (vs OpenGL's [-1,1]). Set once here so every translation
# unit agrees — mismatched GLM flags across files are a classic silent-corruption bug.
target_compile_definitions(PoseStudio PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    # The app's version string (Constants::APP_VERSION, the title bar, the About overlay, the
    # install ping) is stamped from project(... VERSION) above, so a release bumps one number.
    POSESTUDIO_VERSION="${PROJECT_VERSION}"
    # OpenXR's headers only declare the Vulkan graphics binding structs (XrGraphicsBindingVulkanKHR,
    # the KHR_vulkan_enable entry points) under this define — set project-wide so every VR TU
    # agrees, like the GLM flags above.
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
# [ IK HARNESS (tools/ikharness) ]
# ==============================================================================

# The full-body-IK harness: unit tests for the IK math/constraints plus a drag-loop simulator
# that runs the REAL engine loop (the Armature — solve -> extract -> FK, through the same cursor
# filter and tick order as the viewport) on a dumped figure skeleton and gates the measured
# behaviour. Built by default so it can't rot, and registered with CTest; it links only the
# Qt-free, Vulkan-free scene/ik + armature sources (no Qt, no Vulkan). The real-figure phases
# need a skeleton dump (run the app once with POSESTUDIO_DUMP_SKELETON=<path>; the dump is a
# vendor's rig and stays out of the repo) — pass it on the command line or in
# POSESTUDIO_IK_SKELETON; without one those phases are skipped and the unit tests still run.
add_executable(PoseStudioIkHarness
    tools/ikharness/ikharness.cpp   # argument parsing, --custom, main
    tools/ikharness/report.cpp      # gates + phase printing
    tools/ikharness/report.h
    tools/ikharness/simulator.cpp   # the drag-loop simulator over the real Armature
    tools/ikharness/simulator.h
    tools/ikharness/unitphases.cpp  # skeleton-free unit phases
    tools/ikharness/unitphases.h
    tools/ikharness/realphases.cpp  # the gated real-figure phases
    tools/ikharness/realphases.h
    src/viewport/scene/armature.cpp
    src/viewport/scene/armatureik.cpp
    src/viewport/scene/armatureiksolve.cpp
    src/viewport/scene/armatureiksolvedrag.cpp
    src/viewport/scene/armatureiksolveposture.cpp
    src/viewport/scene/armatureiksolverows.cpp
    src/viewport/scene/armatureikpost.cpp
    src/viewport/scene/armatureikdigit.cpp
    src/viewport/scene/armaturelanding.cpp
    src/viewport/scene/armaturehangfeet.cpp
    src/viewport/scene/armaturebreasts.cpp
    src/viewport/scene/armaturebreastfield.cpp
    src/viewport/scene/armaturebuttocks.cpp
    src/viewport/scene/armatureikfigure.cpp
    src/viewport/scene/armaturedials.cpp
    src/viewport/scene/ik/ikmath.cpp
    src/viewport/scene/ik/skeletongraph.cpp
    src/viewport/scene/ik/bonealiases.cpp
    src/viewport/scene/ik/rigroles.cpp
    src/viewport/scene/ik/namerules.cpp
    src/viewport/scene/ik/balancecontroller.cpp
    src/viewport/scene/ik/ikrig.cpp
    src/viewport/scene/ik/ikrigdrag.cpp
    src/viewport/scene/ik/ikrigpolicy.cpp
    src/viewport/scene/ik/ikrigstepping.cpp
    src/viewport/scene/ik/ikrigcontacts.cpp
    src/viewport/scene/ik/ikrigvolumes.cpp
    src/viewport/scene/ik/bodymesh.cpp
    src/viewport/scene/ik/jointsolver.cpp
)
target_include_directories(PoseStudioIkHarness PRIVATE
    tools/ikharness
    src/viewport/scene
    src/viewport/scene/ik
    ${glm_SOURCE_DIR}
)
target_compile_definitions(PoseStudioIkHarness PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)
enable_testing()
add_test(NAME ik-harness COMMAND PoseStudioIkHarness)

# The .pss project codec's round-trip guard (tools/projectfile): writes a fully-populated
# ProjectDocument and reads it back field-by-field, then the reader's rejection cases and a
# forward-compatibility document. Links only scene/projectfile.cpp — no Qt, no Vulkan; its
# scratch files live in the OS temp dir, so it runs on every platform the app builds for.
add_executable(PoseStudioProjectFileCheck
    tools/projectfile/roundtrip.cpp
    src/viewport/scene/projectfile.cpp
)
target_include_directories(PoseStudioProjectFileCheck PRIVATE
    src/viewport/scene
    ${glm_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/single_include
)
add_test(NAME project-file-check COMMAND PoseStudioProjectFileCheck)

# The overlay compositor's guard (tools/overlay): the blend modes and bitmap adjustments on known
# colors, the crop of an overlay image, and whole composites — color over a skin map, gloss into
# the roughness map, a height added about mid grey, a normal blended, a stack's order, a
# mismatched overlay size, a zone without the map. scene/overlaycompositor.cpp alone; the Vulkan
# HEADERS only (modeldata.h's vertex layout names them) — no Qt, no device.
add_executable(PoseStudioOverlayCheck
    tools/overlay/check_overlay.cpp
    src/viewport/scene/overlaycompositor.cpp
)
target_include_directories(PoseStudioOverlayCheck PRIVATE
    src/viewport/scene
    src/viewport/import
    src/viewport/util
    ${glm_SOURCE_DIR}
)
target_compile_definitions(PoseStudioOverlayCheck PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)
target_link_libraries(PoseStudioOverlayCheck PRIVATE Vulkan::Headers)
add_test(NAME overlay-check COMMAND PoseStudioOverlayCheck)

# The guard of a render's Gaussian splat composite (tools/splatcomposite): the display transform
# and its inverse, the capture coming out of a render as it went in, the backdrop behind it, the
# scene laid over it in linear light (a figure's edge, the floor's shadow), the front of the
# capture over the scene, a transparent picture, the linear picture an OpenEXR file is saved
# from. render/splatcomposite.cpp alone — no Qt, no engine, no device.
add_executable(PoseStudioSplatCompositeCheck
    tools/splatcomposite/check_composite.cpp
    src/render/splatcomposite.cpp
)
target_include_directories(PoseStudioSplatCompositeCheck PRIVATE
    src/render
    src/viewport/util
)
add_test(NAME splat-composite-check COMMAND PoseStudioSplatCompositeCheck)

# The script-link reader's guard (tools/scriptlink): writes synthetic content-library scripts —
# folder links, file loaders, web links, chains, programs, an encrypted one — and checks where
# AssetScriptLink::follow() leads, and which folders loneFigureIn() opens. Qt6::Core plus the
# figure importer's document sniff (figureutils + figuredocument + miniz); no widgets, no Vulkan.
add_executable(PoseStudioScriptLinkCheck
    tools/scriptlink/check_links.cpp
    src/assets/assetscriptlink.cpp
    src/viewport/import/figure/figureutils.cpp
    src/viewport/import/figure/figuredocument.cpp
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_zip.c
)
target_include_directories(PoseStudioScriptLinkCheck PRIVATE
    src/assets
    src/core
    src/viewport/import/figure
    ${glm_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/single_include
    ${miniz_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cmake/miniz-shim
)
target_compile_definitions(PoseStudioScriptLinkCheck PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)
target_link_libraries(PoseStudioScriptLinkCheck PRIVATE Qt6::Core)
add_test(NAME script-link-check COMMAND PoseStudioScriptLinkCheck)

# The native figure importer's guard and report tool (tools/figurecheck): with no arguments it
# writes a synthetic content library — a base figure, shape morphs, controls, a two-figure scene —
# into the OS temp folder and checks what a character preset comes out as (the rules that turn "a
# preset dials a control" into "the cage has this shape" and "this zone has that material");
# with --report <preset> it prints what a REAL preset imports as, for a before/after diff across a
# content library. The whole Qt-free importer (import/figure) + miniz; the Vulkan HEADERS only
# (vertex.h names the vertex-input structs) — no Qt, no device.
file(GLOB FIGURE_CHECK_IMPORTER CONFIGURE_DEPENDS src/viewport/import/figure/*.cpp)
add_executable(PoseStudioFigureCheck
    tools/figurecheck/check_figure.cpp
    ${FIGURE_CHECK_IMPORTER}
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_zip.c
)
target_include_directories(PoseStudioFigureCheck PRIVATE
    src/viewport/import
    src/viewport/import/figure
    src/viewport/util
    ${glm_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/single_include
    ${miniz_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cmake/miniz-shim
)
target_compile_definitions(PoseStudioFigureCheck PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)
target_link_libraries(PoseStudioFigureCheck PRIVATE Vulkan::Headers)
add_test(NAME figure-shape-check COMMAND PoseStudioFigureCheck)

# The render engine's guard (tools/rendercheck): loads the engine library the way the application
# does (src/render/renderenginelibrary) and renders a small scene made in the tool through the
# engine interface, checking the conventions the interface states and start / status / cancel.
# Qt6::Core + Gui (QLibrary, QImage for --out); no Vulkan, no figure. WITHOUT an engine — a build
# made without Cycles — it reports that and passes: the application is complete without one.
add_executable(PoseStudioRenderCheck
    tools/rendercheck/check_render.cpp
    src/render/renderenginelibrary.cpp
)
target_include_directories(PoseStudioRenderCheck PRIVATE src/render)
target_link_libraries(PoseStudioRenderCheck PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME render-engine-check COMMAND PoseStudioRenderCheck)

# THE CYCLES RENDER ENGINE, opt-in (-DPOSESTUDIO_WITH_CYCLES=ON; cmake/CyclesEngine.cmake has the
# whole story): fetched, built as the plugin PoseStudioCycles and mirrored to
# <app folder>/engines/cycles. Off, nothing is fetched and the application is built without a
# render engine — which it says where a render is asked for, and is otherwise complete.
include(cmake/CyclesEngine.cmake)
if(POSESTUDIO_WITH_CYCLES)
    add_dependencies(PoseStudioRenderCheck PoseStudioCyclesEngine)
    # (The check renders with the engine as built, wherever the tool's own folder is.)
    set_tests_properties(render-engine-check PROPERTIES ENVIRONMENT "POSESTUDIO_CYCLES_DIR=${POSESTUDIO_CYCLES_ENGINE_DIR}")
endif()

# .pss save/restore diagnostic (see tools/projectfile/probe.cpp): runs the real import + scene
# classes headless and prints which document fields diverge across a save/load round-trip. OPT-IN:
# it compiles the whole engine a second time, which no ordinary build (or the release workflow)
# should pay for.
option(POSESTUDIO_BUILD_PSS_PROBE "Build the .pss save/restore diagnostic (tools/projectfile/probe.cpp)" OFF)
if(POSESTUDIO_BUILD_PSS_PROBE)
file(GLOB PSS_PROBE_RENDERING src/viewport/rendering/*.cpp)
file(GLOB PSS_PROBE_SCENE src/viewport/scene/*.cpp src/viewport/scene/ik/*.cpp)
file(GLOB PSS_PROBE_FIGURE src/viewport/import/figure/*.cpp)
add_executable(PoseStudioPssProbe
    tools/projectfile/probe.cpp
    src/core/preferencesmanager.cpp
    src/core/database.cpp
    src/viewport/figureimportservice.cpp
    src/viewport/modelimportservice.cpp
    src/viewport/import/aobaker.cpp
    src/viewport/import/tangentgen.cpp
    src/viewport/import/texturegutters.cpp
    src/viewport/import/importerregistry.cpp
    src/viewport/import/specgloss.cpp
    src/viewport/import/obj/objimporter.cpp
    ${PSS_PROBE_FIGURE}
    ${PSS_PROBE_RENDERING}
    ${PSS_PROBE_SCENE}
)
target_sources(PoseStudioPssProbe PRIVATE
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_zip.c
)
target_include_directories(PoseStudioPssProbe PRIVATE
    src
    src/core
    src/render                 # renderengineapi.h (scene/renderexport.cpp names its types)
    src/viewport
    src/viewport/util
    src/viewport/rendering
    src/viewport/scene
    src/viewport/scene/ik
    src/viewport/import
    src/viewport/import/obj
    src/viewport/import/figure
    ${glm_SOURCE_DIR}
    ${vma_SOURCE_DIR}/include
    ${tinyobjloader_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/single_include
    ${miniz_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cmake/miniz-shim
    ${stb_SOURCE_DIR}
    ${tinyexr_SOURCE_DIR}
)
target_compile_definitions(PoseStudioPssProbe PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    POSESTUDIO_VERSION="0.0.0"
)
target_link_libraries(PoseStudioPssProbe PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Sql
    Qt6::Concurrent
    Vulkan::Vulkan
)
add_dependencies(PoseStudioPssProbe PoseStudioShaders)
endif()

# The user manual's checker (docs/manual/README.md): the manifest, the pages, every link, and a
# warning per menu action the manual does not mention. Needs only Python.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME manual-check
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/manual/check_manual.py"
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
if(DEFINED ENV{POSESTUDIO_IK_SKELETON})
    set_tests_properties(ik-harness PROPERTIES
        ENVIRONMENT "POSESTUDIO_IK_SKELETON=$ENV{POSESTUDIO_IK_SKELETON}")
endif()

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
# [ SHADER COMPILATION (GLSL -> SPIR-V) ]
# ==============================================================================

# Vulkan consumes SPIR-V, not GLSL, so every shader is compiled by glslc at build time.
# The compiled .spv files are written to the build tree, then mirrored next to the
# executable (same approach as Maquettes) where VulkanRenderer loads them at runtime
# from <applicationDirPath>/shaders. Add new shaders to this list and they're picked up
# automatically; editing a .vert/.frag re-triggers just that compile via the DEPENDS.
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
    "${SHADER_INCLUDE_DIR}/color.glsl"       # ACES tonemap + the selection accent color
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

# Mirror the compiled shaders next to the executable on every build.
#
# On multi-config generators (Visual Studio, Xcode) the executable lands in a
# per-config subdirectory (e.g. build/Release/), which differs from where the
# shaders are compiled (${CMAKE_BINARY_DIR}/shaders), so they must be mirrored.
# On single-config generators (Ninja, Unix Makefiles) the executable lives in
# the build root, so $<TARGET_FILE_DIR:PoseStudio>/shaders IS SHADER_BINARY_DIR:
# the unconditional "rm -rf <dir> && copy_directory <dir> <dir>" would delete the
# freshly compiled SPIR-V and then fail copying a directory onto itself. Only
# mirror when the destination actually differs from the compile output.
get_property(_posestudio_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_posestudio_multi_config)
    add_custom_command(
        TARGET PoseStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
                "$<TARGET_FILE_DIR:PoseStudio>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${SHADER_BINARY_DIR}"
                "$<TARGET_FILE_DIR:PoseStudio>/shaders"
        COMMENT "Mirroring compiled SPIR-V shaders next to the executable"
    )
endif()