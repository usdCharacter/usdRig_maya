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
