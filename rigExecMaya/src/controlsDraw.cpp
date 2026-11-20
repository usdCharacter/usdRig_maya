// The rig's control curves, drawn by one node instead of a curve shape per
// control.
//
// Maya draws a curve shape as an object of its own: every pose, each of a
// rig's hundreds of curves is updated and drawn on its own, and that costs
// more than the rig. A rigExecControls locator under the rig's controls
// group draws them all instead. It takes every control's frame from the rig
// node in one connection (no waiting on the control transforms) and draws
// every curve together, so a pose only moves the points of the curves
// whose controls moved.
//
// Clicking or marquee-selecting a drawn curve selects its control, as the
// curve shape would; selected controls are drawn in Maya's selection
// colours, the last selected as the lead. A curve is hidden with its
// control (and its control's parents), and with Show > NURBS Curves.
//
// rigexec_maya.scene builds the node from the curve shapes and then hides
// those; the curves are kept, so they can be shown again.
#include "controlsDraw.h"
#include "frameData.h"

#define MNoVersionString
#define MNoPluginEntry
#include <maya/MFnPlugin.h>

#include <maya/MCallbackIdArray.h>
#include <maya/MDagPath.h>
#include <maya/MDrawRegistry.h>
#include <maya/MFnDoubleArrayData.h>
#include <maya/MFnIntArrayData.h>
#include <maya/MFnMessageAttribute.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnPluginData.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFrameContext.h>
#include <maya/MGlobal.h>
#include <maya/MHWGeometry.h>
#include <maya/MMatrix.h>
#include <maya/MModelMessage.h>
#include <maya/MNodeMessage.h>
#include <maya/MObjectHandle.h>
#include <maya/MPlug.h>
#include <maya/MPlugArray.h>
#include <maya/MPxLocatorNode.h>
#include <maya/MPxSubSceneOverride.h>
#include <maya/MSelectionList.h>
#include <maya/MSelectionMask.h>
#include <maya/MShaderManager.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

using namespace MHWRender;

namespace {

const MString kClassification("drawdb/subscene/rigExecControls");
const MString kRegistrant("rigExecControls");
const MString kFilter("rigExecControls");
// The one item every curve is drawn in.
const MString kDrawnName("drawnCurves");

// Maya's selection colours for curves: the lead (last selected) and the
// rest of the selection.
const float kLeadColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
const float kSelectedColor[4] = {0.26f, 1.0f, 0.64f, 1.0f};

// How often a viewport checks whether a control has been shown or hidden,
// in seconds: often enough to look immediate, rarely enough to cost
// nothing while posing.
constexpr double kVisibilityEvery = 0.25;

// Bumped whenever the selection changes, so each viewport redraws.
std::atomic<unsigned> gSelectionVersion{0};
MCallbackId gSelectionCallback = 0;

class ControlsLocator : public MPxLocatorNode
{
public:
    static const MTypeId id;
    static MObject aRigFrame, aControl;
    static MObject aCurveControl, aCurveCounts, aCurvePoints, aCurveColors;

    static void *creator() { return new ControlsLocator(); }
    static MStatus initialize();
    bool isBounded() const override { return false; }
};

const MTypeId ControlsLocator::id(0x13a18d);
MObject ControlsLocator::aRigFrame, ControlsLocator::aControl;
MObject ControlsLocator::aCurveControl, ControlsLocator::aCurveCounts;
MObject ControlsLocator::aCurvePoints, ControlsLocator::aCurveColors;

MStatus
ControlsLocator::initialize()
{
    MFnTypedAttribute typed;
    MFnMessageAttribute message;
    // From the rig node's outControlFrames: every control's frame.
    aRigFrame = typed.create("rigFrame", "rfr", rigExecMaya::FrameData::id);
    typed.setStorable(false);
    // Each control's transform, by its index in the rig.
    aControl = message.create("control", "ctl");
    message.setArray(true);
    message.setIndexMatters(true);
    // The curves: each one's control, point count, points (x y z in its
    // control's space, a closed curve repeating its first point) and colour
    // (r g b).
    aCurveControl = typed.create("curveControl", "cvc", MFnData::kIntArray);
    aCurveCounts = typed.create("curveCounts", "cvn", MFnData::kIntArray);
    aCurvePoints = typed.create("curvePoints", "cvp", MFnData::kDoubleArray);
    aCurveColors = typed.create("curveColors", "cvr", MFnData::kDoubleArray);
    for (const MObject &a : {aRigFrame, aControl, aCurveControl, aCurveCounts,
                             aCurvePoints, aCurveColors}) {
        MStatus status = addAttribute(a);
        if (!status) {
            return status;
        }
    }
    return MS::kSuccess;
}

// The control frames the rig node last handed `node`, or null.
const rigExecMaya::FrameData *
ControlFrames(const MObject &node, MObject *holder)
{
    *holder = MPlug(node, ControlsLocator::aRigFrame).asMObject();
    if (holder->isNull()) {
        return nullptr;
    }
    return dynamic_cast<const rigExecMaya::FrameData *>(
        MFnPluginData(*holder).constData());
}

double
Generation(const MObject &node)
{
    MObject holder;
    const rigExecMaya::FrameData *frames = ControlFrames(node, &holder);
    return frames ? frames->generation : -1.0;
}

std::vector<int>
IntArray(const MObject &node, const MObject &attribute)
{
    MObject data = MPlug(node, attribute).asMObject();
    if (data.isNull()) {
        return {};
    }
    MIntArray array = MFnIntArrayData(data).array();
    std::vector<int> out(array.length());
    for (unsigned i = 0; i < array.length(); ++i) {
        out[i] = array[i];
    }
    return out;
}

std::vector<double>
DoubleArray(const MObject &node, const MObject &attribute)
{
    MObject data = MPlug(node, attribute).asMObject();
    if (data.isNull()) {
        return {};
    }
    MDoubleArray array = MFnDoubleArrayData(data).array();
    std::vector<double> out(array.length());
    for (unsigned i = 0; i < array.length(); ++i) {
        out[i] = array[i];
    }
    return out;
}

class ControlsOverride : public MPxSubSceneOverride
{
public:
    static MPxSubSceneOverride *creator(const MObject &object)
    {
        return new ControlsOverride(object);
    }

    explicit ControlsOverride(const MObject &object)
        : MPxSubSceneOverride(object), _node(object)
    {
        // The curves or the controls connected anew: build again.
        _callbacks.append(MNodeMessage::addAttributeChangedCallback(
            _node,
            [](MNodeMessage::AttributeMessage, MPlug &plug, MPlug &,
               void *self) {
                const MObject attribute = plug.isElement()
                                              ? plug.array().attribute()
                                              : plug.attribute();
                if (attribute != ControlsLocator::aRigFrame) {
                    static_cast<ControlsOverride *>(self)->_built = false;
                }
            },
            this));
    }

    ~ControlsOverride() override
    {
        MMessage::removeCallbacks(_callbacks);
        MShaderManager *shaders = _Shaders();
        for (auto &[key, shader] : _shaderCache) {
            if (shaders) {
                shaders->releaseShader(shader);
            }
        }
        if (shaders && _drawnShader) {
            shaders->releaseShader(_drawnShader);
        }
    }

    DrawAPI supportedDrawAPIs() const override { return kAllDevices; }

    bool requiresUpdate(const MSubSceneContainer &,
                        const MFrameContext &frameContext) const override
    {
        if (!_built ||
            Generation(_node) != _generation ||
            gSelectionVersion.load() != _selectionVersion ||
            _Hidden(frameContext) != _hidden) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - _visibilityChecked).count() <
            kVisibilityEvery) {
            return false;
        }
        _visibilityChecked = now;
        return _VisibilitySignature() != _visibility;
    }

    void update(MSubSceneContainer &container,
                const MFrameContext &frameContext) override
    {
        if (!_built) {
            _Build(container);
        }
        MObject holder;
        const rigExecMaya::FrameData *frames = ControlFrames(_node, &holder);
        _generation = frames ? frames->generation : -1.0;
        _selectionVersion = gSelectionVersion.load();
        _hidden = _Hidden(frameContext);
        // Which controls are shown: looked at again only now and then (see
        // requiresUpdate), as walking every control's parents takes time.
        const auto now = std::chrono::steady_clock::now();
        if (_visible.size() != _controls.size() ||
            std::chrono::duration<double>(now - _visibilityChecked).count() >=
                kVisibilityEvery) {
            _visibilityChecked = now;
            _visibility = _VisibilitySignature();
        }
        const std::vector<char> &visible = _visible;

        // Where each control is now, from the rig.
        const uint8_t *frame = frames ? frames->Bytes(128) : nullptr;
        const size_t count = frame ? frames->count : 0;
        MDagPath self;
        MDagPath::getAPathTo(_node, self);
        const MMatrix space = self.inclusiveMatrix();

        // Which controls are selected, and the lead.
        MSelectionList selection;
        MGlobal::getActiveSelectionList(selection);
        std::unordered_set<unsigned> selected;
        unsigned lead = 0;
        bool hasLead = false;
        for (unsigned i = 0; i < selection.length(); ++i) {
            MObject object;
            if (selection.getDependNode(i, object)) {
                const unsigned hash = MObjectHandle(object).hashCode();
                selected.insert(hash);
                lead = hash;
                hasLead = true;
            }
        }

        bool moved = false, recolored = false;
        for (Curve &curve : _curves) {
            MRenderItem *item = container.find(curve.name);
            if (!item) {
                continue;
            }
            const bool shown = !_hidden && frame &&
                               curve.control < visible.size() &&
                               visible[curve.control] &&
                               curve.control < count;
            if (shown != curve.shown) {
                item->enable(shown);
                curve.shown = shown;
                _linesChanged = true;
            }
            if (!shown) {
                continue;
            }
            // Only a curve whose control moved is placed again.
            double values[4][4];
            std::memcpy(values, frame + 128 * curve.control,
                        sizeof(values));
            const MMatrix matrix = MMatrix(values) * space;
            if (!curve.placed || matrix != curve.matrix) {
                item->setMatrix(&matrix);
                curve.matrix = matrix;
                curve.placed = true;
                _Place(curve);
                moved = true;
            }
            const unsigned hash =
                MObjectHandle(_controls[curve.control].node()).hashCode();
            const float *color = curve.color;
            if (selected.count(hash)) {
                color = hasLead && hash == lead ? kLeadColor : kSelectedColor;
            }
            if (std::memcmp(color, curve.drawnColor, sizeof(curve.drawnColor))) {
                std::memcpy(curve.drawnColor, color, sizeof(curve.drawnColor));
                for (size_t v = curve.first; v < curve.first + curve.count; ++v) {
                    std::memcpy(&_colors[v * 4], color, 4 * sizeof(float));
                }
                recolored = true;
            }
        }
        _Draw(container, moved, recolored);
    }

    bool getSelectionPath(const MRenderItem &item,
                          MDagPath &path) const override
    {
        // A curve selects its control.
        const auto found = _byName.find(item.name().asChar());
        if (found == _byName.end()) {
            return false;
        }
        const unsigned control = _curves[found->second].control;
        if (control >= _controls.size() || !_controls[control].isValid()) {
            return false;
        }
        path = _controls[control];
        return true;
    }

    bool getInstancedSelectionPath(const MRenderItem &item,
                                   const MIntersection &,
                                   MDagPath &path) const override
    {
        return getSelectionPath(item, path);
    }

private:
    struct Curve
    {
        MString name;
        unsigned control = 0;
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        // Its points in the drawn curves: the first, and how many.
        size_t first = 0, count = 0;
        // What it was last drawn with.
        float drawnColor[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        bool shown = true;
        MMatrix matrix;
        bool placed = false;
    };

    // Moves the curve's drawn points to where its matrix puts them.
    void _Place(const Curve &curve)
    {
        float m[4][4];
        curve.matrix.get(m);
        for (size_t v = curve.first; v < curve.first + curve.count; ++v) {
            const float *p = &_local[v * 3];
            float *out = &_world[v * 3];
            for (int j = 0; j < 3; ++j) {
                out[j] = p[0] * m[0][j] + p[1] * m[1][j] + p[2] * m[2][j] +
                         m[3][j];
            }
        }
    }

    // Hands the drawn curves what changed: the points that moved, the
    // colours, and which curves are shown.
    void _Draw(MSubSceneContainer &container, bool moved, bool recolored)
    {
        MRenderItem *item = container.find(kDrawnName);
        if (!item || !_positions || !_colorBuffer) {
            return;
        }
        const unsigned vertices = unsigned(_local.size() / 3);
        if (moved) {
            void *data = _positions->acquire(vertices, true);
            if (data) {
                std::memcpy(data, _world.data(), _world.size() * sizeof(float));
                _positions->commit(data);
            }
        }
        if (recolored) {
            void *data = _colorBuffer->acquire(vertices, true);
            if (data) {
                std::memcpy(data, _colors.data(),
                            _colors.size() * sizeof(float));
                _colorBuffer->commit(data);
            }
        }
        if (_linesChanged) {
            std::vector<unsigned> ends;
            for (const Curve &curve : _curves) {
                if (!curve.shown) {
                    continue;
                }
                for (size_t v = curve.first; v + 1 < curve.first + curve.count;
                     ++v) {
                    ends.push_back(unsigned(v));
                    ends.push_back(unsigned(v + 1));
                }
            }
            _lineCount = ends.size();
            if (!ends.empty()) {
                _lines.reset(new MIndexBuffer(MGeometry::kUnsignedInt32));
                void *data = _lines->acquire(unsigned(ends.size()), true);
                std::memcpy(data, ends.data(), ends.size() * sizeof(unsigned));
                _lines->commit(data);
            }
        }
        item->enable(_lineCount > 0);
        if (_lineCount == 0 || !(moved || _linesChanged)) {
            _linesChanged = false;
            return;
        }
        _linesChanged = false;
        MBoundingBox box;
        for (const Curve &curve : _curves) {
            if (!curve.shown) {
                continue;
            }
            for (size_t v = curve.first; v < curve.first + curve.count; ++v) {
                box.expand(MPoint(_world[v * 3], _world[v * 3 + 1],
                                  _world[v * 3 + 2]));
            }
        }
        MVertexBufferArray buffers;
        buffers.addBuffer("", _positions.get());
        buffers.addBuffer("", _colorBuffer.get());
        setGeometryForRenderItem(*item, buffers, *_lines, &box);
    }

    static MShaderManager *_Shaders()
    {
        MRenderer *renderer = MRenderer::theRenderer();
        return renderer ? const_cast<MShaderManager *>(
                              renderer->getShaderManager())
                        : nullptr;
    }

    MShaderInstance *_Shader(const float color[4])
    {
        const auto key = std::make_tuple(color[0], color[1], color[2]);
        auto found = _shaderCache.find(key);
        if (found != _shaderCache.end()) {
            return found->second;
        }
        MShaderManager *shaders = _Shaders();
        MShaderInstance *shader =
            shaders ? shaders->getStockShader(MShaderManager::k3dSolidShader)
                    : nullptr;
        if (shader) {
            shader->setParameter("solidColor", color);
        }
        _shaderCache[key] = shader;
        return shader;
    }

    bool _Hidden(const MFrameContext &frameContext) const
    {
        return (frameContext.objectTypeExclusions() &
                MFrameContext::kExcludeNurbsCurves) != 0;
    }

    // Which controls are shown now (kept in _visible), as a number that
    // changes when one is shown or hidden.
    size_t _VisibilitySignature() const
    {
        _visible.assign(_controls.size(), 0);
        size_t signature = 1469598103934665603ull;
        for (size_t c = 0; c < _controls.size(); ++c) {
            if (_drawn.size() > c && _drawn[c]) {
                const bool visible =
                    _controls[c].isValid() && _controls[c].isVisible();
                _visible[c] = visible;
                signature = (signature ^ (visible ? c + 1 : 0)) *
                            1099511628211ull;
            }
        }
        return signature;
    }

    void _Build(MSubSceneContainer &container)
    {
        _built = true;
        container.clear();
        _buffers.clear();
        _local.clear();
        _positions.reset();
        _colorBuffer.reset();
        _curves.clear();
        _byName.clear();

        // The controls' transforms, by rig index.
        MPlug controls(_node, ControlsLocator::aControl);
        MIntArray indices;
        controls.getExistingArrayAttributeIndices(indices);
        _controls.clear();
        for (unsigned i = 0; i < indices.length(); ++i) {
            const unsigned index = unsigned(indices[i]);
            if (_controls.size() <= index) {
                _controls.resize(index + 1);
            }
            const MPlug source =
                controls.elementByLogicalIndex(index).source();
            if (!source.isNull()) {
                MDagPath::getAPathTo(source.node(), _controls[index]);
            }
        }
        _drawn.assign(_controls.size(), false);

        const std::vector<int> owners =
            IntArray(_node, ControlsLocator::aCurveControl);
        const std::vector<int> counts =
            IntArray(_node, ControlsLocator::aCurveCounts);
        const std::vector<double> points =
            DoubleArray(_node, ControlsLocator::aCurvePoints);
        const std::vector<double> colors =
            DoubleArray(_node, ControlsLocator::aCurveColors);
        if (owners.size() != counts.size() ||
            colors.size() != counts.size() * 3) {
            return;
        }
        size_t at = 0;
        for (size_t k = 0; k < counts.size(); ++k) {
            const int count = counts[k];
            if (count < 2 || owners[k] < 0 ||
                (at + size_t(count)) * 3 > points.size()) {
                at += size_t(std::max(count, 0));
                continue;
            }
            Curve curve;
            curve.name = MString("curve") + int(k);
            curve.control = unsigned(owners[k]);
            for (int i = 0; i < 3; ++i) {
                curve.color[i] = float(colors[k * 3 + size_t(i)]);
            }
            if (curve.control < _drawn.size()) {
                _drawn[curve.control] = true;
            }

            // Each curve's own item is only for picking: the curves are all
            // drawn as one (kDrawnName), which costs Maya one draw instead
            // of hundreds.
            MRenderItem *item = MRenderItem::Create(
                curve.name, MRenderItem::NonMaterialSceneItem,
                MGeometry::kLines);
            item->setDrawMode(MGeometry::kSelectionOnly);
            item->depthPriority(MRenderItem::sDormantWireDepthPriority);
            item->setSelectionMask(
                MSelectionMask(MSelectionMask::kSelectNurbsCurves));
            item->setShader(_Shader(curve.color));
            container.add(item);
            curve.first = _local.size() / 3;
            curve.count = size_t(count);
            for (int i = 0; i < count * 3; ++i) {
                _local.push_back(float(points[at * 3 + size_t(i)]));
            }

            MVertexBufferDescriptor position("", MGeometry::kPosition,
                                             MGeometry::kFloat, 3);
            MVertexBuffer *vertices = new MVertexBuffer(position);
            float *xyz =
                static_cast<float *>(vertices->acquire(unsigned(count), true));
            MBoundingBox box;
            for (int i = 0; i < count; ++i) {
                for (int j = 0; j < 3; ++j) {
                    xyz[i * 3 + j] = float(points[(at + size_t(i)) * 3 + j]);
                }
                box.expand(MPoint(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]));
            }
            vertices->commit(xyz);
            MIndexBuffer *lines = new MIndexBuffer(MGeometry::kUnsignedInt32);
            unsigned *ends = static_cast<unsigned *>(
                lines->acquire(unsigned(count - 1) * 2, true));
            for (int i = 0; i + 1 < count; ++i) {
                ends[i * 2] = unsigned(i);
                ends[i * 2 + 1] = unsigned(i + 1);
            }
            lines->commit(ends);
            MVertexBufferArray buffers;
            buffers.addBuffer("", vertices);
            setGeometryForRenderItem(*item, buffers, *lines, &box);
            _buffers.emplace_back(vertices, lines);

            _byName[curve.name.asChar()] = _curves.size();
            _curves.push_back(curve);
            at += size_t(count);
        }

        // Every curve, drawn as one item in world space, each point in its
        // curve's colour.
        _world.assign(_local.size(), 0.0f);
        _colors.assign(_local.size() / 3 * 4, 1.0f);
        _linesChanged = true;
        _lineCount = 0;
        _lines.reset();
        MShaderManager *shaders = _Shaders();
        MShaderInstance *shader =
            shaders ? shaders->getStockShader(MShaderManager::k3dCPVSolidShader)
                    : nullptr;
        if (_local.empty() || !shader) {
            return;
        }
        if (_drawnShader && shaders) {
            shaders->releaseShader(_drawnShader);
        }
        _drawnShader = shader;
        MRenderItem *drawn = MRenderItem::Create(
            kDrawnName, MRenderItem::NonMaterialSceneItem, MGeometry::kLines);
        drawn->setDrawMode(MGeometry::kAll);
        drawn->depthPriority(MRenderItem::sDormantWireDepthPriority);
        drawn->setSelectionMask(MSelectionMask());
        drawn->setShader(shader);
        drawn->enable(false);
        container.add(drawn);
        _positions.reset(new MVertexBuffer(MVertexBufferDescriptor(
            "", MGeometry::kPosition, MGeometry::kFloat, 3)));
        _colorBuffer.reset(new MVertexBuffer(
            MVertexBufferDescriptor("", MGeometry::kColor, MGeometry::kFloat, 4)));
    }

    MObject _node;
    MCallbackIdArray _callbacks;
    bool _built = false;
    double _generation = -1.0;
    unsigned _selectionVersion = ~0u;
    bool _hidden = false;
    size_t _visibility = 0;
    mutable std::vector<char> _visible;
    mutable std::chrono::steady_clock::time_point _visibilityChecked;
    std::vector<MDagPath> _controls;
    std::vector<bool> _drawn;
    std::vector<Curve> _curves;
    std::map<std::string, size_t> _byName;
    std::map<std::tuple<float, float, float>, MShaderInstance *> _shaderCache;
    struct Buffers
    {
        Buffers(MVertexBuffer *v, MIndexBuffer *i) : vertices(v), indices(i) {}
        std::unique_ptr<MVertexBuffer> vertices;
        std::unique_ptr<MIndexBuffer> indices;
    };
    std::vector<Buffers> _buffers;
    // The drawn curves: every curve's points in its control's space and in
    // the world, their colours, and the buffers handed to Maya.
    std::vector<float> _local, _world, _colors;
    std::unique_ptr<MVertexBuffer> _positions, _colorBuffer;
    std::unique_ptr<MIndexBuffer> _lines;
    size_t _lineCount = 0;
    bool _linesChanged = true;
    MShaderInstance *_drawnShader = nullptr;
};

}  // namespace

namespace rigExecMaya {

MStatus
RegisterControlsDraw(MFnPlugin &plugin)
{
    MStatus status = plugin.registerNode(
        "rigExecControls", ControlsLocator::id, ControlsLocator::creator,
        ControlsLocator::initialize, MPxNode::kLocatorNode, &kClassification);
    if (status) {
        status = MDrawRegistry::registerSubSceneOverrideCreator(
            kClassification, kRegistrant, ControlsOverride::creator);
    }
    if (status) {
        // Shown and hidden with Show > Plugin Shapes > RigExec Controls,
        // rather than with the locators.
        plugin.registerDisplayFilter(kFilter, "RigExec Controls",
                                     kClassification);
        gSelectionCallback = MModelMessage::addCallback(
            MModelMessage::kActiveListModified,
            [](void *) { gSelectionVersion += 1; }, nullptr, &status);
    }
    return status;
}

void
DeregisterControlsDraw(MFnPlugin &plugin)
{
    if (gSelectionCallback) {
        MMessage::removeCallback(gSelectionCallback);
        gSelectionCallback = 0;
    }
    plugin.deregisterDisplayFilter(kFilter);
    MDrawRegistry::deregisterSubSceneOverrideCreator(kClassification,
                                                     kRegistrant);
    plugin.deregisterNode(ControlsLocator::id);
}

}  // namespace rigExecMaya
