// The hover picker's surface: an image drawn over a viewport in 2D, through
// Viewport 2.0, so the scene shows through everywhere the image is clear.
//
// rigexec_maya.hover paints the picker into an RGBA image per model panel
// and hands it over with
//
//     rigExecHoverImage -panel <modelPanel> -size <w> <h> -address <ptr>
//     rigExecHoverImage -panel <modelPanel> -clear
//
// (the address points at w * h * 4 bytes, top row first, straight alpha;
// the pixels are copied before the command returns). A rigExecHoverPicker
// locator in the scene draws each panel's image over that panel only.
// A Qt widget laid over the viewport can't do this: Maya's viewport is a
// native window, and a translucent child paints opaque over it.
#include "hoverOverlay.h"

// The plug-in entry points and version string live in rigExecRigNode.cpp.
#define MNoVersionString
#define MNoPluginEntry
#include <maya/MFnPlugin.h>

#include <maya/MArgList.h>
#include <maya/MDrawRegistry.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFrameContext.h>
#include <maya/MGlobal.h>
#include <maya/MPointArray.h>
#include <maya/MPxCommand.h>
#include <maya/MPxDrawOverride.h>
#include <maya/MPxLocatorNode.h>
#include <maya/MShaderManager.h>
#include <maya/MStateManager.h>
#include <maya/MTextureManager.h>
#include <maya/MUIDrawManager.h>
#include <maya/MUserData.h>
#include <maya/MViewport2Renderer.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct PanelImage {
    int width = 0;
    int height = 0;
    // Top row first, straight alpha, as the texture is sampled.
    std::vector<uint8_t> pixels;
    uint64_t version = 0;
};

std::mutex gImagesMutex;
std::map<std::string, PanelImage> gImages;
uint64_t gVersion = 0;

const MString kClassification("drawdb/geometry/rigExecHoverPicker");
const MString kRegistrant("rigExecHoverPicker");

class HoverPickerLocator : public MPxLocatorNode
{
public:
    static const MTypeId id;
    static void *creator() { return new HoverPickerLocator(); }
    static MStatus initialize() { return MS::kSuccess; }
    bool isBounded() const override { return false; }
};

const MTypeId HoverPickerLocator::id(0x0007F2A2);

// What one draw needs: the panel being drawn and its viewport size.
class HoverDrawData : public MUserData
{
public:
    HoverDrawData() : MUserData(false) {}
    std::string panel;
    int width = 0;
    int height = 0;
};

class HoverPickerDrawOverride : public MHWRender::MPxDrawOverride
{
public:
    static MHWRender::MPxDrawOverride *creator(const MObject &object)
    {
        return new HoverPickerDrawOverride(object);
    }

    ~HoverPickerDrawOverride() override { _Release(); }

    MHWRender::DrawAPI supportedDrawAPIs() const override
    {
        return MHWRender::kAllDevices;
    }

    bool isBounded(const MDagPath &, const MDagPath &) const override
    {
        return false;
    }

    bool hasUIDrawables() const override { return true; }

    MUserData *prepareForDraw(const MDagPath &, const MDagPath &,
                              const MHWRender::MFrameContext &context,
                              MUserData *old) override
    {
        auto *data = dynamic_cast<HoverDrawData *>(old);
        if (!data) {
            data = new HoverDrawData();
        }
        MString destination;
        context.renderingDestination(destination);
        data->panel = destination.asChar();
        int x = 0, y = 0;
        context.getViewportDimensions(x, y, data->width, data->height);
        return data;
    }

    void addUIDrawables(const MDagPath &,
                        MHWRender::MUIDrawManager &draw,
                        const MHWRender::MFrameContext &,
                        const MUserData *userData) override
    {
        const auto *data = dynamic_cast<const HoverDrawData *>(userData);
        if (!data || data->width <= 0 || data->height <= 0) {
            return;
        }
        MHWRender::MTexture *texture = _TextureFor(data->panel);
        if (!texture) {
            return;
        }
        // The image is made at the viewport's size; one drawn for an
        // older size waits for the next one rather than stretching.
        if (_size[0] != data->width || _size[1] != data->height) {
            return;
        }
        draw.beginDrawable(MHWRender::MUIDrawManager::kNonSelectable);
        draw.setColor(MColor(1.0f, 1.0f, 1.0f, 1.0f));
        draw.setTexture(texture);
        draw.setTextureSampler(MHWRender::MSamplerState::kMinMagMipPoint,
                               MHWRender::MSamplerState::kTexClamp);
        draw.rect2d(MPoint(data->width * 0.5, data->height * 0.5),
                    MVector(0.0, 1.0, 0.0), data->width * 0.5,
                    data->height * 0.5, true);
        draw.endDrawable();
    }

private:
    explicit HoverPickerDrawOverride(const MObject &object)
        // Always dirty: the image changes without the node changing, so
        // every redraw has to ask for it again.
        : MHWRender::MPxDrawOverride(object, nullptr, true)
    {
    }

    // The panel's image as a texture, re-uploaded when it changes.
    MHWRender::MTexture *_TextureFor(const std::string &panel)
    {
        std::lock_guard<std::mutex> lock(gImagesMutex);
        const auto found = gImages.find(panel);
        if (found == gImages.end() || found->second.pixels.empty()) {
            return nullptr;
        }
        const PanelImage &image = found->second;
        auto &slot = _textures[panel];
        if (slot.texture && slot.version == image.version) {
            _size[0] = slot.width;
            _size[1] = slot.height;
            return slot.texture;
        }
        MHWRender::MRenderer *renderer = MHWRender::MRenderer::theRenderer();
        MHWRender::MTextureManager *manager =
            renderer ? renderer->getTextureManager() : nullptr;
        if (!manager) {
            return nullptr;
        }
        if (slot.texture) {
            manager->releaseTexture(slot.texture);
            slot.texture = nullptr;
        }
        MHWRender::MTextureDescription description;
        description.setToDefault2DTexture();
        description.fWidth = unsigned(image.width);
        description.fHeight = unsigned(image.height);
        description.fDepth = 1;
        description.fBytesPerRow = unsigned(image.width) * 4;
        description.fBytesPerSlice = description.fBytesPerRow *
                                     unsigned(image.height);
        description.fMipmaps = 1;
        description.fArraySlices = 1;
        description.fFormat = MHWRender::kR8G8B8A8_UNORM;
        description.fTextureType = MHWRender::kImage2D;
        description.fEnvMapType = MHWRender::kEnvNone;
        slot.texture = manager->acquireTexture(
            "", description, image.pixels.data(), false);
        slot.version = image.version;
        slot.width = image.width;
        slot.height = image.height;
        _size[0] = slot.width;
        _size[1] = slot.height;
        return slot.texture;
    }

    void _Release()
    {
        MHWRender::MRenderer *renderer = MHWRender::MRenderer::theRenderer();
        MHWRender::MTextureManager *manager =
            renderer ? renderer->getTextureManager() : nullptr;
        for (auto &entry : _textures) {
            if (manager && entry.second.texture) {
                manager->releaseTexture(entry.second.texture);
            }
        }
        _textures.clear();
    }

    struct Slot {
        MHWRender::MTexture *texture = nullptr;
        uint64_t version = 0;
        int width = 0;
        int height = 0;
    };
    std::map<std::string, Slot> _textures;
    int _size[2] = {0, 0};
};

// rigExecHoverImage -panel <name> (-size <w> <h> -address <ptr> | -clear)
class HoverImageCommand : public MPxCommand
{
public:
    static void *creator() { return new HoverImageCommand(); }
    MStatus doIt(const MArgList &args) override
    {
        std::string panel;
        int width = 0, height = 0;
        uint64_t address = 0;
        bool clear = false;
        for (unsigned i = 0; i < args.length(); ++i) {
            const MString arg = args.asString(i);
            if (arg == "-panel" && i + 1 < args.length()) {
                panel = args.asString(++i).asChar();
            } else if (arg == "-size" && i + 2 < args.length()) {
                width = args.asInt(++i);
                height = args.asInt(++i);
            } else if (arg == "-address" && i + 1 < args.length()) {
                address = std::strtoull(args.asString(++i).asChar(),
                                        nullptr, 10);
            } else if (arg == "-clear") {
                clear = true;
            }
        }
        if (panel.empty()) {
            displayError("rigExecHoverImage: -panel is required");
            return MS::kFailure;
        }
        std::lock_guard<std::mutex> lock(gImagesMutex);
        if (clear) {
            gImages.erase(panel);
            return MS::kSuccess;
        }
        if (width <= 0 || height <= 0 || address == 0) {
            displayError("rigExecHoverImage: -size and -address are required");
            return MS::kFailure;
        }
        PanelImage &image = gImages[panel];
        image.width = width;
        image.height = height;
        image.pixels.resize(size_t(width) * size_t(height) * 4);
        std::memcpy(image.pixels.data(),
                    reinterpret_cast<const uint8_t *>(address),
                    image.pixels.size());
        image.version = ++gVersion;
        return MS::kSuccess;
    }
};

}  // namespace

namespace rigExecMaya {

MStatus
RegisterHoverOverlay(MFnPlugin &plugin)
{
    MStatus status = plugin.registerNode(
        "rigExecHoverPicker", HoverPickerLocator::id,
        HoverPickerLocator::creator, HoverPickerLocator::initialize,
        MPxNode::kLocatorNode, &kClassification);
    if (status) {
        status = MHWRender::MDrawRegistry::registerDrawOverrideCreator(
            kClassification, kRegistrant, HoverPickerDrawOverride::creator);
    }
    if (status) {
        status = plugin.registerCommand("rigExecHoverImage",
                                        HoverImageCommand::creator);
    }
    return status;
}

void
DeregisterHoverOverlay(MFnPlugin &plugin)
{
    plugin.deregisterCommand("rigExecHoverImage");
    MHWRender::MDrawRegistry::deregisterDrawOverrideCreator(kClassification,
                                                            kRegistrant);
    plugin.deregisterNode(HoverPickerLocator::id);
    std::lock_guard<std::mutex> lock(gImagesMutex);
    gImages.clear();
}

}  // namespace rigExecMaya
