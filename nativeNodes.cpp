// Native USD Exec computations authored by the translator. No Maya runtime.
#include "rigExec/types.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/exec/exec/builtinComputations.h"
#include "pxr/exec/exec/registerSchema.h"
#include "pxr/exec/vdf/context.h"
#include "pxr/exec/vdf/readIterator.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE
TF_DEFINE_PRIVATE_TOKENS(_pin,
    ((matrix,"outputs:matrix")) ((points,"inputs:points"))
    ((rationalWeights,"inputs:rationalWeights"))
    ((uKnots,"inputs:uKnots")) ((vKnots,"inputs:vKnots"))
    ((uDegree,"inputs:uDegree")) ((vDegree,"inputs:vDegree"))
    ((uCount,"inputs:uCount")) ((vCount,"inputs:vCount"))
    ((u,"inputs:u")) ((v,"inputs:v"))
    ((normalAxis,"inputs:normalAxis")) ((tangentAxis,"inputs:tangentAxis"))
    ((indices,"inputs:jointIndices")) ((weights,"inputs:jointWeights"))
    ((slots,"inputs:elementSize")) ((influences,"rigExec:influences"))
    ((local,"inputs:local")) ((parent,"rigExec:parent"))
    (computeMatrix) (computePointFrame)
);
namespace {
template<class T> std::vector<T> Collect(const VdfContext &ctx,const TfToken &name) {
    std::vector<T> result;
    for(VdfReadIterator<T> it(ctx,name);!it.IsAtEnd();++it) result.push_back(*it);
    return result;
}
// Cox-de Boor basis and its analytic derivative, using full clamped knots.
double Basis(int i,int degree,double parameter,const std::vector<double> &knots) {
    if(degree==0) return parameter>=knots[i] && parameter<knots[i+1] ? 1 : 0;
    double a=knots[i+degree]-knots[i],b=knots[i+degree+1]-knots[i+1];
    return (a>0 ? (parameter-knots[i])/a*Basis(i,degree-1,parameter,knots) : 0)+
        (b>0 ? (knots[i+degree+1]-parameter)/b*Basis(i+1,degree-1,parameter,knots) : 0);
}
double Derivative(int i,int degree,double parameter,const std::vector<double> &knots) {
    if(degree==0) return 0;
    double a=knots[i+degree]-knots[i],b=knots[i+degree+1]-knots[i+1];
    return (a>0 ? degree/a*Basis(i,degree-1,parameter,knots) : 0)-
        (b>0 ? degree/b*Basis(i+1,degree-1,parameter,knots) : 0);
}
GfMatrix4d SurfaceFrame(const VdfContext &ctx) {
    auto points=Collect<GfVec3d>(ctx,_pin->points);
    auto rational=Collect<double>(ctx,_pin->rationalWeights);
    auto uk=Collect<double>(ctx,_pin->uKnots),vk=Collect<double>(ctx,_pin->vKnots);
    auto ids=Collect<int>(ctx,_pin->indices);auto weights=Collect<float>(ctx,_pin->weights);
    auto matrices=Collect<GfMatrix4d>(ctx,_pin->influences);
    int ud=ctx.GetInputValue<int>(_pin->uDegree),vd=ctx.GetInputValue<int>(_pin->vDegree);
    int un=ctx.GetInputValue<int>(_pin->uCount),vn=ctx.GetInputValue<int>(_pin->vCount);
    int slots=ctx.GetInputValue<int>(_pin->slots);
    if(un<2 || vn<2 || points.size()!=size_t(un*vn) || uk.size()!=size_t(un+ud+1) || vk.size()!=size_t(vn+vd+1))
        throw std::runtime_error("Invalid native NURBS surface layout");
    if(!ids.empty()) {
        if(ids.size()!=points.size()*slots || weights.size()!=ids.size()) throw std::runtime_error("Invalid surface skin layout");
        for(size_t p=0;p<points.size();++p) {
            GfVec3d result(0);double sum=0;
            for(int slot=0;slot<slots;++slot) {
                size_t k=p*slots+slot;
                if(ids[k]<0 || size_t(ids[k])>=matrices.size()) throw std::runtime_error("Invalid surface influence slot");
                result+=matrices[ids[k]].Transform(points[p])*weights[k];sum+=weights[k];
            }
            if(sum>0) points[p]=result;
        }
    }
    double u=std::clamp(ctx.GetInputValue<double>(_pin->u),uk[ud],uk[un]);
    double v=std::clamp(ctx.GetInputValue<double>(_pin->v),vk[vd],vk[vn]);
    // Evaluate the one-sided derivative at a clamped upper endpoint.
    if(u==uk[un]) u=std::nextafter(u,uk[ud]);
    if(v==vk[vn]) v=std::nextafter(v,vk[vd]);
    GfVec3d p(0),du(0),dv(0);double w=0,wu=0,wv=0;
    for(int i=0;i<un;++i) for(int j=0;j<vn;++j) {
        size_t k=i*vn+j; double rw=rational.empty()?1:rational.at(k);
        double bu=Basis(i,ud,u,uk),bv=Basis(j,vd,v,vk);
        double b=bu*bv*rw,dbu=Derivative(i,ud,u,uk)*bv*rw,dbv=bu*Derivative(j,vd,v,vk)*rw;
        p+=points[k]*b;du+=points[k]*dbu;dv+=points[k]*dbv;w+=b;wu+=dbu;wv+=dbv;
    }
    if(std::abs(w)<1e-12) throw std::runtime_error("Degenerate NURBS surface weight");
    p/=w;du=(du-p*wu)/w;dv=(dv-p*wv)/w;
    GfVec3d normal=GfCross(du,dv),tangent=du;
    if(normal.Normalize()<1e-12 || tangent.Normalize()<1e-12) throw std::runtime_error("Degenerate NURBS surface tangent");
    int n=ctx.GetInputValue<int>(_pin->normalAxis),t=ctx.GetInputValue<int>(_pin->tangentAxis);
    if(n<0 || n>2 || t<0 || t>2 || n==t) throw std::runtime_error("Invalid surface pin axes");
    GfVec3d axes[3]; axes[n]=normal; axes[t]=tangent;
    axes[3-n-t]=GfCross(normal,tangent)*((n+1)%3==t ? 1.0 : -1.0);
    GfMatrix4d matrix(1);for(int a=0;a<3;++a) for(int c=0;c<3;++c) matrix[a][c]=axes[a][c];matrix.SetTranslateOnly(p);
    matrix=ctx.GetInputValue<GfMatrix4d>(_pin->local)*matrix;
    if(const auto *parent=ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_pin->parent)) {
        GfMatrix4d pm(1);auto origin=parent->Origin();
        GfVec3d a[3]={parent->X()-origin,parent->Y()-origin,parent->Z()-origin};
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) pm[i][j]=a[i][j];pm.SetTranslateOnly(origin);matrix*=pm;
    }
    return matrix;
}
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecNurbsSurfacePin) {
    self.AttributeExpression(_pin->matrix)
        .Callback<GfMatrix4d>(&SurfaceFrame)
        .Inputs(
            Prim().AttributeValue<GfVec3d>(_pin->points),
            Prim().AttributeValue<double>(_pin->rationalWeights),
            Prim().AttributeValue<double>(_pin->uKnots),Prim().AttributeValue<double>(_pin->vKnots),
            Prim().AttributeValue<int>(_pin->uDegree),Prim().AttributeValue<int>(_pin->vDegree),
            Prim().AttributeValue<int>(_pin->uCount),Prim().AttributeValue<int>(_pin->vCount),
            Prim().AttributeValue<double>(_pin->u),Prim().AttributeValue<double>(_pin->v),
            Prim().AttributeValue<int>(_pin->normalAxis),Prim().AttributeValue<int>(_pin->tangentAxis),
            Prim().AttributeValue<int>(_pin->indices),Prim().AttributeValue<float>(_pin->weights),
            Prim().AttributeValue<int>(_pin->slots),Prim().AttributeValue<GfMatrix4d>(_pin->local),
            Prim().Relationship(_pin->influences).TargetedObjects<GfMatrix4d>(_pin->computeMatrix).InputName(_pin->influences),
            Prim().Relationship(_pin->parent).TargetedObjects<rigExec::RigExecPointFrame>(_pin->computePointFrame).InputName(_pin->parent));
}
