#include "parser.h"
#include <cctype>
#include <stdexcept>
#include <set>

namespace usdMayaRig {
namespace {
using Words = std::vector<std::string>;
std::string Option(const Words &w, const std::string &a, const std::string &b = {}) {
    for (size_t i = 1; i + 1 < w.size(); ++i)
        if (w[i] == a || (!b.empty() && w[i] == b)) return w[i+1];
    return {};
}
bool Flag(const Words &w, const std::string &a, const std::string &b = {}) {
    for (const auto &s : w) if (s == a || (!b.empty() && s == b)) return true;
    return false;
}
bool IsFlag(const std::string &s) {
    return s.size() > 1 && s[0] == '-' && std::isalpha(static_cast<unsigned char>(s[1]));
}
}
Scene Parse(const std::string &text) {
    Scene scene;
    std::vector<Words> commands;
    Words words;
    std::string token;
    bool quoted = false, active = false;
    auto flush = [&] { if (active) { words.push_back(token); token.clear(); active = false; } };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') quoted = false;
            else if (c == '\\' && i + 1 < text.size()) {
                char n = text[++i];
                if (n == '"' || n == '\\') token += n;
                else if (n == 'n') token += '\n';
                else if (n == 'r') token += '\r';
                else if (n == 't') token += '\t';
                else { token += '\\'; token += n; }
            } else token += c;
        } else if (c == '"') { quoted = true; active = true; }
        else if (c == '/' && i + 1 < text.size() && text[i+1] == '/') {
            flush(); while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i+1] == '*') {
            flush(); auto end = text.find("*/", i+2);
            if (end == std::string::npos) throw std::runtime_error("unterminated block comment");
            i = end+1;
        } else if (c == ';') { flush(); if (!words.empty()) commands.push_back(words); words.clear(); }
        else if (std::isspace(static_cast<unsigned char>(c))) flush();
        else { token += c; active = true; }
    }
    flush();
    if (quoted || !words.empty()) throw std::runtime_error("unterminated Maya ASCII command/string");
    int current = -1;
    auto findNode = [&](const std::string &name) {
        int found = -1;
        const std::string clean=(!name.empty() && name[0]==':') ? name.substr(1) : name;
        for (size_t i = 0; i < scene.nodes.size(); ++i) if (scene.nodes[i].name == clean) {
            if (found >= 0) throw std::runtime_error("ambiguous node name: " + name);
            found = static_cast<int>(i);
        }
        return found;
    };
    auto ensureDefault = [&](const std::string &name) {
        const std::string clean=(!name.empty() && name[0]==':') ? name.substr(1) : name;
        int found=findNode(clean);
        if(found<0 && (clean=="lambert1" || clean=="initialShadingGroup")) {
            Node n; n.name=clean; n.type=clean=="lambert1" ? "lambert" : "shadingEngine";
            scene.nodes.push_back(n); found=static_cast<int>(scene.nodes.size()-1);
        }
        return found;
    };
    for (const auto &w : commands) {
        if (w[0] == "createNode") {
            if (w.size() < 2) throw std::runtime_error("createNode missing type");
            Node n; n.type = w[1]; n.name = Option(w, "-n", "-name");
            n.parent = Option(w, "-p", "-parent");
            if (n.name.empty()) throw std::runtime_error("createNode missing explicit name");
            scene.nodes.push_back(n); current = static_cast<int>(scene.nodes.size()-1);
        } else if (w[0] == "select") {
            current = -1;
            if (!Flag(w, "-cl", "-clear")) for (size_t i = 1; i < w.size(); ++i)
                if (!IsFlag(w[i])) current = ensureDefault(w[i]);
        } else if (w[0] == "setAttr") {
            Attribute a;
            for (size_t i = 1; i < w.size(); ++i) {
                if (!a.values.empty() || (a.type=="string" && !a.name.empty())) a.values.push_back(w[i]);
                else if (IsFlag(w[i])) {
                    if (w[i] == "-type" || w[i] == "-typ") {
                        if (++i >= w.size()) throw std::runtime_error("setAttr missing type");
                        a.type = w[i];
                    } else if (w[i] == "-ch" || w[i] == "-caching" || w[i] == "-k" || w[i] == "-keyable" ||
                        w[i] == "-l" || w[i] == "-lock" || w[i] == "-cb" || w[i] == "-channelBox" ||
                        w[i] == "-s" || w[i] == "-size" || w[i] == "-capacityHint" || w[i] == "-ca") {
                        if (++i >= w.size()) throw std::runtime_error("setAttr missing flag value");
                    } else if (w[i] != "-av" && w[i] != "-alteredValue" && w[i] != "-clamp") {
                        throw std::runtime_error("unsupported setAttr flag: " + w[i]);
                    }
                } else if (a.name.empty()) a.name = w[i];
                else a.values.push_back(w[i]);
            }
            if (a.name.empty()) throw std::runtime_error("setAttr missing attribute");
            int target = current;
            if (a.name[0] == '.') a.name.erase(0,1);
            else {
                auto dot = a.name.find('.');
                if (dot == std::string::npos) throw std::runtime_error("setAttr missing node.attribute");
                target = findNode(a.name.substr(0,dot)); a.name.erase(0,dot+1);
            }
            if (target >= 0) scene.nodes[target].attributes.push_back(std::move(a));
            else if (!a.values.empty()) scene.diagnostics.push_back("setAttr on external/default node: " + a.name);
        } else if (w[0] == "connectAttr") {
            Words plugs;
            for (size_t i=1; i<w.size(); ++i) {
                if (w[i] == "-rd" || w[i] == "-referenceDest" || w[i] == "-l" || w[i] == "-lock") ++i;
                else if (!IsFlag(w[i])) plugs.push_back(w[i]);
            }
            if (plugs.size() != 2) throw std::runtime_error("connectAttr needs two plugs");
            scene.connections.push_back({plugs[0],plugs[1]});
            ensureDefault(plugs[0].substr(0,plugs[0].find('.')));
            ensureDefault(plugs[1].substr(0,plugs[1].find('.')));
        } else if (w[0] == "currentUnit") {
            auto l=Option(w,"-l","-linear"), a=Option(w,"-a","-angle"), t=Option(w,"-t","-time");
            if (!l.empty()) scene.linearUnit=l;
            if (!a.empty()) scene.angularUnit=a;
            if (!t.empty()) scene.timeUnit=t;
        } else if (w[0] == "upAxis") scene.zUp = Option(w,"-ax","-axis") == "z";
        else if (w[0] == "file" && Flag(w,"-r","-reference") && !Flag(w,"-rdi","-referenceDepthInfo")) {
            if (w.size()<2) throw std::runtime_error("file reference missing asset");
            scene.references.push_back({w.back(),Option(w,"-ns","-namespace")});
        } else {
            static const std::set<std::string> harmless = {"requires","fileInfo","rename","addAttr","relationship"};
            std::string command;
            for (const auto &s : w) { if (!command.empty()) command += ' '; command += s; }
            scene.commands.push_back(command);
            if (!harmless.count(w[0]) && !(w[0]=="file" && Flag(w,"-rdi","-referenceDepthInfo")))
                scene.diagnostics.push_back("command preserved, not executed: " + w[0]);
        }
    }
    const std::map<std::string,std::string> unitAliases={
        {"millimeter","mm"},{"centimeter","cm"},{"meter","m"},{"kilometer","km"},
        {"inch","in"},{"foot","ft"},{"yard","yd"},{"mile","mi"}};
    if(unitAliases.count(scene.linearUnit)) scene.linearUnit=unitAliases.at(scene.linearUnit);
    if(scene.angularUnit=="degree") scene.angularUnit="deg";
    if(scene.angularUnit=="radian") scene.angularUnit="rad";
    if(findNode("initialShadingGroup")>=0) {
        ensureDefault("lambert1"); bool connected=false;
        for(const auto &c:scene.connections) if(c.destination=="initialShadingGroup.ss" || c.destination==":initialShadingGroup.ss" || c.destination=="initialShadingGroup.surfaceShader") connected=true;
        if(!connected) scene.connections.push_back({"lambert1.oc","initialShadingGroup.ss"});
    }
    return scene;
}
}
