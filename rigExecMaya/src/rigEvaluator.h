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

class TaskPool;

// Everything about one rig an evaluation needs, fixed once it is loaded and
// shared by every evaluator (the node's and the fill pool's).
//
// A frame is, in order: every mesh's points as f32 xyz in mesh order, then
// every control's asset-space matrix as 16 f64 (row-major, row vectors, as
// in Maya and USD), then every mesh's world matrix the same way, then every
// shader matrix (an eye's projector and dials) in asset space.
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
    // The matrix primvars the rig publishes for shaders, by property path
    // ("<mesh>.primvars:eyeProjector").
    std::vector<std::string> shaderMatrices;

    size_t PointOffset(size_t mesh) const;
    size_t ControlMatricesOffset() const;
    size_t MeshMatricesOffset() const;
    size_t ShaderMatricesOffset() const;
    size_t FrameSize() const;
};

// The category the rig's work is named under in Maya's Profiler.
int ProfilerCategory();

// What one channel can move, whatever its value, by index into the setup's
// meshes (points, and world matrices), controls (their frames) and shader
// matrices.
struct ChannelReach {
    std::vector<uint32_t> meshes, meshMatrices, controls, shaders;
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

    // A value the rig publishes for switching without moving, with
    // `values` posed: "<match>.outputs:ikValues" / "outputs:fkValues" (an
    // IK/FK match constraint's) or "<control>.spaces:matchValues" (a space
    // switch's, six channels per space). False when the rig publishes none.
    bool Published(const std::vector<double> &values, const std::string &path,
                   std::vector<double> *out, std::string *error);

    // Runs each evaluation's independent parts on `pool` (none: one
    // thread). The node's own evaluator uses the shared pool; the playback
    // fill's evaluators already run side by side, one thread each.
    void SetTaskPool(TaskPool *pool) { _pool = pool; }

    // What each channel (in the setup's channel order) can move. Known once
    // the evaluator has evaluated; false before then, or when the rig cannot
    // tell.
    bool Reach(std::vector<ChannelReach> *reach) const;

    // Channels the bake does not take as inputs, by path. Maya can still
    // animate them; they just don't move the rig.
    const std::vector<std::string> &Refused() const { return _refusedPaths; }

private:
    RigEvaluator() = default;
    bool _Run(const std::vector<double> &values,
              const std::vector<std::pair<std::string, double>> &overrides,
              std::string *error);

    // The final frame the last run published for a control (or any other
    // provider) at `path`, as a row-vector matrix; false when none is.
    bool _ControlFrame(const std::string &path, double matrix[16]) const;

    std::shared_ptr<const RigSetup> _setup;
    TaskPool *_pool = nullptr;
    // Each published provider frame's slot, by path.
    std::map<std::string, size_t> _frameSlots;
    std::unique_ptr<rigExec::RigExecRuntimeReader> _reader;
    bool SetChannel(size_t input, double value);
    // Per channel: the input it sets (-1 when the bake has none), and the
    // value that input holds now (NaN when it must be set again).
    std::vector<long> _inputs;
    std::vector<double> _held;
    // Inputs an override set, put back on the next run.
    std::vector<std::string> _overridden;
    std::vector<std::string> _refusedPaths;
    // Where each mesh's points sit in the reader's point list, or -1.
    std::vector<long> _pointSlots;
};

}  // namespace rigExecMaya

#endif
