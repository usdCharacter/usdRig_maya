// rigExecDeform: puts one of a rigExecRig's meshes into Maya's own
// deformer chain. Maya then only updates the points it draws, rather than
// taking a whole new mesh on every pose, which is most of the cost of a
// redraw.
//
// The rig's points for the mesh come in through the rigFrame connection.
// With GPU Override on (Preferences > Animation), the points go straight
// to the graphics card and Maya works out the mesh's normals there.
#include "deformNode.h"
#include "frameData.h"
#include "rigEvaluator.h"

#define MNoVersionString
#define MNoPluginEntry
#include <maya/MFnPlugin.h>

#include <maya/MDataBlock.h>
#include <maya/MDataHandle.h>
#include <maya/MEvaluationNode.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MGPUDeformerRegistry.h>
#include <maya/MItGeometry.h>
#include <maya/MOpenCLInfo.h>
#include <maya/MPointArray.h>
#include <maya/MProfiler.h>
#include <maya/MPxDeformerNode.h>
#include <maya/MPxGPUDeformer.h>
#include <maya/MStringArray.h>

#include <chrono>
#include <cstring>
#include <vector>

using rigExecMaya::FrameData;

namespace {

const MString kGpuRegistrant("rigExecDeform");

class DeformNode : public MPxDeformerNode
{
public:
    static const MTypeId id;
    static MObject aRigFrame;

    static void *creator() { return new DeformNode(); }
    static MStatus initialize();
    // Reads only its own inputs, so meshes deform side by side.
    SchedulingType schedulingType() const override { return kParallel; }
    MStatus deform(MDataBlock &data, MItGeometry &iter, const MMatrix &,
                   unsigned) override;
};

const MTypeId DeformNode::id(0x13a18c);
MObject DeformNode::aRigFrame;

MStatus
DeformNode::initialize()
{
    MFnTypedAttribute typed;
    // From the rig node's outMeshFrame: the mesh's points as the rig posed
    // them.
    aRigFrame = typed.create("rigFrame", "rfr", FrameData::id);
    typed.setStorable(false);
    MStatus status = addAttribute(aRigFrame);
    if (status) {
        status = attributeAffects(aRigFrame, outputGeom);
    }
    return status;
}

// Times one deform for the frame-rate display.
struct Tally
{
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    ~Tally()
    {
        rigExecMaya::CountDeform(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
    }
};

MStatus
DeformNode::deform(MDataBlock &data, MItGeometry &iter, const MMatrix &,
                   unsigned)
{
    MDataHandle handle = data.inputValue(aRigFrame);
    Tally tally;
    MProfilingScope scope(rigExecMaya::ProfilerCategory(),
                          MProfiler::kColorB_L1, "rigExecDeform");
    const FrameData *points = FrameData::From(handle);
    const float weight = data.inputValue(envelope).asFloat();
    const uint8_t *bytes = points ? points->Bytes(12) : nullptr;
    if (!bytes || weight <= 0.0f || points->count != size_t(iter.count())) {
        return MS::kSuccess;
    }
    const int count = int(points->count);
    float xyz[3];
    MPointArray positions;
    if (weight >= 1.0f) {
        positions.setLength(unsigned(count));
        for (int i = 0; i < count; ++i) {
            std::memcpy(xyz, bytes + 12 * size_t(i), 12);
            positions[unsigned(i)] = MPoint(xyz[0], xyz[1], xyz[2]);
        }
    } else {
        iter.allPositions(positions);
        for (int i = 0; i < count; ++i) {
            std::memcpy(xyz, bytes + 12 * size_t(i), 12);
            MPoint &point = positions[unsigned(i)];
            point += (MPoint(xyz[0], xyz[1], xyz[2]) - point) * weight;
        }
    }
    return iter.setAllPositions(positions);
}

// The same deformer under GPU Override: the rig's points are copied into
// the mesh's buffer on the graphics card.
class GpuDeform : public MPxGPUDeformer
{
public:
    DeformerStatus evaluate(MDataBlock &block, const MEvaluationNode &,
                            const MPlug &, const MPlugArray &,
                            const MGPUDeformerData &input,
                            MGPUDeformerData &output) override
    {
        MDataHandle handle = block.inputValue(DeformNode::aRigFrame);
        Tally tally;
        MProfilingScope scope(rigExecMaya::ProfilerCategory(),
                              MProfiler::kColorB_L1, "rigExecDeform (GPU)");
        const MGPUDeformerBuffer positions =
            input.getBuffer(sPositionsName());
        if (!positions.isValid()) {
            return kDeformerFailure;
        }
        const FrameData *points = FrameData::From(handle);
        const uint8_t *bytes = points ? points->Bytes(12) : nullptr;
        if (!bytes || block.inputValue(DeformNode::envelope).asFloat() <= 0 ||
            points->count != positions.elementCount()) {
            return kDeformerPassThrough;
        }
        const unsigned size = positions.elementSizeInBytes();
        const void *source = bytes;
        if (size == 16) {
            // Points padded to four floats.
            _padded.assign(points->count * 4, 1.0f);
            for (size_t i = 0; i < points->count; ++i) {
                std::memcpy(&_padded[i * 4], bytes + 12 * i, 12);
            }
            source = _padded.data();
        } else if (size != 12) {
            return kDeformerFailure;
        }
        MGPUDeformerBuffer result = createOutputBuffer(positions);
        if (!result.isValid()) {
            return kDeformerFailure;
        }
        cl_event ready = positions.bufferReadyEvent().get();
        MAutoCLEvent written;
        const cl_int error = clEnqueueWriteBuffer(
            MOpenCLInfo::getMayaDefaultOpenCLCommandQueue(),
            result.buffer().get(), CL_TRUE, 0, size_t(size) * points->count,
            source, ready ? 1 : 0, ready ? &ready : nullptr,
            written.getReferenceForAssignment());
        if (error != CL_SUCCESS) {
            MOpenCLInfo::checkCLErrorStatus(error);
            return kDeformerFailure;
        }
        result.setBufferReadyEvent(written);
        output.setBuffer(result);
        return kDeformerSuccess;
    }

    void terminate() override { _padded.clear(); }

private:
    std::vector<float> _padded;
};

class GpuDeformInfo : public MGPUDeformerRegistrationInfo
{
public:
    MPxGPUDeformer *createGPUDeformer() override { return new GpuDeform(); }

    bool validateNodeInGraph(MDataBlock &, const MEvaluationNode &,
                             const MPlug &, MStringArray *) override
    {
        return true;
    }

    bool validateNodeValues(MDataBlock &block, const MEvaluationNode &,
                            const MPlug &plug,
                            MStringArray *messages) override
    {
        // A mesh half way between its own points and the rig's stays on
        // the CPU.
        const float weight =
            block.inputValue(DeformNode::envelope).asFloat();
        if (weight > 0.0f && weight < 1.0f) {
            MOpenCLInfo::appendMessage(
                messages,
                "%s: an envelope between 0 and 1 is only supported off the "
                "GPU",
                MFnDependencyNode(plug.node()).name().asChar());
            return false;
        }
        return true;
    }
};

GpuDeformInfo gGpuDeformInfo;

}  // namespace

namespace rigExecMaya {

MStatus
RegisterDeformNode(MFnPlugin &plugin)
{
    MStatus status = plugin.registerNode(
        "rigExecDeform", DeformNode::id, DeformNode::creator,
        DeformNode::initialize, MPxNode::kDeformerNode);
    if (status) {
        status = MGPUDeformerRegistry::registerGPUDeformerCreator(
            "rigExecDeform", kGpuRegistrant, &gGpuDeformInfo);
    }
    if (status) {
        status = MGPUDeformerRegistry::addConditionalAttribute(
            "rigExecDeform", kGpuRegistrant, MPxDeformerNode::envelope);
    }
    return status;
}

void
DeregisterDeformNode(MFnPlugin &plugin)
{
    MGPUDeformerRegistry::deregisterGPUDeformerCreator("rigExecDeform",
                                                       kGpuRegistrant);
    plugin.deregisterNode(DeformNode::id);
}

}  // namespace rigExecMaya
