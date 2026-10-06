// rigExecRig: drives a Maya scene from a RigExec rig evaluated in the
// worker process (see workerProcess.h). No USD is linked here: Maya's own
// USD has the same library names and an older OpenExec.
//
// Inputs are every animator channel (inLinear / inAngle, indexed by the
// rig's channel order) and the time. Outputs:
//
//   outMesh[i]           mesh i's deformed points on a copy of inMesh[i]
//   outMeshMatrix[i]     mesh i's world matrix
//   outControlMatrix[c]  control c's offsetParentMatrix: with the control's
//                        own translate/rotate/scale composed on top, it
//                        lands exactly on the frame the rig computed
//
// The tables (control parents, rotate orders, TRS channel slots, channel
// defaults, mesh point counts) are set once by rigexec_maya.scene, which
// reads them from the worker's description.

#include "frameCache.h"
#include "workerProcess.h"

#include <maya/MAngle.h>
#include <maya/MAnimControl.h>
#include <maya/MAnimUtil.h>
#include <maya/MArgList.h>
#include <maya/MDGContext.h>
#include <maya/MDGContextGuard.h>
#include <maya/MTimerMessage.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MPxCommand.h>
#include <maya/MArrayDataBuilder.h>
#include <maya/MArrayDataHandle.h>
#include <maya/MDataBlock.h>
#include <maya/MDataHandle.h>
#include <maya/MDoubleArray.h>
#include <maya/MEulerRotation.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnDoubleArrayData.h>
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
#include <maya/MPxNode.h>
#include <maya/MTime.h>
#include <maya/MTransformationMatrix.h>
#include <maya/MVector.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <chrono>
#include <thread>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using rigExecMaya::FillJob;
using rigExecMaya::FillPool;
using rigExecMaya::Frame;
using rigExecMaya::FrameCache;
using rigExecMaya::WorkerProcess;

namespace {

// Where the plugin was loaded from, for the worker launcher and the RigExec
// checkout or install it belongs to.
std::string gPluginDirectory;

std::string
Parent(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".")
                                      : path.substr(0, slash);
}

std::string
Launcher()
{
    // Beside the plugin in this module; RIGEXEC_ROOT only picks the RigExec
    // checkout the launcher runs under.
    const std::string worker = Parent(gPluginDirectory) + "/worker/";
#ifdef _WIN32
    return worker + "launch_worker.bat";
#else
    return worker + "launch_worker.sh";
#endif
}

std::string
RigExecRoot()
{
    if (const char *explicitRoot = std::getenv("RIGEXEC_ROOT")) {
        return explicitRoot;
    }
    // plug-ins -> rigExecMaya -> this sidecar, staged beside the usdRig
    // checkout.
    return Parent(Parent(Parent(gPluginDirectory))) + "/usdRig";
}

// One worker per (stage, rig) in this Maya process, shared by every node
// that names it.
std::map<std::string, std::unique_ptr<WorkerProcess>> gWorkers;

WorkerProcess *
Worker(const std::string &stage, const std::string &rigPath,
       std::string *error)
{
    const std::string key = stage + "\n" + rigPath;
    std::unique_ptr<WorkerProcess> &worker = gWorkers[key];
    if (worker && worker->IsRunning()) {
        return worker.get();
    }
    worker.reset(new WorkerProcess());
    if (!worker->Start(Launcher(), RigExecRoot(), stage, rigPath, error)) {
        worker.reset();
        return nullptr;
    }
    return worker.get();
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

}  // namespace

class RigExecRigNode : public MPxNode
{
public:
    static const MTypeId id;
    static MObject aStagePath, aRigPath, aTime, aInLinear, aInAngle, aInMesh;
    static MObject aControlParent, aControlRotateOrder, aControlTrs;
    static MObject aChannelDefault, aMeshPointCount;
    static MObject aOutMesh, aOutMeshMatrix, aOutControlMatrix;

    static void *creator() { return new RigExecRigNode(); }
    static MStatus initialize();
    void postConstructor() override;
    ~RigExecRigNode() override;
    MStatus compute(const MPlug &plug, MDataBlock &data) override;

    // Called from a timer on the UI thread: queues the playback range's
    // missing frames for the fill pool (see frameCache.h).
    void FillPlaybackCache();
    size_t CachedFrames() const { return _cache->Size(); }
    size_t PendingFrames() const { return _pool ? _pool->Pending() : 0; }

private:
    struct Tables {
        std::vector<int> parent, rotateOrder, trs, pointCount;
        std::vector<double> defaults;
        size_t controlMatricesOffset = 0, meshMatricesOffset = 0, size = 0;
        std::vector<size_t> pointOffset;
    };

    bool _ReadTables(MDataBlock &data, Tables *tables);
    std::vector<double> _ReadChannels(MDataBlock &data, const Tables &tables);
    MMatrix _Trs(int control, const Tables &tables,
                 const std::vector<double> &values) const;
    void _WriteControls(MDataBlock &data, const Tables &tables);
    void _WriteMeshMatrices(MDataBlock &data, const Tables &tables);
    void _WriteMeshes(MDataBlock &data, const Tables &tables);

    double _time = 0.0;
    std::vector<double> _values;
    Frame _frame;
    // Every frame this node has evaluated or the pool has filled, keyed by
    // the full request; capacity from RIGEXEC_MAYA_CACHE_FRAMES.
    std::shared_ptr<FrameCache> _cache;
    std::unique_ptr<FillPool> _pool;
    std::string _stage, _rigPath;
    // Set by a compute that had to ask its worker: the animation, or the
    // values it is played with, may have changed.
    bool _needsFill = false;
};

const MTypeId RigExecRigNode::id(0x0007F2A1);
MObject RigExecRigNode::aStagePath, RigExecRigNode::aRigPath,
    RigExecRigNode::aTime, RigExecRigNode::aInLinear,
    RigExecRigNode::aInAngle, RigExecRigNode::aInMesh,
    RigExecRigNode::aControlParent, RigExecRigNode::aControlRotateOrder,
    RigExecRigNode::aControlTrs, RigExecRigNode::aChannelDefault,
    RigExecRigNode::aMeshPointCount, RigExecRigNode::aOutMesh,
    RigExecRigNode::aOutMeshMatrix, RigExecRigNode::aOutControlMatrix;

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
    aMeshPointCount = typed.create("meshPointCount", "mpc",
                                   MFnData::kIntArray);

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

    const MObject inputs[] = {aStagePath, aRigPath, aTime, aInLinear,
                              aInAngle, aInMesh, aControlParent,
                              aControlRotateOrder, aControlTrs,
                              aChannelDefault, aMeshPointCount};
    const MObject outputs[] = {aOutMesh, aOutMeshMatrix, aOutControlMatrix};
    for (const MObject &a : inputs) {
        addAttribute(a);
    }
    for (const MObject &a : outputs) {
        addAttribute(a);
    }
    for (const MObject &source : inputs) {
        for (const MObject &target : outputs) {
            attributeAffects(source, target);
        }
    }
    return MS::kSuccess;
}

namespace {

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
    tables->size = offset;
    return true;
}

std::vector<double>
RigExecRigNode::_ReadChannels(MDataBlock &data, const Tables &tables)
{
    std::vector<double> values = tables.defaults;
    MArrayDataHandle linear = data.inputArrayValue(aInLinear);
    for (unsigned i = 0, n = linear.elementCount(); i < n; ++i, linear.next()) {
        const unsigned index = linear.elementIndex();
        if (index < values.size()) {
            values[index] = linear.inputValue().asDouble();
        }
    }
    MArrayDataHandle angle = data.inputArrayValue(aInAngle);
    for (unsigned i = 0, n = angle.elementCount(); i < n; ++i, angle.next()) {
        const unsigned index = angle.elementIndex();
        if (index < values.size()) {
            values[index] = angle.inputValue().asAngle().asDegrees();
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
        attribute != aOutControlMatrix) {
        return MS::kUnknownParameter;
    }
    Tables tables;
    if (!_ReadTables(data, &tables)) {
        MGlobal::displayError("rigExecRig: the rig tables do not agree");
        return MS::kFailure;
    }
    const std::string stage =
        data.inputValue(aStagePath).asString().asChar();
    const std::string rigPath =
        data.inputValue(aRigPath).asString().asChar();
    const double time =
        data.inputValue(aTime).asTime().as(MTime::uiUnit());
    std::vector<double> values = _ReadChannels(data, tables);

    if (stage != _stage || rigPath != _rigPath) {
        _pool.reset();
        _cache->Clear();
        _stage = stage;
        _rigPath = rigPath;
    }
    if (!_frame || time != _time || values != _values) {
        Frame frame = _cache->Find(time, values);
        if (!frame) {
            std::string error;
            WorkerProcess *worker = Worker(stage, rigPath, &error);
            std::vector<uint8_t> bytes;
            if (!worker || !worker->Evaluate(time, values, &bytes, &error)) {
                MGlobal::displayError(("rigExecRig: " + error).c_str());
                return MS::kFailure;
            }
            frame = std::make_shared<const std::vector<uint8_t>>(
                std::move(bytes));
            _cache->Insert(time, values, frame);
            _needsFill = true;
        }
        if (frame->size() != tables.size) {
            MGlobal::displayError(
                "rigExecRig: the worker's frame does not match this rig");
            return MS::kFailure;
        }
        _frame = std::move(frame);
        _time = time;
        _values.swap(values);
    }
    if (attribute == aOutControlMatrix) {
        _WriteControls(data, tables);
    } else if (attribute == aOutMeshMatrix) {
        _WriteMeshMatrices(data, tables);
    } else {
        _WriteMeshes(data, tables);
    }
    return MS::kSuccess;
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
                   ? values[size_t(slots[k])]
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
RigExecRigNode::_WriteControls(MDataBlock &data, const Tables &tables)
{
    const size_t controls = tables.parent.size();
    std::vector<MMatrix> frames(controls);
    for (size_t c = 0; c < controls; ++c) {
        frames[c] = ReadMatrix(_frame->data() + tables.controlMatricesOffset +
                               128 * c);
    }
    MArrayDataHandle handle = data.outputArrayValue(aOutControlMatrix);
    MArrayDataBuilder builder = handle.builder();
    for (size_t c = 0; c < controls; ++c) {
        MMatrix offset = _Trs(int(c), tables, _values).inverse() * frames[c];
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
RigExecRigNode::_WriteMeshMatrices(MDataBlock &data, const Tables &tables)
{
    MArrayDataHandle handle = data.outputArrayValue(aOutMeshMatrix);
    MArrayDataBuilder builder = handle.builder();
    for (size_t m = 0; m < tables.pointCount.size(); ++m) {
        builder.addElement(unsigned(m)).setMMatrix(ReadMatrix(
            _frame->data() + tables.meshMatricesOffset + 128 * m));
    }
    handle.set(builder);
    handle.setAllClean();
}

void
RigExecRigNode::_WriteMeshes(MDataBlock &data, const Tables &tables)
{
    MArrayDataHandle inputs = data.inputArrayValue(aInMesh);
    MArrayDataHandle handle = data.outputArrayValue(aOutMesh);
    MArrayDataBuilder builder = handle.builder();
    for (size_t m = 0; m < tables.pointCount.size(); ++m) {
        if (!inputs.jumpToElement(unsigned(m))) {
            continue;
        }
        MObject source = inputs.inputValue().asMesh();
        if (source.isNull()) {
            continue;
        }
        MFnMesh sourceMesh(source);
        const unsigned count = unsigned(tables.pointCount[m]);
        if (unsigned(sourceMesh.numVertices()) != count) {
            continue;
        }
        MFnMeshData container;
        MObject copy = container.create();
        MFnMesh().copy(source, copy);
        const float *points = reinterpret_cast<const float *>(
            _frame->data() + tables.pointOffset[m]);
        MFloatPointArray array(count);
        for (unsigned i = 0; i < count; ++i) {
            array.set(i, points[3 * i], points[3 * i + 1], points[3 * i + 2]);
        }
        MFnMesh(copy).setPoints(array, MSpace::kObject);
        builder.addElement(unsigned(m)).setMObject(copy);
    }
    handle.set(builder);
    handle.setAllClean();
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
}

RigExecRigNode::~RigExecRigNode()
{
    gNodes.erase(this);
}

void
RigExecRigNode::FillPlaybackCache()
{
    if (!_needsFill || !_frame || _stage.empty()) {
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
                    channel.angle
                        ? channel.source.asMAngle().asDegrees()
                        : channel.source.asDouble();
            }
        }
        if (!_cache->Find(job.time, job.values)) {
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
        _pool.reset(new FillPool(Launcher(), RigExecRoot(), _stage, _rigPath,
                                 _cache, workers));
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

MStatus
initializePlugin(MObject plugin)
{
    MFnPlugin fn(plugin, "RigExec", "0.1", "Any");
    gPluginDirectory = fn.loadPath().asChar();
    MStatus status = fn.registerNode("rigExecRig", RigExecRigNode::id,
                                     RigExecRigNode::creator,
                                     RigExecRigNode::initialize);
    if (status) {
        status = fn.registerCommand("rigExecCache",
                                    RigExecCacheCommand::creator);
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
    gWorkers.clear();
    MFnPlugin fn(plugin);
    fn.deregisterCommand("rigExecCache");
    return fn.deregisterNode(RigExecRigNode::id);
}
