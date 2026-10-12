"""Convert an mg-picker 1.9 XML layout to RigExecPicker scene data.

Output is an overlay layer that sublayers the converted rig and authors one
RigExecPicker (tab) with one RigExecPickerPanel per mg panel (sub-tab) and
one RigExecPickerButton per selectButton, plus texture-path overrides that
re-point the rig's Windows-absolute file paths at local copies.

Usage:
    python tools/convert_mgpicker.py <picker.mgpkr> <rig.usdc> <out.usda>
"""
import os
import shutil
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

from pxr import Sdf, Usd


def text(item, path, default=""):
    el = item.find(path)
    if el is None or el.text is None:
        return default
    return el.text.strip()


def color(item, path, default=(128, 128, 128, 255)):
    r = text(item, path + "/r", None)
    if r is None or r == "":
        return tuple(c / 255.0 for c in default)
    return tuple(int(text(item, "%s/%s" % (path, c), str(default[i]))) / 255.0
                 for i, c in enumerate(("r", "g", "b", "alpha")))


def short_names(stage):
    """Maya short node name -> [prim paths] for selectable prims."""
    found = defaultdict(list)
    for prim in stage.Traverse():
        if str(prim.GetTypeName()) not in ("RigExecControl", "RigExecJoint"):
            continue
        full = prim.GetCustomDataByKey("maya:name") or ""
        short = full.split("|")[-1] if full else prim.GetName()
        found[short].append(str(prim.GetPath()))
    return found


def main(mgpkr, rig_usdc, out_usda):
    root = ET.parse(mgpkr).getroot()
    if root.tag != "mg-picker":
        raise ValueError("expected mg-picker XML, got " + root.tag)
    items = [c for c in root if c.tag == "item"]
    panels = [i for i in items if text(i, "type") == "panel"]
    buttons = [i for i in items if text(i, "type") == "selectButton"]
    others = [i for i in items
              if text(i, "type") not in ("panel", "selectButton")]
    print("panels=%d buttons=%d other_items=%d" % (
        len(panels), len(buttons), len(others)))
    for o in others:
        print("  unhandled item type:", text(o, "type"), o.get("id"))

    rig = Usd.Stage.Open(rig_usdc)
    names = short_names(rig)
    print("selectable rig prims: %d (%d unique short names)" % (
        sum(len(v) for v in names.values()), len(names)))

    # Resolution report before authoring anything.
    missing = defaultdict(int)
    ambiguous = defaultdict(list)
    for b in buttons:
        for obj in [o.strip() for o in text(b, "objects").split(",")
                    if o.strip()]:
            if obj not in names:
                missing[obj] += 1
            elif len(names[obj]) > 1:
                ambiguous[obj] = names[obj]
    print("unresolvable control names: %d" % len(missing))
    for name, count in sorted(missing.items()):
        print("  MISSING %s (x%d buttons)" % (name, count))
    print("ambiguous control names: %d" % len(ambiguous))
    for name, paths in sorted(ambiguous.items()):
        print("  AMBIGUOUS %s -> %s" % (name, paths))

    outdir = os.path.dirname(os.path.abspath(out_usda))
    os.makedirs(outdir, exist_ok=True)
    layer = Sdf.Layer.CreateAnonymous("mg-picker.usda")
    layer.subLayerPaths = [os.path.abspath(rig_usdc)]
    stage = Usd.Stage.Open(layer)
    stage.SetDefaultPrim(stage.GetPrimAtPath("/Rig"))
    picker = stage.DefinePrim(Sdf.Path("/ImportedPicker"), "RigExecPicker")
    picker.GetAttribute("ui:label").Set(os.path.splitext(os.path.basename(mgpkr))[0].replace("_", " "))
    picker.CreateRelationship("rigExec:picker:rig").SetTargets(
        [Sdf.Path("/Rig")])
    # Provenance for anything the schema cannot carry.
    picker.SetCustomDataByKey("mgPicker:source",
                              os.path.basename(mgpkr))
    picker.SetCustomDataByKey("mgPicker:namespace", text(root, "namespace"))
    picker.SetCustomDataByKey("mgPicker:version", root.get("version"))

    panel_paths = {}
    panel_origins = {}
    for panel in sorted(panels, key=lambda p: int(text(p, "panelIndex", "0"))):
        pid = panel.get("id")
        prim = stage.DefinePrim(
            picker.GetPath().AppendChild(pid), "RigExecPickerPanel")
        prim.GetAttribute("ui:label").Set(text(panel, "label/text") or pid)
        prim.GetAttribute("ui:order").Set(int(text(panel, "panelIndex", "0")))
        prim.GetAttribute("ui:size").Set(
            (float(text(panel, "size/width", "306")),
             float(text(panel, "size/height", "424"))))
        prim.GetAttribute("ui:background").Set(
            color(panel, "fillColor", (153, 153, 153, 255)))
        bg = text(panel, "bgImage")
        if bg:
            prim.SetCustomDataByKey("mgPicker:bgImage", bg)
            name = bg.replace("\\", "/").split("/")[-1]
            candidates = [os.path.join(os.path.dirname(mgpkr), "pickerimages", name),
                          os.path.join(os.path.dirname(mgpkr), name)]
            original = next((path for path in candidates if os.path.isfile(path)), None)
            if not original:
                raise FileNotFoundError("picker background image: " + bg)
            image_dir = os.path.join(outdir, "pickerimages")
            os.makedirs(image_dir, exist_ok=True)
            target = os.path.join(image_dir, name)
            if os.path.abspath(original) != os.path.abspath(target):
                shutil.copy(original, target)
            prim.GetAttribute("ui:backgroundImage").Set(Sdf.AssetPath("./pickerimages/" + name))
        panel_paths[pid] = prim.GetPath()
        panel_origins[pid] = (float(text(panel, "position/x", "0")),
                              float(text(panel, "position/y", "0")))

    n_buttons = 0
    for b in buttons:
        pid = text(b, "parent")
        if pid not in panel_paths:
            print("  SKIP %s: unknown parent %r" % (b.get("id"), pid))
            continue
        prim = stage.DefinePrim(
            panel_paths[pid].AppendChild(b.get("id")),
            "RigExecPickerButton")
        shape_el = b.find("shape")
        shape = text(shape_el, "type", "rectangle") if shape_el is not None \
            else "rectangle"
        prim.GetAttribute("ui:shape").Set(shape)
        prim.GetAttribute("ui:position").Set(
            (float(text(b, "position/x", "0")) - panel_origins[pid][0],
             float(text(b, "position/y", "0")) - panel_origins[pid][1]))
        prim.GetAttribute("ui:size").Set(
            (float(text(b, "size/width", "10")),
             float(text(b, "size/height", "10"))))
        if shape_el is not None:
            prim.GetAttribute("ui:roundness").Set(
                float(text(shape_el, "roundness", "0")))
            direction = text(shape_el, "shapeDirection", "")
            if direction:
                prim.GetAttribute("ui:direction").Set(direction)
            poly = shape_el.find("polygon")
            if poly is not None:
                pts = []
                for pt in poly.findall("polygonPoint"):
                    x, y = pt.text.split(",")
                    pts.append((float(x), float(y)))
                prim.GetAttribute("ui:polygon").Set(pts)
        prim.GetAttribute("ui:fill").Set(color(b, "fillColor"))
        prim.GetAttribute("ui:stroke").Set(
            color(b, "stroke/color", (0, 0, 0, 200)))
        prim.GetAttribute("ui:strokeWidth").Set(
            float(text(b, "stroke/width", "1")))
        prim.GetAttribute("ui:text").Set(text(b, "label/text"))
        prim.GetAttribute("ui:fontSize").Set(
            float(text(b, "label/size", "8")))
        prim.GetAttribute("ui:bold").Set(text(b, "label/bold", "0") == "1")
        prim.GetAttribute("ui:textAlign").Set(
            text(b, "label/hAlign", "center"))
        prim.GetAttribute("ui:textColor").Set(
            color(b, "label/color", (0, 0, 0, 255)))
        prim.GetAttribute("ui:rotation").Set(float(text(b, "rotation", "0")))
        prim.GetAttribute("ui:depth").Set(float(text(b, "zValue", "0")))
        targets = []
        for obj in [o.strip() for o in text(b, "objects").split(",")
                    if o.strip()]:
            if obj in names and len(names[obj]) == 1:
                targets.append(Sdf.Path(names[obj][0]))
        if targets:
            prim.CreateRelationship("rigExec:picker:controls").SetTargets(
                targets)
        elif text(b, "objects").strip():
            # Authored-but-empty keeps it a (dead) button rather than
            # silently becoming decoration; see pickerScene.objects.
            prim.CreateRelationship("rigExec:picker:controls").SetTargets([])
        n_buttons += 1
    # Mirrors resolve after every button prim exists.
    for b in buttons:
        mirror = text(b, "mirror")
        if not mirror:
            continue
        src = stage.GetPrimAtPath(
            panel_paths[text(b, "parent")].AppendChild(b.get("id")))
        dst = None
        for pid, ppath in panel_paths.items():
            cand = stage.GetPrimAtPath(ppath.AppendChild(mirror))
            if cand and cand.IsValid():
                dst = cand.GetPath()
                break
        if dst:
            src.CreateRelationship("rigExec:picker:mirror").SetTargets([dst])
        else:
            print("  mirror target not found: %s -> %s"
                  % (b.get("id"), mirror))
    print("authored buttons: %d" % n_buttons)

    # Texture repair: the rig's file nodes carry the author's Windows
    # absolute paths. Re-point every translated UsdUVTexture file input at
    # a local copy beside this layer, matched by file name.
    imgdir = os.path.join(outdir, "sourceimages")
    os.makedirs(imgdir, exist_ok=True)
    srcdir = os.path.join(os.path.dirname(os.path.abspath(mgpkr)), "sourceimages")
    n_tex = 0
    for prim in rig.Traverse():
        if str(prim.GetTypeName()) != "Shader":
            continue
        if prim.GetAttribute("info:id").Get() != "UsdUVTexture":
            continue
        authored = prim.GetAttribute("inputs:file").Get()
        name = os.path.basename(
            authored.path if hasattr(authored, "path")
            else authored.GetAssetPath()).replace("\\", "/").split("/")[-1]
        local = os.path.join(srcdir, name)
        if os.path.isfile(local):
            target = os.path.join(imgdir, name)
            if os.path.abspath(local) != os.path.abspath(target):
                shutil.copy(local, target)
        else:
            print("  no local copy for texture: %s" % name)
            continue
        over = stage.OverridePrim(prim.GetPath())
        over.GetAttribute("inputs:file").Set(
            Sdf.AssetPath("./sourceimages/" + name))
        n_tex += 1
    print("texture overrides: %d" % n_tex)

    layer.subLayerPaths = [os.path.relpath(os.path.abspath(rig_usdc), outdir)]
    if not layer.Export(out_usda):
        raise IOError("could not export picker: " + out_usda)
    print("wrote %s (%.1f KB)" % (
        out_usda, os.path.getsize(out_usda) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
