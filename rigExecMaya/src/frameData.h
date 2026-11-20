// A part of one evaluated rig frame, passed from rigExecRig to the nodes
// that draw it (a mesh's deformer, the drawn controls) through an ordinary
// connection. Because the frame travels with the connection, whatever
// Maya evaluates (the frame on screen, a frame cached playback fills in the
// background, a frame on the GPU) gets the pose of its own time.
#ifndef RIGEXEC_MAYA_FRAME_DATA_H
#define RIGEXEC_MAYA_FRAME_DATA_H

#include "frameCache.h"

#include <maya/MPxData.h>
#include <maya/MString.h>
#include <maya/MTypeId.h>

#include <cstddef>

class MFnPlugin;
class MDataHandle;

namespace rigExecMaya {

class FrameData : public MPxData
{
public:
    static const MTypeId id;
    static const MString typeName;
    static void *creator() { return new FrameData(); }

    // The whole frame, the bytes this part starts at and the number of
    // items in it (points or matrices).
    Frame frame;
    size_t offset = 0;
    size_t count = 0;
    // Changes with every new pose.
    double generation = 0.0;

    // The data in `handle`, or null when it holds none.
    static const FrameData *From(MDataHandle &handle);
    // Puts a part of `frame` in `handle`.
    static void Set(MDataHandle &handle, const Frame &frame, size_t offset,
                    size_t count, double generation);

    // The bytes this part covers, or null when they are not all there.
    const uint8_t *Bytes(size_t itemSize) const
    {
        return frame && offset + count * itemSize <= frame->size()
                   ? frame->data() + offset
                   : nullptr;
    }

    void copy(const MPxData &) override;
    MTypeId typeId() const override { return id; }
    MString name() const override { return typeName; }
};

MStatus RegisterFrameData(MFnPlugin &plugin);
void DeregisterFrameData(MFnPlugin &plugin);

}  // namespace rigExecMaya

#endif
