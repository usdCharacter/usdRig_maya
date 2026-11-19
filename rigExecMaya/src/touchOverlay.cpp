// TouchPose's highlight: the touch regions under the cursor and of the
// selected controls, tinted on the rig's own skin through Viewport 2.0.
//
// A rigExecTouchHighlight locator per touch layer reads its mesh (the
// deformed shape's worldMesh, so it follows every pose) and three face
// lists that rigexec_maya.touchpose sets: the hovered region, the lead
// selection (the control selected last) and the rest of the selection. It
// draws those faces in their colours, translucent, just in front of the
// skin. Nothing is drawn while its lists are empty.
#include "touchOverlay.h"

#define MNoVersionString
#define MNoPluginEntry
#include <maya/MFnPlugin.h>

#include <maya/MDrawRegistry.h>
#include <maya/MFnMesh.h>
#include <maya/MFnIntArrayData.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFrameContext.h>
#include <maya/MIntArray.h>
#include <maya/MMatrix.h>
#include <maya/MPoint.h>
#include <maya/MPlug.h>
#include <maya/MPointArray.h>
#include <maya/MPxDrawOverride.h>
#include <maya/MPxLocatorNode.h>
#include <maya/MHWGeometry.h>
#include <maya/MUIDrawManager.h>
#include <maya/MUserData.h>

#include <vector>

namespace {

const MString kClassification("drawdb/geometry/rigExecTouchHighlight");
const MString kRegistrant("rigExecTouchHighlight");

// The three states, drawn in this order so the hovered region lands on top.
const char *const kStates[] = {"selected", "lead", "hover"};
constexpr int kStateCount = 3;
// How far toward the camera the highlight is drawn, as a fraction of each
// point's distance from it.
constexpr double kTowardCamera = 0.001;

class TouchHighlightLocator : public MPxLocatorNode
{
public:
    static const MTypeId id;
    static MObject aInMesh;
    static MObject aFaces[kStateCount];
    static MObject aColors[kStateCount];
    static MObject aAlpha;

    static void *creator() { return new TouchHighlightLocator(); }
    static MStatus initialize();
    bool isBounded() const override { return false; }
};

const MTypeId TouchHighlightLocator::id(0x13a18b);
MObject TouchHighlightLocator::aInMesh;
MObject TouchHighlightLocator::aFaces[kStateCount];
MObject TouchHighlightLocator::aColors[kStateCount];
MObject TouchHighlightLocator::aAlpha;

MStatus
TouchHighlightLocator::initialize()
{
    MFnTypedAttribute typed;
    MFnNumericAttribute numeric;
    // Named apart from every locator's own attributes: a clash leaves the
    // attribute out without an error.
    aInMesh = typed.create("touchMesh", "tchm", MFnData::kMesh);
    typed.setStorable(false);
    MStatus status = addAttribute(aInMesh);
    if (!status) {
        return status;
    }
    for (int s = 0; s < kStateCount; ++s) {
        const MString state(kStates[s]);
        aFaces[s] = typed.create(state + "Faces", state + "F",
                                 MFnData::kIntArray);
        typed.setStorable(false);
        if (!(status = addAttribute(aFaces[s]))) {
            return status;
        }
        aColors[s] = numeric.createColor(state + "Color", state + "C");
        numeric.setDefault(1.0f, 0.6f, 0.0f);
        if (!(status = addAttribute(aColors[s]))) {
            return status;
        }
    }
    aAlpha = numeric.create("alpha", "al", MFnNumericData::kFloat, 0.5);
    numeric.setMin(0.0);
    numeric.setMax(1.0);
    return addAttribute(aAlpha);
}

// One draw: each state's triangles, world space, and its colour.
class TouchDrawData : public MUserData
{
public:
    TouchDrawData() : MUserData(false) {}
    MPointArray triangles[kStateCount];
    MColor colors[kStateCount];
};

class TouchHighlightDrawOverride : public MHWRender::MPxDrawOverride
{
public:
    static MHWRender::MPxDrawOverride *creator(const MObject &object)
    {
        return new TouchHighlightDrawOverride(object);
    }

    MHWRender::DrawAPI supportedDrawAPIs() const override
    {
        return MHWRender::kAllDevices;
    }

    bool isBounded(const MDagPath &, const MDagPath &) const override
    {
        return false;
    }

    bool hasUIDrawables() const override { return true; }

    MUserData *prepareForDraw(const MDagPath &path, const MDagPath &,
                              const MHWRender::MFrameContext &frameContext,
                              MUserData *old) override
    {
        auto *data = dynamic_cast<TouchDrawData *>(old);
        if (!data) {
            data = new TouchDrawData();
        }
        const MObject node = path.node();
        MPlug alphaPlug(node, TouchHighlightLocator::aAlpha);
        const float alpha = alphaPlug.asFloat();
        std::vector<MIntArray> faces(kStateCount);
        bool any = false;
        for (int s = 0; s < kStateCount; ++s) {
            data->triangles[s].clear();
            MObject list =
                MPlug(node, TouchHighlightLocator::aFaces[s]).asMObject();
            if (!list.isNull()) {
                faces[s] = MFnIntArrayData(list).array();
                any = any || faces[s].length() > 0;
            }
            MPlug color(node, TouchHighlightLocator::aColors[s]);
            data->colors[s] = MColor(color.child(0).asFloat(),
                                     color.child(1).asFloat(),
                                     color.child(2).asFloat(), alpha);
        }
        if (!any) {
            return data;
        }
        MObject mesh = MPlug(node, TouchHighlightLocator::aInMesh).asMObject();
        if (mesh.isNull()) {
            return data;
        }
        MFnMesh fn(mesh);
        // The mesh's own points, read in place: only the lit faces' are
        // used.
        MStatus status;
        const float *raw = fn.getRawPoints(&status);
        if (!status || !raw) {
            return data;
        }
        // Each point drawn a little toward the camera, by a fraction of its
        // distance from it: in front of the skin it lies on, whose depth it
        // would otherwise fight, and still behind anything really in front.
        const MMatrix viewInverse = frameContext.getMatrix(
            MHWRender::MFrameContext::kViewInverseMtx);
        const MPoint eye =
            MPoint(viewInverse[3][0], viewInverse[3][1], viewInverse[3][2]) *
            path.inclusiveMatrixInverse();
        const auto point = [&](int vertex) {
            const MPoint p(raw[3 * vertex], raw[3 * vertex + 1],
                           raw[3 * vertex + 2]);
            return p + (eye - p) * kTowardCamera;
        };
        const int polygons = fn.numPolygons();
        MIntArray vertices;
        for (int s = 0; s < kStateCount; ++s) {
            for (unsigned i = 0; i < faces[s].length(); ++i) {
                const int face = faces[s][i];
                if (face < 0 || face >= polygons) {
                    continue;
                }
                fn.getPolygonVertices(face, vertices);
                // A fan: the touch faces are the cage's quads and tris.
                for (unsigned v = 1; v + 1 < vertices.length(); ++v) {
                    data->triangles[s].append(point(vertices[0]));
                    data->triangles[s].append(point(vertices[v]));
                    data->triangles[s].append(point(vertices[v + 1]));
                }
            }
        }
        return data;
    }

    void addUIDrawables(const MDagPath &, MHWRender::MUIDrawManager &draw,
                        const MHWRender::MFrameContext &,
                        const MUserData *userData) override
    {
        const auto *data = dynamic_cast<const TouchDrawData *>(userData);
        if (!data) {
            return;
        }
        draw.beginDrawable(MHWRender::MUIDrawManager::kNonSelectable);
        draw.setDepthPriority(MHWRender::MRenderItem::sActiveWireDepthPriority);
        for (int s = 0; s < kStateCount; ++s) {
            if (data->triangles[s].length() == 0) {
                continue;
            }
            draw.setColor(data->colors[s]);
            draw.mesh(MHWRender::MUIDrawManager::kTriangles,
                      data->triangles[s]);
        }
        draw.endDrawable();
    }

private:
    explicit TouchHighlightDrawOverride(const MObject &object)
        : MHWRender::MPxDrawOverride(object, nullptr, true)
    {
    }
};

}  // namespace

namespace rigExecMaya {

MStatus
RegisterTouchOverlay(MFnPlugin &plugin)
{
    MStatus status = plugin.registerNode(
        "rigExecTouchHighlight", TouchHighlightLocator::id,
        TouchHighlightLocator::creator, TouchHighlightLocator::initialize,
        MPxNode::kLocatorNode, &kClassification);
    if (status) {
        status = MHWRender::MDrawRegistry::registerDrawOverrideCreator(
            kClassification, kRegistrant, TouchHighlightDrawOverride::creator);
    }
    return status;
}

void
DeregisterTouchOverlay(MFnPlugin &plugin)
{
    MHWRender::MDrawRegistry::deregisterDrawOverrideCreator(kClassification,
                                                            kRegistrant);
    plugin.deregisterNode(TouchHighlightLocator::id);
}

}  // namespace rigExecMaya
