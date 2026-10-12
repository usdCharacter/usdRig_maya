#include "pxr/pxr.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/sdf/assetPath.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <stdexcept>
PXR_NAMESPACE_USING_DIRECTIVE
#define CHECK(x) do { if(!(x)) throw std::runtime_error("check failed: " #x); } while(false)
int main(int argc,char **argv) {
    try {
        CHECK(argc==4);
        PlugRegistry::GetInstance().RegisterPlugins(argv[1]);
        PlugRegistry::GetInstance().RegisterPlugins(argv[2]);
        auto ff=SdfFileFormat::FindByExtension("ma"); CHECK(ff);
        CHECK(ff->GetFormatId()==TfToken("mayaRig"));
        CHECK(SdfFileFormat::FormatSupportsReading("ma"));
        CHECK(!SdfFileFormat::FormatSupportsWriting("ma"));
        std::string fixture=std::string(argv[3])+"/rig.ma";
        CHECK(ff->CanRead(fixture));
        auto stage=UsdStage::Open(fixture); CHECK(stage);
        CHECK(stage->GetDefaultPrim().GetTypeName()==TfToken("RigExecRoot"));
        CHECK(UsdGeomGetStageMetersPerUnit(stage)==0.01);
        CHECK(stage->GetFramesPerSecond()==24);
        CHECK(stage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/nroot/ntip")).GetTypeName()==TfToken("RigExecJoint"));
        int meshCount=0,skinCount=0;
        for(auto p:stage->Traverse()) {
            if(p.IsA<UsdGeomMesh>()) {
                ++meshCount; UsdGeomMesh mesh(p); VtVec3fArray points; VtIntArray indices;
                CHECK(mesh.GetPointsAttr().Get(&points)); CHECK(points.size()==4);
                CHECK(points[2]==GfVec3f(1,2,0));
                CHECK(mesh.GetFaceVertexIndicesAttr().Get(&indices)); CHECK(indices==VtIntArray({0,1,2,3}));
                auto st=UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("st")); CHECK(st.IsIndexed());
                CHECK(st.GetIndices(&indices)); CHECK(indices==VtIntArray({0,1,2,3}));
                CHECK(UsdShadeMaterialBindingAPI(p).ComputeBoundMaterial());
                auto subsets=UsdShadeMaterialBindingAPI(p).GetMaterialBindSubsets(); CHECK(subsets.size()==1);
                CHECK(subsets[0].GetIndicesAttr().Get(&indices)); CHECK(indices==VtIntArray{0});
                CHECK(UsdShadeMaterialBindingAPI(subsets[0].GetPrim()).ComputeBoundMaterial());
            }
            if(p.GetTypeName()==TfToken("RigExecSkinMover")) {
                ++skinCount; VtIntArray indices; VtFloatArray weights; int slots=0;
                CHECK(p.GetAttribute(TfToken("rigExec:elementSize")).Get(&slots)); CHECK(slots==2);
                CHECK(p.GetAttribute(TfToken("rigExec:jointIndices")).Get(&indices));
                CHECK(p.GetAttribute(TfToken("rigExec:jointWeights")).Get(&weights));
                CHECK(indices.size()==8 && weights.size()==8);
                CHECK(indices[4]==1 && weights[4]==1);
                CHECK(weights[6]==0.25f && weights[7]==0.75f);
            }
        }
        CHECK(meshCount==1 && skinCount==1);
        CHECK(stage->GetRootLayer()->GetCustomLayerData()["mayaRig:complete"].Get<bool>());
        // Strict reads must accept the supported fixture and reject lossy input.
        auto strict=SdfLayer::FindOrOpen(fixture,{{"strict","1"}}); CHECK(strict);
        auto refStage=UsdStage::Open(std::string(argv[3])+"/referenced.ma"); CHECK(refStage);
        CHECK(refStage->GetPrimAtPath(SdfPath("/Rig/Dag/nhero_3acharacter/nhero_3aroot")));
        CHECK(refStage->GetRootLayer()->GetExternalAssetDependencies().size()==2);
        auto texture=UsdShadeShader(refStage->GetPrimAtPath(SdfPath("/Rig/Materials/nhero_3apaint/Texture")));
        CHECK(texture); SdfAssetPath textureAsset;
        CHECK(texture.GetInput(TfToken("file")).Get(&textureAsset));
        CHECK(textureAsset.GetAssetPath().find("textures/checker.ppm")!=std::string::npos);
        CHECK(!textureAsset.GetResolvedPath().empty());
        { TfErrorMark errors;
          CHECK(!UsdStage::Open(std::string(argv[3])+"/cycle.ma")); CHECK(!errors.IsClean()); errors.Clear(); }
        std::string serialized; CHECK(stage->GetRootLayer()->ExportToString(&serialized));
        auto roundtrip=SdfLayer::CreateAnonymous("roundtrip.usda"); CHECK(roundtrip->ImportFromString(serialized));
        // Quoted semicolons, URLs and comment delimiters must remain data.
        auto layer=SdfLayer::CreateAnonymous("inline.ma"); CHECK(layer);
        CHECK(layer->ImportFromString("//Maya ASCII\ncreateNode transform -n \"a:b\"; setAttr \".note\" -type \"string\" \"a;b // data /* x */\";"));
        CHECK(layer->GetPrimAtPath(SdfPath("/Rig/Dag/na_3ab")));
        std::string before; CHECK(layer->ExportToString(&before));
        auto reject=[&](const std::string &bad) {
            TfErrorMark errors; CHECK(!layer->ImportFromString(bad)); CHECK(!errors.IsClean()); errors.Clear();
            std::string after; CHECK(layer->ExportToString(&after)); CHECK(before==after);
        };
        reject("createNode transform -n \"unterminated;");
        reject("createNode joint -n \"j\" -p \"missing\";");
        reject("createNode transform -n \"j\"; setAttr \".t\" -type \"double3\" nan 0 0;");
        reject("createNode transform -n \"a\" -p \"b\"; createNode transform -n \"b\" -p \"a\";");
        reject("createNode mesh -n \"m\"; setAttr \".vt[0:2]\" -type \"float3\" 0 0 0;");
        auto strictString=SdfLayer::CreateAnonymous("strict.ma",ff,{{"strict","1"}});
        CHECK(strictString);
        { TfErrorMark errors; CHECK(!strictString->ImportFromString("createNode expression -n \"code\";")); CHECK(!errors.IsClean()); errors.Clear(); }
        CHECK(layer->ImportFromString("createNode transform -n \"a\"; setAttr \".note\" -type \"string\" \"-notAFlag\";"));
        CHECK(layer->ImportFromString("createNode expression -n \"code\"; setAttr \".ex\" -type \"string\" \"system(never_run);\";"));
        CHECK(!layer->GetCustomLayerData()["mayaRig:complete"].Get<bool>());
        CHECK(!layer->GetCustomLayerData()["mayaRig:diagnostics"].Get<VtStringArray>().empty());
        for(const std::string kind:{"parentConstraint","pointConstraint","orientConstraint","scaleConstraint"}) {
            const std::string channel=kind=="orientConstraint" ? "r" : kind=="scaleConstraint" ? "s" : "t";
            CHECK(strictString->ImportFromString(
                "createNode transform -n \"source\"; createNode joint -n \"target\"; createNode "+kind+" -n \"c\"; "
                "connectAttr \"source.t\" \"c.tg[0].tt\"; connectAttr \"c.c"+channel+"\" \"target."+channel+"\";"));
        }
        CHECK(strictString->ImportFromString(
            "createNode transform -n \"curve\"; createNode nurbsCurve -n \"shape\" -p \"curve\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 1 1 0 no 3 2 0 1 2 0 0 0 1 1 0;"));
        CHECK(strictString->ImportFromString("select -ne \":initialShadingGroup\"; select -ne \":lambert1\"; setAttr \".c\" -type \"float3\" 0.2 0.4 0.6;"));
        CHECK(strictString->GetPrimAtPath(SdfPath("/Rig/Materials/nlambert1")));
        // Large Maya meshes omit -type on subsequent polyFaces chunks.
        // Check both topology and UVs survive an untyped continuation.
        CHECK(strictString->ImportFromString(
            "createNode mesh -n \"chunked\"; "
            "setAttr \".vt[0:3]\" -type \"float3\" 0 0 0 1 0 0 1 1 0 0 1 0; "
            "setAttr \".pt[1]\" -type \"float3\" 0 0 2; "
            "setAttr \".pt[3]\" 0 0 3; "
            "setAttr \".ed[0:4]\" 0 1 0 1 2 0 2 0 0 2 3 0 3 0 0; "
            "setAttr \".uvst[0].uvsp[0:3]\" -type \"float2\" 0 0 1 0 1 1 0 1; "
            "setAttr \".fc[0]\" -type \"polyFaces\" f 3 0 1 2 mu 0 3 0 1 2; "
            "setAttr \".fc[1]\" f 3 -3 3 4 mu 0 3 0 2 3;"));
        auto chunked=UsdStage::Open(strictString); CHECK(chunked);
        int chunkedMeshes=0;
        for(auto p:chunked->Traverse()) if(p.IsA<UsdGeomMesh>()) {
            ++chunkedMeshes; VtIntArray values; UsdGeomMesh mesh(p);
            VtVec3fArray vertices; CHECK(mesh.GetPointsAttr().Get(&vertices));
            CHECK(vertices==VtVec3fArray({GfVec3f(0,0,0),GfVec3f(1,0,2),GfVec3f(1,1,0),GfVec3f(0,1,3)}));
            CHECK(mesh.GetFaceVertexCountsAttr().Get(&values)); CHECK(values==VtIntArray({3,3}));
            CHECK(mesh.GetFaceVertexIndicesAttr().Get(&values)); CHECK(values==VtIntArray({0,1,2,0,2,3}));
            CHECK(UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("st")).GetIndices(&values));
            CHECK(values==VtIntArray({0,1,2,0,2,3}));
        }
        CHECK(chunkedMeshes==1);
        // Arnold's underscore attribute names must behave like camelCase.
        CHECK(strictString->ImportFromString(
            "createNode aiStandardSurface -n \"arnold\"; "
            "setAttr \".base_color\" -type \"float3\" 0.1 0.2 0.3; "
            "setAttr \".specular_roughness\" 0.4;"));
        auto arnold=UsdStage::Open(strictString); CHECK(arnold);
        auto arnoldSurface=UsdShadeShader(arnold->GetPrimAtPath(SdfPath("/Rig/Materials/narnold/Surface"))); CHECK(arnoldSurface);
        GfVec3f arnoldDiffuse; CHECK(arnoldSurface.GetInput(TfToken("diffuseColor")).Get(&arnoldDiffuse));
        CHECK(arnoldDiffuse==GfVec3f(0.1f,0.2f,0.3f));
        float arnoldRoughness=0; CHECK(arnoldSurface.GetInput(TfToken("roughness")).Get(&arnoldRoughness));
        CHECK(arnoldRoughness==0.4f);
        CHECK(layer->ImportFromString(
            "createNode aiStandardSurface -n \"textured\"; createNode file -n \"map\"; "
            "setAttr \".ftn\" -type \"string\" \"missing.png\"; connectAttr \"map.oc\" \"textured.base_color\";"));
        auto textured=UsdStage::Open(layer); CHECK(textured);
        CHECK(textured->GetPrimAtPath(SdfPath("/Rig/Materials/ntextured/Texture")));
        // Multi-map materials: diffuse and roughness textures share one
        // material under stable role names, with resolved assets.
        auto shading=SdfLayer::FindOrOpen(std::string(argv[3])+"/shading.ma",{{"strict","1"}}); CHECK(shading);
        auto shadingStage=UsdStage::Open(shading); CHECK(shadingStage);
        auto skinSurface=UsdShadeShader(shadingStage->GetPrimAtPath(SdfPath("/Rig/Materials/nskin/Surface"))); CHECK(skinSurface);
        UsdShadeConnectableAPI connectedSource; TfToken connectedName; UsdShadeAttributeType connectedType;
        CHECK(skinSurface.GetInput(TfToken("diffuseColor")).GetConnectedSource(&connectedSource,&connectedName,&connectedType));
        CHECK(connectedName==TfToken("rgb"));
        CHECK(connectedSource.GetPath()==SdfPath("/Rig/Materials/nskin/Texture"));
        CHECK(skinSurface.GetInput(TfToken("roughness")).GetConnectedSource(&connectedSource,&connectedName,&connectedType));
        CHECK(connectedName==TfToken("r"));
        CHECK(connectedSource.GetPath()==SdfPath("/Rig/Materials/nskin/Roughness"));
        auto roughTexture=UsdShadeShader(shadingStage->GetPrimAtPath(SdfPath("/Rig/Materials/nskin/Roughness"))); CHECK(roughTexture);
        SdfAssetPath roughAsset; CHECK(roughTexture.GetInput(TfToken("file")).Get(&roughAsset));
        CHECK(!roughAsset.GetResolvedPath().empty());
        // A normal map through bump2d is preserved as a normal connection,
        // with the skipped bump evaluation cited rather than silent.
        CHECK(layer->ImportFromString(
            "createNode file -n \"nrm\"; setAttr \".ftn\" -type \"string\" \"missing.png\"; "
            "createNode bump2d -n \"b\"; createNode aiStandardSurface -n \"bumped\"; "
            "connectAttr \"nrm.ocr\" \"b.bv\"; connectAttr \"b.o\" \"bumped.n\";"));
        auto bumped=UsdStage::Open(layer); CHECK(bumped);
        auto bumpedSurface=UsdShadeShader(bumped->GetPrimAtPath(SdfPath("/Rig/Materials/nbumped/Surface"))); CHECK(bumpedSurface);
        CHECK(bumpedSurface.GetInput(TfToken("normal")).GetConnectedSource(&connectedSource,&connectedName,&connectedType));
        CHECK(connectedName==TfToken("rgb"));
        CHECK(connectedSource.GetPath()==SdfPath("/Rig/Materials/nbumped/Normal"));
        CHECK(!layer->GetCustomLayerData()["mayaRig:diagnostics"].Get<VtStringArray>().empty());
        // Multi-chain deformers are elementwise: og[N] must resolve the
        // ip[N] shell, never the ip[0] shell of a sibling chain.
        auto elementLayer=SdfLayer::CreateAnonymous("element.ma");
        CHECK(elementLayer->ImportFromString(
            "createNode transform -n \"g0\"; createNode transform -n \"g1\"; "
            "createNode mesh -n \"shell0\" -p \"g0\"; "
            "setAttr \".vt[0:3]\" -type \"float3\" 0 0 0 1 0 0 1 1 0 0 1 0; "
            "setAttr \".ed[0:3]\" 0 1 0 1 2 0 2 3 0 0 3 0; "
            "setAttr \".fc[0]\" -type \"polyFaces\" f 4 0 1 2 -4; "
            "createNode mesh -n \"shell1\" -p \"g1\"; "
            "setAttr \".vt[0:3]\" -type \"float3\" 10 0 0 11 0 0 11 1 0 10 1 0; "
            "setAttr \".ed[0:3]\" 0 1 0 1 2 0 2 3 0 0 3 0; "
            "setAttr \".fc[0]\" -type \"polyFaces\" f 4 0 1 2 -4; "
            "createNode nonLinear -n \"bend\"; "
            "createNode transform -n \"h0\"; createNode mesh -n \"out0\" -p \"h0\"; "
            "createNode transform -n \"h1\"; createNode mesh -n \"out1\" -p \"h1\"; "
            "connectAttr \"shell0.w\" \"bend.ip[0].ig\"; connectAttr \"shell1.w\" \"bend.ip[1].ig\"; "
            "connectAttr \"bend.og[0]\" \"out0.i\"; connectAttr \"bend.og[1]\" \"out1.i\";"));
        auto element=UsdStage::Open(elementLayer); CHECK(element);
        int nearShell=0,farShell=0,elementMeshes=0;
        for(auto p:element->Traverse()) if(p.IsA<UsdGeomMesh>()) {
            ++elementMeshes; VtVec3fArray vertices;
            CHECK(UsdGeomMesh(p).GetPointsAttr().Get(&vertices)); CHECK(vertices.size()==4);
            if(vertices[1]==GfVec3f(1,0,0)) ++nearShell;
            else if(vertices[1]==GfVec3f(11,0,0)) ++farShell;
            else CHECK(false);
        }
        CHECK(elementMeshes==4 && nearShell==2 && farShell==2);
        // A skin binding survives through an unmodeled post-skin deformer,
        // recorded on the mesh instead of silently dropped.
        auto deformLayer=SdfLayer::CreateAnonymous("deform.ma");
        CHECK(deformLayer->ImportFromString(
            "createNode joint -n \"j\"; createNode transform -n \"g\"; "
            "createNode mesh -n \"shell\" -p \"g\"; "
            "setAttr \".vt[0:3]\" -type \"float3\" 0 0 0 1 0 0 1 1 0 0 1 0; "
            "setAttr \".ed[0:3]\" 0 1 0 1 2 0 2 3 0 0 3 0; "
            "setAttr \".fc[0]\" -type \"polyFaces\" f 4 0 1 2 -4; "
            "createNode skinCluster -n \"skin\"; "
            "setAttr \".pm[0]\" -type \"matrix\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1; "
            "setAttr \".wl[0:3].w\" 1 0 1 1 0 1 1 0 1 1 0 1; "
            "createNode ffd -n \"lattice\"; "
            "createNode transform -n \"h\"; createNode mesh -n \"out\" -p \"h\"; "
            "connectAttr \"j.wm[0]\" \"skin.ma[0]\"; connectAttr \"shell.w\" \"skin.ip[0].ig\"; "
            "connectAttr \"skin.og[0]\" \"lattice.ip[2].ig\"; connectAttr \"lattice.og[2]\" \"out.i\";"));
        auto deform=UsdStage::Open(deformLayer); CHECK(deform);
        int deformSkins=0;
        for(auto p:deform->Traverse()) if(p.GetTypeName()==TfToken("RigExecSkinMover")) ++deformSkins;
        CHECK(deformSkins==1);
        auto deformMesh=deform->GetPrimAtPath(SdfPath("/Rig/Geometry/n_7ch_7cout"));
        CHECK(deformMesh && deformMesh.IsA<UsdGeomMesh>());
        VtStringArray unsupported;
        CHECK(deformMesh.GetAttribute(TfToken("maya:unsupportedPostSkinFilters")).Get(&unsupported));
        CHECK(unsupported==VtStringArray({"lattice"}));
        bool citedDeform=false;
        for(const auto &d:deform->GetRootLayer()->GetCustomLayerData()["mayaRig:diagnostics"].Get<VtStringArray>())
            if(d.find("unsupported post-skin deformer")!=std::string::npos) citedDeform=true;
        CHECK(citedDeform);
        {
            std::ifstream input(std::string(argv[3])+"/liveBlend.ma");std::stringstream text;text<<input.rdbuf();
            auto source=text.str();const auto at=source.find("targetShape.w");CHECK(at!=std::string::npos);source.replace(at,std::string("targetShape.w").size(),"bodyShape.w");
            auto cyclic=SdfLayer::CreateAnonymous("cyclicBlend.ma");TfErrorMark errors;
            CHECK(!cyclic->ImportFromString(source));CHECK(!errors.IsClean());errors.Clear();
        }
        // A procedural input with no stored vertices anywhere upstream names
        // its blocker instead of failing silently.
        auto procedural=SdfLayer::CreateAnonymous("procedural.ma");
        CHECK(procedural->ImportFromString(
            "createNode polyDisc -n \"gen\"; createNode transform -n \"g\"; "
            "createNode mesh -n \"out\" -p \"g\"; connectAttr \"gen.output\" \"out.i\";"));
        bool citedProcedural=false;
        for(const auto &d:procedural->GetCustomLayerData()["mayaRig:diagnostics"].Get<VtStringArray>())
            if(d.find("no stored static vertices upstream")!=std::string::npos &&
               d.find("gen.output")!=std::string::npos) citedProcedural=true;
        CHECK(citedProcedural);
        CHECK(!UsdStage::Open(procedural)->GetPrimAtPath(SdfPath("/Rig/Geometry/n_7cg_7cout")));
        // Controller shapes belong to their native control. No authored
        // geometry or matrix mover is needed to draw/follow them.
        auto guideLayer=SdfLayer::CreateAnonymous("guide.ma");
        CHECK(guideLayer->ImportFromString(
            "createNode transform -n \"ctrl\"; createNode nurbsCurve -n \"shape\" -p \"ctrl\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 1 1 0 no 3 2 0 1 2 0 0 0 1 1 0;"));
        auto guide=UsdStage::Open(guideLayer); CHECK(guide);
        auto guideCurve=guide->GetPrimAtPath(SdfPath("/Rig/Geometry/n_7cctrl_7cshape"));
        CHECK(!guideCurve);
        auto nativeControl=guide->GetPrimAtPath(SdfPath("/Rig/Dag/nctrl"));
        TfToken guideShape,guidePurpose;
        CHECK(nativeControl.GetAttribute(TfToken("guide:shape")).Get(&guideShape)); CHECK(guideShape==TfToken("custom"));
        VtVec3fArray customPoints; VtIntArray customCounts;
        CHECK(nativeControl.GetAttribute(TfToken("guide:points")).Get(&customPoints));
        CHECK(customPoints==VtVec3fArray({GfVec3f(0,0,0),GfVec3f(1,1,0)}));
        CHECK(nativeControl.GetAttribute(TfToken("guide:curveVertexCounts")).Get(&customCounts)); CHECK(customCounts==VtIntArray({2}));
        CHECK(nativeControl.GetCustomDataByKey(TfToken("maya:controlShapes")).IsHolding<VtDictionary>());
        CHECK(!guide->GetPrimAtPath(SdfPath("/Rig/Source")));
        CHECK(guide->GetDefaultPrim().GetCustomDataByKey(TfToken("maya:sourceNodes")).Get<VtDictionary>().size()==2);
        CHECK(guideLayer->ImportFromString(
            "createNode transform -n \"ctrl\"; setAttr \".t\" -type \"double3\" 10 20 30; "
            "createNode nurbsCurve -n \"a\" -p \"ctrl\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 1 1 0 no 3 2 0 1 2 0 0 0 1 1 0; "
            "createNode nurbsCurve -n \"b\" -p \"ctrl\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 2 1 0 yes 3 4 0 0 1 1 3 1 0 0 1 1 1 0 0.7071067811865476 0 1 0 1;"));
        guide=UsdStage::Open(guideLayer);nativeControl=guide->GetPrimAtPath(SdfPath("/Rig/Dag/nctrl"));
        CHECK(nativeControl.GetAttribute(TfToken("guide:points")).Get(&customPoints));
        CHECK(nativeControl.GetAttribute(TfToken("guide:curveVertexCounts")).Get(&customCounts));
        CHECK(customCounts==VtIntArray({2,25}));
        CHECK((customPoints[14]-GfVec3f(std::sqrt(0.5),std::sqrt(0.5),0)).GetLength()<1e-6);
        CHECK(customPoints[0]==GfVec3f(0,0,0));
        // Geometry is flattened outside the DAG. Carry visibility through
        // every Maya ancestor so hidden construction rails do not draw or
        // inflate frame-all, while retaining their executable data.
        CHECK(guideLayer->ImportFromString(
            "createNode transform -n \"hidden\"; setAttr \".v\" no; "
            "createNode transform -n \"group\" -p \"hidden\"; "
            "createNode joint -n \"j\" -p \"group\"; "
            "createNode nurbsCurve -n \"rail\" -p \"group\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 1 1 0 no 3 2 0 1 2 0 0 0 1 1 0; "
            "createNode pointOnCurveInfo -n \"measure\"; connectAttr \"rail.ws\" \"measure.ic\";"));
        guide=UsdStage::Open(guideLayer);
        auto hiddenCurve=guide->GetPrimAtPath(SdfPath("/Rig/Geometry/n_7chidden_7cgroup_7crail"));
        CHECK(hiddenCurve);
        CHECK(UsdGeomImageable(hiddenCurve).ComputeVisibility()==TfToken("invisible"));
        double hiddenRadius=1.0;
        CHECK(guide->GetPrimAtPath(SdfPath("/Rig/Dag/nhidden/ngroup/nj")).GetAttribute(TfToken("guide:radius")).Get(&hiddenRadius));
        CHECK(hiddenRadius==0.0);
        // A curve consumed as rig data becomes a basis curve, still a
        // guide. Linear splines are exact b-splines; higher degrees are
        // exact Bezier decompositions (values verified against independent
        // de Boor / de Casteljau evaluation).
        auto basisLayer=SdfLayer::CreateAnonymous("basis.ma");
        CHECK(basisLayer->ImportFromString(
            "createNode transform -n \"rail\"; createNode nurbsCurve -n \"spline\" -p \"rail\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 1 1 0 no 3 2 0 1 2 0 0 0 1 1 0; "
            "createNode pointOnCurveInfo -n \"measure\"; "
            "connectAttr \"spline.ws\" \"measure.ic\"; "
            "createNode transform -n \"bend\"; createNode nurbsCurve -n \"arc\" -p \"bend\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 2 2 0 no 3 5 0 0 0.5 1 1 4 0 0 0 3 0 0 3 3 0 0 3 0; "
            "createNode pointOnCurveInfo -n \"probe\"; "
            "connectAttr \"arc.ws\" \"probe.ic\"; "
            "createNode transform -n \"back\"; createNode nurbsCurve -n \"column\" -p \"back\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 1 1 0 no 3 2 0 1 2 2 0 0 3 3 0; "
            "createNode skinCluster -n \"bendSkin\"; "
            "connectAttr \"column.ws\" \"bendSkin.ip[0].ig\";"));
        auto basis=UsdStage::Open(basisLayer); CHECK(basis);
        auto basisCurve=basis->GetPrimAtPath(SdfPath("/Rig/Geometry/n_7crail_7cspline")); CHECK(basisCurve);
        CHECK(basisCurve.GetTypeName()==TfToken("BasisCurves"));
        CHECK(basisCurve.GetAttribute(TfToken("purpose")).Get(&guidePurpose));
        CHECK(guidePurpose==TfToken("guide"));
        TfToken basisType,basisWrap;
        CHECK(basisCurve.GetAttribute(TfToken("type")).Get(&basisType)); CHECK(basisType==TfToken("linear"));
        CHECK(basisCurve.GetAttribute(TfToken("wrap")).Get(&basisWrap)); CHECK(basisWrap==TfToken("nonperiodic"));
        VtVec3fArray basisPoints; CHECK(basisCurve.GetAttribute(TfToken("points")).Get(&basisPoints));
        CHECK(basisPoints==VtVec3fArray({GfVec3f(0,0,0),GfVec3f(1,1,0)}));
        auto bezierCurve=basis->GetPrimAtPath(SdfPath("/Rig/Geometry/n_7cbend_7carc")); CHECK(bezierCurve);
        CHECK(bezierCurve.GetTypeName()==TfToken("BasisCurves"));
        CHECK(bezierCurve.GetAttribute(TfToken("type")).Get(&basisType)); CHECK(basisType==TfToken("cubic"));
        CHECK(bezierCurve.GetAttribute(TfToken("basis")).Get(&basisType)); CHECK(basisType==TfToken("bezier"));
        VtVec3fArray bezierPoints; CHECK(bezierCurve.GetAttribute(TfToken("points")).Get(&bezierPoints));
        CHECK(bezierPoints==VtVec3fArray({GfVec3f(0,0,0),GfVec3f(2,0,0),GfVec3f(3,0.5f,0),
            GfVec3f(3,1.5f,0),GfVec3f(3,2.5f,0),GfVec3f(2,3,0),GfVec3f(0,3,0)}));
        VtIntArray bezierCounts; CHECK(bezierCurve.GetAttribute(TfToken("curveVertexCounts")).Get(&bezierCounts));
        CHECK(bezierCounts==VtIntArray({7}));
        // A curve feeding a skin is rig data even in bind (Orig) state.
        auto skinnedCurve=basis->GetPrimAtPath(SdfPath("/Rig/Geometry/n_7cback_7ccolumn")); CHECK(skinnedCurve);
        CHECK(skinnedCurve.GetTypeName()==TfToken("BasisCurves"));
        // Cubic decomposition over two spans (same independent check).
        auto cubicLayer=SdfLayer::CreateAnonymous("cubic.ma");
        CHECK(cubicLayer->ImportFromString(
            "createNode transform -n \"rail\"; createNode nurbsCurve -n \"spline\" -p \"rail\"; "
            "setAttr \".cc\" -type \"nurbsCurve\" 3 2 0 no 3 7 0 0 0 0.5 1 1 1 5 "
            "0 0 0 4 0 0 4 4 0 0 4 0 0 8 0; "
            "createNode pointOnCurveInfo -n \"measure\"; "
            "connectAttr \"spline.ws\" \"measure.ic\";"));
        auto cubic=UsdStage::Open(cubicLayer); CHECK(cubic);
        auto cubicCurve=cubic->GetPrimAtPath(SdfPath("/Rig/Geometry/n_7crail_7cspline")); CHECK(cubicCurve);
        CHECK(cubicCurve.GetTypeName()==TfToken("BasisCurves"));
        CHECK(cubicCurve.GetAttribute(TfToken("type")).Get(&basisType)); CHECK(basisType==TfToken("cubic"));
        CHECK(cubicCurve.GetAttribute(TfToken("basis")).Get(&basisType)); CHECK(basisType==TfToken("bezier"));
        VtVec3fArray cubicPoints; CHECK(cubicCurve.GetAttribute(TfToken("points")).Get(&cubicPoints));
        CHECK(cubicPoints==VtVec3fArray({GfVec3f(0,0,0),GfVec3f(4,0,0),GfVec3f(4,2,0),GfVec3f(3,3,0),
            GfVec3f(2,4,0),GfVec3f(0,4,0),GfVec3f(0,8,0)}));
        std::cout << "usdMayaRig integration checks passed\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
