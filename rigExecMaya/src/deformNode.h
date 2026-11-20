// rigExecDeform, the deformer that moves a mesh the rig poses (see
// deformNode.cpp).
#ifndef RIGEXEC_MAYA_DEFORM_NODE_H
#define RIGEXEC_MAYA_DEFORM_NODE_H

#include <maya/MStatus.h>

class MFnPlugin;

namespace rigExecMaya {

MStatus RegisterDeformNode(MFnPlugin &plugin);
void DeregisterDeformNode(MFnPlugin &plugin);

// For the frame-rate display: counts a mesh deformed, and how long it took.
void CountDeform(long long nanoseconds);

}  // namespace rigExecMaya

#endif
