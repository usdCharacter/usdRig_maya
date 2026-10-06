//Maya ASCII 2025 scene
currentUnit -l cm -a deg -t film;
file -r -ns "hero" -typ "mayaAscii" "rig.ma";
createNode file -n "texture";
    setAttr ".ftn" -type "string" "textures/checker.ppm";
    setAttr ".cs" -type "string" "Raw";
connectAttr "texture.oc" "hero:paint.c";
