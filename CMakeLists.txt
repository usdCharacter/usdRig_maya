cmake_minimum_required(VERSION 3.26)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(usdMayaRig VERSION 0.1.0 LANGUAGES CXX)
    find_package(rigExec REQUIRED CONFIG)
    set(_maya_rigging rigExec::rigExecRigging)
    include(CTest)
    set(_maya_tests_default ${BUILD_TESTING})
else()
    set(_maya_rigging rigExecRigging)
    set(_maya_tests_default ${RIGEXEC_BUILD_TESTS})
endif()

add_library(usdMayaRig SHARED fileFormat.cpp parser.cpp translator.cpp nativeNodes.cpp nativeDeformers.cpp)
target_compile_features(usdMayaRig PRIVATE cxx_std_17)
target_link_libraries(usdMayaRig PRIVATE ${_maya_rigging} usdGeom usdShade usdSkel ar plug)
if(TARGET rigExec)
    target_link_libraries(usdMayaRig PRIVATE rigExec exec vdf)
else()
    target_link_libraries(usdMayaRig PRIVATE rigExec::rigExec exec vdf)
endif()
set_target_properties(usdMayaRig PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
if (MSVC)
    target_compile_definitions(usdMayaRig PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(usdMayaRig PRIVATE /bigobj /Zc:__cplusplus /permissive-)
endif()
set(_maya_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/usdMayaRig/resources")
file(GENERATE OUTPUT "${_maya_resources}/plugInfo.json" CONTENT
"{\"Plugins\":[{\"Type\":\"library\",\"Name\":\"usdMayaRig\",\"Root\":\"..\",\"LibraryPath\":\"$<TARGET_FILE:usdMayaRig>\",\"ResourcePath\":\"resources\",\"Info\":{\"RigExecMoverPlugin\":5,\"Exec\":{\"Schemas\":{\"RigExecMayaScalar\":{},\"RigExecMayaTranslation\":{},\"RigExecMayaCurvePoint\":{},\"RigExecNurbsSurfacePin\":{},\"RigExecMeshPin\":{},\"RigExecMayaMatrixSpace\":{},\"RigExecMayaSkinInfluence\":{}}},\"Types\":{\"RigExecMayaFfd\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaFfd\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaCurvePoint\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaCurvePoint\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaTranslation\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaTranslation\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaScalar\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaScalar\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaSkinInfluence\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaSkinInfluence\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaMatrixSpace\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaMatrixSpace\",\"schemaKind\":\"concreteTyped\"},\"RigExecMeshPin\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMeshPin\",\"schemaKind\":\"concreteTyped\"},\"RigExecNurbsSurfacePin\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecNurbsSurfacePin\",\"schemaKind\":\"concreteTyped\"},\"UsdMayaRigFileFormat\":{\"bases\":[\"SdfFileFormat\"],\"displayName\":\"Maya ASCII Rig\",\"extensions\":[\"ma\"],\"formatId\":\"mayaRig\",\"primary\":true,\"target\":\"usd\",\"supportsReading\":true,\"supportsWriting\":false,\"supportsEditing\":false}}}}]}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/install/plugInfo.json" CONTENT
"{\"Plugins\":[{\"Type\":\"library\",\"Name\":\"usdMayaRig\",\"Root\":\"..\",\"LibraryPath\":\"../../$<TARGET_FILE_NAME:usdMayaRig>\",\"ResourcePath\":\"resources\",\"Info\":{\"RigExecMoverPlugin\":5,\"Exec\":{\"Schemas\":{\"RigExecMayaScalar\":{},\"RigExecMayaTranslation\":{},\"RigExecMayaCurvePoint\":{},\"RigExecNurbsSurfacePin\":{},\"RigExecMeshPin\":{},\"RigExecMayaMatrixSpace\":{},\"RigExecMayaSkinInfluence\":{}}},\"Types\":{\"RigExecMayaFfd\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaFfd\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaCurvePoint\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaCurvePoint\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaTranslation\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaTranslation\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaScalar\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaScalar\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaSkinInfluence\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaSkinInfluence\",\"schemaKind\":\"concreteTyped\"},\"RigExecMayaMatrixSpace\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMayaMatrixSpace\",\"schemaKind\":\"concreteTyped\"},\"RigExecMeshPin\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecMeshPin\",\"schemaKind\":\"concreteTyped\"},\"RigExecNurbsSurfacePin\":{\"bases\":[\"UsdTyped\"],\"schemaIdentifier\":\"RigExecNurbsSurfacePin\",\"schemaKind\":\"concreteTyped\"},\"UsdMayaRigFileFormat\":{\"bases\":[\"SdfFileFormat\"],\"displayName\":\"Maya ASCII Rig\",\"extensions\":[\"ma\"],\"formatId\":\"mayaRig\",\"primary\":true,\"target\":\"usd\",\"supportsReading\":true,\"supportsWriting\":false,\"supportsEditing\":false}}}}]}")
file(GENERATE OUTPUT "${_maya_resources}/generatedSchema.usda" CONTENT
"#usda 1.0
class RigExecMayaFfd \"RigExecMayaFfd\" {}
class RigExecMayaScalar \"RigExecMayaScalar\" {}
class RigExecMayaTranslation \"RigExecMayaTranslation\" {}
class RigExecMayaCurvePoint \"RigExecMayaCurvePoint\" {}
class RigExecNurbsSurfacePin \"RigExecNurbsSurfacePin\" {}
class RigExecMeshPin \"RigExecMeshPin\" {}
class RigExecMayaMatrixSpace \"RigExecMayaMatrixSpace\" {}
class RigExecMayaSkinInfluence \"RigExecMayaSkinInfluence\" {}
")
install(FILES "${_maya_resources}/generatedSchema.usda" DESTINATION lib/usd/usdMayaRig/resources)
install(TARGETS usdMayaRig LIBRARY DESTINATION lib RUNTIME DESTINATION lib
    ARCHIVE DESTINATION lib)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/install/plugInfo.json"
    DESTINATION lib/usd/usdMayaRig/resources)
install(FILES usdview/plugInfo.json usdview/usdMayaRigUsdview.py
    DESTINATION lib/python/usdMayaRigUsdview)
if (APPLE)
    set_target_properties(usdMayaRig PROPERTIES INSTALL_RPATH "@loader_path")
elseif (UNIX)
    set_target_properties(usdMayaRig PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

# This test intentionally links only USD: discovery must load the plugin.
option(USDMAYARIG_BUILD_TESTS "Build Maya ASCII reader tests" ${_maya_tests_default})
if (USDMAYARIG_BUILD_TESTS)
    enable_testing()
    add_executable(testUsdMayaRig tests/testUsdMayaRig.cpp)
    target_compile_features(testUsdMayaRig PRIVATE cxx_std_17)
    target_link_libraries(testUsdMayaRig PRIVATE usd usdGeom usdShade plug)
    add_dependencies(testUsdMayaRig usdMayaRig)
    if (TARGET rigExecRigging)
        set(_maya_schema "${CMAKE_SOURCE_DIR}/plugin/rigExecSchema/resources")
    else()
        set(_maya_schema "${rigExec_PLUGIN_DIR}/rigExecSchema/resources")
    endif()
    add_test(NAME testUsdMayaRig COMMAND testUsdMayaRig
        "${_maya_resources}" "${_maya_schema}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_executable(testUsdMayaRigEvaluation tests/testEvaluation.cpp)
    target_compile_features(testUsdMayaRigEvaluation PRIVATE cxx_std_17)
    if (TARGET rigExec)
        target_link_libraries(testUsdMayaRigEvaluation PRIVATE rigExec)
    else()
        target_link_libraries(testUsdMayaRigEvaluation PRIVATE rigExec::rigExec)
    endif()
    add_dependencies(testUsdMayaRigEvaluation usdMayaRig)
    if(MSVC)
        foreach(_test testUsdMayaRig testUsdMayaRigEvaluation)
            target_compile_definitions(${_test} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
            target_compile_options(${_test} PRIVATE /bigobj /Zc:__cplusplus /permissive-)
        endforeach()
    endif()
    add_test(NAME testUsdMayaRigEvaluation COMMAND testUsdMayaRigEvaluation
        "${_maya_resources}" "${_maya_schema}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
endif()
