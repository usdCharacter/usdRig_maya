"""Run with the usdRig Python environment: python tests/testPickerConversion.py."""
import importlib.util
import pathlib
import tempfile
import unittest

from pxr import Usd

spec = importlib.util.spec_from_file_location(
    "convert_mgpicker", pathlib.Path(__file__).resolve().parents[1] / "tools/convert_mgpicker.py")
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)


class PickerConversionTest(unittest.TestCase):
    def test_alpha_and_panel_coordinates(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            rig = Usd.Stage.CreateNew(str(folder / "rig.usda"))
            rig.DefinePrim("/Rig", "RigExecRoot")
            control = rig.DefinePrim("/Rig/Control", "RigExecControl")
            control.SetCustomDataByKey("maya:name", "|rig|control")
            rig.GetRootLayer().Save()
            source = folder / "picker.mgpkr"
            source.write_text('''<mg-picker version="1.9">
                <item id="panel2"><type>panel</type><panelIndex>1</panelIndex>
                    <position><x>311</x><y>20</y></position>
                    <size><width>306</width><height>424</height></size></item>
                <item id="button"><type>selectButton</type><parent>panel2</parent>
                    <position><x>440</x><y>275</y></position><objects>control</objects>
                    <fillColor><r>255</r><g>25</g><b>25</b><alpha>87</alpha></fillColor>
                    <stroke><color><r>31</r><g>31</g><b>31</b><alpha>255</alpha></color></stroke>
                    <label><text>Head</text><color><r>0</r><g>0</g><b>0</b><alpha>255</alpha></color></label>
                </item></mg-picker>''')
            target = folder / "converted.usda"
            converter.main(str(source), str(folder / "rig.usda"), str(target))
            stage = Usd.Stage.Open(str(target))
            self.assertEqual(str(stage.GetDefaultPrim().GetPath()), "/Rig")
            button = stage.GetPrimAtPath("/ImportedPicker/panel2/button")
            self.assertEqual(tuple(button.GetAttribute("ui:position").Get()), (129, 255))
            self.assertAlmostEqual(button.GetAttribute("ui:fill").Get()[3], 87 / 255)
            self.assertEqual(button.GetAttribute("ui:stroke").Get()[3], 1)
            self.assertEqual(button.GetAttribute("ui:textColor").Get()[3], 1)
            self.assertEqual(str(button.GetRelationship("rigExec:picker:controls").GetTargets()[0]),
                             "/Rig/Control")


if __name__ == "__main__":
    unittest.main()
