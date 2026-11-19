// TouchPose's highlight locator (see touchOverlay.cpp).
#ifndef RIGEXEC_MAYA_TOUCH_OVERLAY_H
#define RIGEXEC_MAYA_TOUCH_OVERLAY_H

#include <maya/MStatus.h>

class MFnPlugin;

namespace rigExecMaya {

MStatus RegisterTouchOverlay(MFnPlugin &plugin);
void DeregisterTouchOverlay(MFnPlugin &plugin);

}  // namespace rigExecMaya

#endif
