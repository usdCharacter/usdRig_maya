#include "translator.h"
#include "rigExecRigging/rigBuilder.h"
#include "rigExecRigging/schemaAuthoring.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/nurbsCurves.h"
#include "pxr/usd/usdGeom/nurbsPatch.h"
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
    std::map<int,SkinLayout> skinLayouts;
    std::vector<SdfAssetPath> dependencies;
    explicit Importer(Scene s,std::string p):scene(std::move(s)),source(std::move(p)) {
        size_t n=scene.nodes.size(); parent.resize(n,-1); state.resize(n,0);
        full.resize(n); dag.resize(n); geometry.resize(n); world.resize(n,GfMatrix4d(1));
    }
    void Warn(const std::string &s) { scene.diagnostics.push_back(s); }
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
    std::string Asset(const std::string &s) {
        if (s.empty()) return s;
        auto id=ArGetResolver().CreateIdentifier(s,ArResolvedPath(source));
        dependencies.emplace_back(id);
        if (ArGetResolver().Resolve(id).empty()) Warn("unresolved asset dependency: " + id);
        return id;
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
            if (stage->GetPrimAtPath(dag[i])) throw std::runtime_error("duplicate DAG path: " + full[i]);
            auto schema=RigExecSchemaPrim::Define(stage,dag[i],TfToken(n.type=="joint" ? "RigExecJoint" : "RigExecControl"));
            schema.ApplyAPI(TfToken("NodeGraphNodeAPI"));
            if (n.type=="transform") schema.ApplyAPI(TfToken("RigExecControlAPI"));
            double angle=scene.angularUnit=="rad" ? 180.0/3.14159265358979323846 : 1;
            auto t=Vector(n,"t","translate"), r=Vector(n,"r","rotate")*angle;
            auto scale=Vector(n,"s","scale",GfVec3d(1));
            auto shear=Vector(n,"sh","shear");
            auto ra=Vector(n,"ra","rotateAxis")*angle, jo=Vector(n,"jo","jointOrient")*angle;
            auto rp=Vector(n,"rp","rotatePivot"), sp=Vector(n,"sp","scalePivot");
            GfMatrix4d sm(1); sm.SetScale(scale);
            GfMatrix4d sh(1); sh[1][0]=shear[0]; sh[2][0]=shear[1]; sh[2][1]=shear[2];
            GfMatrix4d local=Translation(-sp)*sm*sh*Translation(sp)*Translation(Vector(n,"spt","scalePivotTranslate"))*
                Translation(-rp)*Rotation(ra,0)*Rotation(r,static_cast<int>(Scalar(n,"ro","rotateOrder",0)))*
                Rotation(jo,0)*Translation(rp)*Translation(Vector(n,"rpt","rotatePivotTranslate"))*Translation(t);
            if (scale!=GfVec3d(1) || shear!=GfVec3d(0)) Warn("non-rigid bind transform retained; RigExec rest frames orthonormalize scale/shear: " + full[i]);
            if (Scalar(n,"it","inheritsTransform",1)==0) {
                Warn("inheritsTransform=false requires independent rig hierarchy: " + full[i]);
            }
            local*=Matrix(Attr(n,"opm","offsetParentMatrix"));
            world[i]=local*(parent[i]<0 ? GfMatrix4d(1) : world[parent[i]]);
            schema.SetAttribute(TfToken("rest:space"),VtValue(local));
            schema.GetPrim().SetCustomDataByKey(TfToken("maya:name"),VtValue(full[i]));
            schema.GetPrim().SetDisplayName(n.name);
            // Maya's curve shapes are the interaction surface. Generic
            // RigExec rings on every group/offset transform obscure it.
            if(n.type=="transform") schema.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
            UsdGeomImageable(schema.GetPrim()).CreatePurposeAttr().Set(TfToken("guide"));
            translated.insert(i);
        } else if (parent[i]>=0) world[i]=world[parent[i]];
        state[i]=2;
    }
    void Graph() {
        stage->DefinePrim(SdfPath("/Rig/Dag"),TfToken("Scope"));
        for (size_t i=0;i<scene.nodes.size();++i) BuildDag(static_cast<int>(i));
        for (const auto &c:scene.connections) {
            auto a=Plug(c.source),b=Plug(c.destination);
            links.push_back({Find(a.first),Find(b.first),a.second,b.second,false});
        }
        auto root=stage->GetPrimAtPath(SdfPath("/Rig"));
        Strings src,dst;
        for (const auto &c:scene.connections) { src.push_back(c.source); dst.push_back(c.destination); }
        root.CreateAttribute(TfToken("maya:connectionSources"),SdfValueTypeNames->StringArray).Set(Vt(src));
        root.CreateAttribute(TfToken("maya:connectionDestinations"),SdfValueTypeNames->StringArray).Set(Vt(dst));
        root.CreateAttribute(TfToken("maya:commands"),SdfValueTypeNames->StringArray).Set(Vt(scene.commands));
        for (size_t i=0;i<scene.nodes.size();++i) {
            const auto &n=scene.nodes[i];
            auto p=stage->DefinePrim(SdfPath("/Rig/Source/Nodes/n"+std::to_string(i)),TfToken("Scope"));
            p.SetCustomDataByKey(TfToken("maya:name"),VtValue(full[i]));
            p.SetCustomDataByKey(TfToken("maya:type"),VtValue(n.type));
            VtDictionary attributes;
            for (size_t j=0;j<n.attributes.size();++j) {
                const auto &a=n.attributes[j]; VtDictionary data;
                data["name"]=VtValue(a.name); data["type"]=VtValue(a.type); data["values"]=VtValue(Vt(a.values));
                attributes["a"+std::to_string(j)]=VtValue(data);
                static const std::set<std::string> assets={"ftn","fileTextureName","fn","filename","fileName","abc_File","cachePath","cacheName"};
                if (assets.count(a.name) && a.type=="string" && !a.values.empty())
                    p.CreateAttribute(TfToken("maya:asset"+std::to_string(j)),SdfValueTypeNames->Asset).Set(SdfAssetPath(Asset(a.values[0])));
            }
            p.SetCustomDataByKey(TfToken("maya:attributes"),VtValue(attributes));
            if (!dag[i].IsEmpty()) p.CreateRelationship(TfToken("maya:translated")).SetTargets({dag[i]});
        }
    }
    int InputMesh(int index,std::set<int> &visited) {
        if (!visited.insert(index).second) throw std::runtime_error("geometry dependency cycle");
        if (scene.nodes[index].type=="mesh" || scene.nodes[index].type=="nurbsSurface") return index;
        for (auto &l:links) if (l.dst==index && l.src>=0 &&
            (l.in=="ig" || l.in=="inputGeometry" || l.in=="input[0].inputGeometry" || l.in=="ip[0].ig")) {
            l.used=true;
            if (scene.nodes[index].type=="groupParts") translated.insert(index);
            return InputMesh(l.src,visited);
        }
        return -1;
    }
    int MeshData(int index) {
        for (auto &l:links) if (l.dst==index && l.src>=0 && (l.in=="i" || l.in=="inMesh")) {
            std::set<int> visited; int found=InputMesh(l.src,visited);
            if (found>=0) { l.used=true; return found; }
        }
        return index;
    }
    int InputSkin(int index,std::set<int> &visited) const {
        if(!visited.insert(index).second) throw std::runtime_error("skin input dependency cycle");
        if(scene.nodes[index].type=="skinCluster") return index;
        for(const auto &l:links) if(l.dst==index && l.src>=0 &&
            (l.in=="i" || l.in=="inMesh" || l.in=="cr" || l.in=="create" || l.in=="ig" || l.in=="inputGeometry" ||
             l.in=="input[0].inputGeometry" || l.in=="ip[0].ig"))
            return InputSkin(l.src,visited);
        return -1;
    }
    // Geometry filters may sit between a skin and its visible shape. Follow
    // only geometry plugs, preserving the output element, never message/set
    // connections. A nontrivial downstream filter remains a diagnostic.
    void SkinOutputs(int index,const std::string &output,std::set<int> &visiting,
                     std::set<int> &targets,std::vector<std::string> &filters) {
        if(!visiting.insert(index).second) throw std::runtime_error("skin output dependency cycle");
        for(auto &l:links) if(l.src==index && l.dst>=0 &&
            (output.empty() ? (l.out=="og" || l.out=="outputGeometry" || l.out=="og[0]" || l.out=="outputGeometry[0]") : l.out==output)) {
            const auto &next=scene.nodes[l.dst];
            if(((next.type=="mesh" && (l.in=="i" || l.in=="inMesh")) ||
                (next.type=="nurbsSurface" && (l.in=="cr" || l.in=="create"))) && !geometry[l.dst].IsEmpty()) {
                targets.insert(l.dst); l.used=true;
            } else if(next.type=="groupParts" && (l.in=="ig" || l.in=="inputGeometry")) {
                l.used=true; translated.insert(l.dst); SkinOutputs(l.dst,"",visiting,targets,filters);
            } else if(next.type=="deltaMush" && (l.in=="ip[0].ig" || l.in=="input[0].inputGeometry")) {
                // deltaMush preserves point indices, so its presence must not
                // erase the upstream skin binding. Its correction is not LBS.
                l.used=true;
                if(Scalar(next,"en","envelope",1)!=0) filters.push_back(next.name);
                else translated.insert(l.dst);
                SkinOutputs(l.dst,"",visiting,targets,filters);
            }
        }
        visiting.erase(index);
    }
    void Surface(int index) {
        const auto &node=scene.nodes[index];
        if(Scalar(node,"io","intermediateObject",0)!=0) {translated.insert(index);return;}
        int dataIndex=index;
        for(auto &l:links) if(l.dst==index && l.src>=0 && (l.in=="cr" || l.in=="create")) {
            std::set<int> seen; int found=InputMesh(l.src,seen);
            if(found>=0) {dataIndex=found;l.used=true;break;}
        }
        const auto &data=scene.nodes[dataIndex];auto a=Attr(data,"cc","cached");
        if(!a || a->type!="nurbsSurface") {Warn("surface has no stored NURBS data: "+full[index]);return;}
        size_t k=0; auto take=[&]() -> const std::string & {if(k>=a->values.size())throw std::runtime_error("truncated NURBS surface");return a->values[k++];};
        int ud=Integer(take()),vd=Integer(take()),uf=Integer(take()),vf=Integer(take());bool rational=Number(take())!=0;
        if(ud<1 || ud>16 || vd<1 || vd>16 || uf!=0 || vf!=0) {Warn("unsupported non-open NURBS surface: "+full[index]);return;}
        std::vector<double> uk,vk;size_t un=Count(take());for(size_t i=0;i<un;++i)uk.push_back(Number(take()));
        size_t vn=Count(take());for(size_t i=0;i<vn;++i)vk.push_back(Number(take()));
        if(un<size_t(ud+1) || vn<size_t(vd+1))throw std::runtime_error("invalid NURBS knot counts");
        un=un-ud+1;vn=vn-vd+1;
        uk.insert(uk.begin(),uk.front());uk.push_back(uk.back());vk.insert(vk.begin(),vk.front());vk.push_back(vk.back());
        if(Count(take())!=un*vn)throw std::runtime_error("NURBS CV count mismatch");
        GfMatrix4d gm=world[index];std::set<int> visited;int skin=InputSkin(index,visited);
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
    void SurfacePins() {
        for(size_t index=0;index<scene.nodes.size();++index) if(scene.nodes[index].type=="uvPin") {
            const auto &node=scene.nodes[index];int sourceSurface=-1;
            for(auto &l:links) if(l.dst==int(index) && l.src>=0 && (l.in=="curgeom" || l.in=="deformedGeometry")) {sourceSurface=l.src;l.used=true;}
            if(sourceSurface<0 || geometry[sourceSurface].IsEmpty() || !stage->GetPrimAtPath(geometry[sourceSurface]).IsA<UsdGeomNurbsPatch>()) {Warn("uvPin requires a translated NURBS surface: "+node.name);continue;}
            std::set<int> visited;int skin=InputSkin(sourceSurface,visited);
            if(skin<0 || !skinLayouts.count(skin) || skinLayouts[skin].method!=0) {Warn("uvPin requires a linear-skinned NURBS surface: "+node.name);continue;}
            if(Scalar(node,"novr","normalOverride",0)!=0) {Warn("uvPin normal override is unsupported: "+node.name);continue;}
            const auto &layout=skinLayouts[skin];
            auto surface=UsdGeomNurbsPatch(stage->GetPrimAtPath(geometry[sourceSurface]));
            VtVec3fArray raw;surface.GetPointsAttr().Get(&raw);std::vector<GfVec3d> points(raw.begin(),raw.end());
            auto coordinates=Array(node,{"coord","coordinate"},2);
            for(auto &l:links) if(l.src==int(index) && l.dst>=0 && !dag[l.dst].IsEmpty() && (Starts(l.out,"omat[") || Starts(l.out,"outputMatrix[")) && (l.in=="opm" || l.in=="offsetParentMatrix")) {
                size_t slot=Range(l.out,Starts(l.out,"omat[")?"omat":"outputMatrix").first;
                if(2*slot+1>=coordinates.size())throw std::runtime_error("uvPin coordinate index out of range");
                auto pin=stage->DefinePrim(SdfPath("/Rig/Mechanisms").AppendChild(TfToken(Name(node.name)+"_"+std::to_string(slot))),TfToken("RigExecNurbsSurfacePin"));
                auto set=[&](const char *name,const SdfValueTypeName &type,const auto &value){pin.CreateAttribute(TfToken(name),type).Set(value);};
                set("inputs:points",SdfValueTypeNames->Double3Array,Vt(points));
                auto copy=[&](const char *to,UsdAttribute from){VtValue value;from.Get(&value);pin.CreateAttribute(TfToken(to),from.GetTypeName()).Set(value);};
                copy("inputs:rationalWeights",surface.GetPointWeightsAttr());copy("inputs:uKnots",surface.GetUKnotsAttr());copy("inputs:vKnots",surface.GetVKnotsAttr());
                copy("inputs:uCount",surface.GetUVertexCountAttr());copy("inputs:vCount",surface.GetVVertexCountAttr());
                int uo=0,vo=0;surface.GetUOrderAttr().Get(&uo);surface.GetVOrderAttr().Get(&vo);
                set("inputs:uDegree",SdfValueTypeNames->Int,uo-1);set("inputs:vDegree",SdfValueTypeNames->Int,vo-1);
                GfVec2d ur,vr;surface.GetURangeAttr().Get(&ur);surface.GetVRangeAttr().Get(&vr);
                bool normalized=Scalar(node,"nrmip","normalizedIsoParms",1)!=0;
                set("inputs:u",SdfValueTypeNames->Double,normalized ? ur[0]+coordinates[2*slot]*(ur[1]-ur[0]) : coordinates[2*slot]);
                set("inputs:v",SdfValueTypeNames->Double,normalized ? vr[0]+coordinates[2*slot+1]*(vr[1]-vr[0]) : coordinates[2*slot+1]);
                set("inputs:normalAxis",SdfValueTypeNames->Int,int(Scalar(node,"nrm","normalAxis",0)));
                set("inputs:tangentAxis",SdfValueTypeNames->Int,int(Scalar(node,"tng","tangentAxis",2)));
                set("inputs:jointIndices",SdfValueTypeNames->IntArray,Vt(layout.indices));set("inputs:jointWeights",SdfValueTypeNames->FloatArray,Vt(layout.weights));
                set("inputs:elementSize",SdfValueTypeNames->Int,layout.slots);
                GfMatrix4d local(1);stage->GetPrimAtPath(dag[l.dst]).GetAttribute(TfToken("rest:space")).Get(&local);
                set("inputs:local",SdfValueTypeNames->Matrix4d,local);
                pin.CreateRelationship(TfToken("rigExec:influences")).SetTargets(layout.providers);
                if(parent[l.dst]>=0)pin.CreateRelationship(TfToken("rigExec:parent")).SetTargets({dag[parent[l.dst]]});
                SdfPathVector poseInputs(layout.providers.begin(),layout.providers.end());
                if(parent[l.dst]>=0)poseInputs.push_back(dag[parent[l.dst]]);
                pin.CreateRelationship(TfToken("rigExec:poseInputs")).SetTargets(poseInputs);
                auto output=pin.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);
                stage->GetPrimAtPath(dag[l.dst]).GetAttribute(TfToken("posed:space")).SetConnections({output.GetPath()});
                l.used=true;translated.insert(int(index));
            }
        }
    }
    void Mesh(int index) {
        const auto &node=scene.nodes[index];
        if (Scalar(node,"io","intermediateObject",0)!=0) { translated.insert(index); return; }
        const auto &data=scene.nodes[MeshData(index)];
        auto vertices=Array(data,{"vt","vrts"},3),edges=Array(data,{"ed","edge"},3);
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
        std::set<int> inputVisited; int inputSkin=InputSkin(index,inputVisited);
        if(inputSkin>=0) geometryMatrix=Matrix(Attr(scene.nodes[inputSkin],"gm","geomMatrix"),geometryMatrix);
        std::vector<GfVec3f> points;
        for (size_t i=0;i<vertices.size();i+=3) points.emplace_back(geometryMatrix.Transform(GfVec3d(vertices[i],vertices[i+1],vertices[i+2])));
        std::vector<Edge> es;
        for (size_t i=0;i<edges.size();i+=3) {
            if (edges[i]!=std::floor(edges[i]) || edges[i+1]!=std::floor(edges[i+1]) || edges[i]<0 || edges[i+1]<0 ||
                edges[i]>=points.size() || edges[i+1]>=points.size()) throw std::runtime_error("invalid mesh edge: " + data.name);
            es.push_back({static_cast<int>(edges[i]),static_cast<int>(edges[i+1])});
        }
        std::map<size_t,Face> faces;
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
        if (Scalar(node,"v","visibility",1)==0 || (parent[index]>=0 && Scalar(scene.nodes[parent[index]],"v","visibility",1)==0))
            mesh.CreateVisibilityAttr().Set(TfToken("invisible"));
        auto uvs=Array(data,{"uvst[0].uvsp","uvSet[0].uvSetPoints"},2);
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
    void Curve(int index) {
        const auto &n=scene.nodes[index]; auto a=Attr(n,"cc","cached");
        if (!a || a->type!="nurbsCurve") { Warn("curve has no cached nurbsCurve data: " + n.name); return; }
        size_t k=0; auto take=[&]() -> const std::string & { if(k>=a->values.size()) throw std::runtime_error("truncated nurbsCurve"); return a->values[k++]; };
        int degree=Integer(take()); Count(take()); int form=Integer(take()); bool rational=Number(take())!=0; int dimension=Integer(take());
        if (degree<1 || degree>16 || dimension!=3) throw std::runtime_error("unsupported NURBS degree/dimension");
        size_t nk=Count(take()); std::vector<double> knots;
        for(size_t j=0;j<nk;++j) knots.push_back(Number(take()));
        size_t np=Count(take()); std::vector<GfVec3f> points; std::vector<double> weights;
        for(size_t j=0;j<np;++j) {
            double x=Number(take()),y=Number(take()),z=Number(take());
            points.emplace_back(world[index].Transform(GfVec3d(x,y,z)));
            if(rational) weights.push_back(Number(take()));
        }
        if(k!=a->values.size() || nk!=np+degree-1 || nk<2 || !std::is_sorted(knots.begin(),knots.end())) throw std::runtime_error("invalid NURBS knot/CV count");
        double first=knots.front(),last=knots.back();
        knots.insert(knots.begin(),form==2 ? first-(knots[1]-first) : first);
        knots.push_back(form==2 ? last+(last-knots[knots.size()-2]) : last);
        auto path=SdfPath("/Rig/Geometry").AppendChild(TfToken(Name(full[index])));
        auto curve=UsdGeomNurbsCurves::Define(stage,path); geometry[index]=path;
        curve.GetPrim().SetDisplayName(n.name);
        curve.GetPrim().SetCustomDataByKey(TfToken("maya:name"),VtValue(full[index]));
        if(parent[index]>=0 && !dag[parent[index]].IsEmpty()) {
            curve.GetPrim().CreateRelationship(TfToken("maya:control")).SetTargets({dag[parent[index]]});
            stage->GetPrimAtPath(dag[parent[index]]).CreateRelationship(TfToken("maya:controlShapes")).AddTarget(path);
        }
        curve.CreatePointsAttr().Set(Vt(points)); curve.CreateCurveVertexCountsAttr().Set(VtIntArray{static_cast<int>(np)});
        curve.CreateOrderAttr().Set(VtIntArray{degree+1}); curve.CreateKnotsAttr().Set(Vt(knots));
        curve.CreateRangesAttr().Set(VtVec2dArray{GfVec2d(knots[degree],knots[np])});
        if(rational) curve.CreatePointWeightsAttr().Set(Vt(weights));
        curve.CreateWidthsAttr().Set(VtFloatArray{0.05f}); curve.SetWidthsInterpolation(TfToken("constant"));
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
            auto c=standard ? Vector(n,"baseColor","baseColor",GfVec3d(0.8)) : Vector(n,"c","color",GfVec3d(0.5));
            shader.CreateInput(TfToken("diffuseColor"),SdfValueTypeNames->Color3f).Set(GfVec3f(c));
            float opacity=standard ? static_cast<float>(Vector(n,"opacity","opacity",GfVec3d(1))[0]) : 1-static_cast<float>(Vector(n,"it","transparency")[0]);
            shader.CreateInput(TfToken("opacity"),SdfValueTypeNames->Float).Set(opacity);
            float roughness=standard ? static_cast<float>(Scalar(n,"specularRoughness","specularRoughness",0.2)) : n.type=="blinn" ? static_cast<float>(Scalar(n,"ec","eccentricity",0.3)) : n.type=="phong" ? static_cast<float>(std::sqrt(2/(Scalar(n,"cp","cosinePower",20)+2))) : 1;
            shader.CreateInput(TfToken("roughness"),SdfValueTypeNames->Float).Set(std::clamp(roughness,0.f,1.f));
            shader.CreateInput(TfToken("metallic"),SdfValueTypeNames->Float).Set(static_cast<float>(Scalar(n,"metalness","metalness",0)));
            shader.CreateOutput(TfToken("surface"),SdfValueTypeNames->Token);
            mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),TfToken("surface"));
            materials[static_cast<int>(i)]=mat; translated.insert(static_cast<int>(i));
            for(auto &l:links) if(l.dst==i && l.src>=0 && (l.in=="c" || l.in=="color" || l.in=="baseColor") && scene.nodes[l.src].type=="file" && (l.out=="oc" || l.out=="outColor")) {
                auto file=Attr(scene.nodes[l.src],"ftn","fileTextureName");
                if(!file || file->values.empty()) continue;
                auto texture=UsdShadeShader::Define(stage,mat.GetPath().AppendChild(TfToken("Texture")));
                texture.CreateIdAttr().Set(TfToken("UsdUVTexture"));
                texture.CreateInput(TfToken("file"),SdfValueTypeNames->Asset).Set(SdfAssetPath(Asset(file->values[0])));
                std::string colorSpace="auto";
                if(auto cs=Attr(scene.nodes[l.src],"cs","colorSpace")) {
                    if(cs->values[0]=="sRGB") colorSpace="sRGB";
                    else if(cs->values[0]=="Raw") colorSpace="raw";
                    else Warn("texture color space approximated as auto: " + cs->values[0]);
                }
                texture.CreateInput(TfToken("sourceColorSpace"),SdfValueTypeNames->Token).Set(TfToken(colorSpace));
                auto reader=UsdShadeShader::Define(stage,mat.GetPath().AppendChild(TfToken("UV")));
                reader.CreateIdAttr().Set(TfToken("UsdPrimvarReader_float2"));
                reader.CreateInput(TfToken("varname"),SdfValueTypeNames->Token).Set(TfToken("st"));
                reader.CreateOutput(TfToken("result"),SdfValueTypeNames->Float2);
                texture.CreateInput(TfToken("st"),SdfValueTypeNames->Float2).ConnectToSource(reader.ConnectableAPI(),TfToken("result"));
                texture.CreateOutput(TfToken("rgb"),SdfValueTypeNames->Float3);
                shader.GetInput(TfToken("diffuseColor")).ConnectToSource(texture.ConnectableAPI(),TfToken("rgb"));
                translated.insert(l.src); l.used=true;
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
                        for(const auto &input:links) if(input.dst==l.src && input.src>=0 && (input.in=="i" || input.in=="inMesh"))
                            InputMesh(input.src,history);
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
    void Constraints() {
        for(size_t i=0;i<scene.nodes.size();++i) {
            const auto &n=scene.nodes[i];
            if(n.type!="parentConstraint" && n.type!="pointConstraint" && n.type!="orientConstraint" && n.type!="scaleConstraint") continue;
            int target=-1; bool t[3]={false,false,false},r[3]={false,false,false},s[3]={false,false,false};
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
                paths.push_back(dag[entry.second]); std::string a="tg["+std::to_string(entry.first)+"]",b="target["+std::to_string(entry.first)+"]";
                weights.push_back(static_cast<float>(Scalar(n,a+".tw",b+".targetWeight",1)));
                offsetsT.push_back(Vector(n,a+".tot",b+".targetOffsetTranslate"));
                offsetsR.push_back(Vector(n,a+".tor",b+".targetOffsetRotate")*(scene.angularUnit=="rad" ? 180.0/3.14159265358979323846 : 1));
            }
            auto chain=rig.NewMoverChain("constraint_"+std::to_string(i),dag[target]);
            rigExec::RigExecSourceConstraintHandle handle;
            GfVec3d offset=Vector(n,"o","offset");
            if(n.type=="parentConstraint") {
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
        SkinOutputs(index,"",visiting,targets,filters);
        if(targets.size()!=1) { Warn("skinCluster needs one supported downstream mesh output: " + node.name); return; }
        int target=*targets.begin(); VtVec3fArray points; UsdGeomPointBased(stage->GetPrimAtPath(geometry[target])).GetPointsAttr().Get(&points);
        std::map<int,int> remap; std::vector<SdfPath> providers;
        for(const auto &pair:influences) {
            int logical=pair.first,src=pair.second; remap[logical]=static_cast<int>(providers.size());
            auto bindAttr=Attr(node,"pm["+std::to_string(logical)+"]","bindPreMatrix["+std::to_string(logical)+"]");
            GfMatrix4d inverseBind=Matrix(bindAttr,world[src].GetInverse());
            if(std::abs(inverseBind.GetDeterminant())<1e-12) throw std::runtime_error("singular skin bindPreMatrix");
            auto bindPath=SdfPath("/Rig/Bindings").AppendChild(TfToken(Name(node.name)+"_"+std::to_string(logical)));
            auto provider=RigExecSchemaPrim::Define(stage,bindPath,TfToken("RigExecJoint"));
            provider.SetAttribute(TfToken("rest:space"),VtValue(inverseBind.GetInverse()));
            // The proxy keeps the actual bind matrix while a parent constraint
            // follows the saved/current DAG frame, including non-bind poses.
            auto constraint=rig.NewMoverChain("bind_"+std::to_string(index)+"_"+std::to_string(logical)).AddParentConstraint("follow",bindPath);
            constraint.SetSources({dag[src]});
            constraint.SetAffectScale(true,true,true);
            constraint.SetReadPhase(TfToken("rigExec:sources"),"final");
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
        mover.SetReadPhase(TfToken("final"));
        mover.SetJointInfluences(ids,values,slots); mover.SetSkinningMethod(TfToken(method==0 ? "classicLinear" : "dualQuaternion"));
        mover.SetDefaultWeight(static_cast<float>(Scalar(node,"en","envelope",1)));
        std::vector<std::string> unsupported;
        TfTokenVector stack{TfToken("skin")};
        for(const auto &name:filters) {
            int f=Find(name);const auto &filter=scene.nodes[f];
            if(!stage->GetPrimAtPath(geometry[target]).IsA<UsdGeomMesh>() || Scalar(filter,"sa","smoothingAlgorithm",0)!=0 ||
               Scalar(filter,"iwc","inwardConstraint",0)!=0 || Scalar(filter,"owc","outwardConstraint",0)!=0 ||
               Vector(filter,"s","scale",GfVec3d(1))!=GfVec3d(1)) {
                unsupported.push_back(name);Warn("unsupported deltaMush algorithm/contour/scale setting: "+name);continue;
            }
            auto mush=chain.AddDeltaMushMover("deltaMush_"+std::to_string(f));
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
                    SdfPathVector connections;auto posed=prim.GetAttribute(TfToken("posed:space"));if(posed)posed.GetConnections(&connections);
                    for(const auto &connection:connections) {
                        auto mechanism=stage->GetPrimAtPath(connection.GetPrimPath());
                        if(mechanism && mechanism.GetTypeName()==TfToken("RigExecNurbsSurfacePin")) {
                            SdfPathVector providers;mechanism.GetRelationship(TfToken("rigExec:influences")).GetTargets(&providers);
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
        // Identity local rest frames let direct wires become avar connections.
        // Each node still composes through its own namespace parent.
        const std::map<std::string,std::string> names={
            {"translate","t"},{"rotate","r"},{"scale","s"},
            {"translateX","tx"},{"translateY","ty"},{"translateZ","tz"},
            {"rotateX","rx"},{"rotateY","ry"},{"rotateZ","rz"},
            {"scaleX","sx"},{"scaleY","sy"},{"scaleZ","sz"}};
        auto canonical=[&](std::string s) { auto it=names.find(s); return it==names.end() ? s : it->second; };
        auto identity=[&](int i) {
            GfMatrix4d m(1); stage->GetPrimAtPath(dag[i]).GetAttribute(TfToken("rest:space")).Get(&m);
            for(int row=0;row<4;++row) for(int col=0;col<4;++col)
                if(std::abs(m[row][col]-(row==col ? 1.0 : 0.0))>1e-10) return false;
            return true;
        };
        for(auto &l:links) {
            if(l.src<0 || l.dst<0 || dag[l.src].IsEmpty() || dag[l.dst].IsEmpty()) continue;
            auto a=canonical(l.out),b=canonical(l.in);
            auto channel=[](const std::string &c) {
                return (c.size()==1 || (c.size()==2 && std::string("xyz").find(c[1])!=std::string::npos)) && std::string("trs").find(c[0])!=std::string::npos;
            };
            if(!channel(a) || !channel(b) || a.size()!=b.size()) continue;
            if(!identity(l.src) || !identity(l.dst)) { Warn("direct channel connection needs identity local rest frames: "+scene.nodes[l.src].name+" -> "+scene.nodes[l.dst].name); continue; }
            auto src=stage->GetPrimAtPath(dag[l.src]),dst=stage->GetPrimAtPath(dag[l.dst]);
            for(char axis:std::string(a.size()==1 ? "xyz" : " ")) {
                const std::string from="avars:"+a+(a.size()==1 ? std::string(1,axis) : "");
                const std::string to="avars:"+b+(b.size()==1 ? std::string(1,axis) : "");
                dst.GetAttribute(TfToken(to)).SetConnections({src.GetPath().AppendProperty(TfToken(from))});
            }
            l.used=true;
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
        ChannelConnections();
        for(size_t i=0;i<scene.nodes.size();++i) {
            if(scene.nodes[i].type=="mesh") Mesh(static_cast<int>(i));
            else if(scene.nodes[i].type=="nurbsCurve") Curve(static_cast<int>(i));
            else if(scene.nodes[i].type=="nurbsSurface") Surface(static_cast<int>(i));
        }
        Materials();
        std::set<int> skinned;
        for(size_t i=0;i<scene.nodes.size();++i) if(scene.nodes[i].type=="skinCluster") Skin(static_cast<int>(i),skinned);
        SurfacePins();
        Constraints();
        Ik();
        for(size_t i=0;i<geometry.size();++i) if(!geometry[i].IsEmpty() && !skinned.count(static_cast<int>(i)) && parent[i]>=0 && !dag[parent[i]].IsEmpty()) {
            auto mover=rig.NewMoverChain("shape_"+std::to_string(i),geometry[i].AppendProperty(TfToken("points"))).AddMatrixMover("follow",dag[parent[i]],{}, {},TfToken("final"));
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
