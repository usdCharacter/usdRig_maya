//Maya ASCII 2026 scene
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
createNode joint -n "root";
    setAttr ".sx" 2;
createNode joint -n "child" -p "root";
    setAttr ".tx" 3;
    setAttr ".sx" 0.5;
connectAttr "root.s" "child.is";
createNode joint -n "tip" -p "child";
    setAttr ".tx" 3;
connectAttr "child.s" "tip.is";
