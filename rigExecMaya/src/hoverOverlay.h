// The hover picker's viewport surface (see hoverOverlay.cpp).
#ifndef RIGEXEC_MAYA_HOVER_OVERLAY_H
#define RIGEXEC_MAYA_HOVER_OVERLAY_H

#include <maya/MStatus.h>

class MFnPlugin;

namespace rigExecMaya {

MStatus RegisterHoverOverlay(MFnPlugin &plugin);
void DeregisterHoverOverlay(MFnPlugin &plugin);

}  // namespace rigExecMaya

#endif
