// The rig's control curves, drawn by one node (see controlsDraw.cpp).
#ifndef RIGEXEC_MAYA_CONTROLS_DRAW_H
#define RIGEXEC_MAYA_CONTROLS_DRAW_H

#include <maya/MStatus.h>

class MFnPlugin;

namespace rigExecMaya {

MStatus RegisterControlsDraw(MFnPlugin &plugin);
void DeregisterControlsDraw(MFnPlugin &plugin);

}  // namespace rigExecMaya

#endif
