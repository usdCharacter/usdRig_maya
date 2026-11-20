# The rigExecRig Maya node. Builds against a Maya devkit and usdRig's
# USD-free runtime, whose sources are compiled in from the usdRig checkout
# (RIGEXEC_ROOT, by default ../usdRig beside this sidecar). No USD is
# linked: the rig is a poseable .rigexec bake, played in Maya's process.
#
#   cmake -S rigExecMaya -B build-rigexecmaya -G Ninja ^
#         -DMAYA_DEVKIT_LOCATION=<devkitBase>
#   cmake --build build-rigexecmaya
#
# The plugin lands in plug-ins/ beside the Python half, so the module file
# (rigExecMaya.mod) serves both.
cmake_minimum_required(VERSION 3.26)
project(rigExecMaya C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MAYA_DEVKIT_LOCATION "$ENV{MAYA_DEVKIT_LOCATION}" CACHE PATH
    "The Maya devkit's devkitBase directory")
if (NOT EXISTS "${MAYA_DEVKIT_LOCATION}/include/maya/MFnPlugin.h")
    message(FATAL_ERROR
        "MAYA_DEVKIT_LOCATION must name a Maya devkitBase directory "
        "(got '${MAYA_DEVKIT_LOCATION}')")
endif()

set(RIGEXEC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../usdRig" CACHE PATH
    "The usdRig checkout whose runtime is compiled in")
if (NOT EXISTS "${RIGEXEC_ROOT}/libs/rigExecRuntime/runtime.h")
    message(FATAL_ERROR
        "RIGEXEC_ROOT must name a usdRig checkout (got '${RIGEXEC_ROOT}')")
endif()
set(RIGEXEC_BUILD "${RIGEXEC_ROOT}/build" CACHE PATH
    "usdRig's build directory, which lists the runtime's sources")
if (NOT EXISTS "${RIGEXEC_BUILD}/runtimeSources.txt")
    message(FATAL_ERROR
        "RIGEXEC_BUILD must name a built usdRig (no runtimeSources.txt in "
        "'${RIGEXEC_BUILD}'); build usdRig first")
endif()
# usdRig's runtime, the op graph it schedules with and the binary format it
# reads (flatbuffers, LZMA-compressed): no USD in any of them. The runtime's
# own sources, the movers built into it among them, come from the list
# usdRig's build writes for builds outside it.
file(STRINGS "${RIGEXEC_BUILD}/runtimeSources.txt" _rigexec_listed)
file(STRINGS "${RIGEXEC_BUILD}/runtimeIncludes.txt" _rigexec_includes)
set(_rigexec_runtime_sources)
foreach(_source IN LISTS _rigexec_listed)
    if (_source MATCHES "cmake_pch")
        continue()
    endif()
    if (NOT IS_ABSOLUTE "${_source}")
        set(_source "${RIGEXEC_ROOT}/${_source}")
    endif()
    list(APPEND _rigexec_runtime_sources "${_source}")
endforeach()
list(FILTER _rigexec_includes EXCLUDE REGEX "cmake_pch")
file(GLOB _rigexec_format_sources
    "${RIGEXEC_ROOT}/libs/rigExecBinary/*.cpp"
    "${RIGEXEC_ROOT}/libs/rigExecGraph/*.cpp")
list(APPEND _rigexec_runtime_sources ${_rigexec_format_sources})
set(_rigexec_lzma "${RIGEXEC_ROOT}/thirdparty/lzma/C")
add_library(rigExecMayaLzma STATIC
    "${_rigexec_lzma}/LzmaEnc.c" "${_rigexec_lzma}/LzmaDec.c"
    "${_rigexec_lzma}/LzFind.c" "${_rigexec_lzma}/CpuArch.c")
target_compile_definitions(rigExecMayaLzma PRIVATE Z7_ST
                           RIGEXEC_LZMA_PORTABLE_SCALAR)
set_target_properties(rigExecMayaLzma PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(rigExecMayaRuntime STATIC ${_rigexec_runtime_sources})
target_include_directories(rigExecMayaRuntime PUBLIC "${RIGEXEC_ROOT}/libs"
    "${RIGEXEC_ROOT}/thirdparty/flatbuffers/include" ${_rigexec_includes})
target_include_directories(rigExecMayaRuntime PRIVATE "${_rigexec_lzma}")
target_link_libraries(rigExecMayaRuntime PRIVATE rigExecMayaLzma)
set_target_properties(rigExecMayaRuntime PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
# The runtime's results must match usdRig's bit for bit.
if (MSVC)
    target_compile_options(rigExecMayaRuntime PRIVATE /fp:precise /bigobj)
    target_compile_definitions(rigExecMayaRuntime PRIVATE NOMINMAX)
else()
    target_compile_options(rigExecMayaRuntime PRIVATE -fno-fast-math)
endif()

add_library(rigExecMaya MODULE
    src/rigExecRigNode.cpp
    src/rigEvaluator.cpp
    src/hoverOverlay.cpp
    src/touchOverlay.cpp
    src/controlsDraw.cpp
    src/deformNode.cpp
    src/frameData.cpp
    src/frameCache.cpp
    src/taskPool.cpp)
target_link_libraries(rigExecMaya PRIVATE rigExecMayaRuntime)
target_include_directories(rigExecMaya PRIVATE "${MAYA_DEVKIT_LOCATION}/include")
target_link_directories(rigExecMaya PRIVATE "${MAYA_DEVKIT_LOCATION}/lib")
# clew: the OpenCL calls the GPU Override deformer makes.
target_link_libraries(rigExecMaya PRIVATE OpenMaya OpenMayaAnim OpenMayaRender
                      OpenMayaUI Foundation clew)
if (WIN32)
    target_compile_definitions(rigExecMaya PRIVATE NT_PLUGIN REQUIRE_IOSTREAM
                               _BOOL NOMINMAX)
    set_target_properties(rigExecMaya PROPERTIES SUFFIX ".mll" PREFIX "")
    target_link_options(rigExecMaya PRIVATE "/export:initializePlugin"
                        "/export:uninitializePlugin")
elseif (APPLE)
    target_compile_definitions(rigExecMaya PRIVATE MAC_PLUGIN OSMac_ _BOOL
                               REQUIRE_IOSTREAM)
    set_target_properties(rigExecMaya PROPERTIES SUFFIX ".bundle" PREFIX "")
else()
    target_compile_definitions(rigExecMaya PRIVATE LINUX _BOOL
                               REQUIRE_IOSTREAM)
    set_target_properties(rigExecMaya PROPERTIES PREFIX "")
endif()
# Where the plugin is written. The default sits it beside the Python half;
# point it elsewhere to build while a Maya session holds the default copy.
set(RIGEXEC_MAYA_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plug-ins" CACHE PATH
    "Output directory for rigExecMaya")
set_target_properties(rigExecMaya PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${RIGEXEC_MAYA_PLUGIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${RIGEXEC_MAYA_PLUGIN_DIR}")
