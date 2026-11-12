# THE CYCLES ENGINE BRIDGE — PoseStudioCycles, the shared library PoseStudio loads at runtime
# (src/render/renderengineapi.h is the contract).
#
# This file is NOT part of PoseStudio's own CMake project. It is processed from INSIDE the Cycles
# build tree, in the scope of Cycles's src/CMakeLists.txt (cmake/CyclesEngine.cmake fetches
# Cycles and adds this directory there, the way Cycles adds its own src/app): the bridge is
# compiled with exactly the definitions, flags and include paths Cycles compiles itself with.
# That is the point — Cycles's classes change layout with its WITH_* definitions, and a bridge
# built with its own guess of them would corrupt memory quietly.
#
# Given by the including scope: POSESTUDIO_RENDER_API_DIR (the folder of renderengineapi.h).

set(SRC
  cyclesengine.cpp
  memoryimage.cpp
  memoryimage.h
  scenebuilder.cpp
  scenebuilder.h
  ${POSESTUDIO_RENDER_API_DIR}/renderengineapi.h
)

# The libraries src/app links for the `cycles` executable.
set(LIB
  cycles_device
  cycles_kernel
  cycles_scene
  cycles_session
  cycles_bvh
  cycles_subd
  cycles_graph
  cycles_util
  extern_sky
)
if(WITH_CYCLES_OSL)
  list(APPEND LIB cycles_kernel_osl)
endif()
cycles_external_libraries_append(LIB)

add_library(PoseStudioCycles SHARED ${SRC})
target_include_directories(PoseStudioCycles PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..   # (unused in the Cycles tree; harmless)
  ${PROJECT_SOURCE_DIR}/src        # Cycles's own headers: "scene/scene.h", "session/session.h"
  ${POSESTUDIO_RENDER_API_DIR}
)
target_link_libraries(PoseStudioCycles PRIVATE ${LIB})

# The one exported symbol (posestudio_create_render_engine).
include(GenerateExportHeader)
generate_export_header(PoseStudioCycles
  BASE_NAME posestudiocycles
  EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/posestudiocycles_export.h)
target_include_directories(PoseStudioCycles PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(PoseStudioCycles PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# Next to the `cycles` executable: the library, and (cycles_install_libraries) the precompiled
# runtime libraries it loads; the kernels and shaders are installed by Cycles's own rules.
cycles_install_libraries(PoseStudioCycles)
install(TARGETS PoseStudioCycles
  RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX})
