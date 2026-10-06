"""Render measured character results, including failures; never infer parity from opening."""
import argparse, collections, json, pathlib
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

repo=pathlib.Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=pathlib.Path,default=repo/'build/maya-character-proof')
out=parser.parse_args().output.resolve()
reference=json.loads((out/'maya-reference.json').read_text())
result=json.loads((out/'comparison.json').read_text())
imported=json.loads((out/'import-result.json').read_text())
source=json.loads((out/'sources.json').read_text())
body=next(n for n in reference['meshes'] if n.endswith('|rhino_body_geo_Shape'))
def faces(record):
    offset=0
    for count in record['counts']:
        yield record['indices'][offset:offset+count]
        offset+=count
def canonical(record, reverse=False):
    def cycle(face):
        candidates=[tuple(face[i:]+face[:i]) for i in range(len(face))]
        if reverse:
            f=face[::-1]
            candidates.extend(tuple(f[i:]+f[:i]) for i in range(len(f)))
        return min(candidates)
    return collections.Counter(cycle(f) for f in faces(record))

topology={}
for name,want in reference['poses']['saved_pose']['meshes'].items():
    got=result['poses']['saved_pose']['meshes'][name]
    topology[name]={'exact':got['topologyMatches'],
        'sameOrientedFacesIgnoringOrder':canonical(want)==canonical(got),
        'sameFacesIgnoringWindingAndOrder':canonical(want,True)==canonical(got,True)}
(out/'topology-analysis.json').write_text(json.dumps(topology,indent=2))

labels=[('saved_pose','Saved pose'),('head_turn','Head control: +15 degrees'),('front_foot_ik','Front foot IK: +10 cm')]
fig=plt.figure(figsize=(16,9),facecolor='#101923')
fig.suptitle('RHINO CHARACTER / '+('PASS' if result['passed'] else 'FAIL'),color='white',fontsize=23,x=.05,ha='left',y=.97,weight='bold')
fig.text(.05,.919,'105 joints  |  12 IK handles  |  7 skin clusters  |  Maya 2025 vs native USD evaluation',color='#a8b9c9',fontsize=12)
for row in range(2):
    for col,(pose_name,title) in enumerate(labels):
        ax=fig.add_subplot(2,3,row*3+col+1,projection='3d',facecolor='#101923')
        record=(reference if row==0 else result)['poses'][pose_name]['meshes'][body]
        geometry=record if row==0 else result['poses']['saved_pose']['meshes'][body]
        points=np.array(record['points'])[:,[2,0,1]]
        polygons=[points[f] for f in faces(geometry)]
        color=np.array(matplotlib.colors.to_rgb('#71cbd0' if row==0 else '#eda56b'))
        normals=np.array([np.cross(p[1]-p[0],p[2]-p[0]) for p in polygons])
        normals/=np.maximum(np.linalg.norm(normals,axis=1)[:,None],1e-20)
        light=np.array([.2,-.5,.85]);light/=np.linalg.norm(light)
        shade=.40+.60*np.clip(normals@light,0,1)
        colors=np.clip(shade[:,None]*color,0,1)
        ax.add_collection3d(Poly3DCollection(polygons,facecolors=colors,edgecolors='none',linewidths=0,rasterized=True))
        ax.set_xlim(-130,140);ax.set_ylim(-65,65);ax.set_zlim(-10,175)
        ax.set_box_aspect((270,130,185));ax.view_init(elev=13,azim=-70);ax.set_proj_type('ortho');ax.set_axis_off()
        if row==0: ax.set_title(title,color='white',fontsize=14,pad=1)
        text='MAYA reference' if row==0 else f"USD / body error {record['maxErrorCm']:.3f} cm"
        ax.text2D(.08,.04,text,transform=ax.transAxes,color='#71cbd0' if row==0 else '#eda56b',fontsize=12)
fig.text(.05,.065,'Body shown at identical scale and camera. Native USD deformation works; full source parity still fails.',color='white',fontsize=12)
worst=max(m['maxErrorCm'] for p in result['poses'].values() for m in p['meshes'].values())
fig.text(.05,.027,f'Eyes/teeth omitted from this view; full six-mesh comparison also fails (worst vertex error: {worst:.2f} cm).',color='#a8b9c9',fontsize=11)
fig.subplots_adjust(left=.015,right=.985,top=.86,bottom=.10,wspace=0,hspace=.02)
fig.savefig(out/'rhino-comparison.png',dpi=140,facecolor=fig.get_facecolor())
plt.close(fig)

rows=[]
for label,p in result['poses'].items():
    b=p['meshes'][body]
    worst=max(m['maxErrorCm'] for m in p['meshes'].values())
    rows.append(f"| {label} | {b['mayaMaxMotionCm']:.6f} | {b.get('usdMaxMotionCm',0):.6f} | {b['maxErrorCm']:.6f} | {worst:.6f} | {'PASS' if p['passed'] else 'FAIL'} |")
categories=collections.Counter(d.split(':')[0] for d in imported['diagnostics'])
diagnostic_rows='\n'.join(f'| {name} | {count} |' for name,count in categories.most_common())
missing='\n'.join('- `'+d+'`' for d in imported['diagnostics'] if d.startswith('unresolved asset dependency'))
topo_rows='\n'.join(f"| {n.split('|')[-1]} | {t['exact']} | {t['sameOrientedFacesIgnoringOrder']} | {t['sameFacesIgnoringWindingAndOrder']} |" for n,t in topology.items())
verdict='PASS' if result['passed'] else 'FAIL'
report=f'''# Autodesk rhino character: {verdict}

The original Maya ASCII file converts to an editable native usdRig. Skin binding, deltaMush, ribbon-surface pins, constraints and IK execute without Maya. The exported [USD](rhino_imported.usda) can be reopened independently of the source scene. **It does not reproduce the complete character rig:** all five whole-character reference tests still fail at the original tolerance. All five eye/lens/teeth meshes pass the five pose comparisons.

Native execution is checked separately by `proofNativeUsd.py`, which opens only the exported USD in a fresh process, edits world/head/foot/tail/spine controls, measures body deformation, verifies the IK ankle endpoint, and checks exact reset. See [native-usd-proof.json](native-usd-proof.json). This is evidence of native execution and binding, not a source-parity pass.

![Measured body comparison](rhino-comparison.png)

## Source and method

- [Autodesk Maya USD sample]({source['sourcePage']}), pinned commit `{source['commit']}`.
- Original download: [{source['file']}]({source['file']}), {imported['bytes']:,} bytes; SHA-256 `{source['sha256']}`.
- 667 source nodes: 105 joints, 12 IK handles, 7 skin clusters, 102 parent constraints; ribbon surfaces, uvPin and deltaMush.
- The previously captured Maya {reference['mayaVersion']} reference opened the original file with script-node execution disabled. No graph connections were disconnected, no file resaved, and no rig features bypassed. That JSON is an offline comparison oracle; Maya is not launched or used by the converted rig.
- Native reader/evaluator: open that same `.ma` directly with `Usd.Stage.Open`, compile `rigexec.Rig`, apply corresponding control deltas in an anonymous session layer, compare evaluated mesh points in rig/world space. All poses return `valid=True`; this does **not** imply Maya equivalence.
- Six character meshes, 20,034 vertices, five poses: **100,170 vertex comparisons**. Tolerance: {result['toleranceCm']} cm. Pointwise tests include geometry and exact indexed topology. Reported geometric failures are far beyond tolerance even without the topology check.
- The plot uses actual evaluated points and each source's own face indices, with a fixed camera and scale. It shows body geometry only; the numerical results cover all six meshes. This is a geometry plot, not a textured Maya viewport screenshot.

## Measured results

All distances are centimeters. Motion is measured against each system's own saved pose.

| Pose | Maya body motion | USD body motion | Max body error | Max six-mesh error | Result |
|---|---:|---:|---:|---:|---|
{chr(10).join(rows)}

Control edits: `world_cnt_001.tx += 10`, `cn_head_jnt_001_cnt.rz += 15`, `lf_ft_foot_001_cnt.ty += 10`, `cn_tail_jnt_003_cnt.ry += 20`. Reference snapshots are in [maya-reference.json](maya-reference.json); evaluated points and per-mesh measurements are in [comparison.json](comparison.json).

## Diagnosed limitations

There are **{len(imported['diagnostics']):,} import diagnostics**, retained in [import-result.json](import-result.json) and the [imported USD layer](rhino_imported.usda).

- The body skin cluster does not directly drive its mesh: `skinCluster4 -> deltaMush1GroupParts -> deltaMush1 -> rhino_body_geo_Shape`. The importer follows that geometry chain, retains the skin binding, palette, weights and bind placement, and appends a native `RigExecDeltaMushMover`. Native regression checks cover indirect output traversal, bind placement, control-to-vertex motion and enabled/disabled deltaMush history. The operator improves the body comparison, but the measured differences below remain.
- `skinCluster3` now becomes a native skinned NURBS patch. Seven `RigExecNurbsSurfacePin` computations evaluate rational surface positions and derivatives against current constrained skin influences. The 12 IK handles become `RigExecSingleChainIkConstraint` operations with pole controls and preserved joint orientations. Native regressions check non-X bone orientations and refreshed surface-pin dependencies.
- Fixed: constrained children were evaluated before constrained ancestors, so later parent edits applied a second transform. The dependency order now includes each target's parent hierarchy. Stored sparse mesh `.pt` offsets are also applied before skinning; ignoring them misplaced the right eye/lens. Native regression tests cover both defects.
- Control validation compares all 54 curve shapes and their parent frames in every pose; `comparison.json` records the individual results. Small scapular discrepancies and untested custom attributes remain; this is not proof of complete mechanism coverage.
- The usdview companion plugin maps curve selection to its `maya:control` relationship, and generic guide rings on Maya group transforms are suppressed.
- Non-rigid bind/rest frames and unsupported graph connections remain diagnosed.
- Six USD material objects and mesh bindings are authored. Four referenced textures are unresolved; this sample is not a self-contained textured character. No material appearance parity is claimed.

| Diagnostic category | Count |
|---|---:|
{diagnostic_rows}

Unresolved dependency diagnostics:

{missing}

## Topology details

This test exposed and fixed a reader bug: Maya omits `-type "polyFaces"` on continuation chunks of large face arrays. The reader now imports those chunks, with a native regression check for topology and UV indices. Both native plugin tests and the earlier two downloaded example comparisons pass after the fix. The measurements above use the rebuilt plugin.

Exact topology comparison checks both face counts and the flattened vertex indices. The supplemental comparison allows face reordering and cyclic start-index changes; the final column also ignores winding.

| Mesh | Exact arrays | Same oriented faces, order ignored | Same faces, winding/order ignored |
|---|---|---|---|
{topo_rows}

## Reproduce

The live usdview check also records [usdview-inspection.json](usdview-inspection.json): selecting a curve resolves to its rig control, and editing `avars:rz` through the Avar Editor changes its evaluated curve at the current frame. It restores the value afterward. [Actual usdview window capture](usdview-inspection.png).

From the repository root with the plugin and rigexec Python extension built:

```powershell
python plugin/usdMayaRig/tests/proofCharacter.py --download-only
$env:MAYA_APP_DIR = "$PWD/build/maya-character-proof/maya-settings"
$env:MAYA_SKIP_USERSETUP_PY = "1"
$env:PYTHONPATH = ""
& "C:/Program Files/Autodesk/Maya2025/bin/mayapy.exe" plugin/usdMayaRig/tests/proofCharacterMaya.py build/maya-character-proof
python plugin/usdMayaRig/tests/proofCharacter.py
# Expected current exit code: 1 (measured parity failure).
python plugin/usdMayaRig/tests/renderCharacterProof.py
# Interactive control selection/editor check; leaves usdview open:
python plugin/usdMayaRig/tests/inspectCharacter.py
```

Use Python matching the USD build (Python 3.10 in this workspace), with numpy and matplotlib. In this native comparison, Maya is used only to obtain the independent reference. The native reader remains Maya-independent. The large upstream asset stays in the build output; its pinned manifest and validation scripts are in the plugin tests directory.

'''
(out/'REPORT.md').write_text(report,encoding='utf-8')
print(out/'REPORT.md')
print(json.dumps(topology,indent=2))
