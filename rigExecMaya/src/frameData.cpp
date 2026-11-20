#include "frameData.h"

#define MNoVersionString
#define MNoPluginEntry
#include <maya/MFnPlugin.h>

#include <maya/MDataHandle.h>
#include <maya/MFnPluginData.h>

namespace rigExecMaya {

const MTypeId FrameData::id(0x13a18e);
const MString FrameData::typeName("rigExecFrame");

const FrameData *
FrameData::From(MDataHandle &handle)
{
    return dynamic_cast<const FrameData *>(handle.asPluginData());
}

void
FrameData::Set(MDataHandle &handle, const Frame &frame, size_t offset,
               size_t count, double generation)
{
    MFnPluginData fn;
    MObject holder = fn.create(FrameData::id);
    FrameData *data = static_cast<FrameData *>(fn.data());
    data->frame = frame;
    data->offset = offset;
    data->count = count;
    data->generation = generation;
    handle.set(holder);
}

void
FrameData::copy(const MPxData &other)
{
    if (const auto *source = dynamic_cast<const FrameData *>(&other)) {
        frame = source->frame;
        offset = source->offset;
        count = source->count;
        generation = source->generation;
    }
}

MStatus
RegisterFrameData(MFnPlugin &plugin)
{
    return plugin.registerData(FrameData::typeName, FrameData::id,
                               FrameData::creator);
}

void
DeregisterFrameData(MFnPlugin &plugin)
{
    plugin.deregisterData(FrameData::id);
}

}  // namespace rigExecMaya
