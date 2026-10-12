#include "translator.h"
#include "rigExecRigging/rigBuilder.h"
#include "rigExecRigging/schemaAuthoring.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/copyUtils.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/nurbsCurves.h"
#include "pxr/usd/usdGeom/nurbsPatch.h"
#include "pxr/usd/usdSkel/blendShape.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/usd/primRange.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE
namespace usdMayaRig {
namespace {
using rigExec::RigExecSchemaPrim;
using rigExec::RigExecRigBuilder;
using Strings = std::vector<std::string>;
constexpr size_t MaxElements = 10000000;
bool Starts(const std::string &s,const std::string &p) { return s.compare(0,p.size(),p)==0; }
double Number(const std::string &s) {
    if (s == "yes" || s == "on" || s == "true") return 1;
    if (s == "no" || s == "off" || s == "false") return 0;
    size_t n=0; double d=std::stod(s,&n);
    if (n!=s.size() || !std::isfinite(d)) throw std::runtime_error("invalid finite number: " + s);
    return d;
}
int Integer(const std::string &s) {
    double d=Number(s);
    if (std::floor(d)!=d || d < std::numeric_limits<int>::min() || d > std::numeric_limits<int>::max())
        throw std::runtime_error("invalid integer: " + s);
    return static_cast<int>(d);
}
size_t Count(const std::string &s) {
    int v=Integer(s);
    if (v<0 || static_cast<size_t>(v)>MaxElements) throw std::runtime_error("invalid array size: " + s);
    return static_cast<size_t>(v);
}
// Escape underscores too: namespaces/punctuation can never collide with names
// that already contain the escape sequence.
std::string Name(const std::string &s) {
    std::string r="n"; const char *hex="0123456789abcdef";
    for (unsigned char c : s) {
        if ((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9')) r+=c;
        else { r+='_'; r+=hex[c>>4]; r+=hex[c&15]; }
    }
    return r;
}
std::pair<std::string,std::string> Plug(const std::string &s) {
    auto dot=s.find('.');
    if (dot==std::string::npos) throw std::runtime_error("invalid connection plug: " + s);
    return {s.substr(0,dot),s.substr(dot+1)};
}
const Attribute *Attr(const Node &n,const std::string &a,const std::string &b={}) {
    for (auto i=n.attributes.rbegin();i!=n.attributes.rend();++i)
        if ((i->name==a || (!b.empty() && i->name==b)) && !i->values.empty()) return &*i;
    return nullptr;
}
double Scalar(const Node &n,const std::string &a,const std::string &b,double fallback) {
    auto p=Attr(n,a,b); return p ? Number(p->values.at(0)) : fallback;
}
GfVec3d Vector(const Node &n,const std::string &a,const std::string &b,GfVec3d fallback=GfVec3d(0)) {
    // Apply values in file order, including partial channel overrides.
    for (const auto &v : n.attributes) {
        if (v.values.empty()) continue;
        if (v.name==a || v.name==b) {
            if (v.values.size()!=3) throw std::runtime_error("expected triple: " + n.name + "." + v.name);
            for (int j=0;j<3;++j) fallback[j]=Number(v.values[j]);
        }
        for (int j=0;j<3;++j) {
            const char axis="xyz"[j], longAxis="XYZ"[j];
            if (v.name==a+axis || v.name==b+longAxis) fallback[j]=Number(v.values.at(0));
        }
    }
    return fallback;
}
GfMatrix4d Matrix(const Attribute *a, GfMatrix4d fallback=GfMatrix4d(1)) {
    if (!a) return fallback;
    if (a->values.size()!=16) throw std::runtime_error("expected numeric 4x4 matrix: " + a->name);
    GfMatrix4d m(1);
    for (int i=0;i<16;++i) m[i/4][i%4]=Number(a->values[i]);
    return m;
}
GfMatrix4d Translation(const GfVec3d &v) { GfMatrix4d m(1); m.SetTranslate(v); return m; }
GfMatrix4d Rotation(GfVec3d v,int order) {
    static const char *orders[]={"XYZ","YZX","ZXY","XZY","YXZ","ZYX"};
    if (order<0 || order>5) throw std::runtime_error("invalid rotateOrder");
    GfMatrix4d m(1);
    for (char c:std::string(orders[order])) {
        int a=c-'X'; GfVec3d axis(0); axis[a]=1;
        m*=GfMatrix4d(GfRotation(axis,v[a]),GfVec3d(0));
    }
    return m;
}
// An indexed setAttr may be split over arbitrary ranges. Preserve sparse
// logical indices and last-write-wins semantics; never concatenate chunks.
// Maya geometry deformers are elementwise: output element og[N] deforms
// input element ip[N]. Bare geometry outputs carry element 0. Anything
// else is not followable geometry (-1).
long OutputElement(const std::string &plug) {
    for (const std::string &prefix : {"og", "outputGeometry"}) {
        if (plug == prefix) return 0;
        if (Starts(plug, prefix + "[")) {
            auto end = plug.find(']', prefix.size() + 1);
            if (end == std::string::npos || end + 1 != plug.size()) return -1;
            try { return Integer(plug.substr(prefix.size() + 1, end - prefix.size() - 1)); }
            catch (...) { return -1; }
        }
    }
    if (plug=="o" || plug=="os" || plug=="output" || plug=="w" || plug=="ws" || plug=="wl") return 0;
    return -1;
}
long InputElement(const std::string &plug) {
    for (const std::string &prefix : {"ip", "input"}) {
        if (!Starts(plug, prefix + "[")) continue;
        auto end = plug.find(']', prefix.size() + 1);
        std::string suffix = prefix == "ip" ? ".ig" : ".inputGeometry";
        if (end == std::string::npos || plug.substr(end) != "]" + suffix) return -1;
        try { return Integer(plug.substr(prefix.size() + 1, end - prefix.size() - 1)); }
        catch (...) { return -1; }
    }
    if (plug=="ig" || plug=="inputGeometry" || plug=="i" || plug=="inMesh" || plug=="li" || plug=="latticeInput" ||
        plug=="cr" || plug=="create" || plug=="is" || plug=="inputSurface") return 0;
    return -1;
}
// Single Boehm knot insertion for a non-rational B-spline. U has
// P.size()+p+1 entries; u must sit on the curve range with interior
// multiplicity at most p.
void InsertKnot(std::vector<GfVec3d> &P,std::vector<double> &U,int p,double u) {
    size_t k=static_cast<size_t>(p);
    while (k+1<U.size() && U[k+1]<=u) ++k;
    std::vector<GfVec3d> Q(P.size()+1);
    for (size_t j=0;j<=k-static_cast<size_t>(p);++j) Q[j]=P[j];
    for (size_t j=k-static_cast<size_t>(p)+1;j<=k;++j) {
        double denom=U[j+static_cast<size_t>(p)]-U[j];
        if (denom==0) throw std::runtime_error("invalid NURBS knot multiplicity");
        double a=(u-U[j])/denom;
        Q[j]=P[j-1]+(P[j]-P[j-1])*a;
    }
    for (size_t j=k;j<P.size();++j) Q[j+1]=P[j];
    P=std::move(Q);
    U.insert(U.begin()+static_cast<long>(k)+1,u);
}
// Exact Bezier decomposition of a clamped non-rational B-spline of degree
// 2 or 3 (quadratic spans are degree-elevated). U is the padded knot
// vector; every interior knot is raised to full multiplicity so each span
// becomes one cubic Bezier, joints shared.
std::vector<GfVec3f> BezierDecompose(const std::vector<GfVec3f> &points,const std::vector<double> &knots,int p) {
    std::vector<GfVec3d> P(points.begin(),points.end());
    std::vector<double> U=knots;
    size_t np=P.size();
    // Collect first: insertions shift every later index.
    std::vector<std::pair<double,size_t>> interior;
    for (size_t j=static_cast<size_t>(p);j<=np;) {
        size_t m=1;
        while (j+m<U.size() && U[j+m]==U[j]) ++m;
        if (U[j]>U[p] && U[j]<U[np]) {
            if (m>static_cast<size_t>(p)) throw std::runtime_error("invalid NURBS knot multiplicity");
            interior.emplace_back(U[j],m);
        }
        j+=m;
    }
    for (const auto &knot:interior) for (size_t t=knot.second;t<static_cast<size_t>(p);++t) InsertKnot(P,U,p,knot.first);
    // Each distinct interior knot splits one span; np-p would also count
    // the empty spans at full-multiplicity knots.
    size_t spans=interior.size()+1;
    std::vector<GfVec3f> out;
    for (size_t s=0;s<spans;++s) {
        std::vector<GfVec3d> bez(p==3 ?
            std::vector<GfVec3d>{P[s*3],P[s*3+1],P[s*3+2],P[s*3+3]} :
            std::vector<GfVec3d>{P[s*2],(P[s*2]+P[s*2+1]*2.0)/3.0,(P[s*2+1]*2.0+P[s*2+2])/3.0,P[s*2+2]});
        for (size_t j=(s==0 ? 0 : 1);j<bez.size();++j) out.emplace_back(bez[j]);
    }
    return out;
}
// Controller guides are local polylines. Keep the source spline separately;
// evaluate each nonempty knot span, including rational and periodic shapes.
std::vector<GfVec3f> CurveGuide(const std::vector<GfVec3f> &points,
    const std::vector<double> &knots,const std::vector<double> &weights,int degree) {
    if (degree==1) return points;
    if (points.size()<=size_t(degree)) throw std::runtime_error("too few curve CVs");
    auto evaluate=[&](double u,size_t span) {
        std::vector<GfVec4d> d;
        for(int j=0;j<=degree;++j) {
            size_t i=span-size_t(degree)+size_t(j);
            double w=weights.empty() ? 1.0 : weights.at(i);
            d.emplace_back(points[i][0]*w,points[i][1]*w,points[i][2]*w,w);
        }
        for(int r=1;r<=degree;++r) for(int j=degree;j>=r;--j) {
            size_t i=span-size_t(degree)+size_t(j);
            double denominator=knots[i+size_t(degree-r+1)]-knots[i];
            double a=denominator>0 ? (u-knots[i])/denominator : 0;
            d[j]=d[j-1]*(1-a)+d[j]*a;
        }
        if(std::abs(d[degree][3])<1e-12) throw std::runtime_error("zero rational curve weight");
        return GfVec3f(d[degree][0]/d[degree][3],d[degree][1]/d[degree][3],d[degree][2]/d[degree][3]);
    };
    std::vector<GfVec3f> result;
    for(size_t span=size_t(degree);span<points.size();++span) {
        if(knots[span]>=knots[span+1]) continue;
        // Twenty-four segments per span are independent of CV spacing.
        for(int j=0;j<=24;++j) {
            if(j==0 && !result.empty()) continue;
            double u=knots[span]+(knots[span+1]-knots[span])*j/24.0;
            result.push_back(evaluate(u,span));
        }
    }
    return result;
}
std::pair<size_t,size_t> Range(const std::string &name,const std::string &prefix) {
    if (!Starts(name,prefix+"[")) throw std::runtime_error("invalid indexed attribute " + name);
    auto end=name.find(']',prefix.size()+1);
    if (end==std::string::npos) throw std::runtime_error("invalid range " + name);
    std::string r=name.substr(prefix.size()+1,end-prefix.size()-1);
    auto colon=r.find(':');
    size_t first=Count(r.substr(0,colon));
    size_t last=colon==std::string::npos ? first : Count(r.substr(colon+1));
    if (last<first || last>=MaxElements) throw std::runtime_error("invalid range " + name);
    return {first,last};
}
std::vector<double> Array(const Node &node,const Strings &prefixes,size_t width) {
    std::vector<double> result; std::vector<bool> filled;
    for (const auto &a:node.attributes) for (const auto &prefix:prefixes) {
        if (!Starts(a.name,prefix+"[") || a.values.empty()) continue;
        auto r=Range(a.name,prefix); size_t count=r.second-r.first+1;
        if (a.values.size()!=count*width) throw std::runtime_error("array size mismatch: " + node.name+"."+a.name);
        if (result.size()<(r.second+1)*width) { result.resize((r.second+1)*width); filled.resize(r.second+1,false); }
        for (size_t j=0;j<a.values.size();++j) result[r.first*width+j]=Number(a.values[j]);
        for (size_t j=r.first;j<=r.second;++j) filled[j]=true;
    }
    if (std::find(filled.begin(),filled.end(),false)!=filled.end()) throw std::runtime_error("sparse geometry array: " + node.name);
    return result;
}
template<class T> VtArray<T> Vt(const std::vector<T> &v) { return VtArray<T>(v.begin(),v.end()); }
struct Edge { int a,b; };
struct Face { std::vector<int> edges,uv; };
struct Link { int src=-1,dst=-1; std::string out,in; bool used=false; };
class Importer {
public:
    Scene scene;
    std::string source;
    UsdStageRefPtr stage=UsdStage::CreateInMemory();
    RigExecRigBuilder rig=RigExecRigBuilder::Create(stage);
    std::vector<int> parent,state;
    std::vector<std::string> full;
    std::vector<SdfPath> dag,geometry;
    std::vector<GfMatrix4d> world;
    std::vector<Link> links;
    std::set<int> translated;
    std::map<int,UsdShadeMaterial> materials;
    struct SkinLayout { std::vector<SdfPath> providers; std::vector<int> indices; std::vector<float> weights; int slots=1,method=0; };
    // Attachment triangles read only their weighted influences. Retaining
    // the entire mesh palette makes a head edit invalidate unrelated pins
    // throughout the body and evaluates thousands of unused matrices.
    static void CompactSkinPalette(SkinLayout *layout) {
        if(layout->indices.size()!=layout->weights.size())throw std::runtime_error("invalid attachment skin layout");
        std::set<int> used;
        for(size_t j=0;j<layout->indices.size();++j)if(layout->weights[j]!=0) {
            int index=layout->indices[j];
            if(index<0 || size_t(index)>=layout->providers.size())throw std::runtime_error("invalid attachment influence index");
            used.insert(index);
        }
        if(used.empty()) {
            if(layout->providers.empty())return;
            used.insert(0); // Keep zero-weight layout valid; points pass through.
        }
        std::map<int,int> remap;std::vector<SdfPath> providers;
        for(int index:used) {remap[index]=int(providers.size());providers.push_back(layout->providers[index]);}
        for(size_t j=0;j<layout->indices.size();++j)layout->indices[j]=layout->weights[j]==0?0:remap.at(layout->indices[j]);
        layout->providers=std::move(providers);
    }
    std::map<int,SkinLayout> skinLayouts;
    std::map<int,SkinLayout> loftLayouts;
    std::map<int,SdfPath> skinMovers,filterMovers;
    std::map<int,int> meshInputs;
    std::map<int,GfMatrix4d> geometryMatrices;
    struct ScalarDestination { int node; std::string attribute; SdfPath destination; };
    std::vector<ScalarDestination> scalarDestinations;
    std::vector<ScalarDestination> scalarReads;
    std::set<std::pair<int,char>> absoluteScales;
    std::map<int,std::pair<SdfPath,int>> constraintOutputs;
    std::map<int,SdfPath> constraintRotationSources;
    std::map<int,GfMatrix4d> constantOpm;
    std::map<int,GfVec3d> initialInverseScales;
    std::map<int,GfVec3d> constraintRestTranslations,constraintRestRotations;
    bool refreshingDag=false;
    struct BindProvider { int source,bindSource; GfMatrix4d inverseBind; SdfPath path; };
    std::vector<BindProvider> bindProviders;
    std::vector<SdfAssetPath> dependencies;
    explicit Importer(Scene s,std::string p):scene(std::move(s)),source(std::move(p)) {
        size_t n=scene.nodes.size(); parent.resize(n,-1); state.resize(n,0);
        full.resize(n); dag.resize(n); geometry.resize(n); world.resize(n,GfMatrix4d(1));
    }
    void Warn(const std::string &s) { scene.diagnostics.push_back(s); }
    double Constant(int index,const std::string &attr,double fallback,std::set<std::pair<int,std::string>> &active) const {
        auto key=std::make_pair(index,attr);if(!active.insert(key).second)return fallback;
        for(const auto &l:links)if(l.dst==index && l.src>=0 && l.in==attr) {
            double value=Constant(l.src,l.out,fallback,active);active.erase(key);return value;
        }
        const auto &node=scene.nodes[index];double value=Scalar(node,attr,{},fallback);
        if(node.type=="reverse" && attr.size()==2 && attr[0]=='o')value=1-Constant(index,std::string("i")+attr[1],0,active);
        if(node.type=="condition" && (Starts(attr,"oc") || Starts(attr,"outColor"))) {
            const float a=float(Constant(index,"ft",Scalar(node,"ft","firstTerm",0),active)),b=float(Constant(index,"st",Scalar(node,"st","secondTerm",0),active));
            const int op=int(Scalar(node,"op","operation",0));
            if(op<0 || op>5)throw std::runtime_error("invalid Maya condition operation");
            const bool test=op==0?a==b:op==1?a!=b:op==2?a>b:op==3?a>=b:op==4?a<b:a<=b;
            const int axis=int(std::string("rgb").find(char(std::tolower(attr.back()))));
            if(axis<0 || axis>2)throw std::runtime_error("condition scalar output requires a color component");
            const auto shortName=std::string(test?"ct":"cf"),longName=std::string(test?"colorIfTrue":"colorIfFalse");
            value=Constant(index,shortName+"rgb"[axis],Vector(node,shortName,longName,GfVec3d(test?0:1))[axis],active);
        }
        active.erase(key);return value;
    }
    int Find(const std::string &s) const {
        if(!s.empty() && s[0]==':') return Find(s.substr(1));
        int result=-1;
        for (size_t i=0;i<scene.nodes.size();++i) {
            if (s==full[i] || (!full[i].empty() && s==full[i].substr(1))) return static_cast<int>(i);
            if (s==scene.nodes[i].name) {
                if (result>=0) throw std::runtime_error("ambiguous DAG name: " + s);
                result=static_cast<int>(i);
            }
        }
        return result;
    }
    bool Transform(int i) const { return scene.nodes[i].type=="transform" || scene.nodes[i].type=="joint" || scene.nodes[i].type=="ikHandle"; }
    bool Visible(int index) const {
        for(int i=index;i>=0;i=parent[i]) {
            if(Scalar(scene.nodes[i],"v","visibility",1)==0) return false;
        }
        return Scalar(scene.nodes[index],"io","intermediateObject",0)==0;
    }
    std::string Asset(const std::string &s) {
        if (s.empty()) return s;
        auto id=ArGetResolver().CreateIdentifier(s,ArResolvedPath(source));
        dependencies.emplace_back(id);
        if (ArGetResolver().Resolve(id).empty()) Warn("unresolved asset dependency: " + id);
        return id;
    }
    GfVec3d Pivot(int index) const { return index<0 ? GfVec3d(0) : Vector(scene.nodes[index],"rp","rotatePivot"); }
    GfMatrix4d OffsetMatrix(int index) const {
        auto found=constantOpm.find(index);return found==constantOpm.end() ? Matrix(Attr(scene.nodes[index],"opm","offsetParentMatrix")) : found->second;
    }
    void ConstantMatrixInputs() {
        for(const auto &connection:scene.connections) {
            auto from=Plug(connection.source),to=Plug(connection.destination);if(to.second!="opm" && to.second!="offsetParentMatrix")continue;
            int src=Find(from.first),dst=Find(to.first);if(src<0 || dst<0 || scene.nodes[src].type!="composeMatrix")continue;
            const auto &node=scene.nodes[src];bool connected=false;
            for(const auto &input:scene.connections)if(Plug(input.destination).first==from.first && Plug(input.destination).second!="msg")connected=true;
            if(connected || Scalar(node,"uer","useEulerRotation",1)==0)continue;
            auto scale=Vector(node,"is","inputScale",GfVec3d(1));auto shear=Vector(node,"ish","inputShear");
            auto rotation=Vector(node,"ir","inputRotate")*(scene.angularUnit=="rad"?180.0/3.14159265358979323846:1);
            GfMatrix4d m(1),sh(1);m.SetScale(scale);sh[1][0]=shear[0];sh[2][0]=shear[1];sh[2][1]=shear[2];
            constantOpm[dst]=m*sh*Rotation(rotation,int(Scalar(node,"ro","inputRotateOrder",0)))*Translation(Vector(node,"it","inputTranslate"));
            translated.insert(src);
        }
    }
    GfMatrix4d MayaLocal(int index) const { return world[index]*(parent[index]<0 || Scalar(scene.nodes[index],"it","inheritsTransform",1)==0 ? GfMatrix4d(1) : world[parent[index]].GetInverse()); }
    GfVec3d InverseScale(int index) const {
        const auto found=initialInverseScales.find(index);
        return found==initialInverseScales.end()?Vector(scene.nodes[index],"is","inverseScale",GfVec3d(1)):found->second;
    }
    bool ConstraintRestInputs() {
        for(size_t i=0;i<scene.nodes.size();++i) {
            const auto &n=scene.nodes[i];
            if((n.type!="parentConstraint" && n.type!="pointConstraint" && n.type!="orientConstraint") ||
               Scalar(n,"erp","enableRestPosition",0)==0)continue;
            std::set<int> slots;
            for(const auto &l:links)if(l.dst==int(i) && l.src>=0 && !dag[l.src].IsEmpty()) {
                if(Starts(l.in,"tg["))slots.insert(int(Range(l.in,"tg").first));
                else if(Starts(l.in,"target["))slots.insert(int(Range(l.in,"target").first));
            }
            if(slots.empty())continue;
            bool zero=true;
            for(int slot:slots) {
                const auto a="tg["+std::to_string(slot)+"].tw",b="target["+std::to_string(slot)+"].targetWeight";
                std::set<std::pair<int,std::string>> active;
                if(Constant(int(i),a,Scalar(n,a,b,1),active)!=0)zero=false;
            }
            if(!zero)continue;
            // Maya emits the authored local rest channels when every target
            // has zero weight. Seed those channels before geometry placement
            // and affine providers are built; source metadata stays intact.
            for(const auto &l:links)if(l.src==int(i) && l.dst>=0 && !dag[l.dst].IsEmpty()) {
                for(int group=0;group<2;++group) {
                    if((group==0 && n.type=="orientConstraint") || (group==1 && n.type=="pointConstraint"))continue;
                    const std::string out=group==0?"ct":"cr",longOut=group==0?"constraintTranslate":"constraintRotate";
                    const std::string in=group==0?"t":"r",longIn=group==0?"translate":"rotate";
                    auto &overrides=group==0?constraintRestTranslations:constraintRestRotations;
                    auto rest=Vector(n,group==0?"rst":"rsr",group==0?"restTranslate":"restRotate");
                    auto value=overrides.count(l.dst)?overrides.at(l.dst):Vector(scene.nodes[l.dst],in,longIn);
                    bool used=false;
                    if((l.out==out || l.out==longOut) && (l.in==in || l.in==longIn)) {value=rest;used=true;}
                    for(int axis=0;axis<3;++axis)if((l.out==out+"xyz"[axis] || l.out==longOut+"XYZ"[axis]) &&
                        (l.in==in+"xyz"[axis] || l.in==longIn+"XYZ"[axis])) {value[axis]=rest[axis];used=true;}
                    if(used)overrides[l.dst]=value;
                }
            }
        }
        return !constraintRestTranslations.empty() || !constraintRestRotations.empty();
    }
    void BuildDag(int i) {
        if (state[i]==2) return;
        if (state[i]==1) throw std::runtime_error("DAG cycle: " + scene.nodes[i].name);
        state[i]=1;
        const Node &n=scene.nodes[i];
        if (!n.parent.empty()) {
            // Maya writes parents before children. Exact full paths and unique
            // short names are accepted; unresolved parents must not flatten.
            parent[i]=Find(n.parent);
            if (parent[i]<0) throw std::runtime_error("unresolved DAG parent: " + n.parent);
            BuildDag(parent[i]);
        }
        full[i]=(parent[i]<0 ? "" : full[parent[i]])+"|"+n.name;
        if (Transform(i)) {
            auto path=parent[i]<0 ? SdfPath("/Rig/Dag") : dag[parent[i]];
            if (path.IsEmpty()) throw std::runtime_error("non-transform DAG parent: " + n.name);
            dag[i]=path.AppendChild(TfToken(Name(n.name)));
            if (!refreshingDag && stage->GetPrimAtPath(dag[i])) throw std::runtime_error("duplicate DAG path: " + full[i]);
            auto schema=RigExecSchemaPrim::Define(stage,dag[i],TfToken(n.type=="joint" ? "RigExecJoint" : "RigExecControl"));
            schema.ApplyAPI(TfToken("NodeGraphNodeAPI"));
            if (n.type=="transform") schema.ApplyAPI(TfToken("RigExecControlAPI"));
            double angle=scene.angularUnit=="rad" ? 180.0/3.14159265358979323846 : 1;
            auto t=Vector(n,"t","translate"), r=Vector(n,"r","rotate")*angle;
            if(constraintRestTranslations.count(i))t=constraintRestTranslations.at(i);
            if(constraintRestRotations.count(i))r=constraintRestRotations.at(i)*angle;
            auto scale=Vector(n,"s","scale",GfVec3d(1));
            if(n.type=="joint")for(int axis=0;axis<3;++axis)if(scale[axis]==0)scale[axis]=1e-12;
            auto shear=Vector(n,"sh","shear");
            auto ra=Vector(n,"ra","rotateAxis")*angle, jo=Vector(n,"jo","jointOrient")*angle;
            auto rp=Vector(n,"rp","rotatePivot"), sp=Vector(n,"sp","scalePivot");
            static const char *orders[]={"XYZ","YZX","ZXY","XZY","YXZ","ZYX"};
            int rotationOrder=int(Scalar(n,"ro","rotateOrder",0));
            if(rotationOrder<0 || rotationOrder>5)throw std::runtime_error("invalid rotateOrder");
            schema.SetAttribute(TfToken("avars:rotationOrder"),VtValue(TfToken(orders[rotationOrder])));
            GfMatrix4d sm(1); sm.SetScale(scale);
            GfMatrix4d sh(1); sh[1][0]=shear[0]; sh[2][0]=shear[1]; sh[2][1]=shear[2];
            GfMatrix4d compensation(1);
            if(n.type=="joint" && Scalar(n,"ssc","segmentScaleCompensate",1)!=0) {
                auto inverse=InverseScale(i);for(int axis=0;axis<3;++axis)inverse[axis]=1/(inverse[axis]==0?1e-12:inverse[axis]);compensation.SetScale(inverse);
            }
            GfMatrix4d local=Translation(-sp)*sm*sh*Translation(sp)*Translation(Vector(n,"spt","scalePivotTranslate"))*
                Translation(-rp)*Rotation(ra,0)*Rotation(r,static_cast<int>(Scalar(n,"ro","rotateOrder",0)))*
                Rotation(jo,0)*compensation*Translation(rp)*Translation(Vector(n,"rpt","rotatePivotTranslate"))*Translation(t);
            local*=OffsetMatrix(i);
            world[i]=local*(parent[i]<0 || Scalar(n,"it","inheritsTransform",1)==0 ? GfMatrix4d(1) : world[parent[i]]);
            // Constraints target Maya's rotate pivot. Put the RigExec frame at
            // that pivot, retaining the original DAG matrix for geometry data.
            schema.SetAttribute(TfToken("rest:space"),VtValue(Translation(rp)*local*Translation(-Pivot(parent[i]))));
            schema.GetPrim().SetCustomDataByKey(TfToken("maya:name"),VtValue(full[i]));
            schema.GetPrim().SetCustomDataByKey(TfToken("maya:rotatePivot"),VtValue(rp));
            schema.GetPrim().SetDisplayName(n.name);
            // Maya's curve shapes are the interaction surface. Generic
            // RigExec rings on every group/offset transform obscure it.
            if(n.type!="joint") schema.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
            if(!Visible(i)) {
                UsdGeomImageable(schema.GetPrim()).CreateVisibilityAttr().Set(TfToken("invisible"));
                if(n.type=="joint") schema.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
            }
            UsdGeomImageable(schema.GetPrim()).CreatePurposeAttr().Set(TfToken("guide"));
            translated.insert(i);
        } else if (parent[i]>=0) world[i]=world[parent[i]];
        state[i]=2;
    }
    void Graph() {
        stage->DefinePrim(SdfPath("/Rig/Dag"),TfToken("Scope"));
        ConstantMatrixInputs();
        for (size_t i=0;i<scene.nodes.size();++i) BuildDag(static_cast<int>(i));
        for (const auto &c:scene.connections) {
            auto a=Plug(c.source),b=Plug(c.destination);
            links.push_back({Find(a.first),Find(b.first),a.second,b.second,false});
            if(constantOpm.count(links.back().dst) && links.back().src>=0 && scene.nodes[links.back().src].type=="composeMatrix" && (b.second=="opm" || b.second=="offsetParentMatrix"))links.back().used=true;
        }
        bool refresh=ConstraintRestInputs();
        for(const auto &l:links)if(l.src>=0 && l.dst>=0 && scene.nodes[l.dst].type=="joint" &&
            (l.in=="is" || l.in=="inverseScale") && (l.out=="s" || l.out=="scale")) {
            initialInverseScales[l.dst]=Vector(scene.nodes[l.src],"s","scale",GfVec3d(1));refresh=true;
        }
        if(refresh) {
            std::fill(state.begin(),state.end(),0);refreshingDag=true;
            for(size_t i=0;i<scene.nodes.size();++i)BuildDag(int(i));
            refreshingDag=false;
        }
        auto root=stage->GetPrimAtPath(SdfPath("/Rig"));
        Strings src,dst;
        for (const auto &c:scene.connections) { src.push_back(c.source); dst.push_back(c.destination); }
        root.CreateAttribute(TfToken("maya:connectionSources"),SdfValueTypeNames->StringArray).Set(Vt(src));
        root.CreateAttribute(TfToken("maya:connectionDestinations"),SdfValueTypeNames->StringArray).Set(Vt(dst));
        root.CreateAttribute(TfToken("maya:commands"),SdfValueTypeNames->StringArray).Set(Vt(scene.commands));
        VtDictionary sourceNodes;
        for (size_t i=0;i<scene.nodes.size();++i) {
            const auto &n=scene.nodes[i];
            VtDictionary node;
            node["name"]=VtValue(full[i]); node["type"]=VtValue(n.type);
            VtDictionary attributes;
            for (size_t j=0;j<n.attributes.size();++j) {
                const auto &a=n.attributes[j]; VtDictionary data;
                data["name"]=VtValue(a.name); data["type"]=VtValue(a.type); data["values"]=VtValue(Vt(a.values));
                static const std::set<std::string> assets={"ftn","fileTextureName","fn","filename","fileName","abc_File","cachePath","cacheName"};
                if (assets.count(a.name) && a.type=="string" && !a.values.empty())
                    data["asset"]=VtValue(SdfAssetPath(Asset(a.values[0])));
                attributes["a"+std::to_string(j)]=VtValue(data);
            }
            node["attributes"]=VtValue(attributes);
            if (!dag[i].IsEmpty()) node["translated"]=VtValue(dag[i].GetString());
            sourceNodes["n"+std::to_string(i)]=VtValue(node);
        }
        root.SetCustomDataByKey(TfToken("maya:sourceNodes"),VtValue(sourceNodes));
    }
    int InputMesh(int index,long element,std::set<int> &visited) {
        if (!visited.insert(index).second) throw std::runtime_error("geometry dependency cycle");
        const auto &node=scene.nodes[index];
        // The unsubdivided three-sided disc is a common attachment carrier.
        // It has no mesh cache: its three vertices and UVs are generated by
        // the creator. Other disc tessellations remain explicitly unsupported.
        if(node.type=="polyDisc" && Scalar(node,"sides",{},3)==3 &&
           Scalar(node,"subdivisions",{},1)==0 && Scalar(node,"subdivisionMode",{},4)==4)
            return index;
        for (auto &l:links) if (l.dst==index && l.src>=0 && InputElement(l.in)==element) {
            long next=OutputElement(l.out);
            int found=InputMesh(l.src,next<0 ? 0 : next,visited);
            if(found>=0) {
                l.used=true;
                if(node.type=="groupParts") translated.insert(index);
                return found;
            }
        }
        if (node.type=="mesh" || node.type=="nurbsSurface" || node.type=="nurbsCurve" || node.type=="lattice") {
            // Prefer the input history over cached intermediate vertices.
            // Stored points are the fallback for an unmodeled procedural input.
            bool stored=false;
            if (node.type=="mesh") {
                for (const auto &a:node.attributes)
                    if (!a.values.empty() && (Starts(a.name,"vt[") || Starts(a.name,"vrts["))) { stored=true; break; }
            } else {
                auto data=Attr(node,"cc","cached");
                stored=data && data->type==node.type;
            }
            if (stored) return index;
        }
        return -1;
    }
    int MeshData(int index) {
        std::string driver;
        for (auto &l:links) if (l.dst==index && l.src>=0 && (l.in=="i" || l.in=="inMesh")) {
            long element=OutputElement(l.out);
            std::set<int> visited; int found=InputMesh(l.src,element<0 ? 0 : element,visited);
            if (found>=0) { l.used=true; return found; }
            if (driver.empty()) driver=scene.nodes[l.src].name+"."+l.out;
        }
        if (!driver.empty()) Warn("mesh input has no stored static vertices upstream ("+driver+" -> "+full[index]+".i)");
        return index;
    }
    int InputSkin(int index,long element,std::set<int> &visited) const {
        if(!visited.insert(index).second) throw std::runtime_error("skin input dependency cycle");
        if(scene.nodes[index].type=="skinCluster") return index;
        for(const auto &l:links) if(l.dst==index && l.src>=0 && InputElement(l.in)==element) {
            long next=OutputElement(l.out);
            return InputSkin(l.src,next<0 ? 0 : next,visited);
        }
        return -1;
    }
    // Geometry filters may sit between a skin and its visible shape. Follow
    // only geometry plugs, preserving the deformed element in both
    // directions, never message/set connections. A filter without a rig
    // mover stays on the mesh as an unsupported entry, never silently.
    void SkinOutputs(int index,long element,std::set<int> &visiting,
                     std::set<int> &targets,std::vector<std::string> &filters) {
        if(!visiting.insert(index).second) throw std::runtime_error("skin output dependency cycle");
        for(auto &l:links) if(l.src==index && l.dst>=0 && OutputElement(l.out)==element) {
            long next=InputElement(l.in);
            if(next<0) continue;
            const auto &nextNode=scene.nodes[l.dst];
            if(((nextNode.type=="mesh" && (l.in=="i" || l.in=="inMesh")) ||
                ((nextNode.type=="nurbsSurface" || nextNode.type=="nurbsCurve") && (l.in=="cr" || l.in=="create"))) && !geometry[l.dst].IsEmpty()) {
                targets.insert(l.dst); l.used=true;
            } else if(nextNode.type=="groupParts" && (l.in=="ig" || l.in=="inputGeometry")) {
                l.used=true; translated.insert(l.dst); SkinOutputs(l.dst,0,visiting,targets,filters);
            } else if(nextNode.type=="deltaMush" && next>=0) {
                // deltaMush preserves point indices, so its presence must not
                // erase the upstream skin binding. Its correction is not LBS.
                l.used=true;
                if(Scalar(nextNode,"en","envelope",1)!=0) filters.push_back(nextNode.name);
                else translated.insert(l.dst);
                SkinOutputs(l.dst,next,visiting,targets,filters);
            } else if((nextNode.type=="ffd" || nextNode.type=="nonLinear" || nextNode.type=="cluster" ||
                       nextNode.type=="wrap" || nextNode.type=="blendShape") && next>=0) {
                // The skin binding survives through the unmodeled deformer;
                // Skin() records it as unsupported on the target mesh.
                l.used=true; filters.push_back(nextNode.name);
                SkinOutputs(l.dst,next,visiting,targets,filters);
            }
        }
        visiting.erase(index);
    }
    void Surface(int index) {
        const auto &node=scene.nodes[index];
        if(Scalar(node,"io","intermediateObject",0)!=0) {translated.insert(index);return;}
        int dataIndex=index;
        for(auto &l:links) if(l.dst==index && l.src>=0 && (l.in=="cr" || l.in=="create")) {
            long element=OutputElement(l.out);
            std::set<int> seen; int found=InputMesh(l.src,element<0 ? 0 : element,seen);
            if(found>=0) {dataIndex=found;l.used=true;break;}
        }
        const auto &data=scene.nodes[dataIndex];auto a=Attr(data,"cc","cached");
        if(!a || a->type!="nurbsSurface") {
            if(LoftSurface(index))return;
            Warn("surface has no stored NURBS data: "+full[index]);return;
        }
        size_t k=0; auto take=[&]() -> const std::string & {if(k>=a->values.size())throw std::runtime_error("truncated NURBS surface");return a->values[k++];};
        int ud=Integer(take()),vd=Integer(take()),uf=Integer(take()),vf=Integer(take());bool rational=Number(take())!=0;
        if(ud<1 || ud>16 || vd<1 || vd>16 || uf!=0 || vf!=0) {Warn("unsupported non-open NURBS surface: "+full[index]);return;}
        std::vector<double> uk,vk;size_t un=Count(take());for(size_t i=0;i<un;++i)uk.push_back(Number(take()));
        size_t vn=Count(take());for(size_t i=0;i<vn;++i)vk.push_back(Number(take()));
        if(un<size_t(ud+1) || vn<size_t(vd+1))throw std::runtime_error("invalid NURBS knot counts");
        un=un-ud+1;vn=vn-vd+1;
        uk.insert(uk.begin(),uk.front());uk.push_back(uk.back());vk.insert(vk.begin(),vk.front());vk.push_back(vk.back());
        if(Count(take())!=un*vn)throw std::runtime_error("NURBS CV count mismatch");
        GfMatrix4d gm=world[index];std::set<int> visited;int skin=InputSkin(index,0,visited);
        if(skin>=0)gm=Matrix(Attr(scene.nodes[skin],"gm","geomMatrix"),gm);
        std::vector<GfVec3f> points;std::vector<double> weights;
        for(size_t i=0;i<un*vn;++i) {
            double x=Number(take()),y=Number(take()),z=Number(take());
            points.emplace_back(gm.Transform(GfVec3d(x,y,z)));weights.push_back(rational?Number(take()):1.0);
        }
        if(k!=a->values.size())throw std::runtime_error("trailing NURBS surface data");
        auto path=SdfPath("/Rig/Geometry").AppendChild(TfToken(Name(full[index])));
        auto surface=UsdGeomNurbsPatch::Define(stage,path);geometry[index]=path;
        surface.CreatePointsAttr().Set(Vt(points));surface.CreatePointWeightsAttr().Set(Vt(weights));
        surface.CreateUVertexCountAttr().Set(int(un));surface.CreateVVertexCountAttr().Set(int(vn));
        surface.CreateUOrderAttr().Set(ud+1);surface.CreateVOrderAttr().Set(vd+1);
        surface.CreateUKnotsAttr().Set(Vt(uk));surface.CreateVKnotsAttr().Set(Vt(vk));
        surface.CreateURangeAttr().Set(GfVec2d(uk[ud],uk[un]));surface.CreateVRangeAttr().Set(GfVec2d(vk[vd],vk[vn]));
        surface.CreateUFormAttr().Set(TfToken("open"));surface.CreateVFormAttr().Set(TfToken("open"));
        surface.CreatePurposeAttr().Set(TfToken("guide"));
        surface.GetPrim().SetCustomDataByKey(TfToken("maya:name"),VtValue(full[index]));
        translated.insert(index);translated.insert(dataIndex);
    }
    void Lattice(int index) {
        if(Scalar(scene.nodes[index],"io","intermediateObject",0)!=0)return;
        std::set<int> seen;int input=InputMesh(index,0,seen);if(input<0)return;
        auto a=Attr(scene.nodes[input],"cc","cached");
        if(!a || a->type!="lattice" || a->values.size()<4)return;
        int nx=Integer(a->values[0]),ny=Integer(a->values[1]),nz=Integer(a->values[2]);size_t count=Count(a->values[3]);
        if(nx<2 || ny<2 || nz<2 || size_t(nx)*ny*nz!=count || a->values.size()!=4+3*count)throw std::runtime_error("invalid lattice cache");
        const auto path=SdfPath("/Rig/Geometry").AppendChild(TfToken(Name(full[index])));
        std::vector<GfVec3f> points;
        for(size_t j=0;j<count;++j)points.emplace_back(world[index].Transform(GfVec3d(Number(a->values[4+3*j]),Number(a->values[5+3*j]),Number(a->values[6+3*j]))));
        auto prim=stage->DefinePrim(path,TfToken("Points"));UsdGeomPointBased(prim).CreatePointsAttr().Set(Vt(points));
        UsdGeomImageable(prim).CreateVisibilityAttr().Set(TfToken("invisible"));
        prim.SetCustomDataByKey(TfToken("maya:name"),VtValue(full[index]));
        geometry[index]=path;geometryMatrices[index]=world[index];meshInputs[index]=input;translated.insert(index);translated.insert(input);
    }
    bool LoftSurface(int index) {
        int source=index;std::set<int> visited;std::vector<int> rebuilds;
        while(visited.insert(source).second && scene.nodes[source].type!="loft") {
            int next=-1;for(const auto &l:links)if(l.dst==source && l.src>=0 && InputElement(l.in)==0) {next=l.src;break;}
            if(next<0)return false;
            if(scene.nodes[source].type=="rebuildSurface")rebuilds.push_back(source);
            source=next;
        }
        const auto &node=scene.nodes[source];if(node.type!="loft" || Scalar(node,"d","degree",3)!=1 || Scalar(node,"ar","autoReverse",1)!=0 || Scalar(node,"c","close",0)!=0 || Scalar(node,"rsn","reverseSurfaceNormals",0)!=0)return false;
        std::map<size_t,int> rails;
        for(const auto &l:links)if(l.dst==source && l.src>=0 && Starts(l.in,"ic["))rails[Range(l.in,"ic").first]=l.src;
        if(rails.size()<2 || rails.size()>32)return false;
        std::vector<GfVec3f> points;std::vector<int> railNodes;
        for(const auto &entry:rails) {
            int curve=entry.second;const auto *cc=Attr(scene.nodes[curve],"cc","cached");
            if(!cc || cc->type!="nurbsCurve" || parent[curve]<0)return false;
            const auto &v=cc->values;
            if(v.size()!=15 || Integer(v[0])!=1 || Integer(v[2])!=0 || Number(v[3])!=0 || Integer(v[4])!=3 || Integer(v[5])!=2 || Integer(v[8])!=2)return false;
            railNodes.push_back(curve);
            bool reverse=Scalar(node,"r["+std::to_string(entry.first)+"]",{},0)!=0;
            for(int p=0;p<2;++p) {int cp=reverse?1-p:p;points.emplace_back(Number(v[9+3*cp]),Number(v[10+3*cp]),Number(v[11+3*cp]));}
        }
        auto path=SdfPath("/Rig/Geometry").AppendChild(TfToken(Name(full[index])));
        auto surface=UsdGeomNurbsPatch::Define(stage,path);geometry[index]=path;
        std::vector<double> uk{0,0};for(size_t j=1;j<rails.size();++j)uk.push_back(double(j)/(rails.size()-1));uk.push_back(1);
        surface.CreatePointsAttr().Set(Vt(points));surface.CreatePointWeightsAttr().Set(VtDoubleArray(points.size(),1));
        surface.CreateUVertexCountAttr().Set(int(rails.size()));surface.CreateVVertexCountAttr().Set(2);
        surface.CreateUOrderAttr().Set(2);surface.CreateVOrderAttr().Set(2);
        surface.CreateUKnotsAttr().Set(Vt(uk));surface.CreateVKnotsAttr().Set(VtDoubleArray{0,0,1,1});
        surface.CreateURangeAttr().Set(GfVec2d(0,1));surface.CreateVRangeAttr().Set(GfVec2d(0,1));
        surface.CreateUFormAttr().Set(TfToken("open"));surface.CreateVFormAttr().Set(TfToken("open"));
        surface.CreatePurposeAttr().Set(TfToken("guide"));surface.CreateVisibilityAttr().Set(TfToken("invisible"));
        surface.GetPrim().SetCustomDataByKey(TfToken("maya:name"),VtValue(full[index]));
        surface.GetPrim().SetCustomDataByKey(TfToken("maya:loftChordLength"),VtValue(rebuilds.empty() && Scalar(node,"u","uniform",0)==0));
        int joint=parent[parent[railNodes.front()]];
        if(joint>=0 && scene.nodes[joint].type=="joint") {
            int axis=0;auto t=Vector(scene.nodes[joint],"t","translate");
            for(int a=1;a<3;++a)if(std::abs(t[a])>std::abs(t[axis]))axis=a;
            surface.GetPrim().SetCustomDataByKey(TfToken("maya:loftTangentAxis"),VtValue(axis));
        }
        SkinLayout layout;
        for(size_t rail=0;rail<railNodes.size();++rail) {
            int control=parent[railNodes[rail]];
            auto provider=RigExecSchemaPrim::Define(stage,SdfPath("/Rig/BindFrames").AppendChild(TfToken("loft_"+std::to_string(index)+"_"+std::to_string(rail))),TfToken("RigExecJoint"));
            provider.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));provider.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
            auto native=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("loftFrame_"+std::to_string(index)+"_"+std::to_string(rail))),TfToken("RigExecMayaSkinInfluence"));
            native.CreateAttribute(TfToken("inputs:inverseBind"),SdfValueTypeNames->Matrix4d).Set(GfMatrix4d(1));
            native.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(control));
            native.CreateAttribute(TfToken("inputs:bindPivot"),SdfValueTypeNames->Double3).Set(GfVec3d(0));
            native.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[control]});native.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets({dag[control]});
            auto matrix=native.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);provider.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({matrix.GetPath()});
            layout.providers.push_back(provider.GetPath());layout.indices.push_back(int(rail));layout.indices.push_back(int(rail));layout.weights.push_back(1);layout.weights.push_back(1);
        }
        loftLayouts[index]=layout;translated.insert(index);translated.insert(source);
        for(int rebuild:rebuilds)Warn("rebuildSurface evaluated using its linear loft input; rebuild fit is unsupported: "+scene.nodes[rebuild].name);
        for(auto &l:links)if(l.dst==source && Starts(l.in,"ic["))l.used=true;
        return true;
    }
    bool NurbsPin(int sourceSurface,int target,const SdfPath &path,double u,double v,
                  bool normalized,int normal,int tangent,bool localChannels) {
        if(sourceSurface<0 || geometry[sourceSurface].IsEmpty() || !stage->GetPrimAtPath(geometry[sourceSurface]).IsA<UsdGeomNurbsPatch>()) return false;
        std::set<int> visited;int skin=InputSkin(sourceSurface,0,visited);
        if(!loftLayouts.count(sourceSurface) && (skin<0 || !skinLayouts.count(skin) || skinLayouts[skin].method!=0)) return false;
        const auto &layout=loftLayouts.count(sourceSurface) ? loftLayouts.at(sourceSurface) : skinLayouts.at(skin);
        auto surface=UsdGeomNurbsPatch(stage->GetPrimAtPath(geometry[sourceSurface]));
        auto pin=stage->DefinePrim(path,TfToken("RigExecNurbsSurfacePin"));
        auto set=[&](const char *name,const SdfValueTypeName &type,const auto &value){pin.CreateAttribute(TfToken(name),type).Set(value);};
        auto copy=[&](const char *to,UsdAttribute from){VtValue value;from.Get(&value);pin.CreateAttribute(TfToken(to),from.GetTypeName()).Set(value);};
        VtVec3fArray raw;surface.GetPointsAttr().Get(&raw);std::vector<GfVec3d> points(raw.begin(),raw.end());
        set("inputs:points",SdfValueTypeNames->Double3Array,Vt(points));
        auto chord=surface.GetPrim().GetCustomDataByKey(TfToken("maya:loftChordLength"));
        set("inputs:loftChordLength",SdfValueTypeNames->Bool,chord.IsHolding<bool>() && chord.UncheckedGet<bool>());
        auto axis=surface.GetPrim().GetCustomDataByKey(TfToken("maya:loftTangentAxis"));
        set("inputs:loftTangentAxis",SdfValueTypeNames->Int,axis.IsHolding<int>() ? axis.UncheckedGet<int>() : -1);
        copy("inputs:rationalWeights",surface.GetPointWeightsAttr());copy("inputs:uKnots",surface.GetUKnotsAttr());copy("inputs:vKnots",surface.GetVKnotsAttr());
        copy("inputs:uCount",surface.GetUVertexCountAttr());copy("inputs:vCount",surface.GetVVertexCountAttr());
        int uo=0,vo=0;surface.GetUOrderAttr().Get(&uo);surface.GetVOrderAttr().Get(&vo);
        set("inputs:uDegree",SdfValueTypeNames->Int,uo-1);set("inputs:vDegree",SdfValueTypeNames->Int,vo-1);
        GfVec2d ur,vr;surface.GetURangeAttr().Get(&ur);surface.GetVRangeAttr().Get(&vr);
        set("inputs:u",SdfValueTypeNames->Double,normalized ? ur[0]+u*(ur[1]-ur[0]) : u);
        set("inputs:v",SdfValueTypeNames->Double,normalized ? vr[0]+v*(vr[1]-vr[0]) : v);
        set("inputs:normalAxis",SdfValueTypeNames->Int,normal);set("inputs:tangentAxis",SdfValueTypeNames->Int,tangent);
        set("inputs:jointIndices",SdfValueTypeNames->IntArray,Vt(layout.indices));set("inputs:jointWeights",SdfValueTypeNames->FloatArray,Vt(layout.weights));
        set("inputs:elementSize",SdfValueTypeNames->Int,layout.slots);
        GfMatrix4d local(1);
        if(localChannels)local=Translation(Pivot(target))*MayaLocal(target);
        else local=Translation(Pivot(target));
        set("inputs:local",SdfValueTypeNames->Matrix4d,local);
        set("inputs:parentPivot",SdfValueTypeNames->Double3,Pivot(parent[target]));
        pin.CreateRelationship(TfToken("rigExec:influences")).SetTargets(layout.providers);
        SdfPathVector poseInputs(layout.providers.begin(),layout.providers.end());
        if(parent[target]>=0) {
            pin.CreateRelationship(TfToken("rigExec:parent")).SetTargets({dag[parent[target]]});
            poseInputs.push_back(dag[parent[target]]);
        }
        pin.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(poseInputs);
        auto output=pin.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);
        stage->GetPrimAtPath(dag[target]).GetAttribute(TfToken("posed:space")).SetConnections({output.GetPath()});
        return true;
    }
    bool MeshPin(int sourceMesh,int target,const SdfPath &path,double u,double v,
                 int normal,int tangent,const GfMatrix4d *reference=nullptr,
                 double translationOffset=0,double rotationOffset=0) {
        if(sourceMesh<0 || geometry[sourceMesh].IsEmpty()) return false;
        auto mesh=UsdGeomMesh(stage->GetPrimAtPath(geometry[sourceMesh]));if(!mesh)return false;
        std::set<int> visited;int skin=InputSkin(sourceMesh,0,visited);
        if(skin<0 || !skinLayouts.count(skin) || skinLayouts.at(skin).method!=0) return false;
        const auto &layout=skinLayouts.at(skin);
        VtVec3fArray raw;VtIntArray counts,vertices;VtVec2fArray uv;
        mesh.GetPointsAttr().Get(&raw);mesh.GetFaceVertexCountsAttr().Get(&counts);mesh.GetFaceVertexIndicesAttr().Get(&vertices);
        auto st=UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("st"));if(st)st.ComputeFlattened(&uv);
        if(normal<0 || normal>2 || tangent<0 || tangent>2 || normal==tangent)throw std::runtime_error("invalid mesh pin axes");
        int best[3]={-1,-1,-1};GfVec3d bary(0),tw(0);double score=std::numeric_limits<double>::infinity();
        size_t start=0;
        for(int count:counts) {
            for(int j=1;j+1<count;++j) {
                size_t corners[3]={start,start+size_t(j),start+size_t(j+1)};
                GfVec3d p[3];for(int k=0;k<3;++k) {
                    if(corners[k]>=vertices.size() || vertices[corners[k]]<0 || size_t(vertices[corners[k]])>=raw.size())
                        throw std::runtime_error("invalid mesh pin topology");
                    p[k]=GfVec3d(raw[vertices[corners[k]]]);
                }
                GfVec3d a=p[1]-p[0],b=p[2]-p[0],q;
                GfVec3d candidate(0);double distance=0;GfVec3d tangentWeights(-1,1,0);
                if(reference) {
                    q=reference->ExtractTranslation()-p[0];
                    double aa=GfDot(a,a),ab=GfDot(a,b),bb=GfDot(b,b),det=aa*bb-ab*ab;
                    if(det<1e-18)continue;
                    candidate[1]=(GfDot(q,a)*bb-GfDot(q,b)*ab)/det;
                    candidate[2]=(GfDot(q,b)*aa-GfDot(q,a)*ab)/det;candidate[0]=1-candidate[1]-candidate[2];
                    if(candidate[0]<0 || candidate[1]<0 || candidate[2]<0) {
                        double nearest=std::numeric_limits<double>::infinity();
                        for(int k=0;k<3;++k) {
                            int next=(k+1)%3;auto edge=p[next]-p[k];double length=GfDot(edge,edge);
                            double t=length>0 ? std::clamp(GfDot(reference->ExtractTranslation()-p[k],edge)/length,0.0,1.0) : 0;
                            double d=(reference->ExtractTranslation()-p[k]-edge*t).GetLengthSq();
                            if(d<nearest) {nearest=d;candidate=GfVec3d(0);candidate[k]=1-t;candidate[next]=t;}
                        }
                    }
                    GfVec3d hit(0);for(int k=0;k<3;++k)hit+=p[k]*candidate[k];
                    distance=(hit-reference->ExtractTranslation()).GetLengthSq();
                } else {
                    if(uv.size()!=vertices.size())return false;
                    GfVec2d t0(uv[corners[0]]),t1(uv[corners[1]]),t2(uv[corners[2]]);
                    auto e=t1-t0,f=t2-t0,h=GfVec2d(u,v)-t0;double det=e[0]*f[1]-e[1]*f[0];
                    if(std::abs(det)<1e-14)continue;
                    candidate[1]=(h[0]*f[1]-h[1]*f[0])/det;candidate[2]=(e[0]*h[1]-e[1]*h[0])/det;
                    candidate[0]=1-candidate[1]-candidate[2];
                    if(candidate[0]<-1e-7 || candidate[1]<-1e-7 || candidate[2]<-1e-7)continue;
                    tangentWeights=GfVec3d(e[1]-f[1],f[1],-e[1])/det;
                }
                if(distance<score) {
                    score=distance;bary=candidate;tw=tangentWeights;
                    for(int k=0;k<3;++k)best[k]=vertices[corners[k]];
                }
            }
            start+=count;
        }
        if(best[0]<0)return false;
        std::vector<GfVec3d> points;std::vector<int> ids;std::vector<float> weights;
        for(int id:best) {
            points.emplace_back(raw[id]);
            for(int j=0;j<layout.slots;++j) {
                size_t k=size_t(id)*layout.slots+j;
                ids.push_back(layout.indices.at(k));weights.push_back(layout.weights.at(k));
            }
        }
        GfMatrix4d offset(1);
        if(reference) {
            auto n=GfCross(points[1]-points[0],points[2]-points[0]);n.Normalize();
            auto t=points[1]-points[0];t.Normalize();GfVec3d axes[3];axes[normal]=n;axes[tangent]=t;
            axes[3-normal-tangent]=GfCross(n,t)*((normal+1)%3==tangent ? 1.0 : -1.0);
            GfVec3d position(0);for(int j=0;j<3;++j)position+=points[j]*bary[j];
            GfMatrix4d frame(1);for(int i=0;i<3;++i)for(int j=0;j<3;++j)frame[i][j]=axes[i][j];frame.SetTranslateOnly(position);
            auto delta=*reference*frame.GetInverse();
            offset.SetRotate(GfSlerp(rotationOffset,GfQuatd(1),delta.ExtractRotationQuat()));
            offset.SetTranslateOnly(delta.ExtractTranslation()*translationOffset);
        }
        auto pin=stage->DefinePrim(path,TfToken("RigExecMeshPin"));
        auto set=[&](const char *name,const SdfValueTypeName &type,const auto &value){pin.CreateAttribute(TfToken(name),type).Set(value);};
        SkinLayout sampled{layout.providers,ids,weights,layout.slots,layout.method};CompactSkinPalette(&sampled);
        set("inputs:points",SdfValueTypeNames->Double3Array,Vt(points));
        set("inputs:jointIndices",SdfValueTypeNames->IntArray,Vt(sampled.indices));set("inputs:jointWeights",SdfValueTypeNames->FloatArray,Vt(sampled.weights));
        set("inputs:elementSize",SdfValueTypeNames->Int,layout.slots);set("inputs:barycentric",SdfValueTypeNames->Double3,bary);
        set("inputs:tangentWeights",SdfValueTypeNames->Double3,tw);
        set("inputs:normalAxis",SdfValueTypeNames->Int,normal);set("inputs:tangentAxis",SdfValueTypeNames->Int,tangent);
        GfMatrix4d local=Translation(Pivot(target))*MayaLocal(target);
        set("inputs:local",SdfValueTypeNames->Matrix4d,local);set("inputs:offset",SdfValueTypeNames->Matrix4d,offset);
        set("inputs:parentPivot",SdfValueTypeNames->Double3,Pivot(parent[target]));
        pin.CreateRelationship(TfToken("rigExec:influences")).SetTargets(sampled.providers);
        SdfPathVector poseInputs(sampled.providers.begin(),sampled.providers.end());
        if(parent[target]>=0) {pin.CreateRelationship(TfToken("rigExec:parent")).SetTargets({dag[parent[target]]});poseInputs.push_back(dag[parent[target]]);}
        pin.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(poseInputs);
        auto output=pin.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);
        stage->GetPrimAtPath(dag[target]).GetAttribute(TfToken("posed:space")).SetConnections({output.GetPath()});
        return true;
    }
    void SurfacePins() {
        for(size_t index=0;index<scene.nodes.size();++index) if(scene.nodes[index].type=="uvPin") {
            const auto &node=scene.nodes[index];int sourceSurface=-1;
            for(auto &l:links) if(l.dst==int(index) && l.src>=0 && (l.in=="curgeom" || l.in=="deformedGeometry")) sourceSurface=l.src;
            if(Scalar(node,"novr","normalOverride",0)!=0) {Warn("uvPin normal override is unsupported: "+node.name);continue;}
            auto coordinates=Array(node,{"coord","coordinate"},2);
            for(auto &l:links) if(l.src==int(index) && l.dst>=0 && !dag[l.dst].IsEmpty() && (Starts(l.out,"omat[") || Starts(l.out,"outputMatrix[")) && (l.in=="opm" || l.in=="offsetParentMatrix")) {
                size_t slot=Range(l.out,Starts(l.out,"omat[")?"omat":"outputMatrix").first;
                if(2*slot+1>=coordinates.size())throw std::runtime_error("uvPin coordinate index out of range");
                auto path=SdfPath("/Rig/Mechanisms").AppendChild(TfToken(Name(node.name)+"_"+std::to_string(slot)));
                if(NurbsPin(sourceSurface,l.dst,path,coordinates[2*slot],coordinates[2*slot+1],
                            Scalar(node,"nrmip","normalizedIsoParms",1)!=0,
                            int(Scalar(node,"nrm","normalAxis",0)),int(Scalar(node,"tng","tangentAxis",2)),true) ||
                   MeshPin(sourceSurface,l.dst,path,coordinates[2*slot],coordinates[2*slot+1],
                           int(Scalar(node,"nrm","normalAxis",0)),int(Scalar(node,"tng","tangentAxis",2)))) {
                    l.used=true;translated.insert(int(index));
                } else Warn("uvPin requires a translated linear-skinned NURBS surface: "+node.name);
            }
        }
        for(size_t index=0;index<scene.nodes.size();++index) if(scene.nodes[index].type=="proximityPin") {
            const auto &node=scene.nodes[index];int mesh=-1;
            for(const auto &l:links) if(l.dst==int(index) && (l.in=="curgeom" || l.in=="deformedGeometry"))mesh=l.src;
            for(auto &l:links) if(l.src==int(index) && l.dst>=0 && !dag[l.dst].IsEmpty() && Starts(l.out,"omat[") && l.in=="opm") {
                size_t slot=Range(l.out,"omat").first;
                GfMatrix4d reference=Matrix(Attr(node,"imat["+std::to_string(slot)+"]"));
                for(const auto &input:links) if(input.dst==int(index) && input.src>=0 && input.in=="imat["+std::to_string(slot)+"]") {
                    reference=world[input.src];if(input.out=="m" && parent[input.src]>=0)reference*=world[parent[input.src]].GetInverse();
                }
                auto path=SdfPath("/Rig/Mechanisms").AppendChild(TfToken(Name(node.name)+"_"+std::to_string(slot)));
                if(MeshPin(mesh,l.dst,path,0,0,int(Scalar(node,"nrm","normalAxis",0)),int(Scalar(node,"tng","tangentAxis",2)),
                           &reference,Scalar(node,"ostr","offsetTranslation",0),Scalar(node,"osor","offsetOrientation",0))) {
                    l.used=true;translated.insert(int(index));
                } else Warn("proximityPin requires a translated linear-skinned mesh: "+node.name);
            }
        }
        for(size_t index=0;index<scene.nodes.size();++index) if(scene.nodes[index].type=="follicle") {
            const auto &node=scene.nodes[index];int surface=-1,target=-1;
            for(const auto &l:links) {
                if(l.dst==int(index) && (l.in=="is" || l.in=="inputSurface")) surface=l.src;
                if(l.src==int(index) && l.dst>=0 && !dag[l.dst].IsEmpty() &&
                   (l.out=="ot" || l.out=="outTranslate") && (l.in=="t" || l.in=="translate")) target=l.dst;
            }
            if(target<0) continue;
            auto path=SdfPath("/Rig/Mechanisms").AppendChild(TfToken(Name(node.name)));
            if(NurbsPin(surface,target,path,Scalar(node,"pu","parameterU",0),Scalar(node,"pv","parameterV",0),true,1,0,false)) {
                translated.insert(int(index));
                for(auto &l:links) if(l.src==int(index) || l.dst==int(index)) l.used=true;
            } else Warn("follicle requires a translated linear-skinned NURBS surface: "+node.name);
        }
    }
    void Mesh(int index) {
        const auto &node=scene.nodes[index];
        if (Scalar(node,"io","intermediateObject",0)!=0) {
            std::set<int> visited;
            if(InputSkin(index,0,visited)<0) { translated.insert(index); return; }
        }
        const int dataIndex=MeshData(index);
        const auto &data=scene.nodes[dataIndex];meshInputs[index]=dataIndex;
        auto vertices=Array(data,{"vt","vrts"},3),edges=Array(data,{"ed","edge"},3);
        const bool triangleDisc=data.type=="polyDisc";
        if(triangleDisc) {
            const double radius=Scalar(data,"radius",{},1),x=std::sqrt(3.0)*radius/2;
            if(!(radius>0))throw std::runtime_error("polyDisc radius must be positive");
            vertices={-x,0,radius/2, x,0,radius/2, 0,0,-radius};
            edges={0,1,0, 1,2,0, 2,0,0};
        }
        if (vertices.empty()) { Warn("mesh has no supported stored/input vertices: " + full[index]); return; }
        // Stored mesh tweaks are sparse, additive offsets to the vertex
        // array. They are part of the input shape, before its skin history.
        std::map<size_t,GfVec3d> tweaks;
        for(const auto &a:data.attributes) if(!a.values.empty() && (Starts(a.name,"pt[") || Starts(a.name,"pnts["))) {
            auto range=Range(a.name,Starts(a.name,"pt[") ? "pt" : "pnts");
            if(range.second>=vertices.size()/3 || a.values.size()!=3*(range.second-range.first+1))
                throw std::runtime_error("invalid stored mesh tweak range: " + data.name);
            for(size_t v=range.first;v<=range.second;++v) {
                size_t k=3*(v-range.first);
                tweaks[v]=GfVec3d(Number(a.values[k]),Number(a.values[k+1]),Number(a.values[k+2]));
            }
        }
        for(const auto &entry:tweaks) for(int axis=0;axis<3;++axis)
            vertices[3*entry.first+axis]+=entry.second[axis];
        GfMatrix4d geometryMatrix=world[index];
        std::set<int> inputVisited; int inputSkin=InputSkin(index,0,inputVisited);
        if(inputSkin>=0) geometryMatrix=Matrix(Attr(scene.nodes[inputSkin],"gm","geomMatrix"),geometryMatrix);
        geometryMatrices[index]=geometryMatrix;
        std::vector<GfVec3f> points;
        for (size_t i=0;i<vertices.size();i+=3) points.emplace_back(geometryMatrix.Transform(GfVec3d(vertices[i],vertices[i+1],vertices[i+2])));
        std::vector<Edge> es;
        for (size_t i=0;i<edges.size();i+=3) {
            if (edges[i]!=std::floor(edges[i]) || edges[i+1]!=std::floor(edges[i+1]) || edges[i]<0 || edges[i+1]<0 ||
                edges[i]>=points.size() || edges[i+1]>=points.size()) throw std::runtime_error("invalid mesh edge: " + data.name);
            es.push_back({static_cast<int>(edges[i]),static_cast<int>(edges[i+1])});
        }
        std::map<size_t,Face> faces;
        if(triangleDisc)faces[0]=Face{{0,1,2},{0,1,2}};
        // Maya repeats the attribute range but omits -type on continuation
        // chunks (commonly every 500 faces). These still contain polyFaces.
        for (const auto &a:data.attributes) if (!a.values.empty() &&
            (a.type=="polyFaces" || (a.type.empty() && (Starts(a.name,"fc[") || Starts(a.name,"face["))))) {
            std::string prefix=Starts(a.name,"fc[") ? "fc" : "face";
            auto range=Range(a.name,prefix); size_t faceIndex=range.first; Face *face=nullptr;
            size_t k=0;
            auto take=[&]() -> const std::string & { if (k>=a.values.size()) throw std::runtime_error("truncated polyFaces"); return a.values[k++]; };
            while (k<a.values.size()) {
                std::string kind=take();
                if (kind=="f") {
                    if (faceIndex>range.second) throw std::runtime_error("too many polyFaces");
                    face=&faces[faceIndex++]; *face=Face(); size_t n=Count(take());
                    if (n<3) throw std::runtime_error("polygon needs at least three edges");
                    for (size_t j=0;j<n;++j) face->edges.push_back(Integer(take()));
                } else if (kind=="mu" || kind=="mc") {
                    int set=Integer(take()); size_t n=Count(take());
                    if (!face) throw std::runtime_error("polyFaces data before face");
                    for (size_t j=0;j<n;++j) { int value=Integer(take()); if (kind=="mu" && set==0) face->uv.push_back(value); }
                    if (kind=="mc" || set!=0) Warn("additional UV/color set preserved only: " + data.name);
                } else if (kind=="h" || kind=="mf" || kind=="mh" || kind=="fc") {
                    size_t n=Count(take()); for (size_t j=0;j<n;++j) take();
                    throw std::runtime_error("unsupported polygon hole/legacy UV/color encoding: " + data.name);
                } else throw std::runtime_error("unknown polyFaces token: " + kind);
            }
            if (faceIndex!=range.second+1) throw std::runtime_error("polyFaces range mismatch");
        }
        std::vector<int> counts,indices,uvIndices;
        for (const auto &entry:faces) {
            if (entry.first!=counts.size()) throw std::runtime_error("missing face index");
            const auto &f=entry.second; counts.push_back(static_cast<int>(f.edges.size()));
            int first=-1,previous=-1;
            for (int e:f.edges) {
                long long edge=e<0 ? -static_cast<long long>(e)-1 : e;
                if (edge>=static_cast<long long>(es.size())) throw std::runtime_error("face edge out of range");
                auto pair=es[static_cast<size_t>(edge)]; if (e<0) std::swap(pair.a,pair.b);
                if (first<0) first=pair.a;
                else if (previous!=pair.a) throw std::runtime_error("disconnected polygon edge loop");
                previous=pair.b; indices.push_back(pair.a);
            }
            if (previous!=first) throw std::runtime_error("unclosed polygon edge loop");
            uvIndices.insert(uvIndices.end(),f.uv.begin(),f.uv.end());
        }
        if (counts.empty()) { Warn("mesh has no stored polygon faces: " + data.name); return; }
        auto path=SdfPath("/Rig/Geometry").AppendChild(TfToken(Name(full[index])));
        auto mesh=UsdGeomMesh::Define(stage,path); geometry[index]=path;
        mesh.CreatePointsAttr().Set(Vt(points)); mesh.CreateFaceVertexCountsAttr().Set(Vt(counts)); mesh.CreateFaceVertexIndicesAttr().Set(Vt(indices));
        mesh.CreateSubdivisionSchemeAttr().Set(TfToken("none"));
        mesh.GetPrim().SetCustomDataByKey(TfToken("maya:name"),VtValue(full[index]));
        if (!Visible(index))
            mesh.CreateVisibilityAttr().Set(TfToken("invisible"));
        auto uvs=Array(data,{"uvst[0].uvsp","uvSet[0].uvSetPoints"},2);
        if(triangleDisc)uvs={0,0,1,0,0.5,std::sqrt(3.0)/2};
        if (!uvs.empty()) {
            if (uvIndices.size()!=indices.size()) throw std::runtime_error("incomplete face-varying UV indices: " + data.name);
            std::vector<GfVec2f> st;
            for (size_t i=0;i<uvs.size();i+=2) st.emplace_back(static_cast<float>(uvs[i]),static_cast<float>(uvs[i+1]));
            for (int uv:uvIndices) if (uv<0 || static_cast<size_t>(uv)>=st.size()) throw std::runtime_error("UV index out of range");
            auto pv=UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("st"),SdfValueTypeNames->TexCoord2fArray,TfToken("faceVarying"));
            pv.Set(Vt(st)); pv.SetIndices(Vt(uvIndices));
        }
        translated.insert(index);
        if (&data!=&node) translated.insert(static_cast<int>(&data-scene.nodes.data()));
    }
    // A curve consumed as rig data (spline IK, motion paths, loft rails,
    // curve measurement) or produced by skinning/surface sampling is a rig
    // curve, not a controller gizmo. Deformed gizmos (ffd/cluster outputs
    // feeding only a display shape) stay controller shapes by exclusion.
    bool RigCurve(int index) const {
        for (const auto &l:links) if (l.src==index && l.dst>=0) {
            const auto &dst=scene.nodes[l.dst];
            if (dst.type=="ikHandle" && l.in=="ic") return true;
            if (dst.type=="motionPath" && l.in=="gp") return true;
            if (dst.type=="pointOnCurveInfo" && l.in=="ic") return true;
            if (dst.type=="nearestPointOnCurve" && l.in=="ic") return true;
            if (dst.type=="loft" && (Starts(l.in,"ic[") || Starts(l.in,"inputCurve["))) return true;
            // Skins deform rig geometry, never display gizmos; this catches
            // the bind-state Orig of a skinned functional curve.
            if (dst.type=="skinCluster" && InputElement(l.in)>=0) return true;
        }
        for (const auto &l:links) if (l.dst==index && l.src>=0 && (l.in=="cr" || l.in=="create")) {
            const auto &src=scene.nodes[l.src];
            if (src.type=="skinCluster" || src.type=="curveFromSurfaceIso") return true;
        }
        return false;
    }
    void Curve(int index) {
        const auto &n=scene.nodes[index];std::set<int> visited;int data=InputMesh(index,0,visited);auto a=Attr(data<0?n:scene.nodes[data],"cc","cached");
        if (!a || a->type!="nurbsCurve") { Warn("curve has no cached nurbsCurve data: " + n.name); return; }
        size_t k=0; auto take=[&]() -> const std::string & { if(k>=a->values.size()) throw std::runtime_error("truncated nurbsCurve"); return a->values[k++]; };
        int degree=Integer(take()); Count(take()); int form=Integer(take()); bool rational=Number(take())!=0; int dimension=Integer(take());
        if (degree<1 || degree>16 || dimension!=3) throw std::runtime_error("unsupported NURBS degree/dimension");
        size_t nk=Count(take()); std::vector<double> knots;
        for(size_t j=0;j<nk;++j) knots.push_back(Number(take()));
        size_t np=Count(take()); std::vector<GfVec3f> points; std::vector<double> weights;
        for(size_t j=0;j<np;++j) {
            double x=Number(take()),y=Number(take()),z=Number(take());
            points.emplace_back(x,y,z);
            if(rational) weights.push_back(Number(take()));
        }
        if(k!=a->values.size() || nk!=np+degree-1 || nk<2 || !std::is_sorted(knots.begin(),knots.end())) throw std::runtime_error("invalid NURBS knot/CV count");
        double first=knots.front(),last=knots.back();
        knots.insert(knots.begin(),form==2 ? first-(knots[1]-first) : first);
        knots.push_back(form==2 ? last+(last-knots[knots.size()-2]) : last);
        const bool dataCurve=RigCurve(index);
        if (!dataCurve && parent[index]>=0 && scene.nodes[parent[index]].type=="transform") {
            auto control=stage->GetPrimAtPath(dag[parent[index]]);
            auto guide=CurveGuide(points,knots,weights,degree);
            for(auto &p:guide)p-=GfVec3f(Pivot(parent[index]));
            if(guide.size()<2) throw std::runtime_error("empty controller shape");
            VtVec3fArray vertices; VtIntArray counts;
            control.GetAttribute(TfToken("guide:points")).Get(&vertices);
            control.GetAttribute(TfToken("guide:curveVertexCounts")).Get(&counts);
            if(Visible(index)) {
                vertices.insert(vertices.end(),guide.begin(),guide.end());
                counts.push_back(static_cast<int>(guide.size()));
            }
            control.GetAttribute(TfToken("guide:shape")).Set(TfToken("custom"));
            control.GetAttribute(TfToken("guide:points")).Set(vertices);
            control.GetAttribute(TfToken("guide:curveVertexCounts")).Set(counts);
            float opacity=0.0f;
            control.GetAttribute(TfToken("guide:displayOpacity")).Get(&opacity);
            control.GetAttribute(TfToken("guide:displayOpacity")).Set(Visible(index) ? 1.0f : opacity);
            VtDictionary shapes;
            auto existing=control.GetCustomDataByKey(TfToken("maya:controlShapes"));
            if(existing.IsHolding<VtDictionary>()) shapes=existing.UncheckedGet<VtDictionary>();
            VtDictionary shape;
            shape["name"]=VtValue(full[index]); shape["points"]=VtValue(Vt(points));
            shape["knots"]=VtValue(Vt(knots)); shape["weights"]=VtValue(Vt(weights));
            shape["degree"]=VtValue(degree); shape["form"]=VtValue(form);
            shapes[Name(n.name)]=VtValue(shape);
            control.SetCustomDataByKey(TfToken("maya:controlShapes"),VtValue(shapes));
            translated.insert(index);
            return;
        }
        for(auto &p:points) p=GfVec3f(world[index].Transform(GfVec3d(p)));
        auto path=SdfPath("/Rig/Geometry").AppendChild(TfToken(Name(full[index])));
        bool rig=dataCurve;
        std::set<int> skinVisited;
        if(degree>1 && InputSkin(index,0,skinVisited)>=0)rig=false;
        if (rig && rational) { Warn("rational rig curve kept as NURBS: " + full[index]); rig=false; }
        if (rig && form!=0) { Warn("periodic rig curve kept as NURBS: " + full[index]); rig=false; }
        if (rig && degree>3) { Warn("high-degree rig curve kept as NURBS: " + full[index]); rig=false; }
        UsdPrim prim;
        if (rig && degree==1) {
            // A linear spline is a polyline under any parameterization,
            // so the knot-free b-spline form is exact.
            auto basis=UsdGeomBasisCurves::Define(stage,path); prim=basis.GetPrim();
            basis.CreateTypeAttr().Set(TfToken("linear"));
            basis.CreateWrapAttr().Set(TfToken("nonperiodic"));
            basis.CreatePointsAttr().Set(Vt(points));
            basis.CreateCurveVertexCountsAttr().Set(VtIntArray{static_cast<int>(np)});
            basis.CreateWidthsAttr().Set(VtFloatArray{0.05f}); basis.SetWidthsInterpolation(TfToken("constant"));
        } else if (rig) {
            auto beziers=BezierDecompose(points,knots,degree);
            auto basis=UsdGeomBasisCurves::Define(stage,path); prim=basis.GetPrim();
            basis.CreateTypeAttr().Set(TfToken("cubic"));
            basis.CreateBasisAttr().Set(TfToken("bezier"));
            basis.CreateWrapAttr().Set(TfToken("nonperiodic"));
            basis.CreatePointsAttr().Set(Vt(beziers));
            basis.CreateCurveVertexCountsAttr().Set(VtIntArray{static_cast<int>(beziers.size())});
            basis.CreateWidthsAttr().Set(VtFloatArray{0.05f}); basis.SetWidthsInterpolation(TfToken("constant"));
        } else {
            auto curve=UsdGeomNurbsCurves::Define(stage,path); prim=curve.GetPrim();
            curve.CreatePointsAttr().Set(Vt(points)); curve.CreateCurveVertexCountsAttr().Set(VtIntArray{static_cast<int>(np)});
            curve.CreateOrderAttr().Set(VtIntArray{degree+1}); curve.CreateKnotsAttr().Set(Vt(knots));
            curve.CreateRangesAttr().Set(VtVec2dArray{GfVec2d(knots[degree],knots[np])});
            if(rational) curve.CreatePointWeightsAttr().Set(Vt(weights));
            curve.CreateWidthsAttr().Set(VtFloatArray{0.05f}); curve.SetWidthsInterpolation(TfToken("constant"));
        }
        geometry[index]=path;
        prim.SetDisplayName(n.name);
        prim.SetCustomDataByKey(TfToken("maya:name"),VtValue(full[index]));
        // Curves never render in Maya; both controller shapes and rig
        // curves are viewport guides. The type carries the role.
        UsdGeomImageable(prim).CreatePurposeAttr().Set(TfToken("guide"));
        if(!Visible(index)) UsdGeomImageable(prim).CreateVisibilityAttr().Set(TfToken("invisible"));
        translated.insert(index);
    }
    void Materials() {
        for(size_t i=0;i<scene.nodes.size();++i) {
            const auto &n=scene.nodes[i];
            if(n.type!="lambert" && n.type!="blinn" && n.type!="phong" && n.type!="standardSurface" && n.type!="aiStandardSurface") continue;
            auto mat=UsdShadeMaterial::Define(stage,SdfPath("/Rig/Materials").AppendChild(TfToken(Name(n.name))));
            auto shader=UsdShadeShader::Define(stage,mat.GetPath().AppendChild(TfToken("Surface")));
            shader.CreateIdAttr().Set(TfToken("UsdPreviewSurface"));
            bool standard=n.type=="standardSurface" || n.type=="aiStandardSurface";
            auto c=standard ? Vector(n,"base_color","baseColor",GfVec3d(0.8)) : Vector(n,"c","color",GfVec3d(0.5));
            shader.CreateInput(TfToken("diffuseColor"),SdfValueTypeNames->Color3f).Set(GfVec3f(c));
            float opacity=standard ? static_cast<float>(Vector(n,"opacity","opacity",GfVec3d(1))[0]) : 1-static_cast<float>(Vector(n,"it","transparency")[0]);
            shader.CreateInput(TfToken("opacity"),SdfValueTypeNames->Float).Set(opacity);
            float roughness=standard ? static_cast<float>(Scalar(n,"specular_roughness","specularRoughness",0.2)) : n.type=="blinn" ? static_cast<float>(Scalar(n,"ec","eccentricity",0.3)) : n.type=="phong" ? static_cast<float>(std::sqrt(2/(Scalar(n,"cp","cosinePower",20)+2))) : 1;
            shader.CreateInput(TfToken("roughness"),SdfValueTypeNames->Float).Set(std::clamp(roughness,0.f,1.f));
            shader.CreateInput(TfToken("metallic"),SdfValueTypeNames->Float).Set(static_cast<float>(Scalar(n,"metalness","metalness",0)));
            shader.CreateOutput(TfToken("surface"),SdfValueTypeNames->Token);
            mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),TfToken("surface"));
            materials[static_cast<int>(i)]=mat; translated.insert(static_cast<int>(i));
        }
        // Texture maps share one material under stable role names. The same
        // file feeding two inputs reuses one UsdUVTexture; a second file
        // for one role falls back to a file-suffixed name.
        std::map<std::pair<int,int>,SdfPath> textures;
        auto textureFor=[&](int mat,int file,const std::string &role)->UsdShadeShader {
            auto key=std::make_pair(mat,file);
            auto found=textures.find(key);
            if(found!=textures.end()) return UsdShadeShader(stage->GetPrimAtPath(found->second));
            auto path=materials[mat].GetPath().AppendChild(TfToken(role));
            if(stage->GetPrimAtPath(path)) path=materials[mat].GetPath().AppendChild(TfToken(role+"_"+Name(scene.nodes[file].name)));
            auto texture=UsdShadeShader::Define(stage,path);
            texture.CreateIdAttr().Set(TfToken("UsdUVTexture"));
            auto asset=Attr(scene.nodes[file],"ftn","fileTextureName");
            texture.CreateInput(TfToken("file"),SdfValueTypeNames->Asset).Set(SdfAssetPath(Asset(asset->values[0])));
            std::string colorSpace="auto";
            if(auto cs=Attr(scene.nodes[file],"cs","colorSpace")) {
                if(cs->values[0]=="sRGB") colorSpace="sRGB";
                else if(cs->values[0]=="Raw") colorSpace="raw";
                else Warn("texture color space approximated as auto: " + cs->values[0]);
            }
            texture.CreateInput(TfToken("sourceColorSpace"),SdfValueTypeNames->Token).Set(TfToken(colorSpace));
            auto reader=UsdShadeShader::Define(stage,materials[mat].GetPath().AppendChild(TfToken("UV")));
            reader.CreateIdAttr().Set(TfToken("UsdPrimvarReader_float2"));
            reader.CreateInput(TfToken("varname"),SdfValueTypeNames->Token).Set(TfToken("st"));
            reader.CreateOutput(TfToken("result"),SdfValueTypeNames->Float2);
            texture.CreateInput(TfToken("st"),SdfValueTypeNames->Float2).ConnectToSource(reader.ConnectableAPI(),TfToken("result"));
            textures[key]=path;
            return texture;
        };
        for(auto &l:links) {
            if(l.src<0 || !materials.count(l.dst)) continue;
            auto surface=UsdShadeShader(stage->GetPrimAtPath(materials[l.dst].GetPath().AppendChild(TfToken("Surface"))));
            const auto &from=scene.nodes[l.src];
            if(from.type=="file" && (l.out=="oc" || l.out=="outColor") &&
                (l.in=="c" || l.in=="color" || l.in=="baseColor" || l.in=="base_color")) {
                auto file=Attr(from,"ftn","fileTextureName");
                if(!file || file->values.empty()) continue;
                auto texture=textureFor(l.dst,l.src,"Texture");
                texture.CreateOutput(TfToken("rgb"),SdfValueTypeNames->Float3);
                surface.GetInput(TfToken("diffuseColor")).ConnectToSource(texture.ConnectableAPI(),TfToken("rgb"));
                translated.insert(l.src); l.used=true;
            } else if(from.type=="file" && (l.out=="ocr" || l.out=="outColorR") &&
                (l.in=="specular_roughness" || l.in=="specularRoughness")) {
                auto file=Attr(from,"ftn","fileTextureName");
                if(!file || file->values.empty()) continue;
                auto texture=textureFor(l.dst,l.src,"Roughness");
                texture.CreateOutput(TfToken("r"),SdfValueTypeNames->Float);
                surface.GetInput(TfToken("roughness")).ConnectToSource(texture.ConnectableAPI(),TfToken("r"));
                translated.insert(l.src); l.used=true;
            } else if(from.type=="file" && (l.out=="ocr" || l.out=="outColorR") &&
                l.in=="metalness") {
                auto file=Attr(from,"ftn","fileTextureName");
                if(!file || file->values.empty()) continue;
                auto texture=textureFor(l.dst,l.src,"Metallic");
                texture.CreateOutput(TfToken("r"),SdfValueTypeNames->Float);
                surface.GetInput(TfToken("metallic")).ConnectToSource(texture.ConnectableAPI(),TfToken("r"));
                translated.insert(l.src); l.used=true;
            } else if(from.type=="file" && (l.out=="oc" || l.out=="outColor") &&
                (l.in=="n" || l.in=="normal")) {
                auto file=Attr(from,"ftn","fileTextureName");
                if(!file || file->values.empty()) continue;
                auto texture=textureFor(l.dst,l.src,"Normal");
                texture.CreateOutput(TfToken("rgb"),SdfValueTypeNames->Float3);
                surface.CreateInput(TfToken("normal"),SdfValueTypeNames->Normal3f).ConnectToSource(texture.ConnectableAPI(),TfToken("rgb"));
                translated.insert(l.src); l.used=true;
            } else if(from.type=="bump2d" && (l.out=="o" || l.out=="outNormal") &&
                (l.in=="n" || l.in=="normal")) {
                // A normal map routed through bump2d keeps its texture and
                // dependency; the bump evaluation itself is not executed.
                int map=-1; size_t linkIndex=links.size();
                for(size_t j=0;j<links.size();++j) {
                    const auto &input=links[j];
                    if(input.dst==l.src && input.src>=0 && scene.nodes[input.src].type=="file" &&
                        (input.in=="bv" || input.in=="bumpValue") &&
                        (input.out=="oc" || input.out=="outColor" || input.out=="ocr" || input.out=="outColorR")) {
                        auto file=Attr(scene.nodes[input.src],"ftn","fileTextureName");
                        if(file && !file->values.empty()) { map=input.src; linkIndex=j; }
                    }
                }
                if(map<0) continue;
                auto texture=textureFor(l.dst,map,"Normal");
                texture.CreateOutput(TfToken("rgb"),SdfValueTypeNames->Float3);
                surface.CreateInput(TfToken("normal"),SdfValueTypeNames->Normal3f).ConnectToSource(texture.ConnectableAPI(),TfToken("rgb"));
                Warn("bump2d normal perturbation approximated as normal-map passthrough: " + from.name);
                translated.insert(l.src); translated.insert(map); l.used=true; links[linkIndex].used=true;
            }
        }
        for(size_t sg=0;sg<scene.nodes.size();++sg) if(scene.nodes[sg].type=="shadingEngine") {
            UsdShadeMaterial mat;
            for(auto &l:links) if(l.dst==sg && (l.in=="ss" || l.in=="surfaceShader") && materials.count(l.src)) { mat=materials[l.src]; l.used=true; }
            if(!mat) continue;
            translated.insert(static_cast<int>(sg));
            for(auto &l:links) if(l.dst==sg && l.src>=0 && (Starts(l.in,"dsm") || Starts(l.in,"dagSetMembers")) && !geometry[l.src].IsEmpty()) {
                if(l.out=="iog" || l.out=="iog[0]" || l.out=="instObjGroups[0]") {
                    UsdShadeMaterialBindingAPI::Apply(stage->GetPrimAtPath(geometry[l.src])).Bind(mat); l.used=true;
                } else {
                    auto components=Attr(scene.nodes[l.src],l.out+".gcl",l.out+".objectGrpCompList");
                    if(!components) {
                        // Face membership may live upstream on groupParts,
                        // with a shared groupId connecting it to the shape.
                        std::set<int> history;
                        for(const auto &input:links) if(input.dst==l.src && input.src>=0 && (input.in=="i" || input.in=="inMesh")) {
                            long element=OutputElement(input.out);
                            InputMesh(input.src,element<0 ? 0 : element,history);
                        }
                        for(auto &id:links) if(id.dst==l.src && id.in==l.out+".gid" && id.src>=0) {
                            for(auto &part:links) if(part.src==id.src && part.dst>=0 && history.count(part.dst) &&
                                scene.nodes[part.dst].type=="groupParts" && (part.in=="gi" || part.in=="groupId")) {
                                components=Attr(scene.nodes[part.dst],"ic","inputComponents");
                                if(components) {id.used=true;part.used=true;break;}
                            }
                        }
                    }
                    if(!components || components->type!="componentList" || components->values.empty()) {
                        Warn("material membership has no supported component list: " + scene.nodes[l.src].name+"."+l.out); continue;
                    }
                    size_t count=Count(components->values[0]);
                    if(count+1!=components->values.size()) throw std::runtime_error("componentList size mismatch");
                    VtIntArray faces,topology; UsdGeomMesh(stage->GetPrimAtPath(geometry[l.src])).GetFaceVertexCountsAttr().Get(&topology);
                    for(size_t j=1;j<components->values.size();++j) {
                        const auto &component=components->values[j];
                        if(!Starts(component,"f[")) throw std::runtime_error("material component must be a face");
                        auto range=component=="f[*]" ? std::make_pair(size_t(0),topology.empty() ? size_t(0) : topology.size()-1) : Range(component,"f");
                        if(range.second>=topology.size()) throw std::runtime_error("material face index out of range");
                        for(size_t f=range.first;f<=range.second;++f) faces.push_back(static_cast<int>(f));
                    }
                    std::sort(faces.begin(),faces.end()); faces.erase(std::unique(faces.begin(),faces.end()),faces.end());
                    auto binding=UsdShadeMaterialBindingAPI::Apply(stage->GetPrimAtPath(geometry[l.src]));
                    auto subset=binding.CreateMaterialBindSubset(TfToken(Name(scene.nodes[sg].name)),faces);
                    UsdShadeMaterialBindingAPI::Apply(subset.GetPrim()).Bind(mat); l.used=true;
                }
            }
        }
    }
    void Ik() {
        for(size_t i=0;i<scene.nodes.size();++i) if(scene.nodes[i].type=="ikHandle") {
            const auto &node=scene.nodes[i];int first=-1,effector=-1,end=-1,solver=-1;
            for(auto &l:links) if(l.dst==int(i)) {
                if(l.in=="hsj" || l.in=="startJoint") {first=l.src;l.used=true;}
                if(l.in=="hee" || l.in=="endEffector") {effector=l.src;l.used=true;}
                if(l.in=="hsv" || l.in=="ikSolver") {solver=l.src;l.used=true;}
            }
            for(auto &l:links) if(l.dst==effector && l.src>=0 && scene.nodes[l.src].type=="joint" &&
                (l.in=="tx" || l.in=="ty" || l.in=="tz" || l.in=="t")) {end=l.src;l.used=true;}
            // An effector can terminate a bone without a separate child
            // joint. Preserve that endpoint as a native virtual joint.
            if(end<0 && effector>=0 && parent[effector]>=0 && scene.nodes[parent[effector]].type=="joint") {
                end=effector;dag[end]=dag[parent[end]].AppendChild(TfToken(Name(scene.nodes[end].name)));
                auto endpoint=RigExecSchemaPrim::Define(stage,dag[end],TfToken("RigExecJoint"));
                endpoint.SetAttribute(TfToken("rest:space"),VtValue(Translation(Vector(scene.nodes[end],"t","translate"))));
                endpoint.GetPrim().SetCustomDataByKey(TfToken("maya:name"),VtValue(full[end]));
            }
            if(first<0 || end<0 || solver<0 || (scene.nodes[solver].type!="ikRPsolver" && scene.nodes[solver].type!="ikSCsolver")) {
                Warn("unsupported or incomplete IK chain: "+node.name);continue;
            }
            std::vector<SdfPath> poles;std::vector<float> weights;
            for(auto &output:links) if(output.dst==int(i) && output.src>=0 && scene.nodes[output.src].type=="poleVectorConstraint") {
                int constraint=output.src;std::map<int,int> sources;
                for(auto &l:links) if(l.dst==constraint && l.src>=0 && !dag[l.src].IsEmpty() && Starts(l.in,"tg[")) {
                    sources[int(Range(l.in,"tg").first)]=l.src;l.used=true;
                }
                if(poles.empty())for(auto entry:sources) {
                    poles.push_back(dag[entry.second]);weights.push_back(float(Scalar(scene.nodes[constraint],"tg["+std::to_string(entry.first)+"].tw","target["+std::to_string(entry.first)+"].targetWeight",1)));
                }
                output.used=true;translated.insert(constraint);
            }
            auto ik=rig.NewMoverChain("ik_"+std::to_string(i)).AddSingleChainIkConstraint("solve",dag[first],dag[end],dag[i],poles);
            ik.SetSolverMode(TfToken(scene.nodes[solver].type=="ikSCsolver" ? "singleChain" : "rotatePlane"));
            // Imported joints have local rest transforms; measure lengths
            // from their composed incoming frames, including upstream rigging.
            ik.SetEvaluationMode(TfToken("alwaysTS"));
            ik.SetOrientationMode(TfToken("preserve"));
            ik.SetPoleVectorMode(TfToken(poles.empty() ? "vector" : "object"));
            if(!poles.empty())ik.SetPoleVectorWeights(weights);
            else {auto v=Vector(node,"pv","poleVector",GfVec3d(0,1,0));ik.SetPoleVector(v[0],v[1],v[2]);}
            ik.SetTwistDegrees(Scalar(node,"tw","twist",0)*(scene.angularUnit=="rad" ? 180.0/3.14159265358979323846 : 1));
            translated.insert(int(i));translated.insert(effector);translated.insert(solver);
        }
    }
    SdfPath MayaConstraintSource(int index) {
        const auto scale=Vector(scene.nodes[index],"s","scale",GfVec3d(1));
        if(world[index].GetDeterminant()>=0 && scale[0]>=0 && scale[1]>=0 && scale[2]>=0)return dag[index];
        if(constraintRotationSources.count(index))return constraintRotationSources.at(index);
        const auto path=SdfPath("/Rig/BindFrames").AppendChild(TfToken("constraintRotation_"+std::to_string(index)));
        auto frame=RigExecSchemaPrim::Define(stage,path,TfToken("RigExecControl"));frame.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));frame.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
        auto native=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(path.GetNameToken()),TfToken("RigExecMayaMatrixSpace"));
        native.CreateAttribute(TfToken("inputs:mode"),SdfValueTypeNames->Int).Set(4);
        native.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(GfVec3d(0));native.CreateAttribute(TfToken("inputs:sourceParentPivot"),SdfValueTypeNames->Double3).Set(GfVec3d(0));
        native.CreateAttribute(TfToken("inputs:sourceOffsetInverse"),SdfValueTypeNames->Matrix4d).Set(GfMatrix4d(1));
        native.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[index]});native.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets({dag[index]});
        for(int axis=0;axis<3;++axis) {
            const auto name=std::string("s")+"xyz"[axis];auto attr=native.CreateAttribute(TfToken("inputs:"+name),SdfValueTypeNames->Double);attr.Set(Vector(scene.nodes[index],"s","scale",GfVec3d(1))[axis]);scalarReads.push_back({index,name,attr.GetPath()});
        }
        if(parent[index]>=0) {native.CreateRelationship(TfToken("rigExec:sourceParent")).SetTargets({dag[parent[index]]});native.GetRelationship(TfToken("rigExec:poseInputs")).SetTargets({dag[index],dag[parent[index]]});native.GetAttribute(TfToken("inputs:sourceParentPivot")).Set(Pivot(parent[index]));}
        auto matrix=native.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);frame.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({matrix.GetPath()});
        constraintRotationSources[index]=path;return path;
    }
    void Constraints() {
        for(size_t i=0;i<scene.nodes.size();++i) {
            const auto &n=scene.nodes[i];
            if(n.type!="parentConstraint" && n.type!="pointConstraint" && n.type!="orientConstraint" && n.type!="scaleConstraint" && n.type!="aimConstraint") continue;
            int target=-1;SdfPath proxy; bool t[3]={false,false,false},r[3]={false,false,false},s[3]={false,false,false};
            std::vector<size_t> consumed;
            for(size_t j=0;j<links.size();++j) {
                auto &l=links[j]; if(l.src!=i || l.dst<0 || dag[l.dst].IsEmpty()) continue;
                bool output=false;
                for(int group=0;group<3;++group) {
                    const std::string shortName=group==0 ? "ct" : group==1 ? "cr" : "cs";
                    const std::string longName=group==0 ? "constraintTranslate" : group==1 ? "constraintRotate" : "constraintScale";
                    const std::string dstShort=group==0 ? "t" : group==1 ? "r" : "s";
                    const std::string dstLong=group==0 ? "translate" : group==1 ? "rotate" : "scale";
                    bool *mask=group==0 ? t : group==1 ? r : s;
                    if((l.out==shortName || l.out==longName) && (l.in==dstShort || l.in==dstLong)) { mask[0]=mask[1]=mask[2]=true; output=true; }
                    for(int axis=0;axis<3;++axis) if((l.out==shortName+"xyz"[axis] || l.out==longName+"XYZ"[axis]) &&
                        (l.in==dstShort+"xyz"[axis] || l.in==dstLong+"XYZ"[axis])) { mask[axis]=true; output=true; }
                }
                if(output) {
                    if(target>=0 && target!=l.dst) throw std::runtime_error("constraint outputs drive multiple DAG nodes");
                    target=l.dst; consumed.push_back(j);
                }
            }
            if(target<0)for(auto &l:links)if(l.src==int(i) && l.dst>=0 && scene.nodes[l.dst].type=="pairBlend") {
                for(const auto &out:links)if(out.src==l.dst && out.dst>=0 && !dag[out.dst].IsEmpty()) {target=out.dst;break;}
                if(target<0)continue;
                proxy=SdfPath("/Rig/Mechanisms").AppendChild(TfToken("constraintOutput_"+std::to_string(i)));
                auto p=RigExecSchemaPrim::Define(stage,proxy,TfToken("RigExecControl"));p.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
                p.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));
                if(n.type=="pointConstraint")t[0]=t[1]=t[2]=true;
                else if(n.type=="parentConstraint") {t[0]=t[1]=t[2]=r[0]=r[1]=r[2]=true;}
                else r[0]=r[1]=r[2]=true;
                break;
            }
            std::map<int,int> sources;
            for(size_t j=0;j<links.size();++j) {
                auto &l=links[j]; if(l.dst!=i || l.src<0 || dag[l.src].IsEmpty()) continue;
                std::string prefix=Starts(l.in,"tg[") ? "tg" : Starts(l.in,"target[") ? "target" : "";
                if(!prefix.empty()) {
                    int slot=static_cast<int>(Range(l.in,prefix).first);
                    if(sources.count(slot) && sources[slot]!=l.src) { Warn("constraint target assembled from multiple nodes: " + n.name); continue; }
                    sources[slot]=l.src; consumed.push_back(j);
                } else if(l.src==target) consumed.push_back(j);
            }
            if(target<0 || sources.empty()) { Warn("constraint lacks connected sources/output: " + n.name); continue; }
            std::vector<SdfPath> paths; std::vector<float> weights;
            std::vector<GfVec3d> offsetsT,offsetsR;
            for(const auto &entry:sources) {
                paths.push_back(n.type=="parentConstraint" || n.type=="orientConstraint"?MayaConstraintSource(entry.second):dag[entry.second]); std::string a="tg["+std::to_string(entry.first)+"]",b="target["+std::to_string(entry.first)+"]";
                std::set<std::pair<int,std::string>> activeWeights;
                weights.push_back(static_cast<float>(Constant(int(i),a+".tw",Scalar(n,a+".tw",b+".targetWeight",1),activeWeights)));
                offsetsT.push_back(Vector(n,a+".tot",b+".targetOffsetTranslate"));
                offsetsR.push_back(Vector(n,a+".tor",b+".targetOffsetRotate")*(scene.angularUnit=="rad" ? 180.0/3.14159265358979323846 : 1));
            }
            SdfPath output=proxy.IsEmpty()?dag[target]:proxy;
            constraintOutputs[int(i)]={output,parent[target]};
            auto chain=rig.NewMoverChain("constraint_"+std::to_string(i),output);
            rigExec::RigExecSourceConstraintHandle handle;
            GfVec3d offset=Vector(n,"o","offset");
            if(n.type=="aimConstraint") {
                auto c=chain.AddAimConstraint("constraint");
                c.SetAffectRotation(r[0],r[1],r[2]);
                auto aim=Vector(n,"a","aimVector",GfVec3d(1,0,0));
                auto up=Vector(n,"u","upVector",GfVec3d(0,1,0));
                auto worldUp=Vector(n,"wu","worldUpVector",GfVec3d(0,1,0));
                c.SetAimVector(aim[0],aim[1],aim[2]);c.SetUpVector(up[0],up[1],up[2]);
                c.SetWorldUpVector(worldUp[0],worldUp[1],worldUp[2]);
                static const char *types[]={"sceneUp","objectUp","objectRotationUp","vector","none"};
                int type=int(Scalar(n,"wut","worldUpType",3));
                if(type<0 || type>4) throw std::runtime_error("invalid Maya aim worldUpType");
                c.SetWorldUpType(TfToken(types[type]));
                for(auto &l:links) if(l.dst==int(i) && l.src>=0 && !dag[l.src].IsEmpty() && (l.in=="wum" || l.in=="worldUpMatrix")) {
                    c.SetWorldUpObject(dag[l.src]);l.used=true;
                }
                offset*=scene.angularUnit=="rad" ? 180.0/3.14159265358979323846 : 1;
                c.SetRotationOffset(offset[0],offset[1],offset[2]);handle=c;
            } else if(n.type=="parentConstraint") {
                auto c=chain.AddParentConstraint("constraint");
                c.SetAffectTranslation(t[0],t[1],t[2]); c.SetAffectRotation(r[0],r[1],r[2]); c.SetAffectScale(s[0],s[1],s[2]);
                c.SetSources(paths,weights);
                c.SetTranslationOffsets(offsetsT); c.SetRotationOffsets(offsetsR); handle=c;
            } else if(n.type=="pointConstraint") {
                auto c=chain.AddPositionConstraint("constraint"); c.SetAffectTranslation(t[0],t[1],t[2]);
                c.SetTranslationOffset(offset[0],offset[1],offset[2]); handle=c;
            } else if(n.type=="orientConstraint") {
                offset*=scene.angularUnit=="rad" ? 180.0/3.14159265358979323846 : 1;
                // Maya's orient offset is a rotation composition, not an
                // additive offset to the source's world Euler angles.
                auto c=chain.AddParentConstraint("constraint");
                c.SetAffectTranslation(false,false,false);c.SetAffectScale(false,false,false);
                c.SetAffectRotation(r[0],r[1],r[2]);c.SetSources(paths,weights);
                c.SetRotationOffsets(std::vector<GfVec3d>(paths.size(),offset));handle=c;
            } else {
                auto c=chain.AddScaleConstraint("constraint"); c.SetAffectScale(s[0],s[1],s[2]);
                offset=Vector(n,"o","offset",GfVec3d(1));
                if(offset!=GfVec3d(1)) Warn("non-identity multiplicative scale constraint offset unsupported: "+n.name);
                c.SetScaleOffset(0,0,0);handle=c;
            }
            handle.SetSources(paths,weights); handle.SetReadPhase(TfToken("rigExec:sources"),"final");
            translated.insert(static_cast<int>(i)); for(size_t j:consumed) links[j].used=true;
            if(sources.size()>1 && n.type!="pointConstraint") Warn("multi-source Maya rotation/scale interpolation approximated by RigExec blending: " + n.name);
        }
    }
    void Skin(int index,std::set<int> &skinned) {
        const auto &node=scene.nodes[index];
        std::map<int,int> influences;
        for(auto &l:links) if(l.dst==index && l.src>=0 && (Starts(l.in,"ma[") || Starts(l.in,"matrix["))) {
            if(dag[l.src].IsEmpty()) throw std::runtime_error("skin influence is not a transform");
            auto range=Range(l.in,Starts(l.in,"ma[") ? "ma" : "matrix");
            influences[static_cast<int>(range.first)]=l.src; l.used=true;
        }
        if(influences.empty()) { Warn("skinCluster has no connected influences: " + node.name); return; }
        std::set<int> targets,visiting; std::vector<std::string> filters;
        SkinOutputs(index,0,visiting,targets,filters);
        if(targets.size()!=1) { Warn("skinCluster needs one supported downstream mesh output: " + node.name); return; }
        int target=*targets.begin(); VtVec3fArray points; UsdGeomPointBased(stage->GetPrimAtPath(geometry[target])).GetPointsAttr().Get(&points);
        std::map<int,int> remap; std::vector<SdfPath> providers;
        for(const auto &pair:influences) {
            int logical=pair.first,src=pair.second; remap[logical]=static_cast<int>(providers.size());
            auto bindAttr=Attr(node,"pm["+std::to_string(logical)+"]","bindPreMatrix["+std::to_string(logical)+"]");
            GfMatrix4d inverseBind=Matrix(bindAttr,world[src].GetInverse());
            int bindSource=-1;
            for(auto &l:links) if(l.dst==index && l.src>=0 &&
                (l.in=="pm["+std::to_string(logical)+"]" || l.in=="bindPreMatrix["+std::to_string(logical)+"]")) {
                if((l.out=="wim" || l.out=="worldInverseMatrix" || l.out=="wim[0]" || l.out=="worldInverseMatrix[0]") && !dag[l.src].IsEmpty()) {
                    bindSource=l.src;l.used=true;
                } else Warn("skin bindPreMatrix driver requires translation: "+scene.nodes[l.src].name+"."+l.out);
            }
            if(std::abs(inverseBind.GetDeterminant())<1e-12) throw std::runtime_error("singular skin bindPreMatrix");
            auto reused=std::find_if(bindProviders.begin(),bindProviders.end(),[&](const BindProvider &p) {
                return p.source==src && p.bindSource==bindSource && p.inverseBind==inverseBind;
            });
            if(reused!=bindProviders.end()) { providers.push_back(reused->path); continue; }
            auto bindPath=SdfPath("/Rig/Bindings").AppendChild(TfToken(Name(node.name)+"_"+std::to_string(logical)));
            auto provider=RigExecSchemaPrim::Define(stage,bindPath,TfToken("RigExecJoint"));
            // This provider publishes the complete skin matrix. Keeping its
            // rest identity preserves affine binds and live inverse-bind inputs.
            provider.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));
            provider.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
            bindProviders.push_back({src,bindSource,inverseBind,bindPath});
            auto native=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("skinInfluence_"+std::to_string(index)+"_"+std::to_string(logical))),TfToken("RigExecMayaSkinInfluence"));
            native.CreateAttribute(TfToken("inputs:inverseBind"),SdfValueTypeNames->Matrix4d).Set(inverseBind);
            native.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(src));
            native.CreateAttribute(TfToken("inputs:bindPivot"),SdfValueTypeNames->Double3).Set(Pivot(bindSource));
            native.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[src]});
            SdfPathVector inputs{dag[src]};
            if(bindSource>=0) {native.CreateRelationship(TfToken("rigExec:bindSource")).SetTargets({dag[bindSource]});inputs.push_back(dag[bindSource]);}
            native.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(inputs);
            auto matrix=native.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);
            provider.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({matrix.GetPath()});
            providers.push_back(bindPath);
        }
        std::vector<std::map<int,float>> weights(points.size());
        bool clampedRoundoff=false;
        auto weight=[&](const std::string &s) {
            double w=Number(s);
            if(w<0 && w>=-1e-8) { clampedRoundoff=true; return 0.0; }
            if(w<0) throw std::runtime_error("negative skin weight: "+node.name);
            return w;
        };
        for(const auto &a:node.attributes) if(!a.values.empty() && (Starts(a.name,"wl[") || Starts(a.name,"weightList["))) {
            auto vr=Range(a.name,Starts(a.name,"wl[") ? "wl" : "weightList");
            if(vr.second>=points.size()) throw std::runtime_error("skin vertex index out of range");
            const auto suffix=a.name.substr(a.name.find(']')+1);
            if(suffix==".w" || suffix==".weights") {
                // Compact rows: influenceCount (logicalIndex weight)*.
                size_t k=0;
                auto take=[&]() -> const std::string & { if(k>=a.values.size()) throw std::runtime_error("truncated compact skin weights"); return a.values[k++]; };
                for(size_t v=vr.first;v<=vr.second;++v) {
                    size_t n=Count(take()); weights[v].clear();
                    for(size_t j=0;j<n;++j) {
                        int logical=Integer(take()); double w=weight(take());
                        if(!remap.count(logical)) { if(w!=0) throw std::runtime_error("weight references missing influence"); continue; }
                        weights[v][remap[logical]]=static_cast<float>(w);
                    }
                }
                if(k!=a.values.size()) throw std::runtime_error("compact skin weight row count mismatch");
                continue;
            }
            if(vr.first!=vr.second) throw std::runtime_error("indexed skin weights require one vertex");
            auto pos=a.name.find(".w["); size_t len=3;
            if(pos==std::string::npos) { pos=a.name.find(".weights["); len=9; }
            if(pos==std::string::npos) continue;
            auto ir=Range(a.name,a.name.substr(0,pos+len-1));
            if(a.values.size()!=ir.second-ir.first+1) throw std::runtime_error("skin weight range mismatch");
            for(size_t j=0;j<a.values.size();++j) {
                double w=weight(a.values[j]);
                int logical=static_cast<int>(ir.first+j);
                if(!remap.count(logical)) { if(w!=0) throw std::runtime_error("weight references missing influence"); continue; }
                weights[vr.first][remap[logical]]=static_cast<float>(w);
            }
        }
        int slots=1; bool normalize=Scalar(node,"nw","normalizeWeights",1)!=0;
        if(clampedRoundoff) Warn("negative skin weight roundoff within 1e-8 clamped to zero: "+node.name);
        for(auto &row:weights) {
            double sum=0; for(auto it=row.begin();it!=row.end();) { if(it->second==0) it=row.erase(it); else { sum+=it->second; ++it; } }
            if(normalize && sum>0) for(auto &w:row) w.second=static_cast<float>(w.second/sum);
            slots=std::max(slots,static_cast<int>(row.size()));
        }
        if(points.size()*static_cast<size_t>(slots)>MaxElements) throw std::runtime_error("skin weight layout too large");
        std::vector<int> ids(points.size()*slots,0); std::vector<float> values(ids.size(),0);
        for(size_t v=0;v<weights.size();++v) { size_t j=0; for(const auto &w:weights[v]) { ids[v*slots+j]=w.first; values[v*slots+j]=w.second; ++j; } }
        int method=static_cast<int>(Scalar(node,"skm","skinningMethod",0));
        if(method!=0 && method!=1) { Warn("blended linear/DQ skinning is unsupported: " + node.name); return; }
        auto chain=rig.NewMoverChain("skin_"+std::to_string(index),geometry[target].AppendProperty(TfToken("points")));
        auto mover=chain.AddSkinMover("skin",providers,{}, {},TfToken("final"));
        skinMovers[index]=mover.GetPath();
        mover.SetReadPhase(TfToken("final"));
        mover.SetJointInfluences(ids,values,slots); mover.SetSkinningMethod(TfToken(method==0 ? "classicLinear" : "dualQuaternion"));
        mover.SetDefaultWeight(static_cast<float>(Scalar(node,"en","envelope",1)));
        std::vector<std::string> unsupported;
        TfTokenVector stack{TfToken("skin")};
        for(const auto &name:filters) {
            int f=Find(name);const auto &filter=scene.nodes[f];
            if(filter.type!="deltaMush") {
                unsupported.push_back(name); continue;
            }
            if(!stage->GetPrimAtPath(geometry[target]).IsA<UsdGeomMesh>() || Scalar(filter,"sa","smoothingAlgorithm",0)!=0 ||
               Scalar(filter,"iwc","inwardConstraint",0)!=0 || Scalar(filter,"owc","outwardConstraint",0)!=0 ||
               Vector(filter,"s","scale",GfVec3d(1))!=GfVec3d(1)) {
                unsupported.push_back(name);Warn("unsupported deltaMush algorithm/contour/scale setting: "+name);continue;
            }
            auto mush=chain.AddDeltaMushMover("deltaMush_"+std::to_string(f));
            filterMovers[f]=mush.GetPath();
            mush.SetRestPoints({points.begin(),points.end()});
            mush.SetIterations(int(Scalar(filter,"si","smoothingIterations",10)));
            mush.SetStep(float(Scalar(filter,"ss","smoothingStep",0.5)));
            mush.SetPinBorders(Scalar(filter,"pbv","pinBorderVertices",1)!=0);
            mush.SetDistanceWeight(float(Scalar(filter,"dwt","distanceWeight",0)));
            mush.SetDisplacement(float(Scalar(filter,"dsp","displacement",1)));
            mush.SetDefaultWeight(float(Scalar(filter,"en","envelope",1)));
            stack.insert(stack.begin(),mush.GetPath().GetNameToken());
            translated.insert(f);
            // Painted per-vertex filter weights are not the skin weights.
            for(const auto &a:filter.attributes) if(Starts(a.name,"wl[") || Starts(a.name,"weightList["))
                Warn("deltaMush painted weights require translation: "+name);
        }
        stage->GetPrimAtPath(mover.GetPath().GetParentPath()).SetChildrenReorder(stack);
        skinLayouts[index]={providers,ids,values,slots,method};
        auto meshPrim=stage->GetPrimAtPath(geometry[target]);
        meshPrim.CreateRelationship(TfToken("maya:skinInfluences")).SetTargets(providers);
        meshPrim.CreateAttribute(TfToken("maya:skinCluster"),SdfValueTypeNames->String).Set(node.name);
        meshPrim.CreateAttribute(TfToken("maya:unsupportedPostSkinFilters"),SdfValueTypeNames->StringArray).Set(Vt(unsupported));
        skinned.insert(target); translated.insert(index);
    }
    SdfPath SparseBlend(rigExec::RigExecMoverChain &chain,const std::string &name,
                        const SdfPath &target,const std::vector<int> &indices,
                        const std::vector<GfVec3f> &offsets) {
        auto mover=chain.AddBlendShapeMover(name,{},target);
        auto input=rig.AddBlendInput(chain.GetScopePath().GetName()+"_"+name,1);
        auto sample=input.AddSample("sample",1);
        auto shape=UsdSkelBlendShape::Define(stage,input.GetPath().AppendChild(TfToken("offsets")));
        shape.CreatePointIndicesAttr().Set(Vt(indices));shape.CreateOffsetsAttr().Set(Vt(offsets));
        sample.SetBlendShape(shape.GetPath());mover.SetBlendInputs({input.GetPath()});
        return mover.GetPath();
    }
    SdfPath GeometryFollower(int shape) {
        const int source=parent[shape];const auto inverseBind=world[shape].GetInverse();
        for(const auto &p:bindProviders)if(p.source==source && p.bindSource==-1 && p.inverseBind==inverseBind)return p.path;
        const auto path=SdfPath("/Rig/BindFrames").AppendChild(TfToken("shape_"+std::to_string(shape)));
        auto provider=RigExecSchemaPrim::Define(stage,path,TfToken("RigExecJoint"));provider.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));provider.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
        auto native=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(path.GetNameToken()),TfToken("RigExecMayaSkinInfluence"));
        native.CreateAttribute(TfToken("inputs:inverseBind"),SdfValueTypeNames->Matrix4d).Set(inverseBind);
        native.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(source));native.CreateAttribute(TfToken("inputs:bindPivot"),SdfValueTypeNames->Double3).Set(GfVec3d(0));
        native.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[source]});native.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets({dag[source]});
        auto matrix=native.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);provider.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({matrix.GetPath()});
        bindProviders.push_back({source,-1,inverseBind,path});return path;
    }
    // Maya routes evaluated geometry through intermediate shapes and multiple
    // skin stages. Flatten their operations into each destination's point chain;
    // reading a cached intermediate vertex array would drop the upstream pose.
    void GeometryHistories(std::set<int> &skinned) {
        using Element=std::pair<int,long>;
        std::set<Element> active,checked;
        std::function<void(int,long)> validate=[&](int index,long element) {
            const Element key{index,element};if(checked.count(key))return;
            if(!active.insert(key).second)throw std::runtime_error("geometry target/history cycle: "+scene.nodes[index].name);
            for(const auto &l:links)if(l.dst==index && l.src>=0) {
                if(InputElement(l.in)==element) {const auto next=OutputElement(l.out);validate(l.src,next<0?0:next);}
                else if((scene.nodes[index].type=="blendShape" && Starts(l.in,"it["+std::to_string(element)+"].") && l.in.size()>=4 && l.in.substr(l.in.size()-4)==".igt") ||
                        (scene.nodes[index].type=="ffd" && (l.in=="dlp" || l.in=="deformedLatticePoints")))validate(l.src,0);
            }
            active.erase(key);checked.insert(key);
        };
        for(size_t i=0;i<geometry.size();++i)if(!geometry[i].IsEmpty())validate(int(i),0);
        std::map<int,std::set<int>> mapped;
        std::function<void(int,long,std::set<int> &,std::vector<Element> &)> history;
        history=[&](int index,long element,std::set<int> &visiting,std::vector<Element> &nodes) {
            if(!visiting.insert(index).second) throw std::runtime_error("geometry history cycle");
            for(auto &l:links) if(l.dst==index && l.src>=0 && InputElement(l.in)==element) {
                long next=OutputElement(l.out);
                history(l.src,next<0 ? 0 : next,visiting,nodes);
                l.used=true;
                break;
            }
            nodes.push_back({index,element});
            visiting.erase(index);
        };
        auto layer=stage->GetRootLayer();
        struct BlendProxy { int source; SdfPath path; GfMatrix4d geometryMatrix; };
        std::vector<BlendProxy> proxies;
        std::map<int,SdfPath> ffdBases;
        auto applyHistory=[&](int i,const SdfPath &target,const std::string &name) {
            std::set<int> visiting;std::vector<Element> nodes;
            history(int(i),0,visiting,nodes);
            if(nodes.empty()) return;
            auto chain=rig.NewMoverChain(name,target.AppendProperty(TfToken("points")));
            TfTokenVector order;
            SdfPathVector influences;Strings skins;
            if(scene.nodes[i].type=="lattice" && parent[i]>=0) {
                auto follow=chain.AddMatrixMover("cageFollow",GeometryFollower(i),{}, {},TfToken("final"));
                order.push_back(follow.GetPath().GetNameToken());
            }
            for(const auto &entry:nodes) {
                int index=entry.first;long element=entry.second;
                const auto &node=scene.nodes[index];
                const bool skin=skinMovers.count(index);
                if(node.type=="mesh" && meshInputs.count(i) && index!=meshInputs.at(i)) {
                    std::map<size_t,GfVec3d> tweaks;
                    for(const auto &a:node.attributes) if(!a.values.empty() && (Starts(a.name,"pt[") || Starts(a.name,"pnts["))) {
                        auto range=Range(a.name,Starts(a.name,"pt[")?"pt":"pnts");
                        if(a.values.size()!=3*(range.second-range.first+1))throw std::runtime_error("invalid history mesh tweak range");
                        for(size_t v=range.first;v<=range.second;++v) {size_t k=3*(v-range.first);tweaks[v]=GfVec3d(Number(a.values[k]),Number(a.values[k+1]),Number(a.values[k+2]));}
                    }
                    if(!tweaks.empty()) {
                        VtVec3fArray points;UsdGeomPointBased(stage->GetPrimAtPath(target)).GetPointsAttr().Get(&points);
                        std::vector<int> ids;std::vector<GfVec3f> offsets;
                        for(const auto &t:tweaks) {if(t.first>=points.size())throw std::runtime_error("history mesh tweak index out of range");ids.push_back(int(t.first));offsets.emplace_back(geometryMatrices.at(i).TransformDir(t.second));}
                        order.push_back(SparseBlend(chain,"tweaks_"+std::to_string(index),target.AppendProperty(TfToken("points")),ids,offsets).GetNameToken());
                    }
                }
                if(node.type=="blendShape") {
                    // A live target must be evaluated through its own history
                    // before converting local-origin points into the base space.
                    std::vector<std::pair<size_t,int>> targets;
                    const std::string prefix="it["+std::to_string(element)+"].itg[";
                    bool supported=Scalar(node,"or","origin",1)==1;
                    for(const auto &a:node.attributes) if(Starts(a.name,prefix) && !a.values.empty())supported=false;
                    for(auto &l:links) if(l.dst==index && Starts(l.in,prefix)) {
                        auto r=Range(l.in,prefix.substr(0,prefix.size()-1));
                        if(r.first!=r.second || l.in.substr(l.in.find(']',prefix.size())+1)!=".iti[6000].igt" || l.src<0 || geometry[l.src].IsEmpty() || !geometryMatrices.count(l.src)) {supported=false;continue;}
                        targets.push_back({r.first,l.src});
                    }
                    if(!supported || targets.empty()) {Warn("blendShape requires unsupported target/origin translation: "+node.name);continue;}
                    auto mover=chain.AddBlendShapeMover("blend_"+std::to_string(index));
                    mover.SetDefaultWeight(float(Scalar(node,"en","envelope",1)));
                    scalarDestinations.push_back({index,"en",mover.GetPath().AppendProperty(TfToken("inputs:defaultWeight"))});
                    std::vector<SdfPath> inputs;
                    for(const auto &t:targets) {
                        auto input=rig.AddBlendInput(name+"_blend_"+std::to_string(index)+"_"+std::to_string(t.first),float(Scalar(node,"w["+std::to_string(t.first)+"]","weight["+std::to_string(t.first)+"]",0)));
                        scalarDestinations.push_back({index,"w["+std::to_string(t.first)+"]",input.GetPath().AppendProperty(TfToken("inputs:weight"))});
                        auto sample=input.AddSample("sample",1);
                        const auto proxy=SdfPath("/Rig/Geometry").AppendChild(TfToken(name+"_blendTarget_"+std::to_string(index)+"_"+std::to_string(t.first)));
                        VtVec3fArray base;UsdGeomPointBased(stage->GetPrimAtPath(geometry[t.second])).GetPointsAttr().Get(&base);
                        VtVec3fArray receiver;UsdGeomPointBased(stage->GetPrimAtPath(target)).GetPointsAttr().Get(&receiver);
                        if(base.size()!=receiver.size())throw std::runtime_error("live blend target point count differs from base: "+node.name);
                        auto points=UsdGeomPointBased(stage->DefinePrim(proxy,TfToken("Points")));points.CreatePointsAttr().Set(base);
                        UsdGeomImageable(points).CreateVisibilityAttr().Set(TfToken("invisible"));
                        proxies.push_back({t.second,proxy,geometryMatrices.at(i)});
                        sample.SetTargetPoints(proxy.AppendProperty(TfToken("points")));sample.SetReadPhase(TfToken("final"));inputs.push_back(input.GetPath());
                    }
                    mover.SetBlendInputs(inputs);order.push_back(mover.GetPath().GetNameToken());translated.insert(index);mapped[i].insert(index);
                    for(auto &l:links)if(l.dst==index && Starts(l.in,prefix))l.used=true;
                }
                if(node.type=="ffd") {
                    int cage=-1,base=-1;
                    for(auto &l:links)if(l.dst==index && l.src>=0) {
                        if(l.in=="dlp" || l.in=="deformedLatticePoints")cage=l.src;
                        if(l.in=="blm" || l.in=="baseLatticeMatrix")base=parent[l.src];
                    }
                    bool supported=cage>=0 && !geometry[cage].IsEmpty() && meshInputs.count(cage) && base>=0 && !dag[base].IsEmpty() &&
                        Scalar(node,"lo","local",0)==0 && Scalar(node,"fg","freezeGeometry",0)==0 && Scalar(node,"bog","bindToOriginalGeometry",0)==0 && Scalar(node,"upr","usePartialResolution",0)==0;
                    auto cache=supported?Attr(scene.nodes[meshInputs.at(cage)],"cc","cached"):nullptr;
                    const int outside=int(Scalar(node,"ot","outsideLattice",0));
                    if(!cache || cache->values.size()<4 || cache->values[0]!="2" || cache->values[1]!="2" || cache->values[2]!="2" || outside<0 || outside>1) {Warn("FFD requires unsupported cage/bind/outside translation: "+node.name);continue;}
                    if(!ffdBases.count(index)) {
                        const auto path=SdfPath("/Rig/BindFrames").AppendChild(TfToken("ffdBase_"+std::to_string(index)));
                        auto provider=RigExecSchemaPrim::Define(stage,path,TfToken("RigExecJoint"));provider.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));provider.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
                        auto native=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(path.GetNameToken()),TfToken("RigExecMayaSkinInfluence"));
                        native.CreateAttribute(TfToken("inputs:inverseBind"),SdfValueTypeNames->Matrix4d).Set(GfMatrix4d(1));native.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(base));native.CreateAttribute(TfToken("inputs:bindPivot"),SdfValueTypeNames->Double3).Set(GfVec3d(0));
                        native.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[base]});native.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets({dag[base]});
                        auto matrix=native.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);provider.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({matrix.GetPath()});ffdBases[index]=path;
                    }
                    const auto prim=stage->DefinePrim(chain.GetScopePath().AppendChild(TfToken("ffd_"+std::to_string(index))),TfToken("RigExecMayaFfd"));
                    prim.ApplyAPI(TfToken("RigExecMoverAPI"));
                    prim.CreateRelationship(TfToken("rigExec:moves")).SetTargets({target.AppendProperty(TfToken("points"))});
                    prim.CreateRelationship(TfToken("rigExec:cage")).SetTargets({geometry[cage]});prim.CreateRelationship(TfToken("rigExec:transform")).SetTargets({ffdBases.at(index)});
                    prim.CreateAttribute(TfToken("inputs:outside"),SdfValueTypeNames->Int).Set(outside);
                    prim.GetAttribute(TfToken("inputs:defaultWeight")).Set(float(Scalar(node,"en","envelope",1)));
                    scalarDestinations.push_back({index,"en",prim.GetPath().AppendProperty(TfToken("inputs:defaultWeight"))});
                    VtVec3fArray points;UsdGeomPointBased(stage->GetPrimAtPath(target)).GetPointsAttr().Get(&points);VtFloatArray weights(points.size(),1);
                    for(const auto &a:node.attributes) if(!a.values.empty() && Starts(a.name,"wl["+std::to_string(element)+"].w[")) {
                        auto range=Range(a.name,"wl["+std::to_string(element)+"].w");
                        if(range.second>=weights.size() || a.values.size()!=range.second-range.first+1)throw std::runtime_error("invalid FFD weight range");
                        for(size_t j=range.first;j<=range.second;++j)weights[j]=float(Number(a.values[j-range.first]));
                    }
                    prim.CreateAttribute(TfToken("inputs:pointWeights"),SdfValueTypeNames->FloatArray).Set(weights);
                    order.push_back(prim.GetName());translated.insert(index);mapped[i].insert(index);
                    for(auto &l:links)if(l.dst==index && (l.in=="dlp" || l.in=="dlm" || l.in=="blm"))l.used=true;
                }
                if(node.type=="cluster") {
                    int source=-1;bool supported=true;const bool relative=Scalar(node,"rel","relative",0)!=0;
                    const auto tag=Attr(node,"ip["+std::to_string(element)+"].gtg");
                    for(const auto &l:links)if(l.dst==index && l.src>=0 && (l.in=="ma" || l.in=="matrix")) {
                        if(!dag[l.src].IsEmpty() && (l.out=="wm" || l.out=="wm[0]" || l.out=="worldMatrix" || l.out=="worldMatrix[0]"))source=l.src;
                    }
                    if(source<0 || !supported) {Warn("cluster requires unsupported relative/matrix/component translation: "+node.name);continue;}
                    VtVec3fArray points;UsdGeomPointBased(stage->GetPrimAtPath(target)).GetPointsAttr().Get(&points);
                    std::vector<float> weights(points.size(),1);
                    if(tag && tag->values[0]!="*") {
                        supported=false;
                        if(scene.nodes[i].type=="lattice" && meshInputs.count(i)) {
                            const auto &input=scene.nodes[meshInputs.at(i)];auto cache=Attr(input,"cc","cached");
                            for(const auto &a:input.attributes) if(a.type=="string" && a.values==tag->values && a.name.size()>7 && a.name.substr(a.name.size()-7)==".gtagnm") {
                                auto components=Attr(input,a.name.substr(0,a.name.size()-7)+".gtagcmp");
                                if(!components || !cache)break;
                                weights.assign(points.size(),0);supported=true;
                                for(size_t j=1;j<components->values.size();++j) {
                                    const auto &component=components->values[j];size_t pos=2;int xyz[3]={};
                                    if(!Starts(component,"pt[")) {supported=false;break;}
                                    for(int axis=0;axis<3;++axis) {auto end=component.find(']',pos+1);if(end==std::string::npos || component[pos]!='[') {supported=false;break;}xyz[axis]=Integer(component.substr(pos+1,end-pos-1));pos=end+1;}
                                    const int nx=Integer(cache->values[0]),ny=Integer(cache->values[1]),nz=Integer(cache->values[2]);
                                    if(!supported || pos!=component.size() || xyz[0]<0 || xyz[0]>=nx || xyz[1]<0 || xyz[1]>=ny || xyz[2]<0 || xyz[2]>=nz) {supported=false;break;}
                                    weights[size_t((xyz[2]*ny+xyz[1])*nx+xyz[0])]=1;
                                }
                            }
                        }
                        if(!supported) {Warn("cluster component tag requires translation: "+node.name);continue;}
                    }
                    for(const auto &a:node.attributes) if(!a.values.empty() && Starts(a.name,"wl["+std::to_string(element)+"].w[")) {
                        auto prefix="wl["+std::to_string(element)+"].w";auto range=Range(a.name,prefix);
                        if(range.second>=weights.size() || a.values.size()!=range.second-range.first+1)throw std::runtime_error("invalid cluster weight range");
                        for(size_t j=range.first;j<=range.second;++j)weights[j]=float(Number(a.values[j-range.first]));
                    }
                    const auto path=SdfPath("/Rig/BindFrames").AppendChild(TfToken(name+"_cluster_"+std::to_string(index)));
                    auto provider=RigExecSchemaPrim::Define(stage,path,TfToken("RigExecJoint"));provider.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));provider.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
                    auto native=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(path.GetNameToken()),TfToken(relative?"RigExecMayaMatrixSpace":"RigExecMayaSkinInfluence"));
                    native.CreateAttribute(TfToken("inputs:inverseBind"),SdfValueTypeNames->Matrix4d).Set(Matrix(Attr(node,"pm","bindPreMatrix")));
                    native.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(source));native.CreateAttribute(TfToken("inputs:bindPivot"),SdfValueTypeNames->Double3).Set(GfVec3d(0));
                    native.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[source]});native.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets({dag[source]});
                    if(relative) {
                        native.CreateAttribute(TfToken("inputs:mode"),SdfValueTypeNames->Int).Set(5);
                        native.CreateAttribute(TfToken("inputs:sourceParentPivot"),SdfValueTypeNames->Double3).Set(Pivot(parent[source]));
                        native.CreateAttribute(TfToken("inputs:sourceOffsetInverse"),SdfValueTypeNames->Matrix4d).Set(GfMatrix4d(1));
                        native.CreateAttribute(TfToken("inputs:local"),SdfValueTypeNames->Matrix4d).Set(Matrix(Attr(node,"gm["+std::to_string(element)+"]","geomMatrix["+std::to_string(element)+"]")));
                        native.CreateAttribute(TfToken("inputs:parentPivot"),SdfValueTypeNames->Double3).Set(Pivot(parent[i]));
                        if(parent[source]>=0) {native.CreateRelationship(TfToken("rigExec:sourceParent")).SetTargets({dag[parent[source]]});native.GetRelationship(TfToken("rigExec:poseInputs")).SetTargets({dag[source],dag[parent[source]]});}
                        if(parent[i]>=0) {native.CreateRelationship(TfToken("rigExec:parent")).SetTargets({dag[parent[i]]});SdfPathVector deps;native.GetRelationship(TfToken("rigExec:poseInputs")).GetTargets(&deps);deps.push_back(dag[parent[i]]);native.GetRelationship(TfToken("rigExec:poseInputs")).SetTargets(deps);}
                    }
                    auto matrix=native.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);provider.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({matrix.GetPath()});
                    auto identity=RigExecSchemaPrim::Define(stage,path.GetParentPath().AppendChild(TfToken(path.GetName()+"_identity")),TfToken("RigExecJoint"));identity.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));identity.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
                    auto mover=chain.AddSkinMover("cluster_"+std::to_string(index),{identity.GetPath(),path},{}, {},TfToken("final"));
                    std::vector<int> ids;std::vector<float> values;
                    for(float weight:weights) {ids.push_back(0);ids.push_back(1);values.push_back(1-weight);values.push_back(weight);}
                    mover.SetJointInfluences(ids,values,2);mover.SetDefaultWeight(float(Scalar(node,"en","envelope",1)));
                    scalarDestinations.push_back({index,"en",mover.GetPath().AppendProperty(TfToken("inputs:defaultWeight"))});
                    order.push_back(mover.GetPath().GetNameToken());translated.insert(index);mapped[i].insert(index);
                    for(auto &l:links)if(l.dst==index && (l.in=="ma" || l.in=="matrix" || l.in=="x"))l.used=true;
                }
                if(!skin && !filterMovers.count(index))continue;
                const SdfPath from=skin ? skinMovers.at(index) : filterMovers.at(index);
                const SdfPath to=chain.GetScopePath().AppendChild(TfToken("op_"+std::to_string(index)));
                if(!SdfCopySpec(layer,from,layer,to)) throw std::runtime_error("cannot copy geometry history operation");
                auto prim=stage->GetPrimAtPath(to);
                prim.GetRelationship(TfToken("rigExec:moves")).SetTargets({target.AppendProperty(TfToken("points"))});
                scalarDestinations.push_back({index,"en",to.AppendProperty(TfToken("inputs:defaultWeight"))});
                order.push_back(to.GetNameToken());
                mapped[i].insert(index);
                if(skin) {
                    const auto &layout=skinLayouts.at(index);
                    influences.insert(influences.end(),layout.providers.begin(),layout.providers.end());
                    skins.push_back(scene.nodes[index].name);
                }
            }
            if(order.empty()) {stage->RemovePrim(chain.GetScopePath());return;}
            if(scene.nodes[i].type=="mesh" && skins.empty() && parent[i]>=0) {
                auto follow=chain.AddMatrixMover("shapeFollow",GeometryFollower(i),{}, {},TfToken("final"));
                order.push_back(follow.GetPath().GetNameToken());
            }
            std::reverse(order.begin(),order.end());
            stage->GetPrimAtPath(chain.GetScopePath()).SetChildrenReorder(order);
            auto mesh=stage->GetPrimAtPath(target);
            mesh.CreateAttribute(TfToken("maya:skinHistory"),SdfValueTypeNames->StringArray).Set(Vt(skins));
            mesh.CreateRelationship(TfToken("maya:skinInfluences")).SetTargets(influences);
            skinned.insert(int(i));
        };
        for(size_t i=0;i<geometry.size();++i) if(!geometry[i].IsEmpty())applyHistory(int(i),geometry[i],"history_"+std::to_string(i));
        for(size_t j=0;j<proxies.size();++j) {
            const auto proxy=proxies[j];
            applyHistory(proxy.source,proxy.path,"blendTargetHistory_"+std::to_string(j));
            auto frame=RigExecSchemaPrim::Define(stage,SdfPath("/Rig/BindFrames").AppendChild(TfToken("blendTarget_"+std::to_string(j))),TfToken("RigExecJoint"));
            frame.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));frame.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
            auto native=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(frame.GetPath().GetNameToken()),TfToken("RigExecMayaMatrixSpace"));
            native.CreateAttribute(TfToken("inputs:mode"),SdfValueTypeNames->Int).Set(6);
            native.CreateAttribute(TfToken("inputs:local"),SdfValueTypeNames->Matrix4d).Set(proxy.geometryMatrix);
            const auto source=parent[proxy.source];
            native.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(source));
            native.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[source]});native.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets({dag[source]});
            auto matrix=native.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);frame.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({matrix.GetPath()});
            auto chain=rig.NewMoverChain("blendTargetOrigin_"+std::to_string(j),proxy.path.AppendProperty(TfToken("points")));
            auto mover=chain.AddMatrixMover("origin",frame.GetPath(),{}, {},TfToken("final"));
            // Chains are ordered after all targets have been constructed.
        }
        for(const auto &entry:skinMovers) stage->RemovePrim(entry.second.GetParentPath());
        for(size_t i=0;i<geometry.size();++i)if(!geometry[i].IsEmpty()) {
            auto attr=stage->GetPrimAtPath(geometry[i]).GetAttribute(TfToken("maya:unsupportedPostSkinFilters"));VtStringArray filters,remaining;
            if(!attr || !attr.Get(&filters))continue;
            for(const auto &name:filters)if(!mapped[int(i)].count(Find(name))) {
                remaining.push_back(name);Warn("unsupported post-skin deformer requires translation: "+name);
            }
            attr.Set(remaining);
        }
    }
    void MatrixSpaces() {
        auto create=[&](int destination,int source,bool inverse) {
            auto node=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("matrixSpace_"+std::to_string(destination))),TfToken("RigExecMayaMatrixSpace"));
            auto target=stage->GetPrimAtPath(dag[destination]);GfMatrix4d local=Translation(Pivot(destination))*MayaLocal(destination);
            if(source>=0)local*=OffsetMatrix(destination).GetInverse();
            node.CreateAttribute(TfToken("inputs:local"),SdfValueTypeNames->Matrix4d).Set(local);
            // Maya edits absolute Euler channels inside its pivot/scale stack.
            // Prepending a rotation to the saved matrix reverses mirrored axes.
            const auto &n=scene.nodes[destination];const double angle=scene.angularUnit=="rad"?180.0/3.14159265358979323846:1;
            auto t=constraintRestTranslations.count(destination)?constraintRestTranslations.at(destination):Vector(n,"t","translate");
            auto r=(constraintRestRotations.count(destination)?constraintRestRotations.at(destination):Vector(n,"r","rotate"))*angle;
            const auto rp=Pivot(destination),sp=Vector(n,"sp","scalePivot"),shear=Vector(n,"sh","shear");
            GfMatrix4d sh(1);sh[1][0]=shear[0];sh[2][0]=shear[1];sh[2][1]=shear[2];
            node.CreateAttribute(TfToken("inputs:channelComposition"),SdfValueTypeNames->Bool).Set(true);
            node.CreateAttribute(TfToken("inputs:baseTranslate"),SdfValueTypeNames->Double3).Set(t);
            node.CreateAttribute(TfToken("inputs:baseRotate"),SdfValueTypeNames->Double3).Set(r);
            auto baseScale=Vector(n,"s","scale",GfVec3d(1));
            for(int axis=0;axis<3;++axis)if(baseScale[axis]==0) {
                baseScale[axis]=1;absoluteScales.insert({destination,"xyz"[axis]});
                target.GetAttribute(TfToken(std::string("avars:s")+"xyz"[axis])).Set(0.0);
            }
            node.CreateAttribute(TfToken("inputs:baseScale"),SdfValueTypeNames->Double3).Set(baseScale);
            node.CreateAttribute(TfToken("inputs:scalePrefix"),SdfValueTypeNames->Matrix4d).Set(Translation(rp-sp));
            node.CreateAttribute(TfToken("inputs:scaleSuffix"),SdfValueTypeNames->Matrix4d).Set(sh*Translation(sp)*Translation(Vector(n,"spt","scalePivotTranslate"))*Translation(-rp)*Rotation(Vector(n,"ra","rotateAxis")*angle,0));
            node.CreateAttribute(TfToken("inputs:rotateSuffix"),SdfValueTypeNames->Matrix4d).Set(Rotation(Vector(n,"jo","jointOrient")*angle,0)*Translation(rp)*Translation(Vector(n,"rpt","rotatePivotTranslate")));
            node.CreateAttribute(TfToken("inputs:jointChannels"),SdfValueTypeNames->Bool).Set(n.type=="joint");
            if(n.type=="joint" && Scalar(n,"ssc","segmentScaleCompensate",1)!=0) {
                node.CreateAttribute(TfToken("inputs:segmentScaleCompensate"),SdfValueTypeNames->Bool).Set(true);
                node.GetAttribute(TfToken("inputs:rotateSuffix")).Set(Rotation(Vector(n,"jo","jointOrient")*angle,0));
                node.CreateAttribute(TfToken("inputs:postRotate"),SdfValueTypeNames->Matrix4d).Set(Translation(rp)*Translation(Vector(n,"rpt","rotatePivotTranslate")));
                const auto inverse=InverseScale(destination);
                for(int axis=0;axis<3;++axis) {
                    auto a=node.CreateAttribute(TfToken(std::string("inputs:inverseScale")+"XYZ"[axis]),SdfValueTypeNames->Double);a.Set(inverse[axis]);
                    scalarReads.push_back({destination,std::string("is")+"xyz"[axis],a.GetPath()});
                }
            }
            node.CreateAttribute(TfToken("inputs:offset"),SdfValueTypeNames->Matrix4d).Set(source<0?OffsetMatrix(destination):GfMatrix4d(1));
            node.CreateAttribute(TfToken("inputs:mode"),SdfValueTypeNames->Int).Set(inverse ? 1 : 0);
            node.CreateAttribute(TfToken("inputs:parentPivot"),SdfValueTypeNames->Double3).Set(Scalar(scene.nodes[destination],"it","inheritsTransform",1)!=0 ? Pivot(parent[destination]) : GfVec3d(0));
            node.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(source));
            node.CreateAttribute(TfToken("inputs:sourceParentPivot"),SdfValueTypeNames->Double3).Set(source<0 ? GfVec3d(0) : Pivot(parent[source]));
            node.CreateAttribute(TfToken("inputs:sourceOffsetInverse"),SdfValueTypeNames->Matrix4d).Set(source<0 ? GfMatrix4d(1) : OffsetMatrix(source).GetInverse());
            TfToken order("XYZ");target.GetAttribute(TfToken("avars:rotationOrder")).Get(&order);
            node.CreateAttribute(TfToken("inputs:rotationOrder"),SdfValueTypeNames->Token).Set(order);
            for(const char *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"}) {
                auto a=node.CreateAttribute(TfToken(std::string("inputs:")+channel),SdfValueTypeNames->Double);
                a.Set(channel[0]=='s' ? 1.0 : 0.0);
                a.SetConnections({dag[destination].AppendProperty(TfToken(std::string("avars:")+channel))});
            }
            SdfPathVector providers;
            if(source>=0) {
                node.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[source]});providers.push_back(dag[source]);
                if(parent[source]>=0) {node.CreateRelationship(TfToken("rigExec:sourceParent")).SetTargets({dag[parent[source]]});providers.push_back(dag[parent[source]]);}
            }
            if(parent[destination]>=0 && Scalar(scene.nodes[destination],"it","inheritsTransform",1)!=0) {
                node.CreateRelationship(TfToken("rigExec:parent")).SetTargets({dag[parent[destination]]});providers.push_back(dag[parent[destination]]);
            }
            node.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(providers);
            auto matrix=node.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);
            target.GetAttribute(TfToken("posed:space")).SetConnections({matrix.GetPath()});
        };
        for(size_t i=0;i<dag.size();++i) if(!dag[i].IsEmpty()) {
            GfMatrix4d local(1);stage->GetPrimAtPath(dag[i]).GetAttribute(TfToken("rest:space")).Get(&local);
            bool affine=Scalar(scene.nodes[i],"it","inheritsTransform",1)==0 || local.GetDeterminant()<0;
            const auto scale=Vector(scene.nodes[i],"s","scale",GfVec3d(1));
            for(int axis=0;axis<3;++axis)if(scale[axis]<0)affine=true;
            if(scene.nodes[i].type=="joint")for(const auto &l:links)if(l.dst==int(i) && l.src>=0 &&
                ((Scalar(scene.nodes[i],"ssc","segmentScaleCompensate",1)!=0 && (l.in=="is" || l.in=="inverseScale" || Starts(l.in,"is") || Starts(l.in,"inverseScale"))) ||
                 l.in=="s" || l.in=="scale" || l.in=="sx" || l.in=="sy" || l.in=="sz"))affine=true;
            for(int r=0;r<3;++r)for(int c=0;c<3;++c) {
                double dot=0;for(int k=0;k<3;++k)dot+=local[r][k]*local[c][k];
                if(std::abs(dot-(r==c ? 1 : 0))>1e-8)affine=true;
            }
            SdfPathVector connections;stage->GetPrimAtPath(dag[i]).GetAttribute(TfToken("posed:space")).GetConnections(&connections);
            if(affine && connections.empty())create(int(i),-1,false);
        }
        for(auto &l:links) if(l.src>=0 && l.dst>=0 && !dag[l.src].IsEmpty() && !dag[l.dst].IsEmpty() &&
            (l.out=="im" || l.out=="inverseMatrix" || l.out=="m" || l.out=="matrix") &&
            (l.in=="opm" || l.in=="offsetParentMatrix")) {
            create(l.dst,l.src,l.out=="im" || l.out=="inverseMatrix");
            l.used=true;
        }
    }
    void OrderMovers() {
        auto scope=stage->GetPrimAtPath(SdfPath("/Rig/Movers")); if(!scope) return;
        struct Chain { TfToken name; SdfPathVector reads,writes; };
        std::vector<Chain> chains;
        for(auto child:scope.GetChildren()) {
            Chain c; c.name=child.GetName();
            for(auto prim:UsdPrimRange(child)) {
                auto append=[&](const char *name,SdfPathVector &paths) {
                    auto rel=prim.GetRelationship(TfToken(name)); SdfPathVector targets;
                    if(rel) rel.GetTargets(&targets); paths.insert(paths.end(),targets.begin(),targets.end());
                };
                append("rigExec:moves",c.writes); append("rigExec:sources",c.reads);
                append("rigExec:influences",c.reads); append("rigExec:transform",c.reads);
                append("rigExec:effector",c.reads); append("rigExec:poleVectorObjects",c.reads);
                append("rigExec:worldUpObject",c.reads);
                append("rigExec:cage",c.reads);
            }
            // A constraint writes a world frame. Its ancestors must have
            // reached their final pose first, otherwise a later parent edit
            // transforms the already-constrained child a second time.
            for(const auto &target:c.writes) if(target.IsPrimPath())
                c.reads.push_back(target.GetParentPath());
            std::set<SdfPath> expanded;
            for(size_t r=0;r<c.reads.size();++r) {
                for(auto path=c.reads[r].GetPrimPath();path!=SdfPath::AbsoluteRootPath() && !path.IsEmpty();path=path.GetParentPath()) {
                    if(!expanded.insert(path).second)break;
                    auto prim=stage->GetPrimAtPath(path);if(!prim)continue;
                    SdfPathVector channels;auto channelInputs=prim.GetRelationship(TfToken("maya:channelInputs"));if(channelInputs)channelInputs.GetTargets(&channels);c.reads.insert(c.reads.end(),channels.begin(),channels.end());
                    SdfPathVector connections;auto posed=prim.GetAttribute(TfToken("posed:space"));if(posed)posed.GetConnections(&connections);
                    for(const auto &connection:connections) {
                        auto mechanism=stage->GetPrimAtPath(connection.GetPrimPath());
                        if(mechanism) {
                            SdfPathVector providers;mechanism.GetRelationship(TfToken("rigExec:poseInputs")).GetTargets(&providers);
                            c.reads.insert(c.reads.end(),providers.begin(),providers.end());
                        }
                    }
                }
            }
            chains.push_back(std::move(c));
        }
        std::vector<int> state(chains.size(),0); TfTokenVector order;
        std::function<void(size_t)> visit=[&](size_t i) {
            if(state[i]==2) return;
            if(state[i]==1) throw std::runtime_error("cyclic translated rig dependency at " + chains[i].name.GetString());
            state[i]=1;
            for(size_t j=0;j<chains.size();++j) if(i!=j) {
                bool depends=false;
                for(const auto &r:chains[i].reads) for(const auto &w:chains[j].writes)
                    if(r==w || (w.IsPrimPath() && r.HasPrefix(w))) depends=true;
                if(depends) visit(j);
            }
            state[i]=2; order.push_back(chains[i].name);
        };
        for(size_t i=0;i<chains.size();++i) visit(i);
        // RigExec applies sibling operations bottom-to-top.
        std::reverse(order.begin(),order.end()); scope.SetChildrenReorder(order);
    }
    void ChannelConnections() {
        struct Value { SdfPath path; double value=0; SdfPathVector providers; bool valid=true; };
        std::map<std::pair<int,std::string>,Value> cache;
        std::set<std::pair<int,std::string>> active;
        int serial=0;
        const std::map<std::string,std::string> names={
            {"translate","t"},{"rotate","r"},{"scale","s"},
            {"translateX","tx"},{"translateY","ty"},{"translateZ","tz"},
            {"rotateX","rx"},{"rotateY","ry"},{"rotateZ","rz"},
            {"scaleX","sx"},{"scaleY","sy"},{"scaleZ","sz"}};
        auto canonical=[&](std::string a) {auto i=names.find(a);return i==names.end()?a:i->second;};
        auto channel=[](const std::string &a) {return a.size()==2 && std::string("trs").find(a[0])!=std::string::npos && std::string("xyz").find(a[1])!=std::string::npos;};
        auto math=[&](int mode,Value a,Value b,Value c) {
            Value result;if(!a.valid || !b.valid || !c.valid) {result.valid=false;return result;}
            if(a.path.IsEmpty() && b.path.IsEmpty() && c.path.IsEmpty()) {
                switch(mode) {
                case 0:result.value=a.value*b.value+c.value;break;
                case 1:result.value=a.value*c.value+b.value*(1-c.value);break;
                case 2:result.value=b.value==0?0:a.value/b.value;break;
                case 3:result.value=std::pow(a.value,b.value);break;
                case 4:result.value=float(float(a.value)+float(b.value));break;
                case 5:result.value=float(float(a.value)-float(b.value));break;
                case 6:result.value=float(float(a.value)*float(b.value));break;
                case 7:result.value=b.value==0?0:float(float(a.value)/float(b.value));break;
                case 8:result.value=std::clamp(a.value,0.0,1.0);break;
                case 9:result.value=a.value*a.value*(3-2*a.value);break;
                case 10:result.value=a.value>=b.value ? 1 : 0;break;
                case 11:result.value=float(a.value)==float(b.value)?1:0;break;
                case 12:result.value=float(a.value)!=float(b.value)?1:0;break;
                case 13:result.value=float(a.value)>float(b.value)?1:0;break;
                case 14:result.value=float(a.value)>=float(b.value)?1:0;break;
                case 15:result.value=float(a.value)<float(b.value)?1:0;break;
                case 16:result.value=float(a.value)<=float(b.value)?1:0;break;
                case 17:result.value=float(a.value);break;
                default:result.valid=false;
                }
                result.valid=result.valid && std::isfinite(result.value);return result;
            }
            auto prim=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("scalar_"+std::to_string(serial++))),TfToken("RigExecMayaScalar"));
            prim.CreateAttribute(TfToken("inputs:mode"),SdfValueTypeNames->Int).Set(mode);
            for(auto input:std::vector<std::pair<const char *,Value>>{{"inputs:a",a},{"inputs:b",b},{"inputs:c",c}}) {
                auto attr=prim.CreateAttribute(TfToken(input.first),SdfValueTypeNames->Double);attr.Set(input.second.value);
                if(!input.second.path.IsEmpty())attr.SetConnections({input.second.path});
                result.providers.insert(result.providers.end(),input.second.providers.begin(),input.second.providers.end());
            }
            prim.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(result.providers);
            result.path=prim.CreateAttribute(TfToken("outputs:value"),SdfValueTypeNames->Double).GetPath();return result;
        };
        std::function<Value(int,std::string)> output;
        std::function<Value(int,std::string,std::string,int,double)> input;
        input=[&](int node,std::string shortName,std::string longName,int component,double fallback) {
            for(auto &l:links) if(l.dst==node && l.src>=0) {
                std::string from=l.out;
                bool found=l.in==shortName || l.in==longName;
                if(component>=0) {
                    found=(shortName.size()>1 && l.in==shortName.substr(0,shortName.size()-1)+"xyz"[component]+shortName.back()) || l.in==shortName+"xyz"[component] || l.in==shortName+"rgb"[component] || l.in==longName+"XYZ"[component] || l.in==longName+"RGB"[component];
                    if(l.in==shortName || l.in==longName) {
                        found=true;
                        const auto &type=scene.nodes[l.src].type;
                        from+=type=="blendColors" ? "rgb"[component] : "xyz"[component];
                    }
                }
                if(found) {auto v=output(l.src,from);if(v.valid)l.used=true;return v;}
            }
            Value result;
            if(component<0)result.value=Scalar(scene.nodes[node],shortName,longName,fallback);
            else {
                result.value=Vector(scene.nodes[node],shortName,longName,GfVec3d(fallback))[component];
                const auto &n=scene.nodes[node];
                result.value=Scalar(n,shortName+"rgb"[component],longName+"RGB"[component],result.value);
            }
            return result;
        };
        output=[&](int node,std::string attr)->Value {
            attr=canonical(attr);auto key=std::make_pair(node,attr);
            if(cache.count(key))return cache.at(key);
            if(!active.insert(key).second)return Value{{},0,{},false};
            Value result{{},0,{},false};const auto &n=scene.nodes[node];
            if(n.type=="joint" && attr.size()==3 && Starts(attr,"is") && std::string("xyz").find(attr.back())!=std::string::npos) {
                result=input(node,"is","inverseScale",int(std::string("xyz").find(attr.back())),1);
            } else if(!dag[node].IsEmpty() && channel(attr)) {
                const int axis=std::string("xyz").find(attr[1]);
                bool constrained=false;
                for(const auto &l:links)if(l.dst==node && l.src>=0 && canonical(l.in).front()==attr[0] && scene.nodes[l.src].type.find("Constraint")!=std::string::npos)constrained=true;
                if(constrained && attr[0]=='t' && Pivot(node)==GfVec3d(0) && Matrix(Attr(n,"opm","offsetParentMatrix"))==GfMatrix4d(1)) {
                    auto reader=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("translation_"+std::to_string(serial++))),TfToken("RigExecMayaTranslation"));
                    reader.CreateAttribute(TfToken("inputs:axis"),SdfValueTypeNames->Int).Set(axis);
                    reader.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(node));
                    reader.CreateAttribute(TfToken("inputs:parentPivot"),SdfValueTypeNames->Double3).Set(Pivot(parent[node]));
                    reader.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[node]});result.providers={dag[node]};
                    if(parent[node]>=0) {reader.CreateRelationship(TfToken("rigExec:parent")).SetTargets({dag[parent[node]]});result.providers.push_back(dag[parent[node]]);}
                    reader.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(result.providers);
                    result.path=reader.CreateAttribute(TfToken("outputs:value"),SdfValueTypeNames->Double).GetPath();result.valid=true;
                } else if(!constrained) {
                    double base=Vector(n,std::string(1,attr[0]),attr[0]=='t'?"translate":attr[0]=='r'?"rotate":"scale",GfVec3d(attr[0]=='s'?1:0))[axis];
                    if(attr[0]=='r' && scene.angularUnit=="rad")base*=180.0/3.14159265358979323846;
                    Value avar{dag[node].AppendProperty(TfToken("avars:"+attr)),0,{},true};
                    if(attr[0]=='s')result=base==1 || absoluteScales.count({node,attr[1]}) ? avar : math(0,avar,Value{{},base,{},true},Value{});
                    else result=base==0 ? avar : math(0,avar,Value{{},1,{},true},Value{{},base,{},true});
                }
            } else if(constraintOutputs.count(node) && (attr=="ctx" || attr=="cty" || attr=="ctz")) {
                const auto entry=constraintOutputs.at(node);auto reader=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("constraintTranslation_"+std::to_string(serial++))),TfToken("RigExecMayaTranslation"));
                reader.CreateAttribute(TfToken("inputs:axis"),SdfValueTypeNames->Int).Set(int(std::string("xyz").find(attr.back())));
                reader.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(GfVec3d(0));
                reader.CreateAttribute(TfToken("inputs:parentPivot"),SdfValueTypeNames->Double3).Set(Pivot(entry.second));
                reader.CreateRelationship(TfToken("rigExec:source")).SetTargets({entry.first});result.providers={entry.first};
                if(entry.second>=0) {reader.CreateRelationship(TfToken("rigExec:parent")).SetTargets({dag[entry.second]});result.providers.push_back(dag[entry.second]);}
                reader.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(result.providers);
                result.path=reader.CreateAttribute(TfToken("outputs:value"),SdfValueTypeNames->Double).GetPath();result.valid=true;
            } else if((n.type=="pointOnCurveInfo" && (attr=="px" || attr=="py" || attr=="pz" || Starts(attr,"position"))) ||
                      (n.type=="motionPath" && (attr=="acx" || attr=="acy" || attr=="acz" || Starts(attr,"allCoordinates")) && Scalar(n,"fm","fractionMode",0)==0)) {
                int curve=-1;for(const auto &l:links)if(l.dst==node && l.src>=0 && (l.in=="ic" || l.in=="inputCurve" || l.in=="gp" || l.in=="geometryPath"))curve=l.src;
                if(curve>=0) {
                    std::set<int> visited;int data=InputMesh(curve,0,visited);auto cc=data<0?nullptr:Attr(scene.nodes[data],"cc","cached");
                    if(cc && cc->type=="nurbsCurve") {
                        size_t k=0;int degree=Integer(cc->values.at(k++));k++;int form=Integer(cc->values.at(k++));bool rational=Number(cc->values.at(k++))!=0;k++;
                        size_t nk=Count(cc->values.at(k++));std::vector<double> knots;for(size_t j=0;j<nk;++j)knots.push_back(Number(cc->values.at(k++)));
                        const double first=knots.front(),last=knots.back();knots.insert(knots.begin(),form==2?first-(knots[1]-first):first);knots.push_back(form==2?last+(last-knots[knots.size()-2]):last);
                        size_t np=Count(cc->values.at(k++));std::vector<GfVec3d> points;std::vector<double> rationalWeights;
                        for(size_t j=0;j<np;++j) {double x=Number(cc->values.at(k++)),y=Number(cc->values.at(k++)),z=Number(cc->values.at(k++));points.push_back(world[data].Transform(GfVec3d(x,y,z)));rationalWeights.push_back(rational?Number(cc->values.at(k++)):1);}
                        visited.clear();int skin=InputSkin(curve,0,visited);SkinLayout layout;if(skin>=0 && skinLayouts.count(skin))layout=skinLayouts.at(skin);
                        bool staticParameter=true;for(const auto &l:links)if(l.dst==node && (l.in=="u" || l.in=="uValue" || l.in=="pr" || l.in=="parameter"))staticParameter=false;
                        if(layout.method==0 && (n.type!="motionPath" || form==0) && staticParameter && (layout.indices.empty() || layout.indices.size()==np*size_t(layout.slots))) {
                            auto reader=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("curvePoint_"+std::to_string(serial++))),TfToken("RigExecMayaCurvePoint"));
                            reader.CreateAttribute(TfToken("inputs:points"),SdfValueTypeNames->Double3Array).Set(Vt(points));reader.CreateAttribute(TfToken("inputs:uKnots"),SdfValueTypeNames->DoubleArray).Set(Vt(knots));
                            reader.CreateAttribute(TfToken("inputs:rationalWeights"),SdfValueTypeNames->DoubleArray).Set(Vt(rationalWeights));reader.CreateAttribute(TfToken("inputs:uDegree"),SdfValueTypeNames->Int).Set(degree);
                            double parameter=n.type=="motionPath"?Scalar(n,"u","uValue",0):Scalar(n,"pr","parameter",0);if(n.type=="pointOnCurveInfo" && Scalar(n,"top","turnOnPercentage",0)!=0)parameter=knots[degree]+parameter*(knots[np]-knots[degree]);
                            reader.CreateAttribute(TfToken("inputs:u"),SdfValueTypeNames->Double).Set(parameter);reader.CreateAttribute(TfToken("inputs:axis"),SdfValueTypeNames->Int).Set(int(std::string("xyz").find(char(std::tolower(attr.back())))));
                            reader.CreateAttribute(TfToken("inputs:elementSize"),SdfValueTypeNames->Int).Set(layout.slots);reader.CreateAttribute(TfToken("inputs:jointIndices"),SdfValueTypeNames->IntArray).Set(Vt(layout.indices));reader.CreateAttribute(TfToken("inputs:jointWeights"),SdfValueTypeNames->FloatArray).Set(Vt(layout.weights));
                            reader.CreateRelationship(TfToken("rigExec:influences")).SetTargets(layout.providers);reader.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(layout.providers);
                            result={reader.CreateAttribute(TfToken("outputs:value"),SdfValueTypeNames->Double).GetPath(),0,layout.providers,true};translated.insert(node);
                        }
                    }
                }
            } else if(n.type=="locator" && (Starts(attr,"wp") || Starts(attr,"worldPosition")) && parent[node]>=0) {
                int axis=std::string("xyz").find(char(std::tolower(attr.back())));if(axis<0 || axis>2)axis=0;
                auto reader=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("worldPosition_"+std::to_string(serial++))),TfToken("RigExecMayaTranslation"));
                reader.CreateAttribute(TfToken("inputs:axis"),SdfValueTypeNames->Int).Set(axis);
                reader.CreateAttribute(TfToken("inputs:sourcePivot"),SdfValueTypeNames->Double3).Set(Pivot(parent[node]));
                reader.CreateAttribute(TfToken("inputs:parentPivot"),SdfValueTypeNames->Double3).Set(GfVec3d(0));
                reader.CreateRelationship(TfToken("rigExec:source")).SetTargets({dag[parent[node]]});result.providers={dag[parent[node]]};
                reader.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(result.providers);
                result.path=reader.CreateAttribute(TfToken("outputs:value"),SdfValueTypeNames->Double).GetPath();result.valid=true;translated.insert(node);
            } else if(n.type=="distanceBetween" && (attr=="d" || attr=="distance")) {
                int sources[2]={-1,-1};bool supported=true;std::vector<size_t> consumed;
                for(size_t j=0;j<links.size();++j) {
                    const auto &l=links[j];if(l.dst!=node)continue;
                    int slot=l.in=="im1" || l.in=="inMatrix1"?0:l.in=="im2" || l.in=="inMatrix2"?1:-1;
                    if(slot<0)continue;
                    if(l.src<0 || dag[l.src].IsEmpty() || !(l.out=="wm" || Starts(l.out,"wm[") || l.out=="worldMatrix" || Starts(l.out,"worldMatrix[")))supported=false;
                    else {sources[slot]=l.src;consumed.push_back(j);}
                }
                for(const auto &l:links)if(l.dst==node && (Starts(l.in,"p1") || Starts(l.in,"p2") || Starts(l.in,"point1") || Starts(l.in,"point2")))supported=false;
                if(supported) {
                    auto reader=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("distance_"+std::to_string(serial++))),TfToken("RigExecMayaTranslation"));
                    reader.CreateAttribute(TfToken("inputs:axis"),SdfValueTypeNames->Int).Set(-1);
                    for(int slot=0;slot<2;++slot) {
                        auto point=Vector(n,slot==0?"p1":"p2",slot==0?"point1":"point2");
                        GfVec3d pivot;
                        if(sources[slot]>=0) {
                            pivot=Pivot(sources[slot])-point;result.providers.push_back(dag[sources[slot]]);
                            reader.CreateRelationship(TfToken(slot==0?"rigExec:source":"rigExec:parent")).SetTargets({dag[sources[slot]]});
                        } else pivot=-Matrix(Attr(n,slot==0?"im1":"im2",slot==0?"inMatrix1":"inMatrix2")).Transform(point);
                        reader.CreateAttribute(TfToken(slot==0?"inputs:sourcePivot":"inputs:parentPivot"),SdfValueTypeNames->Double3).Set(pivot);
                    }
                    reader.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(result.providers);
                    result.path=reader.CreateAttribute(TfToken("outputs:value"),SdfValueTypeNames->Double).GetPath();result.valid=true;
                    for(auto j:consumed)links[j].used=true;translated.insert(node);
                }
            } else if(n.type=="plusMinusAverage" && (Starts(attr,"o1") || Starts(attr,"o2") || Starts(attr,"o3") || Starts(attr,"output"))) {
                int width=attr=="o1" || attr=="output1D"?1:Starts(attr,"o2") || Starts(attr,"output2D")?2:3;
                int axis=width==1?0:int(std::string("xyz").find(char(std::tolower(attr.back()))));
                if(axis>=0 && axis<width) {
                    std::string shortName="i"+std::to_string(width),longName="input"+std::to_string(width)+"D";
                    std::map<size_t,Value> values;
                    auto prefix=[&](const std::string &name) {return Starts(name,shortName+"[")?shortName:Starts(name,longName+"[")?longName:std::string();};
                    auto component=[&](const std::string &suffix) {return suffix=="."+shortName+"xyz"[axis] || suffix=="."+longName+"xyz"[axis];};
                    for(const auto &a:n.attributes) {
                        auto p=prefix(a.name);if(p.empty() || a.values.empty())continue;
                        auto range=Range(a.name,p);auto suffix=a.name.substr(a.name.find(']')+1);size_t count=range.second-range.first+1;
                        if(a.values.size()!=count*(suffix.empty()?width:1))throw std::runtime_error("invalid plusMinusAverage array: "+n.name+"."+a.name);
                        for(size_t slot=range.first;slot<=range.second;++slot) {
                            values.try_emplace(slot);
                            if(suffix.empty() || component(suffix))values[slot].value=Number(a.values[(slot-range.first)*(suffix.empty()?width:1)+(suffix.empty()?axis:0)]);
                        }
                    }
                    for(auto &l:links) {
                        auto p=prefix(l.in);if(l.dst!=node || l.src<0 || p.empty())continue;
                        auto range=Range(l.in,p);if(range.first!=range.second)throw std::runtime_error("indexed connection spans multiple elements");
                        auto suffix=l.in.substr(l.in.find(']')+1);values.try_emplace(range.first);
                        if(suffix.empty() || component(suffix)) {
                            auto from=l.out;if(suffix.empty() && width>1)from+=scene.nodes[l.src].type=="blendColors"?"rgb"[axis]:"xyz"[axis];
                            values[range.first]=output(l.src,from);if(values[range.first].valid)l.used=true;
                        }
                    }
                    int mode=int(Scalar(n,"op","operation",1));
                    if(mode>=0 && mode<=3) {
                        result=Value{};bool first=true;
                        for(const auto &entry:values) if(mode!=0) {
                            if(first) {result=entry.second;first=false;}
                            else result=math(mode==2?5:4,result,entry.second,Value{});
                        }
                        if(mode==3 && !values.empty())result=math(7,result,Value{{},double(values.size()),{},true},Value{});
                        if(result.valid)translated.insert(node);
                    }
                }
            } else if(n.type=="floatMath" && (attr=="of" || attr=="outFloat")) {
                int mode=int(Scalar(n,"_cnd","operation",0));
                if(mode>=0 && mode<=3) {
                    result=math(4+mode,input(node,"_fa","floatA",-1,0),input(node,"_fb","floatB",-1,0),Value{});
                    if(result.valid)translated.insert(node);
                }
            } else if(n.type=="remapValue" && (attr=="ov" || attr=="outValue")) {
                std::map<size_t,GfVec3d> entries{{0,{0,0,1}},{1,{1,1,1}}};bool supported=true;
                for(const auto &a:n.attributes) if(!a.values.empty() && (Starts(a.name,"vl[") || Starts(a.name,"value["))) {
                    const auto prefix=Starts(a.name,"vl[")?"vl":"value";auto range=Range(a.name,prefix);
                    const auto suffix=a.name.substr(a.name.find(']')+1);
                    if(suffix.empty()) {
                        if(a.values.size()!=3*(range.second-range.first+1))throw std::runtime_error("invalid remapValue ramp range");
                        for(size_t j=range.first;j<=range.second;++j) {size_t k=3*(j-range.first);entries[j]={Number(a.values[k]),Number(a.values[k+1]),Number(a.values[k+2])};}
                    } else {
                        int component=suffix==".vlp" || suffix==".value_Position"?0:suffix==".vlfv" || suffix==".value_FloatValue"?1:suffix==".vli" || suffix==".value_Interp"?2:-1;
                        if(component<0 || range.first!=range.second || a.values.size()!=1) {supported=false;continue;}
                        entries[range.first][component]=Number(a.values[0]);
                    }
                }
                for(const auto &l:links)if(l.dst==node && (Starts(l.in,"vl[") || Starts(l.in,"value[")))supported=false;
                std::vector<GfVec3d> ramp;for(const auto &e:entries)ramp.push_back(e.second);
                std::sort(ramp.begin(),ramp.end(),[](const auto &a,const auto &b){return a[0]<b[0];});
                for(size_t j=0;j<ramp.size();++j)if(ramp[j][2]<0 || ramp[j][2]>2 || std::floor(ramp[j][2])!=ramp[j][2] || (j && ramp[j][0]<=ramp[j-1][0]))supported=false;
                if(supported) {
                    auto lo=input(node,"imn","inputMin",-1,0),hi=input(node,"imx","inputMax",-1,1);
                    auto x=math(8,math(2,math(0,input(node,"i","inputValue",-1,0),Value{{},1,{},true},math(0,lo,Value{{},-1,{},true},Value{})),math(0,lo,Value{{},-1,{},true},hi),Value{}),Value{},Value{});
                    result=Value{{},ramp.front()[1],{},true};
                    for(size_t j=1;j<ramp.size();++j) {
                        auto t=math(8,math(0,x,Value{{},1/(ramp[j][0]-ramp[j-1][0]),{},true},Value{{},-ramp[j-1][0]/(ramp[j][0]-ramp[j-1][0]),{},true}),Value{},Value{});
                        if(ramp[j-1][2]==2)t=math(9,t,Value{},Value{});
                        if(ramp[j-1][2]==0)t=math(10,x,Value{{},ramp[j][0],{},true},Value{});
                        result=math(0,t,Value{{},ramp[j][1]-ramp[j-1][1],{},true},result);
                    }
                    auto omin=input(node,"omn","outputMin",-1,0),omax=input(node,"omx","outputMax",-1,1);
                    result=math(0,result,math(0,omin,Value{{},-1,{},true},omax),omin);translated.insert(node);
                }
            } else if(n.type=="pairBlend" && (Starts(attr,"ot") || Starts(attr,"outTranslate"))) {
                int axis=std::string("xyz").find(char(std::tolower(attr.back())));if(axis<0 || axis>2)axis=0;
                result=math(1,input(node,"it2","inTranslate2",axis,0),input(node,"it1","inTranslate1",axis,0),input(node,"w","weight",-1,0));translated.insert(node);
            } else if(n.type=="blendColors" && (attr=="opr" || attr=="opg" || attr=="opb" || Starts(attr,"output"))) {
                int axis=attr.back()=='r'||attr.back()=='R'?0:attr.back()=='g'||attr.back()=='G'?1:2;
                result=math(1,input(node,"c1","color1",axis,axis==0?1:0),input(node,"c2","color2",axis,axis==2?1:0),input(node,"b","blender",-1,0.5));
                translated.insert(node);
            } else if(n.type=="condition" && (Starts(attr,"oc") || Starts(attr,"outColor"))) {
                const int axis=int(std::string("rgb").find(char(std::tolower(attr.back()))));const int op=int(Scalar(n,"op","operation",0));
                if(axis>=0 && axis<3 && op>=0 && op<=5) {
                    auto test=math(11+op,input(node,"ft","firstTerm",-1,0),input(node,"st","secondTerm",-1,0),Value{});
                    result=math(17,math(1,input(node,"ct","colorIfTrue",axis,0),input(node,"cf","colorIfFalse",axis,1),test),Value{},Value{});
                    if(result.valid)translated.insert(node);
                }
            } else if(n.type=="multiplyDivide" && (Starts(attr,"o") || Starts(attr,"output"))) {
                int axis=std::string("xyz").find(char(std::tolower(attr.back())));if(axis<0 || axis>2)axis=0;
                int mode=int(Scalar(n,"op","operation",1));
                auto a=input(node,"i1","input1",axis,0),b=input(node,"i2","input2",axis,1);
                result=mode==0?a:math(mode==1?0:mode==2?2:3,a,b,Value{});translated.insert(node);
            } else if(n.type=="reverse" && (Starts(attr,"o") || Starts(attr,"output"))) {
                int axis=std::string("xyz").find(char(std::tolower(attr.back())));if(axis<0 || axis>2)axis=0;
                result=math(0,input(node,"i","input",axis,0),Value{{},-1,{},true},Value{{},1,{},true});translated.insert(node);
            } else if((n.type=="multDoubleLinear" || n.type=="addDoubleLinear") && (attr=="o" || attr=="output")) {
                auto a=input(node,"i1","input1",-1,0),b=input(node,"i2","input2",-1,n.type=="multDoubleLinear"?1:0);
                result=n.type=="multDoubleLinear"?math(0,a,b,Value{}):math(0,a,Value{{},1,{},true},b);translated.insert(node);
            } else if(n.type=="unitConversion" && (attr=="o" || attr=="output")) {
                result=math(0,input(node,"i","input",-1,0),input(node,"cf","conversionFactor",-1,1),Value{});translated.insert(node);
            } else if(!dag[node].IsEmpty() && Attr(n,attr)) {
                bool connected=false;
                for(auto &l:links)if(l.dst==node && l.src>=0 && l.in==attr) {result=output(l.src,l.out);connected=true;if(result.valid)l.used=true;break;}
                if(!connected) {
                    auto a=stage->GetPrimAtPath(dag[node]).CreateAttribute(TfToken("avars:maya_"+Name(attr)),SdfValueTypeNames->Double);
                    a.Set(Scalar(n,attr,{},0));result={a.GetPath(),0,{},true};
                }
            }
            active.erase(key);cache[key]=result;return result;
        };
        for(const auto &dest:scalarReads) {
            auto value=output(dest.node,dest.attribute);auto attr=stage->GetAttributeAtPath(dest.destination);
            if(!value.valid) {Warn("Maya scalar input requires translation: "+scene.nodes[dest.node].name+"."+dest.attribute);continue;}
            if(value.path.IsEmpty())attr.Set(value.value);else attr.SetConnections({value.path});
        }
        for(const auto &dest:scalarDestinations) {
            for(auto &l:links) if(l.dst==dest.node && l.src>=0 &&
                (l.in==dest.attribute || l.in==(dest.attribute=="en"?"envelope":"weight"+dest.attribute.substr(1)))) {
                auto value=output(l.src,l.out);
                if(!value.valid) {Warn("deformer weight driver requires translation: "+scene.nodes[l.src].name+"."+l.out);continue;}
                auto attr=stage->GetAttributeAtPath(dest.destination);
                if(dest.attribute=="en" && !value.path.IsEmpty()) {
                    // Exec value connections preserve their registered type.
                    // Deformer weights consume float rather than Maya's double channels.
                    value=math(17,value,Value{},Value{});
                    value.path=stage->GetPrimAtPath(value.path.GetPrimPath()).CreateAttribute(TfToken("outputs:floatValue"),SdfValueTypeNames->Float).GetPath();
                }
                if(value.path.IsEmpty())attr.Set(float(value.value));
                else if(dest.attribute=="en") {
                    auto mover=attr.GetPrim();SdfPathVector targets;mover.GetRelationship(TfToken("rigExec:moves")).GetTargets(&targets);
                    auto weight=rig.AddDynamicWeight("envelope_"+std::to_string(serial++),targets.at(0),{});
                    weight.SetRepresentation(TfToken("constant"));weight.SetRangePolicy(TfToken("clamp"));
                    weight.GetPrim().GetAttribute(TfToken("inputs:driver")).SetConnections({value.path});
                    mover.GetRelationship(TfToken("rigExec:weightObject")).SetTargets({weight.GetPath()});
                } else attr.SetConnections({value.path});
                l.used=true;
            }
        }
        for(auto &l:links) {
            if(l.src<0 || l.dst<0 || dag[l.dst].IsEmpty())continue;
            auto dest=canonical(l.in);if(dest.size()==1 && std::string("trs").find(dest[0])==std::string::npos)continue;
            if(dest.size()!=1 && !channel(dest))continue;
            // Constraints are already represented by movers, not channel wires.
            if(scene.nodes[l.src].type.find("Constraint")!=std::string::npos)continue;
            for(int axis=0;axis<(dest.size()==1?3:1);++axis) {
                auto from=l.out,to=dest;
                if(dest.size()==1) {from+=scene.nodes[l.src].type=="blendColors"?"rgb"[axis]:"xyz"[axis];to+="xyz"[axis];}
                auto value=output(l.src,from);if(!value.valid)continue;
                int component=std::string("xyz").find(to[1]);
                auto base=Vector(scene.nodes[l.dst],std::string(1,to[0]),to[0]=='t'?"translate":to[0]=='r'?"rotate":"scale",GfVec3d(to[0]=='s'?1:0))[component];
                if(to[0]=='r' && scene.angularUnit=="rad")base*=180.0/3.14159265358979323846;
                if(to[0]!='s' && base!=0)value=math(0,value,Value{{},1,{},true},Value{{},-base,{},true});
                if(to[0]=='s' && base!=1 && !absoluteScales.count({l.dst,to[1]})) {
                    if(base==0) {Warn("connected scale has zero static base: "+scene.nodes[l.dst].name);continue;}
                    value=math(2,value,Value{{},base,{},true},Value{});
                }
                auto target=stage->GetPrimAtPath(dag[l.dst]);auto a=target.GetAttribute(TfToken("avars:"+to));
                if(value.path.IsEmpty())a.Set(value.value);else a.SetConnections({value.path});
                auto deps=target.CreateRelationship(TfToken("maya:channelInputs"));SdfPathVector providers;deps.GetTargets(&providers);providers.insert(providers.end(),value.providers.begin(),value.providers.end());deps.SetTargets(providers);
                l.used=true;
            }
        }
    }
    void Run(bool metadataOnly,bool strict) {
        auto root=stage->GetPrimAtPath(SdfPath("/Rig")); stage->SetDefaultPrim(root);
        UsdGeomSetStageUpAxis(stage,TfToken(scene.zUp ? "Z" : "Y"));
        const std::map<std::string,double> units={{"mm",0.001},{"cm",0.01},{"m",1},{"km",1000},{"in",0.0254},{"ft",0.3048},{"yd",0.9144},{"mi",1609.344}};
        auto unit=units.find(scene.linearUnit); if(unit==units.end()) throw std::runtime_error("unknown linear unit: " + scene.linearUnit);
        UsdGeomSetStageMetersPerUnit(stage,unit->second);
        std::map<std::string,double> fps={{"game",15},{"film",24},{"pal",25},{"ntsc",30},{"show",48},{"palf",50},{"ntscf",60},{"sec",1},{"millisec",1000},{"min",1.0/60},{"hour",1.0/3600}};
        double rate=24;
        if(fps.count(scene.timeUnit)) rate=fps[scene.timeUnit];
        else if(scene.timeUnit.size()>3 && scene.timeUnit.substr(scene.timeUnit.size()-3)=="fps") rate=Number(scene.timeUnit.substr(0,scene.timeUnit.size()-3));
        else throw std::runtime_error("unknown time unit: " + scene.timeUnit);
        if(rate<=0) throw std::runtime_error("frame rate must be positive");
        stage->SetTimeCodesPerSecond(rate); stage->SetFramesPerSecond(rate);
        if(scene.angularUnit!="deg" && scene.angularUnit!="rad") throw std::runtime_error("unknown angular unit");
        if(metadataOnly) return;
        Graph();
        for(size_t i=0;i<scene.nodes.size();++i) {
            if(scene.nodes[i].type=="mesh") Mesh(static_cast<int>(i));
            else if(scene.nodes[i].type=="nurbsCurve") Curve(static_cast<int>(i));
            else if(scene.nodes[i].type=="nurbsSurface") Surface(static_cast<int>(i));
            else if(scene.nodes[i].type=="lattice") Lattice(static_cast<int>(i));
        }
        Materials();
        std::set<int> skinned;
        for(size_t i=0;i<scene.nodes.size();++i) if(scene.nodes[i].type=="skinCluster") Skin(static_cast<int>(i),skinned);
        GeometryHistories(skinned);
        for(const auto &entry:loftLayouts)skinned.insert(entry.first);
        SurfacePins();
        MatrixSpaces();
        Constraints();
        Ik();
        ChannelConnections();
        for(size_t i=0;i<geometry.size();++i) if(!geometry[i].IsEmpty() && !skinned.count(static_cast<int>(i)) && parent[i]>=0 && !dag[parent[i]].IsEmpty()) {
            auto mover=rig.NewMoverChain("shape_"+std::to_string(i),geometry[i].AppendProperty(TfToken("points"))).AddMatrixMover("follow",GeometryFollower(int(i)),{}, {},TfToken("final"));
            mover.SetReadPhase(TfToken("final"));
        }
        OrderMovers();
        for(size_t i=0;i<scene.references.size();++i) {
            const auto &ref=scene.references[i]; std::string asset=Asset(ref.asset);
            auto prim=stage->DefinePrim(SdfPath("/Rig/References").AppendChild(TfToken(Name(ref.nameSpace)+"_"+std::to_string(i))));
            prim.CreateAttribute(TfToken("maya:referenceAsset"),SdfValueTypeNames->Asset).Set(SdfAssetPath(asset));
            prim.SetCustomDataByKey(TfToken("maya:loaded"),VtValue(ref.loaded));
            if(!ref.loaded) Warn("external reference retained as dependency without translation: " + asset);
        }
        for(size_t i=0;i<scene.nodes.size();++i) if(!translated.count(static_cast<int>(i))) Warn("node retained without executable translation: " + scene.nodes[i].type+" "+full[i]);
        for(size_t i=0;i<links.size();++i) if(!links[i].used) {
            const auto &l=links[i];
            // Message links do not carry evaluation values.
            if(l.out=="msg" || l.out=="message") continue;
            Warn("connection retained without executable translation: " + scene.connections[i].source+" -> "+scene.connections[i].destination);
        }
        std::sort(scene.diagnostics.begin(),scene.diagnostics.end()); scene.diagnostics.erase(std::unique(scene.diagnostics.begin(),scene.diagnostics.end()),scene.diagnostics.end());
        std::sort(dependencies.begin(),dependencies.end()); dependencies.erase(std::unique(dependencies.begin(),dependencies.end()),dependencies.end());
        root.CreateAttribute(TfToken("maya:dependencies"),SdfValueTypeNames->AssetArray).Set(Vt(dependencies));
        root.CreateAttribute(TfToken("maya:diagnostics"),SdfValueTypeNames->StringArray).Set(Vt(scene.diagnostics));
        VtDictionary data; data["mayaRig:source"]=VtValue(source); data["mayaRig:complete"]=VtValue(scene.diagnostics.empty());
        Strings dependencyIds; for(const auto &asset:dependencies) dependencyIds.push_back(asset.GetAssetPath());
        data["mayaRig:dependencies"]=VtValue(Vt(dependencyIds));
        data["mayaRig:diagnostics"]=VtValue(Vt(scene.diagnostics)); stage->GetRootLayer()->SetCustomLayerData(data);
        if(!scene.diagnostics.empty()) {
            if(strict) throw std::runtime_error("strict translation rejected: " + scene.diagnostics.front());
            TF_WARN("Maya rig import has %zu diagnostics; inspect /Rig.maya:diagnostics",scene.diagnostics.size());
        }
    }
};
}
SdfLayerRefPtr Translate(Scene scene,const std::string &source,bool metadataOnly,bool strict) {
    Importer importer(std::move(scene),source); importer.Run(metadataOnly,strict); return importer.stage->GetRootLayer();
}
}
