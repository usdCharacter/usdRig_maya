#pragma once
#include <map>
#include <string>
#include <vector>

namespace usdMayaRig {
struct Attribute {
    std::string name, type;
    std::vector<std::string> values;
};
struct Node {
    std::string type, name, parent;
    std::vector<Attribute> attributes;
};
struct Connection { std::string source, destination; };
struct Reference { std::string asset, nameSpace; bool loaded = false; };
struct Scene {
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    std::vector<Reference> references;
    std::vector<std::string> diagnostics, commands;
    std::string linearUnit = "cm", angularUnit = "deg", timeUnit = "film";
    bool zUp = false;
};
// A data parser, never a MEL interpreter. Throws on malformed syntax.
Scene Parse(const std::string &text);
}
