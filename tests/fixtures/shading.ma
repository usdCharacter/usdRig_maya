//Maya ASCII 2025 scene
requires maya "2025";
currentUnit -l centimeter -a degree -t film;
createNode aiStandardSurface -n "skin";
createNode file -n "baseMap";
    setAttr ".ftn" -type "string" "textures/checker.ppm";
    setAttr ".cs" -type "string" "sRGB";
createNode file -n "roughMap";
    setAttr ".ftn" -type "string" "textures/checker.ppm";
    setAttr ".cs" -type "string" "Raw";
connectAttr "baseMap.oc" "skin.base_color";
connectAttr "roughMap.ocr" "skin.specular_roughness";
