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
project(rigExecMaya CXX)

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
# usdRig's runtime and the binary format it reads: no USD in either.
file(GLOB _rigexec_runtime_sources
    "${RIGEXEC_ROOT}/libs/rigExecRuntime/*.cpp"
    "${RIGEXEC_ROOT}/libs/rigExecBinary/*.cpp")
add_library(rigExecMayaRuntime STATIC ${_rigexec_runtime_sources})
target_include_directories(rigExecMayaRuntime PUBLIC "${RIGEXEC_ROOT}/libs")
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
    src/frameCache.cpp)
target_link_libraries(rigExecMaya PRIVATE rigExecMayaRuntime)
target_include_directories(rigExecMaya PRIVATE "${MAYA_DEVKIT_LOCATION}/include")
target_link_directories(rigExecMaya PRIVATE "${MAYA_DEVKIT_LOCATION}/lib")
target_link_libraries(rigExecMaya PRIVATE OpenMaya OpenMayaAnim OpenMayaRender
                      OpenMayaUI Foundation)
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
