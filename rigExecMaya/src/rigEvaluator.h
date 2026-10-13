// The rig, evaluated in Maya's own process: a poseable .rigexec bake played
// by usdRig's runtime, which links no USD. Each evaluation poses the bake
// with Maya's channel values and returns one frame, laid out as the node
// reads it (see RigSetup).
#ifndef RIGEXEC_MAYA_RIG_EVALUATOR_H
#define RIGEXEC_MAYA_RIG_EVALUATOR_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rigExec {
class RigExecRuntimeReader;
}

namespace rigExecMaya {

// Everything about one rig an evaluation needs, fixed once it is loaded and
// shared by every evaluator (the node's and the fill pool's).
//
// A frame is, in order: every mesh's points as f32 xyz in mesh order, then
// every control's asset-space matrix as 16 f64 (row-major, row vectors, as
// in Maya and USD), then every mesh's world matrix the same way.
struct RigSetup {
    std::string file;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    double bakeFrame = 1.0;
    std::vector<std::string> channelPaths;
    // Each channel's value in the bake; a channel Maya holds at this value
    // needs no override.
    std::vector<double> bakedValues;
    std::vector<std::string> controlPaths;
    struct Mesh {
        std::string points;      // the moved property, "<mesh>.points"
        size_t count = 0;
        std::vector<float> rest;
        std::string provider;    // the constraint-moved ancestor, or ""
        double local[16] = {};   // rest transform relative to the provider
        double world[16] = {};   // world matrix when nothing moves it
    };
    std::vector<Mesh> meshes;
    double assetToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    size_t PointOffset(size_t mesh) const;
    size_t ControlMatricesOffset() const;
    size_t MeshMatricesOffset() const;
    size_t FrameSize() const;
};

class RigEvaluator
{
public:
    static std::unique_ptr<RigEvaluator>
    Create(std::shared_ptr<const RigSetup> setup, std::string *error);
    ~RigEvaluator();

    // One frame for these channel values (in the rig's channel order).
    // Time is not an input: the bake is one frame, and Maya supplies every
    // pose.
    bool Evaluate(const std::vector<double> &values,
                  std::vector<uint8_t> *frame, std::string *error);

    // The asset-space frames (16 doubles each) of the controls or joints at
    // `paths`, with `values` posed and `overrides` (attribute path, value)
    // on top. For matching switches.
    bool Frames(const std::vector<double> &values,
                const std::vector<std::pair<std::string, double>> &overrides,
                const std::vector<std::string> &paths,
                std::vector<double> *frames, std::string *error);

    // Channels the bake does not take as inputs, by path. Maya can still
    // animate them; they just don't move the rig.
    const std::vector<std::string> &Refused() const { return _refusedPaths; }

private:
    RigEvaluator() = default;
    bool _Run(const std::vector<double> &values,
              const std::vector<std::pair<std::string, double>> &overrides,
              std::string *error);

    std::shared_ptr<const RigSetup> _setup;
    std::unique_ptr<rigExec::RigExecRuntimeReader> _reader;
    std::vector<bool> _refused;
    std::vector<std::string> _refusedPaths;
    // Where each mesh's points sit in the reader's point list, or -1.
    std::vector<long> _pointSlots;
};

}  // namespace rigExecMaya

#endif
