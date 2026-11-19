// rigExecRig: drives a Maya scene from a RigExec rig, evaluated in Maya's
// own process by usdRig's runtime from a poseable .rigexec bake (see
// rigEvaluator.h). No USD is linked here: Maya's own USD has the same
// library names and an older OpenExec, and the runtime needs none.
//
// Inputs are every animator channel (inLinear / inAngle, indexed by the
// rig's channel order) and the time. Outputs:
//
//   outMeshFrame[i]      mesh i's points, for the rigExecDeform deformer
//                        on the mesh (scenes built before it use outMesh
//                        instead)
//   outControlFrames     every control's frame, for the rigExecControls
//                        node that draws the control curves
//   outFrame             a number that changes with every new pose
//   outMesh[i]           mesh i's deformed points on a copy of inMesh[i]
//   outMeshMatrix[i]     mesh i's world matrix
//   outControlMatrix[c]  control c's offsetParentMatrix: with the control's
//                        own translate/rotate/scale composed on top, it
//                        lands exactly on the frame the rig computed
//   outShaderMatrix[s]   shader matrix s (shaderMatrixPath), in the rig's
//                        asset space: an eye's projector, or its dials
//   outShaderValue[16s+k] the same matrices' values one by one, row-major,
//                        for driving a shader's numbers
//
// The tables (the rig file, channel and control paths, control parents,
// rotate orders, TRS channel slots, channel defaults, mesh placement) are
// set once by rigexec_maya.scene from the rig's export.
//
// A channel dirties only the outputs it can move (RigEvaluator::Reach): a
// hand's channel leaves the face's meshes, the eyes and the other limbs'
// controls alone, so Maya redraws only what moved. Set
// RIGEXEC_MAYA_CHECK_REACH=1 to have every pose checked against that, with
// a warning if anything moves that was not expected to.
//
// The node can be evaluated at any time, not just the current one: cached
// playback fills frames in the background while the scene is posed, and
// each evaluation gets the pose of its own time.

#include "frameCache.h"
#include "frameData.h"
#include "deformNode.h"
#include "hoverOverlay.h"
#include "touchOverlay.h"
#include "controlsDraw.h"
#include "rigEvaluator.h"
#include "taskPool.h"

#include <maya/MAngle.h>
#include <maya/MAnimControl.h>
#include <maya/MAnimUtil.h>
#include <maya/MArgList.h>
#include <maya/MDGContext.h>
#include <maya/MDGContextGuard.h>
#include <maya/MEvaluationManager.h>
#include <maya/MEvaluationNode.h>
#include <maya/MEvaluationNodeIterator.h>
#include <maya/MCacheSchema.h>
#include <maya/MNodeCacheDisablingInfo.h>
#include <maya/MNodeCacheSetupInfo.h>
#include <maya/MObjectArray.h>
#include <maya/MTimerMessage.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MProfiler.h>
#include <maya/MPxCommand.h>
#include <maya/MArrayDataBuilder.h>
#include <maya/MArrayDataHandle.h>
#include <maya/MDataBlock.h>
#include <maya/MDataHandle.h>
#include <maya/MDoubleArray.h>
#include <maya/MEulerRotation.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnDoubleArrayData.h>
#include <maya/MFnStringArrayData.h>
#include <maya/MStringArray.h>
#include <maya/MSyntax.h>
#include <maya/MArgDatabase.h>
#include <maya/MSelectionList.h>
#include <maya/MFnIntArrayData.h>
#include <maya/MFnMatrixAttribute.h>
#include <maya/MFnMesh.h>
#include <maya/MFnMeshData.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnPlugin.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnUnitAttribute.h>
#include <maya/MGlobal.h>
#include <maya/MIntArray.h>
#include <maya/MMatrix.h>
#include <maya/MPlug.h>
#include <maya/MPlugArray.h>
#include <maya/MPxNode.h>
#include <maya/MPointArray.h>
#include <maya/MTime.h>
#include <maya/MTransformationMatrix.h>
#include <maya/MVector.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using rigExecMaya::FillJob;
using rigExecMaya::FillPool;
using rigExecMaya::Frame;
using rigExecMaya::FrameData;
using rigExecMaya::FrameCache;
using rigExecMaya::RigEvaluator;
using rigExecMaya::RigSetup;

namespace {

// How hard the rigs are working, for the frame-rate display: every compute,
// the ones that had to evaluate the rig (not found in the cache), and the
// time each took, in nanoseconds. Summed over every rig node.
std::atomic<long long> gComputes{0}, gEvaluations{0};
std::atomic<long long> gComputeNs{0}, gEvaluateNs{0};
// The part of computing spent writing the rig's outputs, and the meshes'
// deformers copying the rig's points in.
std::atomic<long long> gWriteNs{0};
std::atomic<long long> gDeforms{0}, gDeformNs{0};
// Numbers every new pose, across every rig node.
std::atomic<long long> gGenerations{0};

// The rig's work is named in Maya's Profiler (Windows > General Editors >
// Profiler) under "RigExec".
using rigExecMaya::ProfilerCategory;

long long
NanosecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

const MEulerRotation::RotationOrder kOrders[] = {
    MEulerRotation::kXYZ, MEulerRotation::kYZX, MEulerRotation::kZXY,
    MEulerRotation::kXZY, MEulerRotation::kYXZ, MEulerRotation::kZYX};

MMatrix
ReadMatrix(const uint8_t *bytes)
{
    double values[4][4];
    std::memcpy(values, bytes, sizeof(values));
    return MMatrix(values);
}

MFloatPointArray
Points(const uint8_t *bytes, int count)
{
    const float *points = reinterpret_cast<const float *>(bytes);
    MFloatPointArray array(static_cast<unsigned>(count));
    for (unsigned i = 0; i < unsigned(count); ++i) {
        array.set(i, points[3 * i], points[3 * i + 1], points[3 * i + 2]);
    }
    return array;
}

}  // namespace

class RigExecRigNode : public MPxNode
{
public:
    static const MTypeId id;
    static MObject aStagePath, aRigPath, aTime, aInLinear, aInAngle, aInMesh;
    static MObject aControlParent, aControlRotateOrder, aControlTrs;
    static MObject aChannelDefault, aChannelSign, aMeshPointCount;
    static MObject aRigFile, aBakeFrame, aChannelPath, aControlPath;
    static MObject aMeshPoints, aMeshProvider, aMeshLocal, aMeshWorld;
    static MObject aAssetToWorld, aShaderMatrixPath;
    static MObject aOutShaderMatrix, aOutShaderValue;
    static MObject aOutMesh, aOutMeshMatrix, aOutControlMatrix;
    static MObject aOutMeshFrame, aOutFrame, aOutControlFrames;

    static void *creator() { return new RigExecRigNode(); }
    static MStatus initialize();
    void postConstructor() override;
    ~RigExecRigNode() override;
    MStatus compute(const MPlug &plug, MDataBlock &data) override;
    MStatus setDependentsDirty(const MPlug &plug,
                               MPlugArray &affected) override;
    // Only ever computed once at a time for each time Maya evaluates (see
    // _ComputeAt), so rigs evaluate side by side.
    SchedulingType schedulingType() const override { return kParallel; }
    MStatus preEvaluation(const MDGContext &context,
                          const MEvaluationNode &evaluationNode) override;
    // Cached playback keeps the node's outputs, so a cached frame plays
    // back without evaluating the rig.
    void getCacheSetup(const MEvaluationNode &evaluationNode,
                       MNodeCacheDisablingInfo &disablingInfo,
                       MNodeCacheSetupInfo &setupInfo,
                       MObjectArray &monitoredAttributes) const override;
    void configCache(const MEvaluationNode &evaluationNode,
                     MCacheSchema &schema) const override;

    // Called from a timer on the UI thread: queues the playback range's
    // missing frames for the fill pool (see frameCache.h).
    void FillPlaybackCache();
    size_t CachedFrames() const { return _cache->Size(); }
    size_t PendingFrames() const { return _pool ? _pool->Pending() : 0; }

    // For rigExecFrames: the frames of `paths` with the rig posed as Maya
    // has it now and `overrides` on top.
    bool Frames(const std::vector<std::pair<std::string, double>> &overrides,
                const std::vector<std::string> &paths,
                std::vector<double> *frames, std::string *error);
    // For rigExecPublished: a value the rig publishes for matching, with
    // the rig posed as Maya has it now.
    bool Published(const std::string &path, std::vector<double> *out,
                   std::string *error);

private:
    struct Tables {
        std::vector<int> parent, rotateOrder, trs, pointCount;
        std::vector<double> defaults;
        // -1 for a mirrored rotation channel: Maya holds the angle as it
        // turns the control, the rig the value it multiplies by -1.
        std::vector<double> sign;
        size_t controlMatricesOffset = 0, meshMatricesOffset = 0, size = 0;
        size_t shaderMatricesOffset = 0, shaderCount = 0;
        std::vector<size_t> pointOffset;
    };

    // Notes a channel (or a table) that changed, so the next compute reads
    // just that channel.
    void _NoteDirty(const MPlug &plug);
    // The outputs channel `plug` can move, for setDependentsDirty.
    void _AddReach(const MPlug &plug, MPlugArray &affected);
    // Learns what each channel moves, once the rig has evaluated.
    void _LearnReach();
    // RIGEXEC_MAYA_CHECK_REACH: warns when `frame` moved something the
    // channels changed since `before` cannot reach.
    void _CheckReach(const Frame &before, const std::vector<double> &from,
                     const Frame &frame, const std::vector<double> &to);
    bool _ReadTables(MDataBlock &data, Tables *tables);
    // The loaded rig, (re)built when the rig file or its tables change.
    bool _Load(MDataBlock &data, const Tables &tables, std::string *error);
    std::vector<double> _CurrentChannels();
    std::vector<double> _ReadChannels(MDataBlock &data, const Tables &tables,
                                      bool tracked);
    static std::vector<double> _ReadAllChannels(MDataBlock &data,
                                                const Tables &tables);
    // An evaluation at another time than the scene's (cached playback
    // filling ahead, or a script asking for another frame): leaves the
    // node's own pose as it is.
    MStatus _ComputeAt(MDataBlock &data, const MObject &attribute);
    // Which outputs to write: all of them, or under the evaluation manager
    // (which asks for each output on its own) just the one `attribute` is
    // part of.
    enum Outputs : unsigned {
        kControls = 1, kMeshMatrices = 2, kShaders = 4, kFrames = 8,
        kMeshes = 16, kAllOutputs = 31
    };
    static unsigned _OutputsFor(const MObject &attribute,
                                const MDGContext &context);
    void _WriteOutputs(MDataBlock &data, const Tables &tables,
                       const Frame &frame, const std::vector<double> &values,
                       double generation, bool current, unsigned outputs);
    MMatrix _Trs(int control, const Tables &tables,
                 const std::vector<double> &values) const;
    void _WriteControls(MDataBlock &data, const Tables &tables,
                        const Frame &frame,
                        const std::vector<double> &values);
    void _WriteMeshMatrices(MDataBlock &data, const Tables &tables,
                            const Frame &frame);
    void _WriteShaderMatrices(MDataBlock &data, const Tables &tables,
                              const Frame &frame);
    void _WriteMeshes(MDataBlock &data, const Tables &tables,
                      const Frame &frame, bool current);

    double _time = 0.0;
    // Rig values (signs applied), and the signs they were read with.
    std::vector<double> _values;
    std::vector<double> _sign;
    // The tables as last read, re-read only when one of them changes.
    std::shared_ptr<const Tables> _tables;
    bool _tablesDirty = true;
    // The channel values as last read (signs applied), and the channels
    // Maya has dirtied since. Without the evaluation manager, a pose then
    // reads only the channels that changed, not all of them.
    std::vector<double> _channels;
    std::vector<std::pair<bool, unsigned>> _dirtyChannels;
    bool _readAll = true;
    double _generation = 0.0;
    double _Sign(size_t channel) const
    {
        return channel < _sign.size() ? _sign[channel] : 1.0;
    }
    Frame _frame;
    // The frame the output meshes hold, and whether they exist yet.
    Frame _meshFrame;
    bool _meshesWritten = false;
    // Every frame this node has evaluated or the pool has filled, keyed by
    // the full request; capacity from RIGEXEC_MAYA_CACHE_FRAMES.
    std::shared_ptr<FrameCache> _cache;
    std::unique_ptr<FillPool> _pool;
    std::shared_ptr<const RigSetup> _setup;
    std::unique_ptr<RigEvaluator> _evaluator;
    // What each channel moves (empty until known: then everything), and the
    // plugs that dirties, built per channel when first needed.
    std::vector<rigExecMaya::ChannelReach> _reach;
    std::vector<MPlugArray> _reachPlugs;
    std::vector<char> _reachPlugsBuilt;
    bool _checkReach = false;
    // Guards _tables and _setup while evaluations at other times read them.
    std::mutex _loadMutex;
    // The evaluator for other times, and the rig it was made for.
    std::mutex _otherMutex;
    std::unique_ptr<RigEvaluator> _otherEvaluator;
    std::shared_ptr<const RigSetup> _otherSetup;
    // Set by a compute that had to evaluate: the animation, or the values
    // it is played with, may have changed.
    bool _needsFill = false;
};

const MTypeId RigExecRigNode::id(0x13a189);
MObject RigExecRigNode::aStagePath, RigExecRigNode::aRigPath,
    RigExecRigNode::aTime, RigExecRigNode::aInLinear,
    RigExecRigNode::aInAngle, RigExecRigNode::aInMesh,
    RigExecRigNode::aControlParent, RigExecRigNode::aControlRotateOrder,
    RigExecRigNode::aControlTrs, RigExecRigNode::aChannelDefault,
    RigExecRigNode::aChannelSign,
    RigExecRigNode::aMeshPointCount, RigExecRigNode::aRigFile,
    RigExecRigNode::aBakeFrame, RigExecRigNode::aChannelPath,
    RigExecRigNode::aControlPath, RigExecRigNode::aMeshPoints,
    RigExecRigNode::aMeshProvider, RigExecRigNode::aMeshLocal,
    RigExecRigNode::aMeshWorld, RigExecRigNode::aAssetToWorld,
    RigExecRigNode::aOutMesh,
    RigExecRigNode::aOutMeshMatrix, RigExecRigNode::aOutControlMatrix,
    RigExecRigNode::aOutMeshFrame, RigExecRigNode::aOutFrame,
    RigExecRigNode::aOutControlFrames,
    RigExecRigNode::aShaderMatrixPath,
    RigExecRigNode::aOutShaderMatrix, RigExecRigNode::aOutShaderValue;

MStatus
RigExecRigNode::initialize()
{
    MFnTypedAttribute typed;
    MFnUnitAttribute unit;
    MFnNumericAttribute numeric;
    MFnMatrixAttribute matrix;

    aStagePath = typed.create("stagePath", "sp", MFnData::kString);
    aRigPath = typed.create("rigPath", "rp", MFnData::kString);
    aTime = unit.create("time", "tm", MFnUnitAttribute::kTime, 0.0);
    aInLinear = numeric.create("inLinear", "il", MFnNumericData::kDouble,
                               0.0);
    numeric.setArray(true);
    numeric.setUsesArrayDataBuilder(true);
    aInAngle = unit.create("inAngle", "ia", MFnUnitAttribute::kAngle, 0.0);
    unit.setArray(true);
    unit.setUsesArrayDataBuilder(true);
    aInMesh = typed.create("inMesh", "im", MFnData::kMesh);
    typed.setArray(true);
    typed.setUsesArrayDataBuilder(true);

    aControlParent = typed.create("controlParent", "cpa",
                                  MFnData::kIntArray);
    aControlRotateOrder = typed.create("controlRotateOrder", "cro",
                                       MFnData::kIntArray);
    aControlTrs = typed.create("controlTrs", "ctr", MFnData::kIntArray);
    aChannelDefault = typed.create("channelDefault", "chd",
                                   MFnData::kDoubleArray);
    aChannelSign = typed.create("channelSign", "chs",
                                MFnData::kDoubleArray);
    aMeshPointCount = typed.create("meshPointCount", "mpc",
                                   MFnData::kIntArray);
    aRigFile = typed.create("rigFile", "rf", MFnData::kString);
    aBakeFrame = numeric.create("bakeFrame", "bf", MFnNumericData::kDouble,
                                1.0);
    aChannelPath = typed.create("channelPath", "chp",
                                MFnData::kStringArray);
    aControlPath = typed.create("controlPath", "cpt",
                                MFnData::kStringArray);
    aMeshPoints = typed.create("meshPoints", "mpt", MFnData::kStringArray);
    aMeshProvider = typed.create("meshProvider", "mpv",
                                 MFnData::kStringArray);
    aMeshLocal = typed.create("meshLocal", "mlo", MFnData::kDoubleArray);
    aMeshWorld = typed.create("meshWorld", "mwo", MFnData::kDoubleArray);
    aAssetToWorld = typed.create("assetToWorld", "atw",
                                 MFnData::kDoubleArray);
    aShaderMatrixPath = typed.create("shaderMatrixPath", "smp",
                                     MFnData::kStringArray);

    aOutMesh = typed.create("outMesh", "om", MFnData::kMesh);
    typed.setArray(true);
    typed.setUsesArrayDataBuilder(true);
    typed.setStorable(false);
    typed.setWritable(false);
    aOutMeshMatrix = matrix.create("outMeshMatrix", "omm");
    matrix.setArray(true);
    matrix.setUsesArrayDataBuilder(true);
    matrix.setStorable(false);
    matrix.setWritable(false);
    aOutControlMatrix = matrix.create("outControlMatrix", "ocm");
    matrix.setArray(true);
    matrix.setUsesArrayDataBuilder(true);
    matrix.setStorable(false);
    matrix.setWritable(false);
    aOutShaderMatrix = matrix.create("outShaderMatrix", "osm");
    matrix.setArray(true);
    matrix.setUsesArrayDataBuilder(true);
    matrix.setStorable(false);
    matrix.setWritable(false);

    const MObject inputs[] = {aStagePath, aRigPath, aTime, aInLinear,
                              aInAngle, aInMesh, aControlParent,
                              aControlRotateOrder, aControlTrs,
                              aChannelDefault, aChannelSign,
                              aMeshPointCount, aRigFile,
                              aBakeFrame, aChannelPath, aControlPath,
                              aMeshPoints, aMeshProvider, aMeshLocal,
                              aMeshWorld, aAssetToWorld, aShaderMatrixPath};
    aOutMeshFrame = typed.create("outMeshFrame", "omf", FrameData::id);
    typed.setArray(true);
    typed.setUsesArrayDataBuilder(true);
    typed.setStorable(false);
    typed.setWritable(false);
    aOutControlFrames = typed.create("outControlFrames", "ocf",
                                     FrameData::id);
    typed.setStorable(false);
    typed.setWritable(false);

    // A number that changes with every new pose.
    aOutFrame = numeric.create("outFrame", "ofr", MFnNumericData::kDouble,
                               0.0);
    numeric.setStorable(false);
    numeric.setWritable(false);

    aOutShaderValue = numeric.create("outShaderValue", "osv",
                                     MFnNumericData::kDouble, 0.0);
    numeric.setArray(true);
    numeric.setUsesArrayDataBuilder(true);
    numeric.setStorable(false);
    numeric.setWritable(false);

    const MObject outputs[] = {aOutMesh, aOutMeshMatrix, aOutControlMatrix,
                               aOutMeshFrame, aOutFrame, aOutControlFrames,
                               aOutShaderMatrix, aOutShaderValue};
    for (const MObject &a : inputs) {
        addAttribute(a);
    }
    for (const MObject &a : outputs) {
        addAttribute(a);
    }
    // A channel's outputs are dirtied in setDependentsDirty, by what it
    // moves; the time poses nothing (the channels do), and only orders the
    // playback fill.
    for (const MObject &source : inputs) {
        if (source == aTime) {
            continue;
        }
        const bool channel = source == aInLinear || source == aInAngle;
        for (const MObject &target : outputs) {
            if (!channel || target == aOutFrame) {
                attributeAffects(source, target);
            }
        }
    }
    return MS::kSuccess;
}

namespace {

std::vector<std::string>
StringArray(MDataBlock &data, const MObject &attribute)
{
    MFnStringArrayData fn(data.inputValue(attribute).data());
    const MStringArray array = fn.array();
    std::vector<std::string> out(array.length());
    for (unsigned i = 0; i < array.length(); ++i) {
        out[i] = array[i].asChar();
    }
    return out;
}

std::vector<double>
DoubleArray(MDataBlock &data, const MObject &attribute)
{
    MFnDoubleArrayData fn(data.inputValue(attribute).data());
    const MDoubleArray array = fn.array();
    std::vector<double> out(array.length());
    for (unsigned i = 0; i < array.length(); ++i) {
        out[i] = array[i];
    }
    return out;
}

std::vector<int>
IntArray(MDataBlock &data, const MObject &attribute)
{
    MFnIntArrayData fn(data.inputValue(attribute).data());
    const MIntArray array = fn.array();
    std::vector<int> out(array.length());
    for (unsigned i = 0; i < array.length(); ++i) {
        out[i] = array[i];
    }
    return out;
}

}  // namespace

bool
RigExecRigNode::_ReadTables(MDataBlock &data, Tables *tables)
{
    tables->parent = IntArray(data, aControlParent);
    tables->rotateOrder = IntArray(data, aControlRotateOrder);
    tables->trs = IntArray(data, aControlTrs);
    tables->pointCount = IntArray(data, aMeshPointCount);
    MFnDoubleArrayData defaults(data.inputValue(aChannelDefault).data());
    const MDoubleArray array = defaults.array();
    tables->defaults.assign(array.length(), 0.0);
    for (unsigned i = 0; i < array.length(); ++i) {
        tables->defaults[i] = array[i];
    }
    tables->sign.assign(array.length(), 1.0);
    MFnDoubleArrayData signs(data.inputValue(aChannelSign).data());
    const MDoubleArray signArray = signs.array();
    for (unsigned i = 0; i < signArray.length() && i < array.length(); ++i) {
        tables->sign[i] = signArray[i] < 0.0 ? -1.0 : 1.0;
    }
    const size_t controls = tables->parent.size();
    if (tables->rotateOrder.size() != controls ||
        tables->trs.size() != controls * 9) {
        return false;
    }
    size_t offset = 0;
    tables->pointOffset.clear();
    for (int count : tables->pointCount) {
        tables->pointOffset.push_back(offset);
        offset += size_t(count) * 12;
    }
    tables->controlMatricesOffset = offset;
    offset += controls * 128;
    tables->meshMatricesOffset = offset;
    offset += tables->pointCount.size() * 128;
    tables->shaderMatricesOffset = offset;
    tables->shaderCount = StringArray(data, aShaderMatrixPath).size();
    offset += tables->shaderCount * 128;
    tables->size = offset;
    return true;
}

std::vector<double>
RigExecRigNode::_ReadChannels(MDataBlock &data, const Tables &tables,
                              bool tracked)
{
    if (tracked && !_readAll && _channels.size() == tables.defaults.size()) {
        for (const auto &[angle, index] : _dirtyChannels) {
            if (index >= _channels.size()) {
                continue;
            }
            const MPlug element =
                MPlug(thisMObject(), angle ? aInAngle : aInLinear)
                    .elementByLogicalIndex(index);
            MDataHandle handle = data.inputValue(element);
            _channels[index] = (angle ? handle.asAngle().asDegrees()
                                      : handle.asDouble()) *
                               tables.sign[index];
        }
        _dirtyChannels.clear();
        return _channels;
    }
    _dirtyChannels.clear();
    _readAll = false;
    _channels = _ReadAllChannels(data, tables);
    return _channels;
}

std::vector<double>
RigExecRigNode::_ReadAllChannels(MDataBlock &data, const Tables &tables)
{
    std::vector<double> values = tables.defaults;
    MArrayDataHandle linear = data.inputArrayValue(aInLinear);
    for (unsigned i = 0, n = linear.elementCount(); i < n; ++i, linear.next()) {
        const unsigned index = linear.elementIndex();
        if (index < values.size()) {
            values[index] = linear.inputValue().asDouble() *
                            tables.sign[index];
        }
    }
    MArrayDataHandle angle = data.inputArrayValue(aInAngle);
    for (unsigned i = 0, n = angle.elementCount(); i < n; ++i, angle.next()) {
        const unsigned index = angle.elementIndex();
        if (index < values.size()) {
            values[index] = angle.inputValue().asAngle().asDegrees() *
                            tables.sign[index];
        }
    }
    return values;
}

MStatus
RigExecRigNode::compute(const MPlug &plug, MDataBlock &data)
{
    const MObject attribute =
        plug.isElement() ? plug.array().attribute() : plug.attribute();
    if (attribute != aOutMesh && attribute != aOutMeshMatrix &&
        attribute != aOutControlMatrix && attribute != aOutMeshFrame &&
        attribute != aOutFrame && attribute != aOutControlFrames &&
        attribute != aOutShaderMatrix && attribute != aOutShaderValue) {
        return MS::kUnknownParameter;
    }
    const auto computeStart = std::chrono::steady_clock::now();
    MProfilingScope computeScope(ProfilerCategory(), MProfiler::kColorE_L1,
                                 "rigExecRig compute");
    if (!data.context().isNormal()) {
        const MStatus status = _ComputeAt(data, attribute);
        gComputes += 1;
        gComputeNs += NanosecondsSince(computeStart);
        return status;
    }
    std::string error;
    if (_tablesDirty) {
        auto tables = std::make_shared<Tables>();
        if (!_ReadTables(data, tables.get())) {
            MGlobal::displayError("rigExecRig: the rig tables do not agree");
            return MS::kFailure;
        }
        if (!_Load(data, *tables, &error)) {
            MGlobal::displayError(("rigExecRig: " + error).c_str());
            return MS::kFailure;
        }
        {
            std::lock_guard<std::mutex> lock(_loadMutex);
            _tables = std::move(tables);
        }
        _sign = _tables->sign;
        _tablesDirty = false;
        _readAll = true;
    }
    const Tables &tables = *_tables;
    const double time =
        data.inputValue(aTime).asTime().as(MTime::uiUnit());
    std::vector<double> values = _ReadChannels(data, tables, true);
    if (!_frame || values != _values) {
        Frame frame = _cache->Find(values);
        if (!frame) {
            const auto evaluateStart = std::chrono::steady_clock::now();
            MProfilingScope evaluateScope(ProfilerCategory(),
                                          MProfiler::kColorD_L1,
                                          "rigExecRig evaluate");
            std::vector<uint8_t> bytes;
            if (!_evaluator->Evaluate(values, &bytes, &error)) {
                MGlobal::displayError(("rigExecRig: " + error).c_str());
                return MS::kFailure;
            }
            gEvaluations += 1;
            gEvaluateNs += NanosecondsSince(evaluateStart);
            frame = std::make_shared<const std::vector<uint8_t>>(
                std::move(bytes));
            _cache->Insert(values, frame);
            _needsFill = true;
        }
        if (frame->size() != tables.size) {
            MGlobal::displayError(
                "rigExecRig: the evaluated frame does not match this rig");
            return MS::kFailure;
        }
        if (_checkReach && _frame && !_reach.empty()) {
            _CheckReach(_frame, _values, frame, values);
        }
        _frame = std::move(frame);
        _values.swap(values);
        _generation = double(++gGenerations);
        if (_reach.empty()) {
            _LearnReach();
        }
    }
    // The playhead, which only orders the playback fill.
    _time = time;
    _WriteOutputs(data, tables, _frame, _values, _generation, true,
                  _OutputsFor(attribute, data.context()));
    gComputes += 1;
    gComputeNs += NanosecondsSince(computeStart);
    return MS::kSuccess;
}

MStatus
RigExecRigNode::_ComputeAt(MDataBlock &data, const MObject &attribute)
{
    std::shared_ptr<const Tables> tables;
    std::shared_ptr<const RigSetup> setup;
    {
        std::lock_guard<std::mutex> lock(_loadMutex);
        tables = _tables;
        setup = _setup;
    }
    if (!tables || !setup) {
        // Not loaded yet: the scene's own time loads it first.
        return MS::kFailure;
    }
    const std::vector<double> values = _ReadAllChannels(data, *tables);
    Frame frame = _cache->Find(values);
    if (!frame) {
        std::lock_guard<std::mutex> lock(_otherMutex);
        std::string error;
        if (!_otherEvaluator || _otherSetup != setup) {
            _otherEvaluator = RigEvaluator::Create(setup, &error);
            _otherSetup = setup;
        }
        if (!_otherEvaluator) {
            MGlobal::displayError(("rigExecRig: " + error).c_str());
            return MS::kFailure;
        }
        const auto evaluateStart = std::chrono::steady_clock::now();
        MProfilingScope evaluateScope(ProfilerCategory(),
                                      MProfiler::kColorD_L1,
                                      "rigExecRig evaluate (other time)");
        std::vector<uint8_t> bytes;
        if (!_otherEvaluator->Evaluate(values, &bytes, &error)) {
            MGlobal::displayError(("rigExecRig: " + error).c_str());
            return MS::kFailure;
        }
        gEvaluations += 1;
        gEvaluateNs += NanosecondsSince(evaluateStart);
        frame = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        _cache->Insert(values, frame);
    }
    if (frame->size() != tables->size) {
        return MS::kFailure;
    }
    _WriteOutputs(data, *tables, frame, values, double(++gGenerations),
                  false, _OutputsFor(attribute, data.context()));
    return MS::kSuccess;
}

unsigned
RigExecRigNode::_OutputsFor(const MObject &attribute,
                            const MDGContext &context)
{
    // Without the evaluation manager, every output at once: a pose asks for
    // several, and each would otherwise come back here.
    if (!MEvaluationManager::evaluationInExecution(context)) {
        return kAllOutputs;
    }
    if (attribute == aOutControlMatrix) {
        return kControls;
    }
    if (attribute == aOutMeshMatrix) {
        return kMeshMatrices;
    }
    if (attribute == aOutShaderMatrix || attribute == aOutShaderValue) {
        return kShaders;
    }
    if (attribute == aOutMesh) {
        return kMeshes;
    }
    return kFrames;
}

void
RigExecRigNode::_WriteOutputs(MDataBlock &data, const Tables &tables,
                              const Frame &frame,
                              const std::vector<double> &values,
                              double generation, bool current,
                              unsigned outputs)
{
    const auto writeStart = std::chrono::steady_clock::now();
    MProfilingScope writeScope(ProfilerCategory(), MProfiler::kColorC_L1,
                               "rigExecRig write outputs");
    if (outputs & kControls) {
        _WriteControls(data, tables, frame, values);
    }
    if (outputs & kMeshMatrices) {
        _WriteMeshMatrices(data, tables, frame);
    }
    if (outputs & kShaders) {
        _WriteShaderMatrices(data, tables, frame);
    }
    if (outputs & kMeshes) {
        _WriteMeshes(data, tables, frame, current);
    }
    if (!(outputs & kFrames)) {
        gWriteNs += NanosecondsSince(writeStart);
        return;
    }
    MDataHandle frameHandle = data.outputValue(aOutFrame);
    frameHandle.setDouble(generation);
    frameHandle.setClean();
    MDataHandle controlsHandle = data.outputValue(aOutControlFrames);
    FrameData::Set(controlsHandle, frame, tables.controlMatricesOffset,
                   tables.parent.size(), generation);
    controlsHandle.setClean();
    MArrayDataHandle meshes = data.outputArrayValue(aOutMeshFrame);
    MArrayDataBuilder builder = meshes.builder();
    for (size_t m = 0; m < tables.pointCount.size(); ++m) {
        MDataHandle element = builder.addElement(unsigned(m));
        FrameData::Set(element, frame, tables.pointOffset[m],
                       size_t(tables.pointCount[m]), generation);
    }
    meshes.set(builder);
    meshes.setAllClean();
    gWriteNs += NanosecondsSince(writeStart);
}

bool
RigExecRigNode::_Load(MDataBlock &data, const Tables &tables,
                      std::string *error)
{
    const std::string file = data.inputValue(aRigFile).asString().asChar();
    std::vector<std::string> channels = StringArray(data, aChannelPath);
    std::vector<std::string> shaderMatrices =
        StringArray(data, aShaderMatrixPath);
    if (_setup && _evaluator && _setup->file == file &&
        _setup->channelPaths == channels &&
        _setup->shaderMatrices == shaderMatrices) {
        return true;
    }
    _pool.reset();
    _cache->Clear();
    _reach.clear();
    _reachPlugs.clear();
    _reachPlugsBuilt.clear();
    _frame.reset();
    _meshFrame.reset();
    _meshesWritten = false;
    _evaluator.reset();
    {
        std::lock_guard<std::mutex> lock(_loadMutex);
        _setup.reset();
    }
    if (file.empty()) {
        *error = "no rig file; load the rig again from RigExec > Load Rig";
        return false;
    }
    auto setup = std::make_shared<RigSetup>();
    setup->file = file;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        *error = "cannot read the rig file " + file +
                 "; load the rig again from RigExec > Load Rig";
        return false;
    }
    setup->bytes = std::make_shared<const std::vector<uint8_t>>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    setup->bakeFrame = data.inputValue(aBakeFrame).asDouble();
    setup->channelPaths = std::move(channels);
    setup->shaderMatrices = std::move(shaderMatrices);
    setup->bakedValues = tables.defaults;
    setup->controlPaths = StringArray(data, aControlPath);
    const std::vector<std::string> points = StringArray(data, aMeshPoints);
    const std::vector<std::string> providers =
        StringArray(data, aMeshProvider);
    const std::vector<double> locals = DoubleArray(data, aMeshLocal);
    const std::vector<double> worlds = DoubleArray(data, aMeshWorld);
    const std::vector<double> asset = DoubleArray(data, aAssetToWorld);
    const size_t meshes = tables.pointCount.size();
    if (setup->bakedValues.size() != setup->channelPaths.size() ||
        setup->controlPaths.size() != tables.parent.size() ||
        points.size() != meshes || providers.size() != meshes ||
        locals.size() != meshes * 16 || worlds.size() != meshes * 16 ||
        asset.size() != 16) {
        *error = "the rig tables do not agree";
        return false;
    }
    std::copy(asset.begin(), asset.end(), setup->assetToWorld);
    MArrayDataHandle inputs = data.inputArrayValue(aInMesh);
    for (size_t m = 0; m < meshes; ++m) {
        RigSetup::Mesh mesh;
        mesh.points = points[m];
        mesh.provider = providers[m];
        mesh.count = size_t(tables.pointCount[m]);
        std::copy(locals.begin() + 16 * m, locals.begin() + 16 * (m + 1),
                  mesh.local);
        std::copy(worlds.begin() + 16 * m, worlds.begin() + 16 * (m + 1),
                  mesh.world);
        // Rest points, for a mesh the rig does not move.
        if (inputs.jumpToElement(unsigned(m))) {
            MObject source = inputs.inputValue().asMesh();
            if (!source.isNull()) {
                MFloatPointArray rest;
                MFnMesh(source).getPoints(rest, MSpace::kObject);
                if (rest.length() == mesh.count) {
                    mesh.rest.resize(mesh.count * 3);
                    for (unsigned i = 0; i < rest.length(); ++i) {
                        mesh.rest[3 * i] = rest[i].x;
                        mesh.rest[3 * i + 1] = rest[i].y;
                        mesh.rest[3 * i + 2] = rest[i].z;
                    }
                }
            }
        }
        setup->meshes.push_back(std::move(mesh));
    }
    _evaluator = RigEvaluator::Create(setup, error);
    if (!_evaluator) {
        return false;
    }
    _evaluator->SetTaskPool(rigExecMaya::TaskPool::Shared());
    if (!_evaluator->Refused().empty()) {
        MGlobal::displayWarning(
            MString("rigExecRig: ") +
            unsigned(_evaluator->Refused().size()) +
            " channels are not inputs of the baked rig and do not move it");
    }
    std::lock_guard<std::mutex> lock(_loadMutex);
    _setup = std::move(setup);
    return true;
}

std::vector<double>
RigExecRigNode::_CurrentChannels()
{
    std::vector<double> values =
        _setup ? _setup->bakedValues : std::vector<double>();
    const MObject self = thisMObject();
    for (const auto &[attribute, angle] :
         {std::make_pair(aInLinear, false), std::make_pair(aInAngle, true)}) {
        MPlug array(self, attribute);
        MIntArray indices;
        array.getExistingArrayAttributeIndices(indices);
        for (unsigned i = 0; i < indices.length(); ++i) {
            const size_t index = size_t(indices[i]);
            if (index < values.size()) {
                const MPlug element =
                    array.elementByLogicalIndex(unsigned(indices[i]));
                values[index] = (angle ? element.asMAngle().asDegrees()
                                       : element.asDouble()) *
                                _Sign(index);
            }
        }
    }
    return values;
}

bool
RigExecRigNode::Frames(
    const std::vector<std::pair<std::string, double>> &overrides,
    const std::vector<std::string> &paths, std::vector<double> *frames,
    std::string *error)
{
    if (!_evaluator) {
        // Loads the rig through the node's own compute.
        MPlug(thisMObject(), aOutControlMatrix).elementByLogicalIndex(0)
            .asMObject();
    }
    if (!_evaluator) {
        *error = "the rig is not loaded";
        return false;
    }
    // The node's next compute re-poses the evaluator from scratch, so it is
    // free to use here; its cached frame stays as it was.
    return _evaluator->Frames(_CurrentChannels(), overrides, paths, frames,
                              error);
}

bool
RigExecRigNode::Published(const std::string &path, std::vector<double> *out,
                          std::string *error)
{
    if (!_evaluator) {
        MPlug(thisMObject(), aOutControlMatrix).elementByLogicalIndex(0)
            .asMObject();
    }
    if (!_evaluator) {
        *error = "the rig is not loaded";
        return false;
    }
    return _evaluator->Published(_CurrentChannels(), path, out, error);
}

MMatrix
RigExecRigNode::_Trs(int control, const Tables &tables,
                     const std::vector<double> &values) const
{
    // The control's local matrix exactly as Maya composes it from the same
    // channel values, so the offset cancels it to the rig's frame.
    const int *slots = &tables.trs[size_t(control) * 9];
    const auto value = [&](int k, double fallback) {
        return slots[k] >= 0 && size_t(slots[k]) < values.size()
                   ? values[size_t(slots[k])] * tables.sign[size_t(slots[k])]
                   : fallback;
    };
    const double toRadians = 3.14159265358979323846 / 180.0;
    MTransformationMatrix transform;
    const double scale[3] = {value(6, 1.0), value(7, 1.0), value(8, 1.0)};
    transform.setScale(scale, MSpace::kTransform);
    const int order = tables.rotateOrder[size_t(control)];
    transform.rotateTo(MEulerRotation(
        value(3, 0.0) * toRadians, value(4, 0.0) * toRadians,
        value(5, 0.0) * toRadians,
        kOrders[order >= 0 && order < 6 ? order : 0]));
    transform.setTranslation(
        MVector(value(0, 0.0), value(1, 0.0), value(2, 0.0)),
        MSpace::kTransform);
    return transform.asMatrix();
}

void
RigExecRigNode::_WriteControls(MDataBlock &data, const Tables &tables,
                               const Frame &frame,
                               const std::vector<double> &values)
{
    const size_t controls = tables.parent.size();
    std::vector<MMatrix> frames(controls);
    for (size_t c = 0; c < controls; ++c) {
        frames[c] = ReadMatrix(frame->data() + tables.controlMatricesOffset +
                               128 * c);
    }
    MArrayDataHandle handle = data.outputArrayValue(aOutControlMatrix);
    MArrayDataBuilder builder = handle.builder();
    for (size_t c = 0; c < controls; ++c) {
        MMatrix offset = _Trs(int(c), tables, values).inverse() * frames[c];
        const int parent = tables.parent[c];
        if (parent >= 0 && size_t(parent) < controls) {
            offset = offset * frames[size_t(parent)].inverse();
        }
        builder.addElement(unsigned(c)).setMMatrix(offset);
    }
    handle.set(builder);
    handle.setAllClean();
}

void
RigExecRigNode::_WriteMeshMatrices(MDataBlock &data, const Tables &tables,
                                   const Frame &frame)
{
    MArrayDataHandle handle = data.outputArrayValue(aOutMeshMatrix);
    MArrayDataBuilder builder = handle.builder();
    for (size_t m = 0; m < tables.pointCount.size(); ++m) {
        builder.addElement(unsigned(m)).setMMatrix(ReadMatrix(
            frame->data() + tables.meshMatricesOffset + 128 * m));
    }
    handle.set(builder);
    handle.setAllClean();
}

void
RigExecRigNode::_WriteShaderMatrices(MDataBlock &data, const Tables &tables,
                                     const Frame &frame)
{
    MArrayDataHandle matrices = data.outputArrayValue(aOutShaderMatrix);
    MArrayDataBuilder matrixBuilder = matrices.builder();
    MArrayDataHandle values = data.outputArrayValue(aOutShaderValue);
    MArrayDataBuilder valueBuilder = values.builder();
    for (size_t s = 0; s < tables.shaderCount; ++s) {
        const uint8_t *bytes =
            frame->data() + tables.shaderMatricesOffset + 128 * s;
        matrixBuilder.addElement(unsigned(s)).setMMatrix(ReadMatrix(bytes));
        double flat[16];
        std::memcpy(flat, bytes, sizeof(flat));
        for (unsigned k = 0; k < 16; ++k) {
            valueBuilder.addElement(unsigned(16 * s + k)).setDouble(flat[k]);
        }
    }
    matrices.set(matrixBuilder);
    matrices.setAllClean();
    values.set(valueBuilder);
    values.setAllClean();
}

void
RigExecRigNode::_WriteMeshes(MDataBlock &data, const Tables &tables,
                             const Frame &frame, bool current)
{
    const size_t meshes = tables.pointCount.size();
    MArrayDataHandle handle = data.outputArrayValue(aOutMesh);
    // The meshes already written stay, and only those whose points moved
    // since are given the new ones: copying every mesh, and handing the
    // viewport every mesh again, costs more than the rig.
    // Not for another time, whose meshes start from the source meshes.
    if (current && _meshesWritten && _meshFrame) {
        for (size_t m = 0; m < meshes; ++m) {
            const size_t bytes = size_t(tables.pointCount[m]) * 12;
            const uint8_t *now = frame->data() + tables.pointOffset[m];
            if (std::memcmp(now, _meshFrame->data() + tables.pointOffset[m],
                            bytes) == 0 ||
                !handle.jumpToElement(unsigned(m))) {
                continue;
            }
            MObject mesh = handle.outputValue().asMesh();
            if (!mesh.isNull()) {
                MFnMesh(mesh).setPoints(Points(now, tables.pointCount[m]),
                                        MSpace::kObject);
            }
        }
        _meshFrame = frame;
        handle.setAllClean();
        return;
    }
    MArrayDataHandle inputs = data.inputArrayValue(aInMesh);
    MArrayDataBuilder builder = handle.builder();
    for (size_t m = 0; m < meshes; ++m) {
        if (!inputs.jumpToElement(unsigned(m))) {
            continue;
        }
        MObject source = inputs.inputValue().asMesh();
        if (source.isNull() ||
            MFnMesh(source).numVertices() != tables.pointCount[m]) {
            continue;
        }
        // The whole mesh, every UV set included, with the rig's points.
        MDataHandle element = builder.addElement(unsigned(m));
        element.copy(inputs.inputValue());
        MFnMesh(element.asMesh())
            .setPoints(Points(frame->data() + tables.pointOffset[m],
                              tables.pointCount[m]),
                       MSpace::kObject);
    }
    handle.set(builder);
    handle.setAllClean();
    if (current) {
        _meshFrame = frame;
        _meshesWritten = true;
    }
}

MStatus
RigExecRigNode::setDependentsDirty(const MPlug &plug, MPlugArray &affected)
{
    _NoteDirty(plug);
    const MObject attribute =
        plug.isElement() ? plug.array().attribute() : plug.attribute();
    if (attribute == aInLinear || attribute == aInAngle) {
        _AddReach(plug, affected);
    }
    return MPxNode::setDependentsDirty(plug, affected);
}

void
RigExecRigNode::_AddReach(const MPlug &plug, MPlugArray &affected)
{
    const MObject self = thisMObject();
    const size_t channel = plug.isElement() ? plug.logicalIndex() : ~size_t(0);
    if (channel >= _reach.size() || !_tables) {
        // Not known yet: everything.
        for (const MObject &a :
             {aOutMesh, aOutMeshMatrix, aOutControlMatrix, aOutMeshFrame,
              aOutControlFrames, aOutShaderMatrix, aOutShaderValue}) {
            affected.append(MPlug(self, a));
        }
        return;
    }
    if (!_reachPlugsBuilt[channel]) {
        _reachPlugsBuilt[channel] = 1;
        const Tables &tables = *_tables;
        const rigExecMaya::ChannelReach &reach = _reach[channel];
        MPlugArray &plugs = _reachPlugs[channel];
        const auto element = [&](const MObject &a, uint32_t index) {
            plugs.append(MPlug(self, a).elementByLogicalIndex(index));
        };
        for (const uint32_t m : reach.meshes) {
            element(aOutMeshFrame, m);
            element(aOutMesh, m);
        }
        for (const uint32_t m : reach.meshMatrices) {
            element(aOutMeshMatrix, m);
        }
        // A control's offset is its frame under its parent's, less its own
        // channels: it changes with either frame, or with those channels.
        const size_t controls = tables.parent.size();
        std::vector<char> moved(controls, 0), offset(controls, 0);
        for (const uint32_t c : reach.controls) {
            if (c < controls) {
                moved[c] = 1;
                offset[c] = 1;
            }
        }
        for (size_t c = 0; c < controls; ++c) {
            const int parent = tables.parent[c];
            if (parent >= 0 && size_t(parent) < controls && moved[parent]) {
                offset[c] = 1;
            }
            for (int k = 0; k < 9; ++k) {
                if (tables.trs[c * 9 + size_t(k)] == int(channel)) {
                    offset[c] = 1;
                }
            }
        }
        for (size_t c = 0; c < controls; ++c) {
            if (offset[c]) {
                element(aOutControlMatrix, uint32_t(c));
            }
        }
        if (!reach.controls.empty()) {
            plugs.append(MPlug(self, aOutControlFrames));
        }
        for (const uint32_t sh : reach.shaders) {
            element(aOutShaderMatrix, sh);
            for (uint32_t k = 0; k < 16; ++k) {
                element(aOutShaderValue, 16 * sh + k);
            }
        }
    }
    const MPlugArray &plugs = _reachPlugs[channel];
    for (unsigned i = 0; i < plugs.length(); ++i) {
        affected.append(plugs[i]);
    }
}

void
RigExecRigNode::_LearnReach()
{
    if (!_evaluator || !_evaluator->Reach(&_reach)) {
        _reach.clear();
        return;
    }
    _reachPlugs.assign(_reach.size(), MPlugArray());
    _reachPlugsBuilt.assign(_reach.size(), 0);
    // The evaluation manager learned the old dependencies: have it learn
    // these.
    MGlobal::executeCommandOnIdle("evaluationManager -invalidate true");
}

void
RigExecRigNode::_CheckReach(const Frame &before,
                            const std::vector<double> &from,
                            const Frame &frame,
                            const std::vector<double> &to)
{
    const Tables &tables = *_tables;
    std::vector<char> meshes(tables.pointCount.size(), 0);
    std::vector<char> matrices(tables.pointCount.size(), 0);
    std::vector<char> controls(tables.parent.size(), 0);
    std::vector<char> shaders(tables.shaderCount, 0);
    for (size_t i = 0; i < from.size() && i < to.size() && i < _reach.size();
         ++i) {
        if (from[i] == to[i]) {
            continue;
        }
        for (const uint32_t m : _reach[i].meshes) {
            meshes[m] = 1;
        }
        for (const uint32_t m : _reach[i].meshMatrices) {
            matrices[m] = 1;
        }
        for (const uint32_t c : _reach[i].controls) {
            controls[c] = 1;
        }
        for (const uint32_t sh : _reach[i].shaders) {
            shaders[sh] = 1;
        }
    }
    const auto same = [&](size_t offset, size_t bytes) {
        return std::memcmp(before->data() + offset, frame->data() + offset,
                           bytes) == 0;
    };
    MString moved;
    for (size_t m = 0; m < meshes.size(); ++m) {
        if (!meshes[m] &&
            !same(tables.pointOffset[m], size_t(tables.pointCount[m]) * 12)) {
            moved += MString(" mesh ") + unsigned(m);
        }
        if (!matrices[m] && !same(tables.meshMatricesOffset + 128 * m, 128)) {
            moved += MString(" mesh matrix ") + unsigned(m);
        }
    }
    for (size_t c = 0; c < controls.size(); ++c) {
        if (!controls[c] && !same(tables.controlMatricesOffset + 128 * c, 128)) {
            moved += MString(" control ") + unsigned(c);
        }
    }
    for (size_t sh = 0; sh < shaders.size(); ++sh) {
        if (!shaders[sh] && !same(tables.shaderMatricesOffset + 128 * sh, 128)) {
            moved += MString(" shader matrix ") + unsigned(sh);
        }
    }
    if (moved.length()) {
        MGlobal::displayWarning(
            "rigExecRig: these moved though no channel changed reaches them "
            "(please report it):" + moved);
    }
}

MStatus
RigExecRigNode::preEvaluation(const MDGContext &context,
                              const MEvaluationNode &evaluationNode)
{
    // What changed, when the evaluation manager plays the scene back (it
    // does not dirty plugs one by one then).
    if (context.isNormal()) {
        for (MEvaluationNodeIterator it = evaluationNode.iterator();
             !it.isDone(); it.next()) {
            _NoteDirty(it.plug());
        }
    }
    return MS::kSuccess;
}

void
RigExecRigNode::getCacheSetup(const MEvaluationNode &evaluationNode,
                              MNodeCacheDisablingInfo &disablingInfo,
                              MNodeCacheSetupInfo &setupInfo,
                              MObjectArray &monitoredAttributes) const
{
    MPxNode::getCacheSetup(evaluationNode, disablingInfo, setupInfo,
                           monitoredAttributes);
    setupInfo.setPreference(MNodeCacheSetupInfo::kWantToCacheByDefault,
                            true);
}

void
RigExecRigNode::configCache(const MEvaluationNode &evaluationNode,
                            MCacheSchema &schema) const
{
    MPxNode::configCache(evaluationNode, schema);
    for (const MObject &attribute :
         {aOutMeshMatrix, aOutControlMatrix, aOutMeshFrame, aOutFrame,
          aOutControlFrames, aOutShaderMatrix, aOutShaderValue}) {
        schema.add(attribute);
    }
}

void
RigExecRigNode::_NoteDirty(const MPlug &plug)
{
    // New source meshes are copied again on the next write.
    const MObject attribute =
        plug.isElement() ? plug.array().attribute() : plug.attribute();
    if (attribute == aInMesh || attribute == aMeshPointCount) {
        _meshesWritten = false;
    }
    if (attribute == aInLinear || attribute == aInAngle) {
        if (plug.isElement()) {
            _dirtyChannels.emplace_back(attribute == aInAngle,
                                        plug.logicalIndex());
        } else {
            _readAll = true;
        }
    } else if (attribute != aTime && attribute != aInMesh &&
               attribute != aOutMesh && attribute != aOutMeshMatrix &&
               attribute != aOutControlMatrix &&
               attribute != aOutMeshFrame && attribute != aOutFrame &&
               attribute != aOutControlFrames &&
               attribute != aOutShaderMatrix &&
               attribute != aOutShaderValue) {
        _tablesDirty = true;
    }
}

namespace {

// Every live node, for the fill timer. UI thread only.
std::set<RigExecRigNode *> gNodes;
MCallbackId gFillCallback = 0;

size_t
EnvironmentCount(const char *name, size_t fallback)
{
    if (const char *text = std::getenv(name)) {
        const long value = std::strtol(text, nullptr, 10);
        if (value >= 0) {
            return size_t(value);
        }
    }
    return fallback;
}

// Four times a second, not on Maya's idle event: an idle callback keeps the
// event loop spinning, and so a core busy, for as long as Maya is open.
void
OnFillTimer(float, float, void *)
{
    for (RigExecRigNode *node : gNodes) {
        node->FillPlaybackCache();
    }
}

}  // namespace

void
RigExecRigNode::postConstructor()
{
    // 2000 frames of the biped is about a gigabyte.
    _cache = std::make_shared<FrameCache>(
        EnvironmentCount("RIGEXEC_MAYA_CACHE_FRAMES", 2000));
    gNodes.insert(this);
    _checkReach = EnvironmentCount("RIGEXEC_MAYA_CHECK_REACH", 0) > 0;
}

RigExecRigNode::~RigExecRigNode()
{
    gNodes.erase(this);
}

void
RigExecRigNode::FillPlaybackCache()
{
    if (!_needsFill || !_frame || !_setup) {
        return;
    }
    _needsFill = false;
    const size_t workers = EnvironmentCount(
        "RIGEXEC_MAYA_FILL_WORKERS",
        std::max<size_t>(1, std::min<size_t>(
                                8, std::thread::hardware_concurrency() / 8)));
    if (workers == 0) {
        return;
    }

    // Which channels animation moves, and where each one comes from. A
    // channel whose source is not animated holds its current value on every
    // frame. A guess that is wrong costs nothing but a miss: the key is the
    // full request, so a frame filled from a wrong guess is never served.
    struct Animated {
        size_t index;
        MPlug source;
        bool angle;
    };
    std::vector<Animated> animated;
    const MObject self = thisMObject();
    for (const auto &[attribute, angle] :
         {std::make_pair(aInLinear, false), std::make_pair(aInAngle, true)}) {
        MPlug array(self, attribute);
        MIntArray indices;
        array.getExistingArrayAttributeIndices(indices);
        for (unsigned i = 0; i < indices.length(); ++i) {
            const MPlug element =
                array.elementByLogicalIndex(unsigned(indices[i]));
            const MPlug source = element.source();
            if (!source.isNull() && MAnimUtil::isAnimated(source) &&
                size_t(indices[i]) < _values.size()) {
                animated.push_back({size_t(indices[i]), source, angle});
            }
        }
    }

    const MTime::Unit unit = MTime::uiUnit();
    const double first = MAnimControl::minTime().as(unit);
    const double last = MAnimControl::maxTime().as(unit);
    std::vector<FillJob> jobs;
    for (double frame = first; frame <= last + 1e-9; frame += 1.0) {
        FillJob job{frame, _values};
        if (!animated.empty()) {
            MDGContext context(MTime(frame, unit));
            MDGContextGuard guard(context);
            for (const Animated &channel : animated) {
                job.values[channel.index] =
                    (channel.angle ? channel.source.asMAngle().asDegrees()
                                   : channel.source.asDouble()) *
                    _Sign(channel.index);
            }
        }
        if (!_cache->Find(job.values)) {
            jobs.push_back(std::move(job));
        }
    }
    if (jobs.empty()) {
        return;
    }
    // Nearest the current frame first: that is what plays next.
    const double now = _time;
    std::stable_sort(jobs.begin(), jobs.end(),
                     [now](const FillJob &a, const FillJob &b) {
                         const double da = a.time >= now ? a.time - now
                                                         : 1e6 + now - a.time;
                         const double db = b.time >= now ? b.time - now
                                                         : 1e6 + now - b.time;
                         return da < db;
                     });
    if (!_pool) {
        _pool.reset(new FillPool(_setup, _cache, workers));
    }
    _pool->Submit(std::move(jobs));
}

// rigExecCache [-wait] <node>: "cached pending" for one rigExecRig node.
// -wait first blocks until the pool has nothing left to do.
class RigExecCacheCommand : public MPxCommand
{
public:
    static void *creator() { return new RigExecCacheCommand(); }
    MStatus doIt(const MArgList &args) override
    {
        bool wait = false;
        MString name;
        for (unsigned i = 0; i < args.length(); ++i) {
            const MString arg = args.asString(i);
            if (arg == "-wait" || arg == "-w") {
                wait = true;
            } else {
                name = arg;
            }
        }
        for (RigExecRigNode *node : gNodes) {
            if (MFnDependencyNode(node->thisMObject()).name() != name) {
                continue;
            }
            node->FillPlaybackCache();
            while (wait && node->PendingFrames() > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            MIntArray result;
            result.append(int(node->CachedFrames()));
            result.append(int(node->PendingFrames()));
            setResult(result);
            return MS::kSuccess;
        }
        displayError("rigExecCache: no rigExecRig node named " + name);
        return MS::kFailure;
    }
};

// rigExecFrames <node> [-set <attribute path> <value>]... -frame <path>...
// The asset-space frames (16 doubles each, row-major) of the rig's controls
// or joints at the given paths, with the rig posed as Maya has it now and
// each -set value on top. Nothing in the scene changes. Matching IK/FK and
// space switches is built on this (rigexec_maya.matching).
class RigExecFramesCommand : public MPxCommand
{
public:
    static void *creator() { return new RigExecFramesCommand(); }
    MStatus doIt(const MArgList &args) override
    {
        MString name;
        std::vector<std::pair<std::string, double>> overrides;
        std::vector<std::string> paths;
        for (unsigned i = 0; i < args.length(); ++i) {
            const MString arg = args.asString(i);
            if ((arg == "-set" || arg == "-s") && i + 2 < args.length()) {
                const std::string path = args.asString(i + 1).asChar();
                overrides.emplace_back(path, args.asDouble(i + 2));
                i += 2;
            } else if ((arg == "-frame" || arg == "-f") &&
                       i + 1 < args.length()) {
                paths.push_back(args.asString(++i).asChar());
            } else {
                name = arg;
            }
        }
        for (RigExecRigNode *node : gNodes) {
            if (MFnDependencyNode(node->thisMObject()).name() != name) {
                continue;
            }
            std::vector<double> frames;
            std::string error;
            if (!node->Frames(overrides, paths, &frames, &error)) {
                displayError(("rigExecFrames: " + error).c_str());
                return MS::kFailure;
            }
            MDoubleArray result;
            for (double value : frames) {
                result.append(value);
            }
            setResult(result);
            return MS::kSuccess;
        }
        displayError("rigExecFrames: no rigExecRig node named " + name);
        return MS::kFailure;
    }
};

// rigExecStats
// How hard the rigs have worked since the plug-in loaded, summed over every
// rig node: computes, rig evaluations (computes the cache could not answer),
// the milliseconds each took, the milliseconds of computing spent writing
// the outputs, mesh deforms and their milliseconds. The frame-rate display
// reads it twice and shows the difference.
class RigExecStatsCommand : public MPxCommand
{
public:
    static void *creator() { return new RigExecStatsCommand(); }
    MStatus doIt(const MArgList &) override
    {
        MDoubleArray result;
        result.append(double(gComputes.load()));
        result.append(double(gEvaluations.load()));
        result.append(double(gComputeNs.load()) * 1e-6);
        result.append(double(gEvaluateNs.load()) * 1e-6);
        result.append(double(gWriteNs.load()) * 1e-6);
        result.append(double(gDeforms.load()));
        result.append(double(gDeformNs.load()) * 1e-6);
        setResult(result);
        return MS::kSuccess;
    }
};

// rigExecPublished <node> <property path>
// A value the rig publishes for switching without moving, with the rig
// posed as Maya has it now: an IK/FK match constraint's outputs:ikValues
// or outputs:fkValues, or a space-switched control's spaces:matchValues.
// Matched switches are built on this (rigexec_maya.matching).
class RigExecPublishedCommand : public MPxCommand
{
public:
    static void *creator() { return new RigExecPublishedCommand(); }
    MStatus doIt(const MArgList &args) override
    {
        if (args.length() != 2) {
            displayError("rigExecPublished <node> <property path>");
            return MS::kFailure;
        }
        const MString name = args.asString(0);
        const std::string path = args.asString(1).asChar();
        for (RigExecRigNode *node : gNodes) {
            if (MFnDependencyNode(node->thisMObject()).name() != name) {
                continue;
            }
            std::vector<double> values;
            std::string error;
            if (!node->Published(path, &values, &error)) {
                displayError(("rigExecPublished: " + error).c_str());
                return MS::kFailure;
            }
            MDoubleArray result;
            for (double value : values) {
                result.append(value);
            }
            setResult(result);
            return MS::kSuccess;
        }
        displayError("rigExecPublished: no rigExecRig node named " + name);
        return MS::kFailure;
    }
};

namespace rigExecMaya {

void
CountDeform(long long nanoseconds)
{
    gDeforms += 1;
    gDeformNs += nanoseconds;
}

}  // namespace rigExecMaya

MStatus
initializePlugin(MObject plugin)
{
    MFnPlugin fn(plugin, "RigExec", "0.1", "Any");
    MStatus status = rigExecMaya::RegisterFrameData(fn);
    if (status) {
        status = fn.registerNode("rigExecRig", RigExecRigNode::id,
                                 RigExecRigNode::creator,
                                 RigExecRigNode::initialize);
    }
    if (status) {
        status = rigExecMaya::RegisterDeformNode(fn);
    }
    if (status) {
        status = fn.registerCommand("rigExecCache",
                                    RigExecCacheCommand::creator);
    }
    if (status) {
        status = fn.registerCommand("rigExecFrames",
                                    RigExecFramesCommand::creator);
    }
    if (status) {
        status = fn.registerCommand("rigExecPublished",
                                    RigExecPublishedCommand::creator);
    }
    if (status) {
        status = fn.registerCommand("rigExecStats",
                                    RigExecStatsCommand::creator);
    }
    if (status) {
        status = rigExecMaya::RegisterHoverOverlay(fn);
    }
    if (status) {
        status = rigExecMaya::RegisterTouchOverlay(fn);
    }
    if (status) {
        status = rigExecMaya::RegisterControlsDraw(fn);
    }
    if (status) {
        gFillCallback = MTimerMessage::addTimerCallback(0.25f, OnFillTimer,
                                                        nullptr, &status);
    }
    return status;
}

MStatus
uninitializePlugin(MObject plugin)
{
    if (gFillCallback) {
        MMessage::removeCallback(gFillCallback);
        gFillCallback = 0;
    }
    rigExecMaya::TaskPool::ReleaseShared();
    MFnPlugin fn(plugin);
    rigExecMaya::DeregisterControlsDraw(fn);
    rigExecMaya::DeregisterTouchOverlay(fn);
    rigExecMaya::DeregisterHoverOverlay(fn);
    fn.deregisterCommand("rigExecStats");
    fn.deregisterCommand("rigExecPublished");
    fn.deregisterCommand("rigExecFrames");
    fn.deregisterCommand("rigExecCache");
    rigExecMaya::DeregisterDeformNode(fn);
    fn.deregisterNode(RigExecRigNode::id);
    rigExecMaya::DeregisterFrameData(fn);
    return MS::kSuccess;
}
