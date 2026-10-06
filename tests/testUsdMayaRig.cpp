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
        std::cout << "usdMayaRig integration checks passed\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
