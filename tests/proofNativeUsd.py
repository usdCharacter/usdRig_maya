"""Validate deformation from the exported USD alone in a fresh Python process.

This proves native execution and binding, not complete source-rig parity.
No source scene is opened and no Maya modules or executable are used.
"""
import collections, hashlib, json, os, pathlib, sys
import numpy as np

repo = pathlib.Path(__file__).resolve().parents[3]
out = repo / 'build/maya-character-proof'
asset = out / 'rhino_imported.usda'
os.environ['PATH'] = os.pathsep.join([str(repo/'build'), str(repo.parent/'usd-install/lib'), str(repo.parent/'usd-install/bin'), os.environ.get('PATH','')])
sys.path.insert(0, str(repo/'tests/python'))
from rigexec_test_env import SetupPluginTest
SetupPluginTest(repo/'build')
from pxr import Plug, Usd, UsdGeom, UsdShade
Plug.Registry().RegisterPlugins(str(repo/'build/plugin/usdMayaRig/usd/usdMayaRig/resources'))
Plug.Registry().RegisterPlugins(str(repo/'build/usd/rigExecSchema/resources'))
sys.path.insert(0, str(repo/'build/python'))
import rigexec

stage = Usd.Stage.Open(str(asset))
stage.SetEditTarget(stage.GetSessionLayer())
assert all(not layer.identifier.lower().endswith('.ma') for layer in stage.GetUsedLayers())
assert not any(name == 'maya' or name.startswith('maya.') for name in sys.modules)
counts = collections.Counter(str(p.GetTypeName()) for p in stage.Traverse())
assert counts['RigExecSkinMover'] == 7
assert counts['RigExecSingleChainIkConstraint'] == 12
assert counts['RigExecNurbsSurfacePin'] == 7
assert counts['RigExecDeltaMushMover'] == 1
meshes = [p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh) and p.GetAttribute('maya:skinCluster')]
body = next(p for p in meshes if (p.GetCustomDataByKey('maya:name') or '').endswith('|rhino_body_geo_Shape'))
assert body.GetAttribute('maya:skinCluster').Get()
for mesh in meshes:
    binding = UsdShade.MaterialBindingAPI(mesh)
    if not binding.ComputeBoundMaterial()[0]:
        subsets = binding.GetMaterialBindSubsets()
        assert subsets and all(UsdShade.MaterialBindingAPI(s.GetPrim()).ComputeBoundMaterial()[0] for s in subsets)
        faces = [int(i) for s in subsets for i in s.GetIndicesAttr().Get()]
        assert sorted(faces) == list(range(len(UsdGeom.Mesh(mesh).GetFaceVertexCountsAttr().Get())))
rig = rigexec.Rig(stage, '/Rig')
rig.compile()
rest = rig.evaluate(-1)
assert rest.valid and all(d.startswith('mover graph:') for d in rest.diagnostics), list(rest.diagnostics)
properties = [str(p.GetPath()) + '.points' for p in meshes]
base = {key: np.asarray(rest.moved_property(key), dtype=float) for key in properties}
assert sum(len(points) for points in base.values()) == 20034
cases = [('world', 'world_cnt_001', 'tx', 10.),
         ('head', 'cn_head_jnt_001_cnt', 'rz', 15.),
         ('foot', 'lf_ft_foot_001_cnt', 'ty', 10.),
         ('tail', 'cn_tail_jnt_003_cnt', 'ry', 20.),
         ('spine', 'cn_spine_cnt_001', 'ty', 5.)]
controls = {p.GetCustomDataByKey('maya:name').split('|')[-1]: p for p in stage.Traverse()
            if p.GetTypeName() == 'RigExecControl' and p.GetCustomDataByKey('maya:name')}
result = {'asset': str(asset), 'sha256': hashlib.sha256(asset.read_bytes()).hexdigest(),
          'sourceSceneOpened': False, 'mayaRuntimeUsed': False, 'primCounts': dict(counts),
          'meshVertices': sum(len(p) for p in base.values()), 'poses': {},
          'completeSourceRigParity': False,
          'limitations': ['Full source parity still fails; see comparison.json. Native deltaMush now runs after skinning.']}
for label, name, channel, delta in cases:
    control = controls[name]
    attribute = control.GetAttribute('avars:' + channel)
    attribute.Set(delta)
    pose = rig.evaluate(-1)
    assert pose.valid and all(d.startswith('mover graph:') for d in pose.diagnostics), list(pose.diagnostics)
    motion = {key: np.asarray(pose.moved_property(key), dtype=float) - points for key, points in base.items()}
    body_motion = motion[str(body.GetPath()) + '.points']
    maximum = float(np.linalg.norm(body_motion, axis=1).max())
    changed = int((np.linalg.norm(body_motion, axis=1) > 1e-4).sum())
    assert maximum > 0.1 and changed > 10, (label, maximum, changed)
    measured = {'maxBodyMotionCm': maximum, 'bodyVerticesMoved': changed}
    if label == 'head':
        mush = next(p for p in stage.Traverse() if p.GetTypeName() == 'RigExecDeltaMushMover')
        enabled = mush.GetAttribute('inputs:enabled')
        enabled.Set(False)
        unfiltered = rig.evaluate(-1)
        assert unfiltered.valid
        corrected = np.asarray(pose.moved_property(str(body.GetPath()) + '.points'), dtype=float)
        upstream = np.asarray(unfiltered.moved_property(str(body.GetPath()) + '.points'), dtype=float)
        correction = float(np.linalg.norm(corrected - upstream, axis=1).max())
        assert correction > 0.01, 'deltaMush must alter the posed body'
        measured['maxDeltaMushCorrectionCm'] = correction
        enabled.Set(True)
    if label == 'world':
        error = max(float(np.linalg.norm(v - [10., 0., 0.], axis=1).max()) for v in motion.values())
        measured['maxRigidTranslationErrorCm'] = error
        assert error < 1e-3, error
    if label == 'foot':
        ankle = next(p for p in stage.Traverse() if p.GetTypeName() == 'RigExecJoint' and (p.GetCustomDataByKey('maya:name') or '').endswith('|lf_ft_ankle_jnt_001'))
        path = str(ankle.GetPath())
        displacement = np.asarray(pose.joint_frame(path).to_matrix4())[12:15] - np.asarray(rest.joint_frame(path).to_matrix4())[12:15]
        measured['ankleTranslationCm'] = displacement.tolist()
        assert np.linalg.norm(displacement - [0.,10.,0.]) < 1e-3, displacement
    attribute.Set(0.)
    restored = rig.evaluate(-1)
    assert max(float(np.abs(np.asarray(restored.moved_property(key)) - points).max()) for key, points in base.items()) < 1e-6
    result['poses'][label] = measured
result['passed'] = True
(out/'native-usd-proof.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
