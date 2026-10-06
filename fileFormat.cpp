#include "fileFormat.h"
#include "translator.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/usd/ar/asset.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/sdf/layer.h"
#include <stdexcept>
#include <set>
PXR_NAMESPACE_OPEN_SCOPE
namespace {
// Maya reference files are parsed as data and namespaced into this layer.
// A stack of resolved identifiers prevents cycles independently of USD's layer
// registry; no recursive UsdStage::Open or MEL execution occurs during a read.
std::string Namespace(const std::string &name,const std::string &ns) {
    if(name.empty()) return name;
    std::string result; size_t start=0;
    while(start<name.size()) {
        if(name[start]=='|') { result+='|'; ++start; continue; }
        auto end=name.find('|',start);
        result+=ns+":"+name.substr(start,end==std::string::npos ? end : end-start);
        if(end==std::string::npos) break;
        start=end;
    }
    return result;
}
usdMayaRig::Scene Load(const std::string &data,const std::string &source,std::set<std::string> &stack) {
    if(stack.size()>64) throw std::runtime_error("Maya reference depth exceeds 64");
    auto scene=usdMayaRig::Parse(data);
    const size_t count=scene.references.size();
    for(size_t i=0;i<count;++i) {
        auto ref=scene.references[i];
        std::string id=ArGetResolver().CreateIdentifier(ref.asset,ArResolvedPath(source));
        scene.references[i].asset=id;
        auto resolved=ArGetResolver().Resolve(id);
        if(resolved.empty()) { scene.diagnostics.push_back("missing Maya reference: "+id); continue; }
        if(!stack.insert(resolved.GetPathString()).second) throw std::runtime_error("cyclic Maya reference: "+id);
        auto asset=ArGetResolver().OpenAsset(resolved);
        if(!asset) throw std::runtime_error("cannot read Maya reference: "+id);
        auto bytes=asset->GetBuffer();
        if(!bytes) throw std::runtime_error("cannot buffer Maya reference: "+id);
        std::string text(bytes.get(),asset->GetSize());
        if(text.substr(0,256).find("//Maya ASCII")==std::string::npos) {
            scene.diagnostics.push_back("non-ASCII reference retained as dependency: "+id);
            stack.erase(resolved.GetPathString()); continue;
        }
        auto child=Load(text,resolved.GetPathString(),stack); stack.erase(resolved.GetPathString());
        if(child.linearUnit!=scene.linearUnit || child.angularUnit!=scene.angularUnit || child.zUp!=scene.zUp) {
            scene.diagnostics.push_back("reference units/up-axis differ; dependency not merged: "+id); continue;
        }
        std::string ns=ref.nameSpace.empty() ? "reference"+std::to_string(i) : ref.nameSpace;
        for(auto &n:child.nodes) {
            n.name=Namespace(n.name,ns); n.parent=Namespace(n.parent,ns);
            static const std::set<std::string> assets={"ftn","fileTextureName","fn","filename","fileName","abc_File","cachePath","cacheName"};
            for(auto &a:n.attributes) if(assets.count(a.name) && a.type=="string" && !a.values.empty())
                a.values[0]=ArGetResolver().CreateIdentifier(a.values[0],resolved);
            scene.nodes.push_back(std::move(n));
        }
        for(auto c:child.connections) {
            auto qualify=[&](const std::string &plug) { auto dot=plug.find('.'); return Namespace(plug.substr(0,dot),ns)+plug.substr(dot); };
            c.source=qualify(c.source); c.destination=qualify(c.destination); scene.connections.push_back(std::move(c));
        }
        for(auto r:child.references) { r.nameSpace=Namespace(r.nameSpace,ns); scene.references.push_back(std::move(r)); }
        scene.commands.insert(scene.commands.end(),child.commands.begin(),child.commands.end());
        scene.diagnostics.insert(scene.diagnostics.end(),child.diagnostics.begin(),child.diagnostics.end());
        scene.references[i].loaded=true;
    }
    return scene;
}
}
TF_REGISTRY_FUNCTION(TfType) { SDF_DEFINE_FILE_FORMAT(UsdMayaRigFileFormat, SdfFileFormat); }
UsdMayaRigFileFormat::UsdMayaRigFileFormat()
    : SdfFileFormat(TfToken("mayaRig"), TfToken("1.0"), TfToken("usd"), TfToken("ma")) {}
bool UsdMayaRigFileFormat::CanRead(const std::string &path) const {
    auto asset = ArGetResolver().OpenAsset(ArResolvedPath(path));
    if (!asset) return false;
    char header[256] = {};
    const size_t n = asset->Read(header, sizeof(header), 0);
    return std::string(header,n).find("//Maya ASCII") != std::string::npos;
}
bool UsdMayaRigFileFormat::_Read(SdfLayer *layer, const std::string &data,
                               const std::string &source, bool metadataOnly) const {
    try {
        bool strict = false;
        for (const auto &arg : layer->GetFileFormatArguments()) {
            if (arg.first != "strict" || (arg.second != "0" && arg.second != "1"))
                throw std::runtime_error("supported argument: strict=0|1");
            strict = arg.second == "1";
        }
        std::set<std::string> stack;
        if(!source.empty()) stack.insert(source);
        auto scene=metadataOnly ? usdMayaRig::Parse(data) : Load(data,source,stack);
        auto converted = usdMayaRig::Translate(std::move(scene),source,metadataOnly,strict);
        layer->TransferContent(converted);
        return true;
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("Maya ASCII translation failed for %s: %s", source.c_str(),e.what());
        return false;
    }
}
bool UsdMayaRigFileFormat::Read(SdfLayer *layer, const std::string &path, bool metadataOnly) const {
    auto asset = ArGetResolver().OpenAsset(ArResolvedPath(path));
    if (!asset) { TF_RUNTIME_ERROR("Cannot open Maya ASCII asset %s", path.c_str()); return false; }
    auto buffer = asset->GetBuffer();
    if (!buffer && asset->GetSize()) return false;
    return _Read(layer,std::string(buffer ? buffer.get() : "",asset->GetSize()),path,metadataOnly);
}
bool UsdMayaRigFileFormat::ReadFromString(SdfLayer *layer, const std::string &data) const {
    return _Read(layer,data,{},false);
}
bool UsdMayaRigFileFormat::WriteToString(const SdfLayer &layer, std::string *text,
                                       const std::string &comment) const {
    return SdfFileFormat::FindById(TfToken("usda"))->WriteToString(layer,text,comment);
}
bool UsdMayaRigFileFormat::WriteToStream(const SdfSpecHandle &spec, std::ostream &out, size_t indent) const {
    return SdfFileFormat::FindById(TfToken("usda"))->WriteToStream(spec,out,indent);
}
std::set<std::string> UsdMayaRigFileFormat::GetExternalAssetDependencies(const SdfLayer &layer) const {
    std::set<std::string> result;
    auto data=layer.GetCustomLayerData(); auto it=data.find("mayaRig:dependencies");
    if(it!=data.end() && it->second.IsHolding<VtStringArray>())
        for(const auto &id:it->second.UncheckedGet<VtStringArray>()) {
            auto resolved=ArGetResolver().Resolve(id);
            if(!resolved.empty()) result.insert(resolved.GetPathString());
        }
    return result;
}
PXR_NAMESPACE_CLOSE_SCOPE
