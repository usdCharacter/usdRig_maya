#include "rigExec/rigEvaluator.h"
#include "pxr/base/plug/registry.h"
#include "pxr/usd/usd/primRange.h"
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
PXR_NAMESPACE_USING_DIRECTIVE
#define CHECK(x) do { if(!(x)) throw std::runtime_error("check failed: " #x); } while(false)
int main(int argc,char **argv) {
    try {
        CHECK(argc==4);
        PlugRegistry::GetInstance().RegisterPlugins(argv[1]);
        PlugRegistry::GetInstance().RegisterPlugins(argv[2]);
        auto stage=UsdStage::Open(std::string(argv[3])+"/rig.ma"); CHECK(stage);
        rigExec::RigExecRigEvaluator evaluator(stage,SdfPath("/Rig"));
        std::vector<std::string> errors; bool compiled=evaluator.Compile(&errors);
        for(const auto &e:errors) std::cerr<<e<<'\n'; CHECK(compiled); CHECK(errors.empty());
        auto pose=evaluator.Evaluate(UsdTimeCode::Default());
        for(const auto &e:pose.diagnostics) std::cerr<<e<<'\n'; CHECK(pose.valid);
        SdfPath points("/Rig/Geometry/n_7ccharacter_7cbody_7cbodyShape.points");
        CHECK(pose.movedProperties.count(points));
        auto rest=pose.movedProperties.at(points).Get<VtVec3fArray>();
        CHECK(rest.size()==4); CHECK((rest[2]-GfVec3f(1,2,0)).GetLength()<1e-5);
        // Compose an editable USD overlay over the read-only source format.
        auto overlay=SdfLayer::CreateAnonymous("pose.usda");
        stage->GetSessionLayer()->GetSubLayerPaths().push_back(overlay->GetIdentifier());
        stage->SetEditTarget(overlay);
        CHECK(stage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(2.0));
        pose=evaluator.Evaluate(UsdTimeCode::Default());
        for(const auto &e:pose.diagnostics) std::cerr<<e<<'\n'; CHECK(pose.valid);
        auto moved=pose.movedProperties.at(points).Get<VtVec3fArray>();
        CHECK((moved[0]-GfVec3f(0,0,0)).GetLength()<1e-5);
        CHECK((moved[2]-GfVec3f(3,2,0)).GetLength()<1e-5);
        CHECK((moved[3]-GfVec3f(1.5,2,0)).GetLength()<1e-5);
        CHECK(stage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(0.0));
        CHECK(stage->GetPrimAtPath(SdfPath("/Rig/Dag/nmaster")).GetAttribute(TfToken("avars:tx")).Set(3.0));
        pose=evaluator.Evaluate(UsdTimeCode::Default()); CHECK(pose.valid);
        moved=pose.movedProperties.at(points).Get<VtVec3fArray>();
        CHECK((moved[0]-GfVec3f(3,0,0)).GetLength()<1e-5);
        CHECK((moved[2]-GfVec3f(4,2,0)).GetLength()<1e-5);
        CHECK(stage->GetPrimAtPath(SdfPath("/Rig/Dag/nmaster")).GetAttribute(TfToken("avars:tx")).Set(0.0));
        CHECK(stage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/nroot")).GetAttribute(TfToken("avars:sx")).Set(2.0));
        pose=evaluator.Evaluate(UsdTimeCode::Default()); CHECK(pose.valid);
        moved=pose.movedProperties.at(points).Get<VtVec3fArray>();
        CHECK((moved[1]-GfVec3f(2,0,0)).GetLength()<1e-5);
        std::cout<<"Control -> constraint -> bind proxy -> skin evaluation passed\n";
        {
            auto live=UsdStage::Open(std::string(argv[3])+"/liveBlend.ma");CHECK(live);
            live->SetEditTarget(live->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(live,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            const SdfPath output("/Rig/Geometry/n_7cbody_7cbodyShape.points");
            auto sample=[&]() {auto p=e.Evaluate(UsdTimeCode::Default());CHECK(p.valid);return p.movedProperties.at(output).Get<VtVec3fArray>();};
            auto rest=sample();
            CHECK((rest[0]-GfVec3f(0,.2,0)).GetLength()<1e-5);
            CHECK((rest[2]-GfVec3f(1,2,.5)).GetLength()<1e-5);
            auto driver=live->GetPrimAtPath(SdfPath("/Rig/Dag/ndriver"));
            CHECK(driver.GetAttribute(TfToken("avars:maya_nclose")).Set(.5));
            auto changed=sample();CHECK((changed[0]-GfVec3f(0,.1,0)).GetLength()<1e-5);
            CHECK(driver.GetAttribute(TfToken("avars:maya_nblend")).Set(1.0));
            changed=sample();CHECK((changed[2]-GfVec3f(1,2,1)).GetLength()<1e-5);
            CHECK(live->GetPrimAtPath(SdfPath("/Rig/Dag/ninfluence")).GetAttribute(TfToken("avars:tx")).Set(3.0));
            changed=sample();CHECK((changed[2]-GfVec3f(4,2,1)).GetLength()<1e-5);
            CHECK(driver.GetAttribute(TfToken("avars:maya_nblend")).Set(.5));
            CHECK(driver.GetAttribute(TfToken("avars:maya_nclose")).Set(0.0));
            CHECK(live->GetPrimAtPath(SdfPath("/Rig/Dag/ninfluence")).GetAttribute(TfToken("avars:tx")).Set(0.0));
            CHECK(sample()==rest);
            CHECK(live->GetPrimAtPath(SdfPath("/Rig/Dag/ntarget")).GetAttribute(TfToken("avars:tx")).Set(1.0));
            CHECK(sample()==rest);
            CHECK(live->GetPrimAtPath(SdfPath("/Rig/Dag/ntarget")).GetAttribute(TfToken("avars:tx")).Set(0.0));
            CHECK(driver.GetAttribute(TfToken("avars:maya_nenvelope")).Set(0.0));changed=sample();
            CHECK((changed[0]-GfVec3f(0,.05,0)).GetLength()<1e-5);CHECK((changed[2]-GfVec3f(1,2,.125)).GetLength()<1e-5);
            CHECK(driver.GetAttribute(TfToken("avars:maya_nenvelope")).Set(1.0));CHECK(sample()==rest);
            std::cout<<"Live cluster/remap/mesh tweaks -> local-origin blend -> skin parity passed\n";
        }
        {
            auto live=UsdStage::Open(std::string(argv[3])+"/liveFfd.ma");CHECK(live);
            live->SetEditTarget(live->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(live,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            const SdfPath output("/Rig/Geometry/n_7cbody_7cbodyShape.points");
            auto sample=[&]() {auto p=e.Evaluate(UsdTimeCode::Default());CHECK(p.valid);return p.movedProperties.at(output).Get<VtVec3fArray>();};
            const auto rest=sample();CHECK((rest[0]-GfVec3f(0,2,0)).GetLength()<1e-5);CHECK((rest[1]-GfVec3f(2,2,0)).GetLength()<1e-5);
            auto handle=live->GetPrimAtPath(SdfPath("/Rig/Dag/nhandleParent/nhandle"));CHECK(handle.GetAttribute(TfToken("avars:tz")).Set(.4));
            auto changed=sample();CHECK((changed[0]-GfVec3f(0,2,.1)).GetLength()<1e-5);CHECK((changed[1]-GfVec3f(2,2,.3)).GetLength()<1e-5);CHECK((changed[2]-GfVec3f(1,3,1.8)).GetLength()<1e-5);CHECK(changed[3]==rest[3]);
            CHECK(live->GetPrimAtPath(SdfPath("/Rig/Dag/nhandleParent")).GetAttribute(TfToken("avars:tx")).Set(20.0));CHECK(sample()==changed);
            CHECK(handle.GetAttribute(TfToken("avars:tz")).Set(0.0));CHECK(sample()==rest);
            std::cout<<"Live two-by-two Maya FFD: relative tagged corners and outside extrapolation passed\n";
        }
        {
            auto live=UsdStage::Open(std::string(argv[3])+"/mirrorConstraint.ma");CHECK(live);live->SetEditTarget(live->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(live,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            const SdfPath target("/Rig/Dag/ntarget"),follower("/Rig/Dag/nfollower");
            auto frame=pose.controlFrames.at(target);CHECK((frame.Origin()-GfVec3d(3,0,0)).GetLength()<1e-8);CHECK((frame.X()-frame.Origin()-GfVec3d(1,0,0)).GetLength()<1e-8);CHECK((frame.Y()-frame.Origin()-GfVec3d(0,1,0)).GetLength()<1e-8);
            frame=pose.controlFrames.at(follower);const double c=.984807753012208,s=.17364817766693033;
            CHECK((frame.Origin()-GfVec3d(-3,3,0)).GetLength()<1e-8);CHECK((frame.X()-frame.Origin()-GfVec3d(c,-s,0)).GetLength()<1e-8);CHECK((frame.Y()-frame.Origin()-GfVec3d(-s,-c,0)).GetLength()<1e-8);CHECK((frame.Z()-frame.Origin()-GfVec3d(0,0,-1)).GetLength()<1e-8);
            auto source=live->GetPrimAtPath(SdfPath("/Rig/Dag/nsource1"));CHECK(source.GetAttribute(TfToken("avars:rz")).Set(25.0));
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);frame=pose.controlFrames.at(target);
            CHECK((frame.X()-frame.Origin()-GfVec3d(.9063077870366499,.42261826174069944,0)).GetLength()<1e-8);
            CHECK(source.GetAttribute(TfToken("avars:sx")).Set(.5));pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);CHECK(pose.controlFrames.at(target)==frame);
            std::cout<<"Maya mirrored parent constraints preserve local rotation independently of source scale\n";
        }
        {
            auto live=UsdStage::Open(std::string(argv[3])+"/segmentScale.ma");CHECK(live);live->SetEditTarget(live->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(live,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            const SdfPath tip("/Rig/Dag/nroot/nchild/ntip");
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);auto frame=pose.jointFramesFinal.at(tip);
            CHECK((frame.Origin()-GfVec3d(7.5,0,0)).GetLength()<1e-8);CHECK((frame.X()-frame.Origin()-GfVec3d(1,0,0)).GetLength()<1e-8);
            auto scale=live->GetPrimAtPath(SdfPath("/Rig/Dag/nroot")).GetAttribute(TfToken("avars:sx"));CHECK(scale.Set(.25));
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);frame=pose.jointFramesFinal.at(tip);CHECK((frame.Origin()-GfVec3d(3,0,0)).GetLength()<1e-8);CHECK((frame.X()-frame.Origin()-GfVec3d(1,0,0)).GetLength()<1e-8);
            CHECK(scale.Set(0.0));pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);frame=pose.jointFramesFinal.at(tip);
            CHECK((frame.Origin()-GfVec3d(1.500000000003,0,0)).GetLength()<1e-8);CHECK((frame.X()-frame.Origin()-GfVec3d(1,0,0)).GetLength()<1e-8);
            CHECK(scale.Set(1.0));pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);CHECK((pose.jointFramesFinal.at(tip).Origin()-GfVec3d(7.5,0,0)).GetLength()<1e-8);
            std::ifstream input(std::string(argv[3])+"/segmentScale.ma");std::stringstream text;text<<input.rdbuf();auto source=text.str();
            auto at=source.find("\".sx\" 2");CHECK(at!=std::string::npos);source.replace(at,std::string("\".sx\" 2").size(),"\".sx\" 0");
            auto layer=SdfLayer::CreateAnonymous("zeroSegmentScale.ma");CHECK(layer->ImportFromString(source));auto zero=UsdStage::Open(layer);zero->SetEditTarget(zero->GetSessionLayer());
            rigExec::RigExecRigEvaluator z(zero,SdfPath("/Rig"));CHECK(z.Compile(&errors));pose=z.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.jointFramesFinal.at(tip).Origin()-GfVec3d(1.500000000003,0,0)).GetLength()<1e-8);
            CHECK(zero->GetPrimAtPath(SdfPath("/Rig/Dag/nroot")).GetAttribute(TfToken("avars:sx")).Set(.5));pose=z.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.jointFramesFinal.at(tip).Origin()-GfVec3d(3,0,0)).GetLength()<1e-8);
            std::cout<<"Maya joint segment-scale compensation follows live parent scale including zero\n";
        }
        {
            auto native=UsdStage::Open(std::string(argv[3])+"/rig.ma");
            native->SetEditTarget(native->GetSessionLayer());
            auto pin=native->DefinePrim(SdfPath("/Rig/Pin"),TfToken("RigExecNurbsSurfacePin"));
            auto set=[&](const char *name,const SdfValueTypeName &type,const auto &value){pin.CreateAttribute(TfToken(name),type).Set(value);};
            set("inputs:points",SdfValueTypeNames->Double3Array,VtVec3dArray{{0,0,0},{0,0,2},{2,0,0},{2,0,2}});
            set("inputs:rationalWeights",SdfValueTypeNames->DoubleArray,VtDoubleArray{1,1,1,1});
            for(const char *axis:{"u","v"}) {
                set((std::string("inputs:")+axis+"Knots").c_str(),SdfValueTypeNames->DoubleArray,VtDoubleArray{0,0,1,1});
                set((std::string("inputs:")+axis+"Degree").c_str(),SdfValueTypeNames->Int,1);
                set((std::string("inputs:")+axis+"Count").c_str(),SdfValueTypeNames->Int,2);
                set((std::string("inputs:")+axis).c_str(),SdfValueTypeNames->Double,0.5);
            }
            set("inputs:normalAxis",SdfValueTypeNames->Int,0);set("inputs:tangentAxis",SdfValueTypeNames->Int,1);
            set("inputs:jointIndices",SdfValueTypeNames->IntArray,VtIntArray{0,0,0,0});
            set("inputs:jointWeights",SdfValueTypeNames->FloatArray,VtFloatArray{1,1,1,1});
            set("inputs:elementSize",SdfValueTypeNames->Int,1);set("inputs:local",SdfValueTypeNames->Matrix4d,GfMatrix4d(1));
            SdfPath tip("/Rig/Dag/ncharacter/nroot/ntip");
            pin.CreateRelationship(TfToken("rigExec:influences")).SetTargets({tip});
            pin.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets({tip});
            auto output=pin.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);
            auto pinned=native->DefinePrim(SdfPath("/Rig/Pinned"),TfToken("RigExecControl"));
            pinned.GetAttribute(TfToken("posed:space")).SetConnections({output.GetPath()});
            auto follower=native->DefinePrim(SdfPath("/Rig/Follower"),TfToken("RigExecJoint"));
            auto follow=native->DefinePrim(SdfPath("/Rig/Movers/pinFollower"),TfToken("RigExecPositionConstraint"));
            follow.CreateRelationship(TfToken("rigExec:moves")).SetTargets({follower.GetPath()});
            follow.CreateRelationship(TfToken("rigExec:sources")).SetTargets({pinned.GetPath()});
            auto movers=native->GetPrimAtPath(SdfPath("/Rig/Movers"));
            auto order=movers.GetChildrenReorder();order.insert(order.begin(),TfToken("pinFollower"));movers.SetChildrenReorder(order);
            rigExec::RigExecRigEvaluator e(native,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto before=e.Evaluate(UsdTimeCode::Default());CHECK(before.valid);
            CHECK((before.jointFramesFinal.at(follower.GetPath()).Origin()-GfVec3d(1,0,1)).GetLength()<1e-5);
            native->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(3.0);
            auto after=e.Evaluate(UsdTimeCode::Default());CHECK(after.valid);
            CHECK((after.jointFramesFinal.at(follower.GetPath()).Origin()-GfVec3d(4,0,1)).GetLength()<1e-5);
            std::cout<<"Native NURBS pin reads constrained skin influences live\n";
        }
        // A visible shape reached through geometry-history nodes must retain
        // its skin, weights and geometry bind placement. This was the rhino
        // failure: the controls moved but only direct mesh outputs were bound.
        std::ifstream input(std::string(argv[3])+"/rig.ma");
        std::stringstream buffer; buffer<<input.rdbuf();
        {
            std::string text=buffer.str();
            const std::string direct="connectAttr \"skin.og[0]\" \"bodyShape.i\";";
            auto at=text.find(direct);CHECK(at!=std::string::npos);
            text.replace(at,direct.size(),
                "connectAttr \"skin.og[0]\" \"intermediate.i\"; "
                "connectAttr \"intermediate.w\" \"postSkin.ip[0].ig\"; "
                "connectAttr \"postSkin.og[0]\" \"bodyShape.i\";");
            text+=R"(
                createNode mesh -n "intermediate" -p "body";
                    setAttr ".io" yes;
                    setAttr ".vt[0:3]" -type "float3" 100 0 0 101 0 0 101 2 0 100 2 0;
                createNode transform -n "tweakControl";
                createNode joint -n "tweak";
                createNode skinCluster -n "postSkin";
                    setAttr ".pm[0]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
                    setAttr ".wl[0:3].w" 1 0 1 1 0 1 1 0 1 1 0 1;
                connectAttr "tweakControl.t" "tweak.t";
                connectAttr "tweak.wm[0]" "postSkin.ma[0]";
            )";
            auto layer=SdfLayer::CreateAnonymous("serialSkin.ma");CHECK(layer->ImportFromString(text));
            auto serial=UsdStage::Open(layer);serial->SetEditTarget(serial->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(serial,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto p=e.Evaluate(UsdTimeCode::Default());CHECK(p.valid);
            CHECK((p.movedProperties.at(points).Get<VtVec3fArray>()[2]-GfVec3f(1,2,0)).GetLength()<1e-5);
            serial->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(2.0);
            p=e.Evaluate(UsdTimeCode::Default());CHECK(p.valid);
            CHECK((p.movedProperties.at(points).Get<VtVec3fArray>()[2]-GfVec3f(3,2,0)).GetLength()<1e-5);
            serial->GetPrimAtPath(SdfPath("/Rig/Dag/ntweakControl")).GetAttribute(TfToken("avars:tx")).Set(3.0);
            p=e.Evaluate(UsdTimeCode::Default());CHECK(p.valid);
            CHECK((p.movedProperties.at(points).Get<VtVec3fArray>()[2]-GfVec3f(6,2,0)).GetLength()<1e-5);
            CHECK((p.movedProperties.at(points).Get<VtVec3fArray>()[0]-GfVec3f(3,0,0)).GetLength()<1e-5);
            // Facial tweak skins rebase against their moving attachment frame.
            // Moving that frame must cancel in the second skin, rather than
            // applying the attachment's motion to already-skinned points again.
            auto dynamicText=text;
            const std::string joint="createNode joint -n \"tweak\";";
            auto jointAt=dynamicText.find(joint);CHECK(jointAt!=std::string::npos);
            dynamicText.replace(jointAt,joint.size(),
                "createNode transform -n \"bindSpace\"; setAttr \".tx\" 10; "
                "createNode joint -n \"tweak\" -p \"bindSpace\";");
            dynamicText+="connectAttr \"bindSpace.wim\" \"postSkin.pm[0]\";";
            auto dynamicLayer=SdfLayer::CreateAnonymous("liveBind.ma");CHECK(dynamicLayer->ImportFromString(dynamicText));
            auto dynamicStage=UsdStage::Open(dynamicLayer);dynamicStage->SetEditTarget(dynamicStage->GetSessionLayer());
            rigExec::RigExecRigEvaluator dynamic(dynamicStage,SdfPath("/Rig"));CHECK(dynamic.Compile(&errors));
            auto unchanged=dynamic.Evaluate(UsdTimeCode::Default());CHECK(unchanged.valid);
            auto baseline=unchanged.movedProperties.at(points).Get<VtVec3fArray>();
            CHECK((baseline[2]-GfVec3f(1,2,0)).GetLength()<1e-5);
            dynamicStage->GetPrimAtPath(SdfPath("/Rig/Dag/nbindSpace")).GetAttribute(TfToken("avars:tx")).Set(7.0);
            unchanged=dynamic.Evaluate(UsdTimeCode::Default());CHECK(unchanged.valid);
            CHECK(unchanged.movedProperties.at(points).Get<VtVec3fArray>()==baseline);
            dynamicStage->GetPrimAtPath(SdfPath("/Rig/Dag/ntweakControl")).GetAttribute(TfToken("avars:tx")).Set(3.0);
            unchanged=dynamic.Evaluate(UsdTimeCode::Default());CHECK(unchanged.valid);
            CHECK((unchanged.movedProperties.at(points).Get<VtVec3fArray>()[2]-GfVec3f(4,2,0)).GetLength()<1e-5);
            std::cout<<"Serial skins preserve live intermediate geometry and ignore cached points\n";
        }
        {
            auto text=buffer.str()+R"(
                createNode joint -n "unusedInfluence";
                    setAttr ".tx" 100;
                connectAttr "unusedInfluence.wm" "skin.ma[7]";
                setAttr "skin.pm[7]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
                setAttr "skin.wl[0].w[7]" 0;
                createNode uvPin -n "meshPin";
                    setAttr ".coord[0]" -type "double2" 0.5 0.5;
                    setAttr ".nrm" 2;
                    setAttr ".tng" 0;
                createNode transform -n "meshPinned";
                connectAttr "bodyShape.w" "meshPin.curgeom";
                connectAttr "meshPin.omat[0]" "meshPinned.opm";
                createNode transform -n "cancel" -p "control";
                connectAttr "control.im" "cancel.opm";
                createNode composeMatrix -n "pivotPlacement";
                    setAttr ".it" -type "double3" 10 0 0;
                createNode transform -n "matrixPivot";
                connectAttr "pivotPlacement.omat" "matrixPivot.opm";
                createNode transform -n "matrixPivotCancel" -p "matrixPivot";
                connectAttr "matrixPivot.im" "matrixPivotCancel.opm";
            )";
            auto layer=SdfLayer::CreateAnonymous("meshPin.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            for(const auto &p:stage->Traverse())if(p.GetTypeName()==TfToken("RigExecMeshPin")) {
                SdfPathVector refs;CHECK(p.GetRelationship(TfToken("rigExec:influences")).GetTargets(&refs));
                CHECK(refs.size()==2);
            }
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            SdfPath pin("/Rig/Dag/nmeshPinned"),cancel("/Rig/Dag/ncharacter/ncontrol/ncancel");
            CHECK((pose.controlFrames.at(pin).Origin()-GfVec3d(0.5,1,0)).GetLength()<1e-5);
            CHECK(pose.controlFrames.at(cancel).Origin().GetLength()<1e-5);
            SdfPath matrixCancel("/Rig/Dag/nmatrixPivot/nmatrixPivotCancel");
            CHECK((pose.controlFrames.at(matrixCancel).Origin()-GfVec3d(10,0,0)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(2.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.controlFrames.at(pin).Origin()-GfVec3d(1.5,1,0)).GetLength()<1e-5);
            CHECK(pose.controlFrames.at(cancel).Origin().GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/nmatrixPivot")).GetAttribute(TfToken("avars:rz")).Set(90.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.controlFrames.at(matrixCancel).Origin()-GfVec3d(10,0,0)).GetLength()<1e-5);
            std::cout<<"Native mesh UV pin and local inverse-matrix cancellation update live\n";
        }
        {
            auto text=buffer.str()+R"(
                createNode transform -n "pivotDriver";
                    setAttr ".rp" -type "double3" 10 20 0;
                createNode joint -n "pivotFollower";
                createNode pointConstraint -n "pivotConstraint";
                connectAttr "pivotDriver.t" "pivotConstraint.tg[0].tt";
                connectAttr "pivotDriver.rp" "pivotConstraint.tg[0].trp";
                connectAttr "pivotDriver.pm" "pivotConstraint.tg[0].tpm";
                connectAttr "pivotConstraint.ct" "pivotFollower.t";
            )";
            auto layer=SdfLayer::CreateAnonymous("pivot.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            SdfPath follower("/Rig/Dag/npivotFollower");
            CHECK((pose.jointFramesFinal.at(follower).Origin()-GfVec3d(10,20,0)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/npivotDriver")).GetAttribute(TfToken("avars:tx")).Set(3.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.jointFramesFinal.at(follower).Origin()-GfVec3d(13,20,0)).GetLength()<1e-5);
            std::cout<<"Maya constraints follow the rotate pivot rather than the DAG origin\n";
        }
        {
            auto text=buffer.str()+R"(
                createNode transform -n "restParent";
                    setAttr ".t" -type "double3" 10 0 0;
                createNode transform -n "restTarget" -p "restParent";
                    setAttr ".rp" -type "double3" 1 0 0;
                createNode transform -n "restChild" -p "restTarget";
                    setAttr ".t" -type "double3" 1 0 0;
                createNode transform -n "restDriver";
                    addAttr -ln "follow" -at "double" -dv 0;
                    setAttr ".t" -type "double3" 100 0 0;
                createNode parentConstraint -n "zeroWeightRest";
                    setAttr ".erp" yes;
                    setAttr ".rst" -type "double3" 2 3 4;
                    setAttr ".rsr" -type "double3" 0 0 90;
                connectAttr "restDriver.t" "zeroWeightRest.tg[0].tt";
                connectAttr "restDriver.pm" "zeroWeightRest.tg[0].tpm";
                connectAttr "restDriver.follow" "zeroWeightRest.tg[0].tw";
                connectAttr "zeroWeightRest.ct" "restTarget.t";
                connectAttr "zeroWeightRest.cr" "restTarget.r";
            )";
            auto layer=SdfLayer::CreateAnonymous("constraintRest.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            const SdfPath target("/Rig/Dag/nrestParent/nrestTarget"),child=target.AppendChild(TfToken("nrestChild"));
            CHECK((pose.controlFrames.at(target).Origin()-GfVec3d(13,3,4)).GetLength()<1e-5);
            CHECK((pose.controlFrames.at(child).Origin()-GfVec3d(13,3,4)).GetLength()<1e-5);
            CHECK((pose.controlFrames.at(target).X()-pose.controlFrames.at(target).Origin()-GfVec3d(0,1,0)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/nrestDriver")).GetAttribute(TfToken("avars:tx")).Set(20.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.controlFrames.at(target).Origin()-GfVec3d(13,3,4)).GetLength()<1e-5);
            std::cout<<"Zero-weight Maya constraints preserve explicit rest channels, pivots and descendants\n";
        }
        {
            auto text=buffer.str()+R"(
                createNode transform -n "blendDriver";
                    setAttr ".t" -type "double3" 10 20 0;
                createNode transform -n "blendSource";
                createNode pointConstraint -n "blendConstraint";
                connectAttr "blendDriver.t" "blendConstraint.tg[0].tt";
                connectAttr "blendDriver.pm" "blendConstraint.tg[0].tpm";
                connectAttr "blendConstraint.ct" "blendSource.t";
                createNode blendColors -n "positionBlend";
                    setAttr ".c2" -type "float3" 2 4 0;
                createNode transform -n "blendSettings";
                    addAttr -ln "blend" -sn "blend" -at "double" -dv 0.75;
                createNode transform -n "blendRelay";
                    addAttr -ln "blend" -sn "blend" -at "double";
                connectAttr "blendSettings.blend" "blendRelay.blend";
                connectAttr "blendRelay.blend" "positionBlend.b";
                connectAttr "blendSource.t" "positionBlend.c1";
                createNode transform -n "blendTarget";
                connectAttr "positionBlend.op" "blendTarget.t";
                createNode joint -n "blendFollower";
                createNode pointConstraint -n "blendFollow";
                connectAttr "blendTarget.t" "blendFollow.tg[0].tt";
                connectAttr "blendTarget.pm" "blendFollow.tg[0].tpm";
                connectAttr "blendFollow.ct" "blendFollower.t";
            )";
            auto layer=SdfLayer::CreateAnonymous("channelBlend.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            SdfPath follower("/Rig/Dag/nblendFollower");
            CHECK((pose.jointFramesFinal.at(follower).Origin()-GfVec3d(8,16,0)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/nblendDriver")).GetAttribute(TfToken("avars:tx")).Set(4.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.jointFramesFinal.at(follower).Origin()-GfVec3d(11,16,0)).GetLength()<1e-5);
            std::cout<<"Maya channel blends consume evaluated constraint output and invalidate live\n";
        }
        {
            auto text=buffer.str()+R"(
                createNode transform -n "pairDriver";
                    setAttr ".t" -type "double3" 10 20 0;
                createNode transform -n "pairTarget";
                    addAttr -ln "blendParent" -sn "blendParent" -at "double" -dv 1;
                createNode pointConstraint -n "pairConstraint";
                connectAttr "pairDriver.t" "pairConstraint.tg[0].tt";
                connectAttr "pairDriver.pm" "pairConstraint.tg[0].tpm";
                createNode pairBlend -n "pair";
                connectAttr "pairConstraint.ctx" "pair.itx2";
                connectAttr "pairConstraint.cty" "pair.ity2";
                connectAttr "pairConstraint.ctz" "pair.itz2";
                connectAttr "pairTarget.blendParent" "pair.w";
                connectAttr "pair.otx" "pairTarget.tx";
                connectAttr "pair.oty" "pairTarget.ty";
                connectAttr "pair.otz" "pairTarget.tz";
            )";
            auto layer=SdfLayer::CreateAnonymous("pairBlend.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            SdfPath target("/Rig/Dag/npairTarget");
            CHECK((pose.controlFrames.at(target).Origin()-GfVec3d(10,20,0)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/npairDriver")).GetAttribute(TfToken("avars:tx")).Set(4.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.controlFrames.at(target).Origin()-GfVec3d(14,20,0)).GetLength()<1e-5);
            std::cout<<"PairBlend translation preserves default blend weight and constraint inputs\n";
        }
        {
            auto text=buffer.str()+R"(
                createNode transform -n "curveRig";
                createNode nurbsCurve -n "sampleCurveOrig" -p "curveRig";
                    setAttr ".io" yes;
                    setAttr ".cc" -type "nurbsCurve" 1 1 0 no 3 2 0 1 2 0 0 0 2 0 0;
                createNode nurbsCurve -n "sampleCurve" -p "curveRig";
                createNode skinCluster -n "curveSkin";
                    setAttr ".pm[0]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 -2 0 1;
                    setAttr ".wl[0:1].w" 1 0 1 1 0 1;
                connectAttr "sampleCurveOrig.ws" "curveSkin.ip[0].ig";
                connectAttr "tip.wm" "curveSkin.ma[0]";
                connectAttr "curveSkin.og[0]" "sampleCurve.cr";
                createNode pointOnCurveInfo -n "curveSample";
                    setAttr ".pr" 0.25;
                    setAttr ".top" yes;
                connectAttr "sampleCurve.ws" "curveSample.ic";
                createNode transform -n "curveSampled";
                connectAttr "curveSample.p" "curveSampled.t";
            )";
            auto layer=SdfLayer::CreateAnonymous("curveSample.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            SdfPath sampled("/Rig/Dag/ncurveSampled");
            CHECK((pose.controlFrames.at(sampled).Origin()-GfVec3d(0.5,0,0)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(3.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.controlFrames.at(sampled).Origin()-GfVec3d(3.5,0,0)).GetLength()<1e-5);
            std::cout<<"Curve samplers read the live skinned CVs through missing-cache history\n";
        }
        {
            // Maya's stretch network: live curve samples -> world distance ->
            // floatMath -> sparse plusMinusAverage -> joint translation.
            auto text=buffer.str()+R"(
                createNode transform -n "stretchSettings";
                    addAttr -ln "stretch" -at "double" -dv 0;
                    addAttr -ln "globalScale" -at "double" -dv 1;
                createNode transform -n "stretchCurveRig";
                createNode nurbsCurve -n "stretchOrig" -p "stretchCurveRig";
                    setAttr ".io" yes;
                    setAttr ".cc" -type "nurbsCurve" 1 1 0 no 3 2 0 1 2 0 0 0 2 0 0;
                createNode nurbsCurve -n "stretchCurve" -p "stretchCurveRig";
                createNode skinCluster -n "stretchSkin";
                    setAttr ".pm[0]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
                    setAttr ".pm[3]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 -2 0 1;
                    setAttr ".wl[0].w[0]" 1;
                    setAttr ".wl[1].w[3]" 1;
                connectAttr "stretchOrig.ws" "stretchSkin.ip[0].ig";
                connectAttr "root.wm" "stretchSkin.ma[0]";
                connectAttr "tip.wm" "stretchSkin.ma[3]";
                connectAttr "stretchSkin.og[0]" "stretchCurve.cr";
                createNode motionPath -n "sampleStart";
                createNode motionPath -n "sampleEnd";
                    setAttr ".u" 1;
                connectAttr "stretchCurve.ws" "sampleStart.gp";
                connectAttr "stretchCurve.ws" "sampleEnd.gp";
                createNode transform -n "stretchStart";
                createNode transform -n "stretchEnd";
                connectAttr "sampleStart.ac" "stretchStart.t";
                connectAttr "sampleEnd.ac" "stretchEnd.t";
                createNode distanceBetween -n "stretchDistance";
                connectAttr "stretchStart.wm" "stretchDistance.im1";
                connectAttr "stretchEnd.wm" "stretchDistance.im2";
                createNode floatMath -n "normalizeLength";
                    setAttr "._cnd" 3;
                    setAttr "._fb" 2;
                connectAttr "stretchDistance.d" "normalizeLength._fa";
                createNode floatMath -n "subtractScale";
                    setAttr "._cnd" 1;
                connectAttr "normalizeLength.of" "subtractScale._fa";
                connectAttr "stretchSettings.globalScale" "subtractScale._fb";
                createNode floatMath -n "autoStretch";
                    setAttr "._cnd" 2;
                connectAttr "subtractScale.of" "autoStretch._fa";
                connectAttr "stretchSettings.stretch" "autoStretch._fb";
                createNode plusMinusAverage -n "stretchSum";
                    setAttr ".i3[0:1]" -type "float3" 0 0 0 1 0 0;
                connectAttr "autoStretch.of" "stretchSum.i3[0].i3x";
                connectAttr "stretchSettings.globalScale" "stretchSum.i3[1].i3x";
                createNode multDoubleLinear -n "jointLength";
                    setAttr ".i2" 2;
                connectAttr "stretchSum.o3x" "jointLength.i1";
                createNode joint -n "stretchJoint";
                connectAttr "jointLength.o" "stretchJoint.tx";
                createNode plusMinusAverage -n "sparseAverage";
                    setAttr ".op" 3;
                    setAttr ".i3[2]" -type "float3" 2 9 0;
                    setAttr ".i3[5]" -type "float3" 5 0 0;
                    setAttr ".i3[9].i3y" 6;
                connectAttr "stretchSettings.globalScale" "sparseAverage.i3[5].i3x";
                createNode transform -n "averageTarget";
                connectAttr "sparseAverage.o3" "averageTarget.t";
            )";
            auto layer=SdfLayer::CreateAnonymous("stretchNetwork.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            const SdfPath joint("/Rig/Dag/nstretchJoint"),settings("/Rig/Dag/nstretchSettings"),average("/Rig/Dag/naverageTarget");
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK(std::abs(pose.jointFramesFinal.at(joint).Origin()[0]-2)<1e-5);
            CHECK((pose.controlFrames.at(average).Origin()-GfVec3d(1,5,0)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(2.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK(std::abs(pose.jointFramesFinal.at(joint).Origin()[0]-2)<1e-5);
            CHECK(stage->GetPrimAtPath(settings).GetAttribute(TfToken("avars:maya_nstretch")).Set(1.0));
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK(std::abs(pose.jointFramesFinal.at(joint).Origin()[0]-4)<1e-5);
            CHECK(stage->GetPrimAtPath(settings).GetAttribute(TfToken("avars:maya_nglobalScale")).Set(2.0));
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK(std::abs(pose.jointFramesFinal.at(joint).Origin()[0]-4)<1e-5);
            CHECK(std::abs(pose.controlFrames.at(average).Origin()[0]-double(float(4.0f/3.0f)))<1e-5);
            std::cout<<"Maya stretch utilities retain sparse array semantics and live curve dependencies\n";
        }
        {
            // Values independently checked in Maya 2026's lookdevKit and
            // plusMinusAverage, including its zero-output no-operation mode.
            auto text=buffer.str()+std::string(R"(
                createNode transform -n "floatInputs";
                    addAttr -ln "a" -at "double" -dv 2.5;
                createNode plusMinusAverage -n "noOperation";
                    setAttr ".op" 0;
                    setAttr ".i3[0]" -type "float3" 7 8 9;
                createNode transform -n "noOperationTarget";
                connectAttr "noOperation.o3" "noOperationTarget.t";
            )");
            for(int op=0;op<4;++op) {
                const auto name=std::to_string(op);
                text+="createNode floatMath -n \"float"+name+"\"; setAttr \"._cnd\" "+name+"; setAttr \"._fb\" 1.25; ";
                text+="connectAttr \"floatInputs.a\" \"float"+name+"._fa\"; createNode transform -n \"floatTarget"+name+"\"; ";
                text+="connectAttr \"float"+name+".of\" \"floatTarget"+name+".tx\"; ";
            }
            auto layer=SdfLayer::CreateAnonymous("floatOperations.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            const double initial[]={3.75,1.25,3.125,2},edited[]={6.25,3.75,6.25,4};
            for(int edit=0;edit<2;++edit) {
                if(edit)CHECK(stage->GetPrimAtPath(SdfPath("/Rig/Dag/nfloatInputs")).GetAttribute(TfToken("avars:maya_na")).Set(5.0));
                auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
                CHECK(pose.controlFrames.at(SdfPath("/Rig/Dag/nnoOperationTarget")).Origin().GetLength()<1e-5);
                for(int op=0;op<4;++op)CHECK(std::abs(pose.controlFrames.at(SdfPath("/Rig/Dag/nfloatTarget"+std::to_string(op))).Origin()[0]-(edit?edited[op]:initial[op]))<1e-5);
            }
            std::cout<<"FloatMath arithmetic and plusMinusAverage no-operation match Maya\n";
        }
        {
            auto text=buffer.str()+R"(
                createNode transform -n "rail0";
                createNode nurbsCurve -n "railShape0" -p "rail0";
                    setAttr ".cc" -type "nurbsCurve" 1 1 0 no 3 2 0 1 2 0 0 0 2 0 0;
                createNode transform -n "rail1";
                    setAttr ".ty" 2;
                createNode nurbsCurve -n "railShape1" -p "rail1";
                    setAttr ".cc" -type "nurbsCurve" 1 1 0 no 3 2 0 1 2 0 0 0 2 0 0;
                createNode transform -n "rail2";
                    setAttr ".ty" 4;
                createNode nurbsCurve -n "railShape2" -p "rail2";
                    setAttr ".cc" -type "nurbsCurve" 1 1 0 no 3 2 0 1 2 0 0 0 2 0 0;
                createNode loft -n "linearLoft";
                    setAttr ".d" 1;
                    setAttr ".ar" no;
                    setAttr ".u" yes;
                connectAttr "railShape0.ws" "linearLoft.ic[0]";
                connectAttr "railShape1.ws" "linearLoft.ic[1]";
                connectAttr "railShape2.ws" "linearLoft.ic[2]";
                createNode transform -n "loftSurface";
                createNode nurbsSurface -n "loftSurfaceShape" -p "loftSurface";
                connectAttr "linearLoft.os" "loftSurfaceShape.cr";
                createNode uvPin -n "loftPin";
                    setAttr ".coord[0]" -type "double2" 0.25 0.5;
                    setAttr ".nrm" 2;
                    setAttr ".tng" 0;
                createNode transform -n "loftPinned";
                connectAttr "loftSurfaceShape.ws" "loftPin.curgeom";
                connectAttr "loftPin.omat[0]" "loftPinned.opm";
            )";
            auto layer=SdfLayer::CreateAnonymous("linearLoft.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            SdfPath pinned("/Rig/Dag/nloftPinned");
            CHECK((pose.controlFrames.at(pinned).Origin()-GfVec3d(1,1,0)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/nrail1")).GetAttribute(TfToken("avars:ty")).Set(4.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.controlFrames.at(pinned).Origin()-GfVec3d(1,3,0)).GetLength()<1e-5);
            // Coincident rails in a joint-driven initial seed still have a
            // defined longitudinal axis and width. Exercise that limiting
            // tangent separately from the ordinary surface derivative.
            for(const auto &prim:stage->Traverse()) if(prim.GetTypeName()==TfToken("RigExecNurbsSurfacePin"))
                prim.GetAttribute(TfToken("inputs:loftTangentAxis")).Set(1);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/nrail1")).GetAttribute(TfToken("avars:ty")).Set(-2.0);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/nrail2")).GetAttribute(TfToken("avars:ty")).Set(-4.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.controlFrames.at(pinned).Origin()-GfVec3d(1,0,0)).GetLength()<1e-5);
            // Moving the middle rail changes the chord-length parameter
            // knot, so U=.25 samples Y=2 instead of uniform-loft Y=3.
            auto uniform=text.find("setAttr \".u\" yes;");CHECK(uniform!=std::string::npos);
            text.replace(uniform,std::string("setAttr \".u\" yes;").size(),"setAttr \".u\" no;");
            auto chordLayer=SdfLayer::CreateAnonymous("chordLoft.ma");CHECK(chordLayer->ImportFromString(text));
            auto chordStage=UsdStage::Open(chordLayer);chordStage->SetEditTarget(chordStage->GetSessionLayer());
            rigExec::RigExecRigEvaluator chordEval(chordStage,SdfPath("/Rig"));CHECK(chordEval.Compile(&errors));
            chordStage->GetPrimAtPath(SdfPath("/Rig/Dag/nrail1")).GetAttribute(TfToken("avars:ty")).Set(4.0);
            auto chordPose=chordEval.Evaluate(UsdTimeCode::Default());CHECK(chordPose.valid);
            CHECK((chordPose.controlFrames.at(pinned).Origin()-GfVec3d(1,2,0)).GetLength()<1e-5);
            std::cout<<"Linear loft pins follow the live rail controls without cached surfaces\n";
        }
        {
            auto text=buffer.str()+R"(
                createNode transform -n "discCarrier";
                    setAttr ".ty" 2;
                createNode mesh -n "discOrig" -p "discCarrier";
                    setAttr ".io" yes;
                createNode polyDisc -n "triangleDisc";
                    setAttr ".subdivisions" 0;
                    setAttr ".radius" 0.5;
                connectAttr "triangleDisc.output" "discOrig.i";
                createNode skinCluster -n "discSkin";
                    setAttr ".pm[0]" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 -2 0 1;
                    setAttr ".gm" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 2 0 1;
                    setAttr ".wl[0:2].w" 1 0 1 1 0 1 1 0 1;
                connectAttr "discOrig.w" "discSkin.ip[0].ig";
                connectAttr "tip.wm" "discSkin.ma[0]";
                createNode mesh -n "discShape" -p "discCarrier";
                connectAttr "discSkin.og[0]" "discShape.i";
                createNode uvPin -n "discPin";
                    setAttr ".coord[0]" -type "double2" 0.5 0;
                    setAttr ".nrm" 2;
                    setAttr ".tng" 0;
                connectAttr "discShape.w" "discPin.curgeom";
                createNode transform -n "discPinned";
                connectAttr "discPin.omat[0]" "discPinned.opm";
            )";
            auto layer=SdfLayer::CreateAnonymous("disc.ma");CHECK(layer->ImportFromString(text));
            auto stage=UsdStage::Open(layer);stage->SetEditTarget(stage->GetSessionLayer());
            rigExec::RigExecRigEvaluator e(stage,SdfPath("/Rig"));CHECK(e.Compile(&errors));
            auto pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            SdfPath pinned("/Rig/Dag/ndiscPinned");
            CHECK((pose.controlFrames.at(pinned).Origin()-GfVec3d(0,2,0.25)).GetLength()<1e-5);
            stage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(3.0);
            pose=e.Evaluate(UsdTimeCode::Default());CHECK(pose.valid);
            CHECK((pose.controlFrames.at(pinned).Origin()-GfVec3d(3,2,0.25)).GetLength()<1e-5);
            std::cout<<"Procedural triangle-disc attachments retain their live skin driver\n";
        }
        for(int deltaMush:{-1,0,1}) {
            std::string text=buffer.str();
            const std::string direct="connectAttr \"skin.og[0]\" \"bodyShape.i\";";
            auto at=text.find(direct); CHECK(at!=std::string::npos);
            std::string history="createNode groupParts -n \"parts\"; connectAttr \"skin.og[0]\" \"parts.ig\"; ";
            if(deltaMush<0) history+="connectAttr \"parts.og\" \"bodyShape.i\";";
            else history+="createNode deltaMush -n \"mush\"; setAttr \".en\" "+std::to_string(deltaMush)+"; connectAttr \"parts.og\" \"mush.ip[0].ig\"; connectAttr \"mush.og[0]\" \"bodyShape.i\";";
            text.replace(at,direct.size(),history);
            // Bind geometry is independent of the visible shape transform.
            text+=" select -ne \"body\"; setAttr \".tx\" 20; select -ne \"skin\"; setAttr \".gm\" -type \"matrix\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;";
            auto historyLayer=SdfLayer::CreateAnonymous("history.ma");
            CHECK(historyLayer->ImportFromString(text));
            auto historyStage=UsdStage::Open(historyLayer); CHECK(historyStage);
            CHECK(historyLayer->GetCustomLayerData()["mayaRig:complete"].Get<bool>());
            if(deltaMush>0) {
                int count=0;for(const auto &p:historyStage->Traverse()) if(p.GetTypeName()==TfToken("RigExecDeltaMushMover"))++count;
                CHECK(count==1);
            }
            rigExec::RigExecRigEvaluator historyEval(historyStage,SdfPath("/Rig"));
            CHECK(historyEval.Compile(&errors));
            auto historyPose=historyEval.Evaluate(UsdTimeCode::Default()); CHECK(historyPose.valid);
            CHECK((historyPose.movedProperties.at(points).Get<VtVec3fArray>()[2]-GfVec3f(1,2,0)).GetLength()<1e-5);
            historyStage->SetEditTarget(historyStage->GetSessionLayer());
            CHECK(historyStage->GetPrimAtPath(SdfPath("/Rig/Dag/ncharacter/ncontrol")).GetAttribute(TfToken("avars:tx")).Set(2.0));
            historyPose=historyEval.Evaluate(UsdTimeCode::Default()); CHECK(historyPose.valid);
            CHECK((historyPose.movedProperties.at(points).Get<VtVec3fArray>()[2]-GfVec3f(3,2,0)).GetLength()<1e-5);
        }
        std::cout<<"Indirect skin history binding and unsupported-filter diagnostics passed\n";
        auto hierarchy=UsdStage::Open(std::string(argv[3])+"/constraintHierarchy.ma"); CHECK(hierarchy);
        rigExec::RigExecRigEvaluator hierarchyEval(hierarchy,SdfPath("/Rig"));
        CHECK(hierarchyEval.Compile(&errors));
        auto hierarchyPose=hierarchyEval.Evaluate(UsdTimeCode::Default()); CHECK(hierarchyPose.valid);
        SdfPath child("/Rig/Dag/nparent/nchild");
        CHECK((hierarchyPose.controlFrames.at(child).Origin()-GfVec3d(20,5,0)).GetLength()<1e-8);
        auto childFrame=hierarchyPose.controlFrames.at(child);
        CHECK((childFrame.X()-childFrame.Origin()-GfVec3d(1,0,0)).GetLength()<1e-8);
        hierarchy->SetEditTarget(hierarchy->GetSessionLayer());
        CHECK(hierarchy->GetPrimAtPath(SdfPath("/Rig/Dag/nparentDriver")).GetAttribute(TfToken("avars:rz")).Set(35.0));
        hierarchyPose=hierarchyEval.Evaluate(UsdTimeCode::Default()); CHECK(hierarchyPose.valid);
        CHECK((hierarchyPose.controlFrames.at(child).Origin()-GfVec3d(20,5,0)).GetLength()<1e-8);
        childFrame=hierarchyPose.controlFrames.at(child);
        CHECK((childFrame.X()-childFrame.Origin()-GfVec3d(1,0,0)).GetLength()<1e-8);
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
