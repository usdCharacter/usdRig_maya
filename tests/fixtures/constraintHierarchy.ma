//Maya ASCII 2025 scene
createNode transform -n "parentDriver";
setAttr ".t" -type "double3" 10 0 0;
setAttr ".r" -type "double3" 0 0 90;
createNode transform -n "childDriver";
setAttr ".t" -type "double3" 20 5 0;
createNode transform -n "parent";
createNode transform -n "child" -p "parent";
createNode parentConstraint -n "childConstraint";
connectAttr "childDriver.t" "childConstraint.tg[0].tt";
connectAttr "childDriver.r" "childConstraint.tg[0].tr";
connectAttr "childConstraint.ct" "child.t";
connectAttr "childConstraint.cr" "child.r";
createNode parentConstraint -n "parentConstraint";
connectAttr "parentDriver.t" "parentConstraint.tg[0].tt";
connectAttr "parentDriver.r" "parentConstraint.tg[0].tr";
connectAttr "parentConstraint.ct" "parent.t";
connectAttr "parentConstraint.cr" "parent.r";
