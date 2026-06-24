# ==============================================================================
# PoseStudio Open Source Project
# File: CMakeLists.txt
# Description: The primary CMake build script.
#              Configures the C++ compiler, locates the Qt6 framework, handles 
#              Qt's meta-object compilation, and links all source files.
# ==============================================================================

cmake_minimum_required(VERSION 3.21)
project(PoseStudio VERSION 1.0 LANGUAGES CXX)

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
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

# Automatically invoke Qt's pre-processors during the build:
set(CMAKE_AUTOMOC ON) # Meta-Object Compiler (Handles Signals & Slots)
set(CMAKE_AUTOUIC ON) # User Interface Compiler (Handles .ui XML files)
set(CMAKE_AUTORCC ON) # Resource Compiler (Handles .qrc binary embedding)

# ==============================================================================
# [ 3D VIEWPORT DEPENDENCIES (Vulkan, GLM, VMA) ]
# ==============================================================================

# The viewport renders with Vulkan. This requires the LunarG Vulkan SDK to be
# installed (https://vulkan.lunarg.com): it supplies the headers, the loader, the
# validation layers, and the glslc shader compiler used below. find_package fails
# fast with a clear message if the SDK is missing.
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
# implementation in exactly one TU (scene/objloader.cpp), like vma_impl.cpp does for VMA.
FetchContent_Declare(tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG        v2.0.0rc13
    SOURCE_SUBDIR  headers-only-do-not-configure
)
FetchContent_MakeAvailable(glm vma tinyobjloader)

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
    src/shell/splashoverlay.h
    src/shell/appproxystyle.h

    # --- Asset Manager ---
    src/assets/assetmanagerwidget.cpp
    src/assets/assetmanagerwidget.h

    # --- 3D Viewport (Vulkan) ---
    # Qt-facing layer (the only viewport files that touch Qt):
    src/viewport/viewportwidget.cpp
    src/viewport/viewportwidget.h
    src/viewport/vulkanwindow.cpp
    src/viewport/vulkanwindow.h
    # Rendering core (pure Vulkan + std + GLM, no Qt):
    src/viewport/rendering/vulkancommon.h
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
    src/viewport/rendering/vulkancommands.cpp
    src/viewport/rendering/vulkancommands.h
    src/viewport/rendering/vulkanrenderer.cpp
    src/viewport/rendering/vulkanrenderer.h
    src/viewport/rendering/vma_impl.cpp
    # Scene:
    src/viewport/scene/camera.cpp
    src/viewport/scene/camera.h
    src/viewport/scene/grid.cpp
    src/viewport/scene/grid.h
    src/viewport/scene/vertex.h
    src/viewport/scene/objloader.cpp
    src/viewport/scene/objloader.h
    src/viewport/scene/mesh.cpp
    src/viewport/scene/mesh.h
    src/viewport/scene/scene.cpp
    src/viewport/scene/scene.h

    # --- Preferences ---
    src/preferences/preferencesdialog.cpp
    src/preferences/preferencesdialog.h
    src/preferences/preferencespanel.cpp
    src/preferences/preferencespanel.h
    src/preferences/generalpreferencespanel.cpp
    src/preferences/generalpreferencespanel.h
    src/preferences/interfacepreferencespanel.cpp
    src/preferences/interfacepreferencespanel.h
    src/preferences/assetspreferencespanel.cpp
    src/preferences/assetspreferencespanel.h
    src/preferences/inputpreferencespanel.cpp
    src/preferences/inputpreferencespanel.h
    src/preferences/navigationpreferencespanel.cpp
    src/preferences/navigationpreferencespanel.h
    src/preferences/systempreferencespanel.cpp
    src/preferences/systempreferencespanel.h
    src/preferences/factoryresetpreferencespanel.cpp
    src/preferences/factoryresetpreferencespanel.h

    # --- Data Layer & Storage ---
    src/core/database.cpp
    src/core/database.h
    src/core/preferencesmanager.cpp
    src/core/preferencesmanager.h
    
    # --- Embedded Resources ---
    resources/resources.qrc   # Injects images, UI styles, and SQL templates
)

# The Windows executable icon resource (.rc) is understood only by the Windows resource
# compiler. On macOS/Linux (Clang/GCC) CMake has no RC language enabled, so listing it
# unconditionally breaks configuration there — add it only on Windows.
if(WIN32)
    target_sources(PoseStudio PRIVATE resources/PoseStudio.rc) # Injects the .exe icon
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
    src/assets
    src/preferences
    src/viewport
    src/viewport/rendering
    src/viewport/scene
    ${glm_SOURCE_DIR}              # so <glm/glm.hpp> resolves
    ${vma_SOURCE_DIR}/include      # so <vk_mem_alloc.h> resolves
    ${tinyobjloader_SOURCE_DIR}    # so <tiny_obj_loader.h> resolves
)

# GLM must be told, project-wide, to match Vulkan conventions: angles in radians and a
# [0,1] clip-space depth range (vs OpenGL's [-1,1]). Set once here so every translation
# unit agrees — mismatched GLM flags across files are a classic silent-corruption bug.
target_compile_definitions(PoseStudio PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

# Link Qt (Gui is now explicit: QWindow/QVulkanInstance live there) and the Vulkan
# loader. Vulkan::Vulkan also carries the SDK's include path for <vulkan/vulkan.h>.
target_link_libraries(PoseStudio PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Sql
    Vulkan::Vulkan
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

# ==============================================================================
# [ SHADER COMPILATION (GLSL -> SPIR-V) ]
# ==============================================================================

# Vulkan consumes SPIR-V, not GLSL, so every shader is compiled by glslc at build time.
# The compiled .spv files are written to the build tree, then mirrored next to the
# executable (same approach as Maquettes) where VulkanRenderer loads them at runtime
# from <applicationDirPath>/shaders. Add new shaders to this list and they're picked up
# automatically; editing a .vert/.frag re-triggers just that compile via the DEPENDS.
set(SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/viewport/shaders")
set(SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_BINARY_DIR}")

set(SHADER_SOURCES
    grid.vert
    grid.frag
    mesh.vert
    mesh.frag
)

set(COMPILED_SPV_FILES "")
foreach(shader ${SHADER_SOURCES})
    set(spv "${SHADER_BINARY_DIR}/${shader}.spv")
    add_custom_command(
        OUTPUT "${spv}"
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} "${SHADER_SOURCE_DIR}/${shader}" -o "${spv}"
        DEPENDS "${SHADER_SOURCE_DIR}/${shader}"
        COMMENT "Compiling shader ${shader} -> ${shader}.spv"
        VERBATIM
    )
    list(APPEND COMPILED_SPV_FILES "${spv}")
endforeach()

add_custom_target(PoseStudioShaders DEPENDS ${COMPILED_SPV_FILES})
add_dependencies(PoseStudio PoseStudioShaders)

# Mirror the compiled shaders next to the executable on every build.
add_custom_command(
    TARGET PoseStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:PoseStudio>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SHADER_BINARY_DIR}"
            "$<TARGET_FILE_DIR:PoseStudio>/shaders"
    COMMENT "Mirroring compiled SPIR-V shaders next to the executable"
)