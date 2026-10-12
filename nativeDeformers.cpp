// Maya's two-by-two global FFD samples the live input in base-lattice space.
#include "rigExec/movers/moverRegistry.h"
#include "rigExecScene/sceneDescriptors.h"
#include "pxr/usd/usdGeom/pointBased.h"
#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE
namespace {
struct FfdData {
    GfMatrix4d baseInverse{1};
    VtVec3fArray cage;
    VtFloatArray weights;
    int outside=0;
    bool operator==(const FfdData &other) const {
        return baseInverse==other.baseInverse && cage==other.cage && weights==other.weights && outside==other.outside;
    }
};
void BindFfd(const rigExec::RigExecMoverBindContext &ctx) {
    auto &binding=*ctx.binding;
    SdfPathVector cage,base;
    ctx.moverPrim.GetRelationship(TfToken("rigExec:cage")).GetTargets(&cage);
    ctx.moverPrim.GetRelationship(TfToken("rigExec:transform")).GetTargets(&base);
    if(cage.size()==1) {
        binding.cagePoints=cage[0].IsPrimPath()?cage[0].AppendProperty(TfToken("points")):cage[0];
        binding.phases[binding.cagePoints]={rigExec::RigExecReadPhaseKind::Final,{}};
    }
    if(base.size()==1)binding.transform=base[0];
    binding.transformPhase={rigExec::RigExecReadPhaseKind::Final,{}};
}
bool ValidateFfd(const rigExec::RigExecMoverValidateContext &ctx,std::string *error) {
    SdfPathVector cage,base;
    ctx.prim.GetRelationship(TfToken("rigExec:cage")).GetTargets(&cage);
    ctx.prim.GetRelationship(TfToken("rigExec:transform")).GetTargets(&base);
    if(cage.size()!=1 || base.size()!=1 || !ctx.stage->GetPrimAtPath(base[0]) ||
       !UsdGeomPointBased(ctx.stage->GetPrimAtPath(cage[0].GetPrimPath()))) {
        *error="Maya FFD requires one point cage and one base matrix provider";return false;
    }
    VtVec3fArray points;UsdGeomPointBased(ctx.stage->GetPrimAtPath(cage[0].GetPrimPath())).GetPointsAttr().Get(&points);
    if(points.size()!=8) {*error="Maya FFD supports only a two-by-two-by-two cage";return false;}
    return true;
}
void FfdInputs(const SdfPath &mover,const SdfPath &cage,
               std::vector<rigExec::RigExecRevisionLeafKey> *inputs) {
    using namespace rigExec;
    *inputs={
        {cage,RigExecRevisionLeafType::Vec3fArray,RigExecRevisionLeafTime::AtTime,
         RigExecRevisionLeafFlavour::Resolved,VtValue(VtVec3fArray())},
        {mover.AppendProperty(TfToken("inputs:outside")),RigExecRevisionLeafType::Int,
         RigExecRevisionLeafTime::AtTime,RigExecRevisionLeafFlavour::Resolved,VtValue(0)},
        {mover.AppendProperty(TfToken("inputs:pointWeights")),RigExecRevisionLeafType::FloatArray,
         RigExecRevisionLeafTime::AtTime,RigExecRevisionLeafFlavour::Resolved,VtValue(VtFloatArray())}};
}
void DeclareFfdInputs(const rigExec::RigExecMoverBindContext &ctx,
                      std::vector<rigExec::RigExecRevisionLeafKey> *inputs) {
    FfdInputs(ctx.moverPrim.GetPath(),ctx.binding->cagePoints,inputs);
}
bool CompileFfdScene(const rigExec::RigExecSceneDescriptors &scene,const SdfPath &mover,
                     const SdfPath &,rigExec::RigExecRevisionBinding *binding,std::string *error) {
    if(!binding)return false;
    const auto cage=scene.relationships.find(mover.AppendProperty(TfToken("rigExec:cage")));
    const auto base=scene.relationships.find(mover.AppendProperty(TfToken("rigExec:transform")));
    if(cage==scene.relationships.end() || base==scene.relationships.end() ||
       cage->second.fact.targets.size()!=1 || base->second.fact.targets.size()!=1) {
        if(error)*error="Maya FFD requires one point cage and one base matrix provider";
        return false;
    }
    const auto &path=cage->second.fact.targets.front();
    binding->cagePoints=path.IsPrimPath()?path.AppendProperty(TfToken("points")):path;
    binding->phases[binding->cagePoints]={rigExec::RigExecReadPhaseKind::Final,{}};
    binding->transform=base->second.fact.targets.front();
    binding->transformPhase={rigExec::RigExecReadPhaseKind::Final,{}};
    FfdInputs(mover,binding->cagePoints,&binding->externalInputs);
    return true;
}
bool AssembleFfd(const rigExec::RigExecExternalInputContext &ctx,VtValue *data) {
    const auto &values=ctx.values;
    if(!values.transform || std::abs(values.transform->GetDeterminant())<1e-12)return false;
    if(ctx.inputs.size()!=3 || !ctx.inputs[0].IsHolding<VtVec3fArray>() ||
       !ctx.inputs[1].IsHolding<int>() || !ctx.inputs[2].IsHolding<VtFloatArray>())return false;
    FfdData result;result.baseInverse=values.transform->GetInverse();
    for(int r=0;r<4;++r)for(int c=0;c<4;++c)if(!std::isfinite(result.baseInverse[r][c]))return false;
    result.cage=ctx.inputs[0].UncheckedGet<VtVec3fArray>();
    result.outside=ctx.inputs[1].UncheckedGet<int>();
    result.weights=ctx.inputs[2].UncheckedGet<VtFloatArray>();
    if(result.cage.size()!=8)return false;
    if(result.outside<0 || result.outside>1)return false;
    for(const auto &point:result.cage)for(int axis=0;axis<3;++axis)if(!std::isfinite(point[axis]))return false;
    for(float weight:result.weights)if(!std::isfinite(weight))return false;
    *data=VtValue(result);return true;
}
bool ApplyFfd(const VtValue &data,std::vector<GfVec3f> *points) {
    if(!data.IsHolding<FfdData>())return false;
    const auto &p=data.UncheckedGet<FfdData>();
    if(!p.weights.empty() && p.weights.size()!=points->size())return false;
    auto output=*points;
    for(size_t j=0;j<points->size();++j) {
        const GfVec3d uvw=p.baseInverse.Transform(GfVec3d((*points)[j]))+GfVec3d(.5);
        for(int axis=0;axis<3;++axis)if(!std::isfinite(uvw[axis]))return false;
        if(p.outside==0 && (uvw[0]<0 || uvw[0]>1 || uvw[1]<0 || uvw[1]>1 || uvw[2]<0 || uvw[2]>1))continue;
        GfVec3d result(0);
        for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x)
            result+=GfVec3d(p.cage[(z*2+y)*2+x])*(x?uvw[0]:1-uvw[0])*(y?uvw[1]:1-uvw[1])*(z?uvw[2]:1-uvw[2]);
        const float weight=p.weights.empty()?1:p.weights[j];
        output[j]+=GfVec3f(result-GfVec3d((*points)[j]))*weight;
        for(int axis=0;axis<3;++axis)if(!std::isfinite(output[j][axis]))return false;
    }
    points->swap(output);
    return true;
}
rigExec::RigExecMoverHandler Handler() {
    rigExec::RigExecMoverHandler h("RigExecMayaFfd",&rigExec::RigExecFixedMoverOp<rigExec::RigExecRevisionOp::External>,rigExec::RigExecMoverDomain::Points);
    h.singleTarget=true;h.bind=&BindFfd;h.validate=&ValidateFfd;
    h.frameRelationships={"rigExec:transform"};h.transformRelationship="rigExec:transform";
    h.declareExternalInputs=&DeclareFfdInputs;h.compileScene=&CompileFfdScene;
    h.hasScalarOracle=false;h.assembleExternal=&AssembleFfd;h.applyExternal=&ApplyFfd;
    return h;
}
}
RIGEXEC_REGISTER_MOVER(Handler());
