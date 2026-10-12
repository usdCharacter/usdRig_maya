// Native USD Exec computations authored by the translator. No Maya runtime.
#include "rigExec/types.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/gf/rotation.h"
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
    ((barycentric,"inputs:barycentric")) ((tangentWeights,"inputs:tangentWeights"))
    ((offset,"inputs:offset"))
    ((axis,"inputs:axis"))
    ((value,"outputs:value"))
    ((loftChordLength,"inputs:loftChordLength"))
    ((loftTangentAxis,"inputs:loftTangentAxis"))
    ((parentPivot,"inputs:parentPivot"))
    (computeMatrix) (computePointFrame)
);
TF_DEFINE_PRIVATE_TOKENS(_space,
    ((matrix,"outputs:matrix")) ((local,"inputs:local"))
    ((source,"rigExec:source")) ((sourceParent,"rigExec:sourceParent")) ((parent,"rigExec:parent"))
    ((mode,"inputs:mode")) ((order,"inputs:rotationOrder"))
    ((sourceOffsetInverse,"inputs:sourceOffsetInverse"))
    ((parentPivot,"inputs:parentPivot")) ((sourcePivot,"inputs:sourcePivot")) ((sourceParentPivot,"inputs:sourceParentPivot"))
    ((tx,"inputs:tx")) ((ty,"inputs:ty")) ((tz,"inputs:tz"))
    ((rx,"inputs:rx")) ((ry,"inputs:ry")) ((rz,"inputs:rz"))
    ((sx,"inputs:sx")) ((sy,"inputs:sy")) ((sz,"inputs:sz"))
    ((channelComposition,"inputs:channelComposition"))
    ((baseTranslate,"inputs:baseTranslate")) ((baseRotate,"inputs:baseRotate")) ((baseScale,"inputs:baseScale"))
    ((scalePrefix,"inputs:scalePrefix")) ((scaleSuffix,"inputs:scaleSuffix"))
    ((rotateSuffix,"inputs:rotateSuffix")) ((offset,"inputs:offset"))
    ((compensate,"inputs:segmentScaleCompensate")) ((postRotate,"inputs:postRotate"))
    ((isx,"inputs:inverseScaleX")) ((isy,"inputs:inverseScaleY")) ((isz,"inputs:inverseScaleZ"))
    ((jointChannels,"inputs:jointChannels"))
    (computePointFrame)
);
TF_DEFINE_PRIVATE_TOKENS(_skin,
    ((matrix,"outputs:matrix")) ((inverseBind,"inputs:inverseBind"))
    ((source,"rigExec:source")) ((bindSource,"rigExec:bindSource")) (computePointFrame)
    ((sourcePivot,"inputs:sourcePivot")) ((bindPivot,"inputs:bindPivot"))
);
TF_DEFINE_PRIVATE_TOKENS(_math,
    ((value,"outputs:value")) ((a,"inputs:a")) ((b,"inputs:b")) ((c,"inputs:c"))
    ((floatValue,"outputs:floatValue"))
    ((mode,"inputs:mode")) ((axis,"inputs:axis"))
    ((source,"rigExec:source")) ((parent,"rigExec:parent"))
    ((sourcePivot,"inputs:sourcePivot")) ((parentPivot,"inputs:parentPivot"))
    (computePointFrame)
);
namespace {
GfMatrix4d FrameMatrix(const rigExec::RigExecPointFrame *frame) {
    GfMatrix4d result(1);if(!frame)return result;
    auto o=frame->Origin();GfVec3d a[3]={frame->X()-o,frame->Y()-o,frame->Z()-o};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)result[i][j]=a[i][j];result.SetTranslateOnly(o);return result;
}
GfMatrix4d PivotShift(const VdfContext &ctx,const TfToken &name,double sign) {
    GfMatrix4d result(1);if(const auto *v=ctx.GetInputValuePtr<GfVec3d>(name))result.SetTranslate(*v*sign);return result;
}
GfMatrix4d MatrixSpace(const VdfContext &ctx) {
    const auto sourceWorld=PivotShift(ctx,_space->sourcePivot,-1)*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_space->source));
    auto source=sourceWorld;
    source*=FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_space->sourceParent)).GetInverse();
    source*=PivotShift(ctx,_space->sourceParentPivot,1);
    if(const auto *opm=ctx.GetInputValuePtr<GfMatrix4d>(_space->sourceOffsetInverse))source*=*opm;
    if(ctx.GetInputValue<int>(_space->mode)==1)source=source.GetInverse();
    if(ctx.GetInputValue<int>(_space->mode)==2)return source;
    if(ctx.GetInputValue<int>(_space->mode)==6)return sourceWorld.GetInverse()*ctx.GetInputValue<GfMatrix4d>(_space->local);
    if(ctx.GetInputValue<int>(_space->mode)==5) {
        const auto geometry=PivotShift(ctx,_space->parentPivot,-1)*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_space->parent));
        const auto bind=ctx.GetInputValue<GfMatrix4d>(_space->local);
        return geometry.GetInverse()*bind*source*bind.GetInverse()*geometry;
    }
    if(ctx.GetInputValue<int>(_space->mode)==4) {
        const double scales[3]={ctx.GetInputValue<double>(_space->sx),ctx.GetInputValue<double>(_space->sy),ctx.GetInputValue<double>(_space->sz)};
        for(int r=0;r<3;++r) {
            if(std::abs(scales[r])<1e-12)throw std::runtime_error("Degenerate Maya constraint source scale");
            for(int c=0;c<3;++c)source[r][c]/=scales[r];
        }
        const auto parent=PivotShift(ctx,_space->sourceParentPivot,-1)*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_space->sourceParent));
        source*=parent;
        const double sign=source.GetDeterminant()<0?-1:1;
        GfVec3d x(source[0][0],source[0][1],source[0][2]),y(source[1][0],source[1][1],source[1][2]);
        x*=sign;y*=sign;
        if(x.GetLength()<1e-12)throw std::runtime_error("Degenerate Maya constraint rotation source");
        x.Normalize();y-=x*GfDot(x,y);
        if(y.GetLength()<1e-12)throw std::runtime_error("Degenerate Maya constraint rotation source");
        y.Normalize();GfVec3d axes[3]={x,y,GfCross(x,y)};GfMatrix4d result(1);
        for(int r=0;r<3;++r)for(int c=0;c<3;++c)result[r][c]=axes[r][c];result.SetTranslateOnly(sourceWorld.ExtractTranslation());return result;
    }
    const auto *compose=ctx.GetInputValuePtr<bool>(_space->channelComposition);
    const bool mayaChannels=compose && *compose;
    GfVec3d scale(ctx.GetInputValue<double>(_space->sx),ctx.GetInputValue<double>(_space->sy),ctx.GetInputValue<double>(_space->sz));
    GfVec3d translate(ctx.GetInputValue<double>(_space->tx),ctx.GetInputValue<double>(_space->ty),ctx.GetInputValue<double>(_space->tz));
    GfVec3d rotate(ctx.GetInputValue<double>(_space->rx),ctx.GetInputValue<double>(_space->ry),ctx.GetInputValue<double>(_space->rz));
    if(mayaChannels) {
        const auto base=ctx.GetInputValue<GfVec3d>(_space->baseScale);
        for(int i=0;i<3;++i)scale[i]*=base[i];
        const auto *joint=ctx.GetInputValuePtr<bool>(_space->jointChannels);
        if(joint && *joint)for(int i=0;i<3;++i)if(scale[i]==0)scale[i]=1e-12;
        translate+=ctx.GetInputValue<GfVec3d>(_space->baseTranslate);
        rotate+=ctx.GetInputValue<GfVec3d>(_space->baseRotate);
    }
    GfMatrix4d avars(1);avars.SetScale(scale);
    if(mayaChannels)avars=ctx.GetInputValue<GfMatrix4d>(_space->scalePrefix)*avars*ctx.GetInputValue<GfMatrix4d>(_space->scaleSuffix);
    auto order=ctx.GetInputValue<TfToken>(_space->order).GetString();
    for(char c:order) {int axis=c-'X';if(axis<0 || axis>2)throw std::runtime_error("Invalid matrix-space rotation order");GfVec3d a(0);a[axis]=1;avars*=GfMatrix4d(GfRotation(a,rotate[axis]),GfVec3d(0));}
    if(mayaChannels) {
        GfMatrix4d translation(1);translation.SetTranslate(translate);
        avars*=ctx.GetInputValue<GfMatrix4d>(_space->rotateSuffix);
        const auto *compensate=ctx.GetInputValuePtr<bool>(_space->compensate);
        if(compensate && *compensate) {
            GfVec3d inverse(ctx.GetInputValue<double>(_space->isx),ctx.GetInputValue<double>(_space->isy),ctx.GetInputValue<double>(_space->isz));
            for(int i=0;i<3;++i)inverse[i]=1/(inverse[i]==0?1e-12:inverse[i]);
            GfMatrix4d compensation(1);compensation.SetScale(inverse);
            avars*=compensation*ctx.GetInputValue<GfMatrix4d>(_space->postRotate);
        }
        avars*=translation*ctx.GetInputValue<GfMatrix4d>(_space->offset);
        return avars*source*PivotShift(ctx,_space->parentPivot,-1)*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_space->parent));
    }
    avars.SetTranslateOnly(translate);
    return avars*ctx.GetInputValue<GfMatrix4d>(_space->local)*source*PivotShift(ctx,_space->parentPivot,-1)*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_space->parent));
}
GfMatrix4d SkinInfluence(const VdfContext &ctx) {
    auto inverseBind=ctx.GetInputValue<GfMatrix4d>(_skin->inverseBind);
    if(const auto *bind=ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_skin->bindSource))inverseBind=(PivotShift(ctx,_skin->bindPivot,-1)*FrameMatrix(bind)).GetInverse();
    return inverseBind*PivotShift(ctx,_skin->sourcePivot,-1)*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_skin->source));
}
double ScalarMath(const VdfContext &ctx) {
    const double a=ctx.GetInputValue<double>(_math->a),b=ctx.GetInputValue<double>(_math->b),c=ctx.GetInputValue<double>(_math->c);
    switch(ctx.GetInputValue<int>(_math->mode)) {
    case 0:return a*b+c;
    case 1:return a*c+b*(1-c);
    case 2:return b==0 ? 0 : a/b;
    case 3:return std::pow(a,b);
    case 4:return float(float(a)+float(b));
    case 5:return float(float(a)-float(b));
    case 6:return float(float(a)*float(b));
    case 7:return b==0 ? 0 : float(float(a)/float(b));
    case 8:return std::clamp(a,0.0,1.0);
    case 9:return a*a*(3-2*a);
    case 10:return a>=b ? 1 : 0;
    case 11:return float(a)==float(b)?1:0;
    case 12:return float(a)!=float(b)?1:0;
    case 13:return float(a)>float(b)?1:0;
    case 14:return float(a)>=float(b)?1:0;
    case 15:return float(a)<float(b)?1:0;
    case 16:return float(a)<=float(b)?1:0;
    case 17:return float(a);
    default:throw std::runtime_error("Invalid Maya scalar operation");
    }
}
float ScalarMathFloat(const VdfContext &ctx) {return float(ScalarMath(ctx));}
double LocalTranslation(const VdfContext &ctx) {
    auto m=PivotShift(ctx,_math->sourcePivot,-1)*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_math->source));
    const int axis=ctx.GetInputValue<int>(_math->axis);
    if(axis==-1) {
        auto other=PivotShift(ctx,_math->parentPivot,-1)*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_math->parent));
        return (m.ExtractTranslation()-other.ExtractTranslation()).GetLength();
    }
    m*=FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_math->parent)).GetInverse()*PivotShift(ctx,_math->parentPivot,1);
    if(axis<0 || axis>2)throw std::runtime_error("Invalid Maya translation axis");
    return m.ExtractTranslation()[axis];
}
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
double CurvePoint(const VdfContext &ctx) {
    auto points=Collect<GfVec3d>(ctx,_pin->points);auto knots=Collect<double>(ctx,_pin->uKnots);
    auto rational=Collect<double>(ctx,_pin->rationalWeights);
    auto ids=Collect<int>(ctx,_pin->indices);auto weights=Collect<float>(ctx,_pin->weights);auto matrices=Collect<GfMatrix4d>(ctx,_pin->influences);
    const int degree=ctx.GetInputValue<int>(_pin->uDegree),slots=ctx.GetInputValue<int>(_pin->slots),axis=ctx.GetInputValue<int>(_pin->axis);
    if(degree<1 || knots.size()!=points.size()+degree+1 || axis<0 || axis>2)throw std::runtime_error("Invalid Maya curve point layout");
    if(!ids.empty()) {
        if(slots<1 || ids.size()!=points.size()*slots || weights.size()!=ids.size())throw std::runtime_error("Invalid Maya curve skin layout");
        for(size_t p=0;p<points.size();++p) {
            GfVec3d result(0);double sum=0;
            for(int j=0;j<slots;++j) {size_t k=p*slots+j;if(ids[k]<0 || size_t(ids[k])>=matrices.size())throw std::runtime_error("Invalid curve skin influence");result+=matrices[ids[k]].Transform(points[p])*weights[k];sum+=weights[k];}
            if(sum>0)points[p]=result;
        }
    }
    double u=std::clamp(ctx.GetInputValue<double>(_pin->u),knots[degree],knots[points.size()]);
    if(u==knots[points.size()])u=std::nextafter(u,knots[degree]);
    double result=0,total=0;
    for(size_t p=0;p<points.size();++p) {double b=Basis(int(p),degree,u,knots)*(rational.empty()?1:rational.at(p));result+=points[p][axis]*b;total+=b;}
    if(std::abs(total)<1e-12)throw std::runtime_error("Degenerate curve point");return result/total;
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
    if(const auto *chord=ctx.GetInputValuePtr<bool>(_pin->loftChordLength);chord && *chord) {
        if(ud!=1 || vd!=1)throw std::runtime_error("Chord-length loft requires linear rails");
        std::vector<double> distances(size_t(un),0);
        for(int i=1;i<un;++i)distances[i]=distances[i-1]+(points[size_t(i)*vn]-points[size_t(i-1)*vn]).GetLength();
        if(distances.back()<1e-12)throw std::runtime_error("Degenerate loft rail spacing");
        uk={0,0};for(int i=1;i<un;++i)uk.push_back(distances[i]/distances.back());uk.push_back(1);
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
    // Joint-driven loft rails can coincide in the initial pose seed, before
    // their point constraints run. Their longitudinal joint axis supplies
    // the limiting tangent; final evaluation uses the surface derivative.
    if((normal.GetLengthSq()<1e-24 || tangent.GetLengthSq()<1e-24) && !matrices.empty()) {
        if(const auto *axis=ctx.GetInputValuePtr<int>(_pin->loftTangentAxis);axis && *axis>=0 && *axis<3) {
            tangent=GfVec3d(matrices.front()[*axis][0],matrices.front()[*axis][1],matrices.front()[*axis][2]);normal=GfCross(tangent,dv);
        }
    }
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
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) pm[i][j]=a[i][j];pm.SetTranslateOnly(origin);matrix*=PivotShift(ctx,_pin->parentPivot,-1)*pm;
    }
    return matrix;
}
GfMatrix4d MeshFrame(const VdfContext &ctx) {
    auto points=Collect<GfVec3d>(ctx,_pin->points);
    auto ids=Collect<int>(ctx,_pin->indices);auto weights=Collect<float>(ctx,_pin->weights);
    auto matrices=Collect<GfMatrix4d>(ctx,_pin->influences);
    int slots=ctx.GetInputValue<int>(_pin->slots);
    if(points.size()!=3 || slots<1 || ids.size()!=3*size_t(slots) || weights.size()!=ids.size())
        throw std::runtime_error("Invalid native mesh pin layout");
    for(size_t p=0;p<3;++p) {
        GfVec3d value(0);double sum=0;
        for(int j=0;j<slots;++j) {
            size_t k=p*slots+j;
            if(ids[k]<0 || size_t(ids[k])>=matrices.size()) throw std::runtime_error("Invalid mesh pin influence");
            value+=matrices[ids[k]].Transform(points[p])*weights[k];sum+=weights[k];
        }
        if(sum>0)points[p]=value;
    }
    auto bary=ctx.GetInputValue<GfVec3d>(_pin->barycentric);
    auto tw=ctx.GetInputValue<GfVec3d>(_pin->tangentWeights);
    GfVec3d position(0),tangent(0);
    for(int j=0;j<3;++j) {position+=points[j]*bary[j];tangent+=points[j]*tw[j];}
    GfVec3d normal=GfCross(points[1]-points[0],points[2]-points[0]);
    if(normal.Normalize()<1e-12) throw std::runtime_error("Degenerate mesh pin triangle");
    tangent-=normal*GfDot(tangent,normal);
    if(tangent.Normalize()<1e-12) throw std::runtime_error("Degenerate mesh pin tangent");
    int n=ctx.GetInputValue<int>(_pin->normalAxis),t=ctx.GetInputValue<int>(_pin->tangentAxis);
    if(n<0 || n>2 || t<0 || t>2 || n==t) throw std::runtime_error("Invalid mesh pin axes");
    GfVec3d axes[3];axes[n]=normal;axes[t]=tangent;
    axes[3-n-t]=GfCross(normal,tangent)*((n+1)%3==t ? 1.0 : -1.0);
    GfMatrix4d result(1);for(int i=0;i<3;++i)for(int j=0;j<3;++j)result[i][j]=axes[i][j];
    result.SetTranslateOnly(position);
    result=ctx.GetInputValue<GfMatrix4d>(_pin->local)*ctx.GetInputValue<GfMatrix4d>(_pin->offset)*result;
    if(const auto *p=ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_pin->parent)) {
        GfMatrix4d m(1);auto o=p->Origin();GfVec3d a[3]={p->X()-o,p->Y()-o,p->Z()-o};
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)m[i][j]=a[i][j];m.SetTranslateOnly(o);result*=PivotShift(ctx,_pin->parentPivot,-1)*m;
    }
    return result;
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
            Prim().AttributeValue<bool>(_pin->loftChordLength),
            Prim().AttributeValue<int>(_pin->loftTangentAxis),
            Prim().AttributeValue<double>(_pin->u),Prim().AttributeValue<double>(_pin->v),
            Prim().AttributeValue<int>(_pin->normalAxis),Prim().AttributeValue<int>(_pin->tangentAxis),
            Prim().AttributeValue<int>(_pin->indices),Prim().AttributeValue<float>(_pin->weights),
            Prim().AttributeValue<int>(_pin->slots),Prim().AttributeValue<GfMatrix4d>(_pin->local),Prim().AttributeValue<GfVec3d>(_pin->parentPivot),
            Prim().Relationship(_pin->influences).TargetedObjects<GfMatrix4d>(_pin->computeMatrix).InputName(_pin->influences),
            Prim().Relationship(_pin->parent).TargetedObjects<rigExec::RigExecPointFrame>(_pin->computePointFrame).InputName(_pin->parent));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecMeshPin) {
    self.AttributeExpression(_pin->matrix)
        .Callback<GfMatrix4d>(&MeshFrame)
        .Inputs(Prim().AttributeValue<GfVec3d>(_pin->points),
            Prim().AttributeValue<int>(_pin->indices),Prim().AttributeValue<float>(_pin->weights),
            Prim().AttributeValue<int>(_pin->slots),Prim().AttributeValue<GfVec3d>(_pin->barycentric),
            Prim().AttributeValue<GfVec3d>(_pin->tangentWeights),
            Prim().AttributeValue<int>(_pin->normalAxis),Prim().AttributeValue<int>(_pin->tangentAxis),
            Prim().AttributeValue<GfMatrix4d>(_pin->local),Prim().AttributeValue<GfMatrix4d>(_pin->offset),
            Prim().AttributeValue<GfVec3d>(_pin->parentPivot),
            Prim().Relationship(_pin->influences).TargetedObjects<GfMatrix4d>(_pin->computeMatrix).InputName(_pin->influences),
            Prim().Relationship(_pin->parent).TargetedObjects<rigExec::RigExecPointFrame>(_pin->computePointFrame).InputName(_pin->parent));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecMayaMatrixSpace) {
    self.AttributeExpression(_space->matrix).Callback<GfMatrix4d>(&MatrixSpace)
        .Inputs(Prim().AttributeValue<GfMatrix4d>(_space->local),Prim().AttributeValue<int>(_space->mode),
            Prim().AttributeValue<GfMatrix4d>(_space->sourceOffsetInverse),
            Prim().AttributeValue<GfVec3d>(_space->parentPivot),Prim().AttributeValue<GfVec3d>(_space->sourcePivot),Prim().AttributeValue<GfVec3d>(_space->sourceParentPivot),
            Prim().AttributeValue<TfToken>(_space->order),
            Prim().AttributeValue<double>(_space->tx),Prim().AttributeValue<double>(_space->ty),Prim().AttributeValue<double>(_space->tz),
            Prim().AttributeValue<double>(_space->rx),Prim().AttributeValue<double>(_space->ry),Prim().AttributeValue<double>(_space->rz),
            Prim().AttributeValue<double>(_space->sx),Prim().AttributeValue<double>(_space->sy),Prim().AttributeValue<double>(_space->sz),
            Prim().AttributeValue<bool>(_space->channelComposition),
            Prim().AttributeValue<GfVec3d>(_space->baseTranslate),Prim().AttributeValue<GfVec3d>(_space->baseRotate),Prim().AttributeValue<GfVec3d>(_space->baseScale),
            Prim().AttributeValue<GfMatrix4d>(_space->scalePrefix),Prim().AttributeValue<GfMatrix4d>(_space->scaleSuffix),Prim().AttributeValue<GfMatrix4d>(_space->rotateSuffix),Prim().AttributeValue<GfMatrix4d>(_space->offset),
            Prim().AttributeValue<bool>(_space->compensate),Prim().AttributeValue<GfMatrix4d>(_space->postRotate),
            Prim().AttributeValue<double>(_space->isx),Prim().AttributeValue<double>(_space->isy),Prim().AttributeValue<double>(_space->isz),
            Prim().AttributeValue<bool>(_space->jointChannels),
            Prim().Relationship(_space->source).TargetedObjects<rigExec::RigExecPointFrame>(_space->computePointFrame).InputName(_space->source),
            Prim().Relationship(_space->sourceParent).TargetedObjects<rigExec::RigExecPointFrame>(_space->computePointFrame).InputName(_space->sourceParent),
            Prim().Relationship(_space->parent).TargetedObjects<rigExec::RigExecPointFrame>(_space->computePointFrame).InputName(_space->parent));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecMayaSkinInfluence) {
    self.AttributeExpression(_skin->matrix).Callback<GfMatrix4d>(&SkinInfluence)
        .Inputs(Prim().AttributeValue<GfMatrix4d>(_skin->inverseBind),
            Prim().AttributeValue<GfVec3d>(_skin->sourcePivot),Prim().AttributeValue<GfVec3d>(_skin->bindPivot),
            Prim().Relationship(_skin->source).TargetedObjects<rigExec::RigExecPointFrame>(_skin->computePointFrame).InputName(_skin->source),
            Prim().Relationship(_skin->bindSource).TargetedObjects<rigExec::RigExecPointFrame>(_skin->computePointFrame).InputName(_skin->bindSource));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecMayaScalar) {
    self.AttributeExpression(_math->value).Callback<double>(&ScalarMath)
        .Inputs(Prim().AttributeValue<double>(_math->a),Prim().AttributeValue<double>(_math->b),Prim().AttributeValue<double>(_math->c),Prim().AttributeValue<int>(_math->mode));
    self.AttributeExpression(_math->floatValue).Callback<float>(&ScalarMathFloat)
        .Inputs(Prim().AttributeValue<double>(_math->a),Prim().AttributeValue<double>(_math->b),Prim().AttributeValue<double>(_math->c),Prim().AttributeValue<int>(_math->mode));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecMayaTranslation) {
    self.AttributeExpression(_math->value).Callback<double>(&LocalTranslation)
        .Inputs(Prim().AttributeValue<int>(_math->axis),Prim().AttributeValue<GfVec3d>(_math->sourcePivot),Prim().AttributeValue<GfVec3d>(_math->parentPivot),
            Prim().Relationship(_math->source).TargetedObjects<rigExec::RigExecPointFrame>(_math->computePointFrame).InputName(_math->source),
            Prim().Relationship(_math->parent).TargetedObjects<rigExec::RigExecPointFrame>(_math->computePointFrame).InputName(_math->parent));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecMayaCurvePoint) {
    self.AttributeExpression(_pin->value).Callback<double>(&CurvePoint)
        .Inputs(Prim().AttributeValue<GfVec3d>(_pin->points),Prim().AttributeValue<double>(_pin->uKnots),Prim().AttributeValue<double>(_pin->rationalWeights),
            Prim().AttributeValue<int>(_pin->uDegree),Prim().AttributeValue<double>(_pin->u),Prim().AttributeValue<int>(_pin->axis),Prim().AttributeValue<int>(_pin->slots),
            Prim().AttributeValue<int>(_pin->indices),Prim().AttributeValue<float>(_pin->weights),
            Prim().Relationship(_pin->influences).TargetedObjects<GfMatrix4d>(_pin->computeMatrix).InputName(_pin->influences));
}
