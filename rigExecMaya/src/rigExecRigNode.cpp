// rigExecRig: drives a Maya scene from a RigExec rig, evaluated in Maya's
// own process by usdRig's runtime from a poseable .rigexec bake (see
// rigEvaluator.h). No USD is linked here: Maya's own USD has the same
// library names and an older OpenExec, and the runtime needs none.
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
// The tables (the rig file, channel and control paths, control parents,
// rotate orders, TRS channel slots, channel defaults, mesh placement) are
// set once by rigexec_maya.scene from the rig's export.

#include "frameCache.h"
#include "hoverOverlay.h"
#include "rigEvaluator.h"

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
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using rigExecMaya::FillJob;
using rigExecMaya::FillPool;
using rigExecMaya::Frame;
using rigExecMaya::FrameCache;
using rigExecMaya::RigEvaluator;
using rigExecMaya::RigSetup;

namespace {

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
    static MObject aRigFile, aBakeFrame, aChannelPath, aControlPath;
    static MObject aMeshPoints, aMeshProvider, aMeshLocal, aMeshWorld;
    static MObject aAssetToWorld;
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

    // For rigExecFrames: the frames of `paths` with the rig posed as Maya
    // has it now and `overrides` on top.
    bool Frames(const std::vector<std::pair<std::string, double>> &overrides,
                const std::vector<std::string> &paths,
                std::vector<double> *frames, std::string *error);

private:
    struct Tables {
        std::vector<int> parent, rotateOrder, trs, pointCount;
        std::vector<double> defaults;
        size_t controlMatricesOffset = 0, meshMatricesOffset = 0, size = 0;
        std::vector<size_t> pointOffset;
    };

    bool _ReadTables(MDataBlock &data, Tables *tables);
    // The loaded rig, (re)built when the rig file or its tables change.
    bool _Load(MDataBlock &data, const Tables &tables, std::string *error);
    std::vector<double> _CurrentChannels();
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
    std::shared_ptr<const RigSetup> _setup;
    std::unique_ptr<RigEvaluator> _evaluator;
    // Set by a compute that had to evaluate: the animation, or the values
    // it is played with, may have changed.
    bool _needsFill = false;
};

const MTypeId RigExecRigNode::id(0x0007F2A1);
MObject RigExecRigNode::aStagePath, RigExecRigNode::aRigPath,
    RigExecRigNode::aTime, RigExecRigNode::aInLinear,
    RigExecRigNode::aInAngle, RigExecRigNode::aInMesh,
    RigExecRigNode::aControlParent, RigExecRigNode::aControlRotateOrder,
    RigExecRigNode::aControlTrs, RigExecRigNode::aChannelDefault,
    RigExecRigNode::aMeshPointCount, RigExecRigNode::aRigFile,
    RigExecRigNode::aBakeFrame, RigExecRigNode::aChannelPath,
    RigExecRigNode::aControlPath, RigExecRigNode::aMeshPoints,
    RigExecRigNode::aMeshProvider, RigExecRigNode::aMeshLocal,
    RigExecRigNode::aMeshWorld, RigExecRigNode::aAssetToWorld,
    RigExecRigNode::aOutMesh,
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
                              aChannelDefault, aMeshPointCount, aRigFile,
                              aBakeFrame, aChannelPath, aControlPath,
                              aMeshPoints, aMeshProvider, aMeshLocal,
                              aMeshWorld, aAssetToWorld};
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
    const double time =
        data.inputValue(aTime).asTime().as(MTime::uiUnit());
    std::vector<double> values = _ReadChannels(data, tables);

    std::string error;
    if (!_Load(data, tables, &error)) {
        MGlobal::displayError(("rigExecRig: " + error).c_str());
        return MS::kFailure;
    }
    if (!_frame || values != _values) {
        Frame frame = _cache->Find(values);
        if (!frame) {
            std::vector<uint8_t> bytes;
            if (!_evaluator->Evaluate(values, &bytes, &error)) {
                MGlobal::displayError(("rigExecRig: " + error).c_str());
                return MS::kFailure;
            }
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
        _frame = std::move(frame);
        _values.swap(values);
    }
    // The playhead, which only orders the playback fill.
    _time = time;
    if (attribute == aOutControlMatrix) {
        _WriteControls(data, tables);
    } else if (attribute == aOutMeshMatrix) {
        _WriteMeshMatrices(data, tables);
    } else {
        _WriteMeshes(data, tables);
    }
    return MS::kSuccess;
}

bool
RigExecRigNode::_Load(MDataBlock &data, const Tables &tables,
                      std::string *error)
{
    const std::string file = data.inputValue(aRigFile).asString().asChar();
    std::vector<std::string> channels = StringArray(data, aChannelPath);
    if (_setup && _evaluator && _setup->file == file &&
        _setup->channelPaths == channels) {
        return true;
    }
    _pool.reset();
    _cache->Clear();
    _frame.reset();
    _evaluator.reset();
    _setup.reset();
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
    if (!_evaluator->Refused().empty()) {
        MGlobal::displayWarning(
            MString("rigExecRig: ") +
            unsigned(_evaluator->Refused().size()) +
            " channels are not inputs of the baked rig and do not move it");
    }
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
                values[index] = angle ? element.asMAngle().asDegrees()
                                      : element.asDouble();
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
                    channel.angle
                        ? channel.source.asMAngle().asDegrees()
                        : channel.source.asDouble();
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

MStatus
initializePlugin(MObject plugin)
{
    MFnPlugin fn(plugin, "RigExec", "0.1", "Any");
    MStatus status = fn.registerNode("rigExecRig", RigExecRigNode::id,
                                     RigExecRigNode::creator,
                                     RigExecRigNode::initialize);
    if (status) {
        status = fn.registerCommand("rigExecCache",
                                    RigExecCacheCommand::creator);
    }
    if (status) {
        status = fn.registerCommand("rigExecFrames",
                                    RigExecFramesCommand::creator);
    }
    if (status) {
        status = rigExecMaya::RegisterHoverOverlay(fn);
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
    MFnPlugin fn(plugin);
    rigExecMaya::DeregisterHoverOverlay(fn);
    fn.deregisterCommand("rigExecFrames");
    fn.deregisterCommand("rigExecCache");
    return fn.deregisterNode(RigExecRigNode::id);
}
