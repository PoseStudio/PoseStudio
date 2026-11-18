# ==============================================================================
# PoseStudio Open Source Project
# File: cmake/Dependencies.cmake
# Description: What the build takes from outside the repository (the 3D viewport's and the
#              importers' dependencies): the Vulkan SDK, which must be installed, plus the
#              libraries fetched at configure time, each at a pinned tag or commit —
#              consumed as headers or sources: GLM, VMA, tinyobjloader, nlohmann/json,
#              miniz, stb_image, tinyexr, ufbx, cgltf; configured and built as static
#              libraries: LightUSD and the OpenXR loader.
#
#              Included by the root CMakeLists.txt, in its scope. It leaves behind what the
#              targets there name: the Vulkan:: targets and Vulkan_GLSLC_EXECUTABLE, each
#              fetched library's <name>_SOURCE_DIR, the lightusd_static target and
#              POSESTUDIO_OPENXR_TARGET.
# ==============================================================================

# The viewport renders with Vulkan. This requires the LunarG Vulkan SDK to be
# installed (https://vulkan.lunarg.com): it supplies the headers, the loader, the
# validation layers, and the glslc shader compiler cmake/Shaders.cmake uses. find_package
# fails fast with a clear message if the SDK is missing.
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
# include paths, in the root CMakeLists.txt (version-robust: no dependence on their CMake
# target names).
#
# Every dependency pinned to a release TAG is fetched shallow (GIT_SHALLOW: the tagged tree
# alone, not the repository's history, which for some of them is hundreds of megabytes). stb
# is pinned to a commit hash, which a shallow clone cannot ask for by name, so it is fetched whole.
include(FetchContent)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  headers-only-do-not-configure
)
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# tinyobjloader: single-header Wavefront .obj/.mtl parser (used by the OBJ importer). Same
# headers-only treatment as above — we include tiny_obj_loader.h directly and compile its
# implementation in exactly one TU (import/obj/objimporter.cpp), like vma_impl.cpp does for VMA.
FetchContent_Declare(tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG        v2.0.0rc13
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# nlohmann/json: header-only JSON parser used by the native figure importer (import/figure/). The
# figure scene format is JSON, and keeping the parse in a header-only lib (rather than Qt's
# QJsonDocument) is what lets the importer stay Qt-free like the rest of the engine core. Same
# no-configure treatment; we include <nlohmann/json.hpp> from its single_include dir.
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  headers-only-do-not-configure
)
# miniz: small zlib-compatible (de)compressor. The figure importer needs it to inflate gzip-
# compressed scene files. Unlike the header-only libs above, miniz has C implementation files, so
# we still skip its CMake (no-configure) but compile its split C sources ourselves (the library
# posestudio_miniz, in the root CMakeLists.txt) and include <miniz.h> — the same "consume the sources, not their
# build" approach, one level up from header-only.
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.1.2
    GIT_SHALLOW    TRUE
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
# the figure importer uses (posestudio_miniz; TINYEXR_USE_MINIZ, the default) — no zlib dependency.
FetchContent_Declare(tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG        v1.0.13
    GIT_SHALLOW    TRUE
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
