# =============================================================================
# THE CYCLES RENDER ENGINE — an OPT-IN part of the build (-DPOSESTUDIO_WITH_CYCLES=ON)
# =============================================================================
#
# PoseStudio renders final images with Cycles (Blender's path tracer, Apache 2.0). The engine is
# NOT linked into PoseStudio: it is a plugin — the shared library PoseStudioCycles, built from
# engines/cycles/ and loaded at run time from <app folder>/engines/cycles (src/render/
# renderenginelibrary.cpp) behind the plain interface of src/render/renderengineapi.h. An
# application built without it is whole: Render → Render Image says that this build has no
# engine, and nothing else changes. That is why the option is OFF by default — a fresh clone
# still needs only Qt + the Vulkan SDK.
#
# With the option ON, configuring fetches (into the FetchContent folder, build/_deps):
#   cycles-src        Cycles at the pinned tag (projects.blender.org/blender/cycles), and under
#                     its lib/ a SPARSE, git-lfs checkout of Blender's precompiled libraries for
#                     this platform — only the dozen-odd folders Cycles is built with here
#                     (about 0.55 GB of the repository's ~48). NEEDS git-lfs.
#   cuda<major>       (POSESTUDIO_CYCLES_CUDA) NVIDIA's CUDA compiler as REDISTRIBUTABLE ARCHIVES
#                     — nvcc, libnvvm, the CRT headers and cudart — unpacked into one toolkit-
#                     shaped folder: no installer, no admin rights. Only used to compile Cycles's
#                     GPU kernels at build time; nothing of it ships.
#   optix             (POSESTUDIO_CYCLES_OPTIX) the OptiX headers (github.com/NVIDIA/optix-dev).
#                     NOTE: they are under NVIDIA's own SDK license, not an open-source one.
#                     PoseStudio's license permits the combination (LICENSE-EXCEPTION.txt, a GPL
#                     section 7 additional permission); NVIDIA's terms are the distributor's to
#                     meet (docs/RELEASING.md).
# ... patches ONE block into Cycles's src/CMakeLists.txt (it adds engines/cycles as a
# subdirectory THERE, so the bridge is compiled with exactly Cycles's own definitions and flags:
# its classes change layout with its WITH_* switches), configures Cycles into cycles-build, and
# adds the target PoseStudioCyclesEngine, which builds the engine (always in Release: the
# interface is plain data, a Release engine serves a Debug application) and mirrors it —
# library, runtime DLLs, GPU kernels — next to the application as engines/cycles/.
#
# What is built of Cycles: the CPU device (Embree BVH), the denoiser (OpenImageDenoise), SVM
# shading, and the GPU devices asked for. Left out: OSL, OpenVDB / NanoVDB volumes, OpenSubdiv,
# Alembic, USD / Hydra — none of which the scene description reaches yet.

option(POSESTUDIO_WITH_CYCLES
       "Build the Cycles render engine plugin (fetches Cycles + its precompiled libraries, ~0.6 GB; needs git-lfs)" OFF)
if(NOT POSESTUDIO_WITH_CYCLES)
    return()
endif()

set(POSESTUDIO_CYCLES_TAG "v5.3.0" CACHE STRING "The Cycles release the engine is built from (a tag of blender/cycles and of its library repositories)")
if(WIN32 OR (UNIX AND NOT APPLE))
    option(POSESTUDIO_CYCLES_CUDA "Build Cycles's CUDA device (NVIDIA cards)" ON)
    option(POSESTUDIO_CYCLES_OPTIX "Build Cycles's OptiX device (NVIDIA RTX cards; NVIDIA's SDK license applies: LICENSE-EXCEPTION.txt, docs/RELEASING.md)" ON)
else()
    set(POSESTUDIO_CYCLES_CUDA OFF)
    set(POSESTUDIO_CYCLES_OPTIX OFF)
endif()
set(POSESTUDIO_CYCLES_CUDA_VERSION "13.5.0" CACHE STRING "The CUDA redistributable release Cycles's GPU kernels are compiled with")
# One precompiled kernel per card generation CUDA 13 still supports: Turing (GTX 16 / RTX 20),
# Ampere (RTX 30), Ada (RTX 40), Blackwell (RTX 50). Cycles loads the one for the card it runs
# on. (A "compute_75" entry would add a PTX kernel the driver compiles itself for a card with no
# kernel of its own here — but PTX written by a CUDA release NEWER than the user's driver is
# refused, "Unsupported PTX version": seen with CUDA 13.5 against a current driver. Not a
# default.) Narrow the list to your own card's generation for a quicker first build; the
# generations before Turing need an older CUDA release, and an older compiler with it.
set(POSESTUDIO_CYCLES_CUDA_ARCH "sm_75;sm_86;sm_89;sm_120" CACHE STRING "The CUDA kernels to build (Cycles's CYCLES_CUDA_BINARIES_ARCH)")
set(POSESTUDIO_CYCLES_OPTIX_TAG "v9.0.0" CACHE STRING "The OptiX headers' tag (github.com/NVIDIA/optix-dev)")

find_package(Git REQUIRED)

if(FETCHCONTENT_BASE_DIR)
    set(_cycles_deps "${FETCHCONTENT_BASE_DIR}")
else()
    set(_cycles_deps "${CMAKE_BINARY_DIR}/_deps")
endif()
set(_cycles_src "${_cycles_deps}/cycles-src")
set(_cycles_build "${_cycles_deps}/cycles-build")
set(_cycles_install "${_cycles_deps}/cycles-install")

# Runs a command, or stops the configuration saying what was being done.
function(_cycles_run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Cycles engine: ${what} failed (${_result}).\n"
                            "Configure with -DPOSESTUDIO_WITH_CYCLES=OFF to build without the render engine.")
    endif()
endfunction()

# --- 1. Cycles's source at the pinned tag --------------------------------------------------
if(NOT EXISTS "${_cycles_src}/.git")
    message(STATUS "Cycles engine: fetching Cycles ${POSESTUDIO_CYCLES_TAG} ...")
    file(MAKE_DIRECTORY "${_cycles_deps}")
    # (GIT_LFS_SKIP_SMUDGE: the source repository's own LFS files are test data.)
    _cycles_run("fetching Cycles"
        ${CMAKE_COMMAND} -E env GIT_LFS_SKIP_SMUDGE=1
        ${GIT_EXECUTABLE} clone --depth 1 --branch ${POSESTUDIO_CYCLES_TAG}
        https://projects.blender.org/blender/cycles.git "${_cycles_src}")
endif()

# --- 2. The precompiled libraries, sparse ----------------------------------------------------
if(WIN32)
    set(_cycles_platform "windows_x64")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_cycles_platform "macos_arm64")
    else()
        set(_cycles_platform "macos_x64")
    endif()
else()
    set(_cycles_platform "linux_x64")
endif()
set(_cycles_lib "${_cycles_src}/lib/${_cycles_platform}")
# The folders Cycles's configuration reads with the options below (and the SYCL runtime Embree's
# DLL imports on Windows). A missing one shows up as a find_package failure in step 5.
set(_cycles_lib_folders
    OpenImageIO openexr imath openjph opencolorio pugixml tbb zstd zlib pthreads embree
    openimagedenoise fmt aom png jpeg webp openjpeg)
set(_cycles_sparse "/deps.md\n")
foreach(_folder IN LISTS _cycles_lib_folders)
    string(APPEND _cycles_sparse "/${_folder}/\n")
endforeach()
string(APPEND _cycles_sparse "/dpcpp/bin/sycl*.dll\n/dpcpp/bin/ur_*.dll\n/dpcpp/bin/pi_*.dll\n/dpcpp/lib/libsycl.so*\n")
if(NOT EXISTS "${_cycles_lib}/OpenImageIO")
    message(STATUS "Cycles engine: fetching the precompiled libraries (${_cycles_platform}, sparse; about 0.55 GB) ...")
    execute_process(COMMAND ${GIT_EXECUTABLE} lfs version RESULT_VARIABLE _lfs OUTPUT_QUIET ERROR_QUIET)
    if(NOT _lfs EQUAL 0)
        message(FATAL_ERROR "Cycles engine: git-lfs is needed to fetch Cycles's precompiled libraries "
                            "(https://git-lfs.com). Install it, or configure with -DPOSESTUDIO_WITH_CYCLES=OFF.")
    endif()
    file(MAKE_DIRECTORY "${_cycles_lib}")
    if(NOT EXISTS "${_cycles_lib}/.git")
        _cycles_run("preparing the library checkout" ${GIT_EXECUTABLE} -C "${_cycles_lib}" init -q)
        _cycles_run("preparing the library checkout" ${GIT_EXECUTABLE} -C "${_cycles_lib}" remote add origin
                    https://projects.blender.org/blender/lib-${_cycles_platform}.git)
        _cycles_run("preparing the library checkout" ${GIT_EXECUTABLE} -C "${_cycles_lib}" lfs install --local)
    endif()
    # (Written as a file: patterns with a leading slash handed to git on a command line are
    # rewritten into paths by some shells.)
    _cycles_run("preparing the library checkout" ${GIT_EXECUTABLE} -C "${_cycles_lib}" config core.sparseCheckout true)
    _cycles_run("preparing the library checkout" ${GIT_EXECUTABLE} -C "${_cycles_lib}" config core.sparseCheckoutCone false)
    file(WRITE "${_cycles_lib}/.git/info/sparse-checkout" "${_cycles_sparse}")
    _cycles_run("fetching the library tree" ${GIT_EXECUTABLE} -C "${_cycles_lib}" fetch --depth 1 --filter=blob:none origin
                "refs/tags/${POSESTUDIO_CYCLES_TAG}:refs/tags/${POSESTUDIO_CYCLES_TAG}")
    _cycles_run("downloading the libraries" ${GIT_EXECUTABLE} -C "${_cycles_lib}" checkout -q ${POSESTUDIO_CYCLES_TAG})
endif()

# --- 3. The hook: engines/cycles as a subdirectory of Cycles's own src/ ----------------------
file(READ "${_cycles_src}/src/CMakeLists.txt" _cycles_cmake)
if(NOT _cycles_cmake MATCHES "POSESTUDIO_CYCLES_BRIDGE_DIR")
    set(_cycles_anchor "add_subdirectory(util)\n")
    string(FIND "${_cycles_cmake}" "${_cycles_anchor}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "Cycles engine: Cycles's src/CMakeLists.txt is not laid out as ${POSESTUDIO_CYCLES_TAG}'s "
                            "(no \"add_subdirectory(util)\") — the bridge's hook has nowhere to go. See cmake/CyclesEngine.cmake.")
    endif()
    string(REPLACE "${_cycles_anchor}"
           "${_cycles_anchor}\n# PoseStudio: the render engine bridge, built in this scope so that it is compiled with the\n# definitions and flags Cycles itself is (see PoseStudio's engines/cycles/CMakeLists.txt).\nif(POSESTUDIO_CYCLES_BRIDGE_DIR)\n  add_subdirectory(\${POSESTUDIO_CYCLES_BRIDGE_DIR} \${CMAKE_BINARY_DIR}/posestudio_bridge)\nendif()\n"
           _cycles_cmake "${_cycles_cmake}")
    file(WRITE "${_cycles_src}/src/CMakeLists.txt" "${_cycles_cmake}")
endif()

# --- 4. The GPU toolchains (build-time only) ----------------------------------------------------
# What Cycles is configured with, written as an initial-cache script (cmake -C): a value that is
# a LIST — the kernels to build — survives a file where it would not survive a command line.
set(_cycles_cache "")
macro(_cycles_option name value)
    string(APPEND _cycles_cache "set(${name} \"${value}\" CACHE STRING \"\" FORCE)
")
endmacro()
if(POSESTUDIO_CYCLES_CUDA)
    string(REGEX MATCH "^[0-9]+" _cuda_major "${POSESTUDIO_CYCLES_CUDA_VERSION}")
    set(_cycles_cuda "${_cycles_deps}/cuda${_cuda_major}")
    if(WIN32)
        set(_cuda_platform "windows-x86_64")
        set(_cuda_nvcc "${_cycles_cuda}/bin/nvcc.exe")
    else()
        set(_cuda_platform "linux-x86_64")
        set(_cuda_nvcc "${_cycles_cuda}/bin/nvcc")
    endif()
    if(NOT EXISTS "${_cuda_nvcc}")
        message(STATUS "Cycles engine: fetching the CUDA ${POSESTUDIO_CYCLES_CUDA_VERSION} compiler (redistributable archives) ...")
        set(_cuda_base "https://developer.download.nvidia.com/compute/cuda/redist/")
        set(_cuda_tmp "${_cycles_deps}/cuda-download")
        file(MAKE_DIRECTORY "${_cuda_tmp}")
        file(DOWNLOAD "${_cuda_base}redistrib_${POSESTUDIO_CYCLES_CUDA_VERSION}.json" "${_cuda_tmp}/manifest.json" STATUS _status)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            message(FATAL_ERROR "Cycles engine: could not read NVIDIA's manifest for CUDA ${POSESTUDIO_CYCLES_CUDA_VERSION} (${_status}). "
                                "Configure with -DPOSESTUDIO_CYCLES_CUDA=OFF -DPOSESTUDIO_CYCLES_OPTIX=OFF to build the CPU engine alone.")
        endif()
        file(READ "${_cuda_tmp}/manifest.json" _manifest)
        foreach(_package cuda_nvcc libnvvm cuda_crt cuda_cudart)
            # (A package a release does not have — the CRT headers were part of nvcc's before
            # CUDA 13 — is simply not there to fetch.)
            string(JSON _path ERROR_VARIABLE _missing GET "${_manifest}" ${_package} ${_cuda_platform} relative_path)
            if(_missing)
                continue()
            endif()
            string(JSON _sha GET "${_manifest}" ${_package} ${_cuda_platform} sha256)
            get_filename_component(_archive "${_path}" NAME)
            message(STATUS "Cycles engine:   ${_archive}")
            file(DOWNLOAD "${_cuda_base}${_path}" "${_cuda_tmp}/${_archive}" EXPECTED_HASH SHA256=${_sha} STATUS _status)
            list(GET _status 0 _code)
            if(NOT _code EQUAL 0)
                message(FATAL_ERROR "Cycles engine: downloading ${_archive} failed (${_status}).")
            endif()
            file(REMOVE_RECURSE "${_cuda_tmp}/unpacked")
            file(ARCHIVE_EXTRACT INPUT "${_cuda_tmp}/${_archive}" DESTINATION "${_cuda_tmp}/unpacked")
            # (Each archive holds one top folder: its contents are the toolkit's.)
            file(GLOB _top LIST_DIRECTORIES true "${_cuda_tmp}/unpacked/*")
            foreach(_folder IN LISTS _top)
                if(IS_DIRECTORY "${_folder}")
                    file(COPY "${_folder}/" DESTINATION "${_cycles_cuda}")
                endif()
            endforeach()
        endforeach()
        file(REMOVE_RECURSE "${_cuda_tmp}")
        if(NOT EXISTS "${_cuda_nvcc}")
            message(FATAL_ERROR "Cycles engine: the CUDA archives did not yield ${_cuda_nvcc}.")
        endif()
    endif()
    _cycles_option(WITH_CYCLES_DEVICE_CUDA ON)
    _cycles_option(WITH_CYCLES_CUDA_BINARIES ON)
    _cycles_option(CUDA_TOOLKIT_ROOT_DIR "${_cycles_cuda}")
    _cycles_option(CUDA${_cuda_major}_TOOLKIT_ROOT_DIR "${_cycles_cuda}")
    _cycles_option(CUDA${_cuda_major}_NVCC_EXECUTABLE "${_cuda_nvcc}")
    _cycles_option(CYCLES_CUDA_BINARIES_ARCH "${POSESTUDIO_CYCLES_CUDA_ARCH}")
else()
    _cycles_option(WITH_CYCLES_DEVICE_CUDA OFF)
    _cycles_option(WITH_CYCLES_CUDA_BINARIES OFF)
endif()
if(POSESTUDIO_CYCLES_OPTIX)
    if(NOT POSESTUDIO_CYCLES_CUDA)
        message(FATAL_ERROR "Cycles engine: the OptiX device is built with the CUDA compiler — turn POSESTUDIO_CYCLES_CUDA on, or POSESTUDIO_CYCLES_OPTIX off.")
    endif()
    set(_cycles_optix "${_cycles_deps}/optix")
    if(NOT EXISTS "${_cycles_optix}/include/optix.h")
        message(STATUS "Cycles engine: fetching the OptiX ${POSESTUDIO_CYCLES_OPTIX_TAG} headers ...")
        _cycles_run("fetching the OptiX headers" ${GIT_EXECUTABLE} clone --depth 1 --branch ${POSESTUDIO_CYCLES_OPTIX_TAG}
                    https://github.com/NVIDIA/optix-dev.git "${_cycles_optix}")
    endif()
    _cycles_option(WITH_CYCLES_DEVICE_OPTIX ON)
    _cycles_option(OPTIX_ROOT_DIR "${_cycles_optix}")
else()
    _cycles_option(WITH_CYCLES_DEVICE_OPTIX OFF)
endif()

# --- 5. Configure Cycles (again whenever what it is configured with changes) ------------------
foreach(_off OSL USD HYDRA_RENDER_DELEGATE ALEMBIC OPENVDB NANOVDB OPENSUBDIV DEVICE_HIP)
    _cycles_option(WITH_CYCLES_${_off} OFF)
endforeach()
_cycles_option(WITH_CYCLES_EMBREE ON)
_cycles_option(WITH_CYCLES_OPENIMAGEDENOISE ON)
_cycles_option(CMAKE_INSTALL_PREFIX "${_cycles_install}")
_cycles_option(POSESTUDIO_CYCLES_BRIDGE_DIR "${CMAKE_SOURCE_DIR}/engines/cycles")
_cycles_option(POSESTUDIO_RENDER_API_DIR "${CMAKE_SOURCE_DIR}/src/render")
get_property(_cycles_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _cycles_multi)
    _cycles_option(CMAKE_BUILD_TYPE Release)
endif()
set(_cycles_generator -G "${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND _cycles_generator -A "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND _cycles_generator -T "${CMAKE_GENERATOR_TOOLSET}")
endif()
string(PREPEND _cycles_cache "# Written by PoseStudio's cmake/CyclesEngine.cmake (Cycles ${POSESTUDIO_CYCLES_TAG}, ${CMAKE_GENERATOR}).
")
set(_cycles_options_file "${_cycles_deps}/cycles-options.cmake")
set(_cycles_configured "")
if(EXISTS "${_cycles_options_file}" AND EXISTS "${_cycles_build}/CMakeCache.txt")
    file(READ "${_cycles_options_file}" _cycles_configured)
endif()
if(NOT _cycles_configured STREQUAL _cycles_cache)
    message(STATUS "Cycles engine: configuring Cycles ${POSESTUDIO_CYCLES_TAG} ...")
    file(REMOVE "${_cycles_options_file}")
    file(WRITE "${_cycles_deps}/cycles-options.new.cmake" "${_cycles_cache}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${_cycles_src}" -B "${_cycles_build}" ${_cycles_generator}
                -C "${_cycles_deps}/cycles-options.new.cmake"
        RESULT_VARIABLE _result OUTPUT_FILE "${_cycles_deps}/cycles-configure.log" ERROR_FILE "${_cycles_deps}/cycles-configure.log")
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Cycles engine: configuring Cycles failed — see ${_cycles_deps}/cycles-configure.log.
"
                            "Configure with -DPOSESTUDIO_WITH_CYCLES=OFF to build without the render engine.")
    endif()
    # (Kept under its final name only once Cycles has accepted it: a failed configuration is
    # tried again at the next one.)
    file(RENAME "${_cycles_deps}/cycles-options.new.cmake" "${_cycles_options_file}")
endif()

# --- 6. Build it with the application, and mirror it beside the application --------------------
file(GLOB _cycles_bridge_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/engines/cycles/*")
set(_cycles_built "${CMAKE_BINARY_DIR}/cycles-engine/built.stamp")
add_custom_command(
    OUTPUT "${_cycles_built}"
    COMMAND ${CMAKE_COMMAND} --build "${_cycles_build}" --config Release --target install
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/cycles-engine"
    COMMAND ${CMAKE_COMMAND} -E touch "${_cycles_built}"
    DEPENDS ${_cycles_bridge_sources} "${CMAKE_SOURCE_DIR}/src/render/renderengineapi.h" "${_cycles_options_file}"
    COMMENT "Building the Cycles render engine (Release)"
    VERBATIM)
# A true mirror (remove, then copy), per configuration of the application: the engine's folder is
# found next to whichever PoseStudio executable runs.
set(_cycles_mirrored "${CMAKE_BINARY_DIR}/cycles-engine/mirrored-$<CONFIG>.stamp")
add_custom_command(
    OUTPUT "${_cycles_mirrored}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:PoseStudio>/engines/cycles"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PoseStudio>/engines"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_cycles_install}" "$<TARGET_FILE_DIR:PoseStudio>/engines/cycles"
    COMMAND ${CMAKE_COMMAND} -E touch "${_cycles_mirrored}"
    DEPENDS "${_cycles_built}"
    COMMENT "Mirroring the Cycles render engine next to the application"
    VERBATIM)
add_custom_target(PoseStudioCyclesEngine ALL DEPENDS "${_cycles_built}" "${_cycles_mirrored}")

# The engine check (tools/rendercheck) renders with the engine as built here.
set(POSESTUDIO_CYCLES_ENGINE_DIR "${_cycles_install}")
message(STATUS "Cycles engine: ${POSESTUDIO_CYCLES_TAG} (CUDA ${POSESTUDIO_CYCLES_CUDA}, OptiX ${POSESTUDIO_CYCLES_OPTIX}) -> <app folder>/engines/cycles")
