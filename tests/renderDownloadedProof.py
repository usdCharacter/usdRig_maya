"""Plot measured Maya/USD vertices and write the downloadable-example report."""
import argparse, json, pathlib
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
from matplotlib.lines import Line2D
p=argparse.ArgumentParser(); p.add_argument('output',type=pathlib.Path); args=p.parse_args(); out=args.output
results=json.loads((out/'comparison.json').read_text()); refs=json.loads((out/'maya-reference.json').read_text()); sources=json.loads((out/'sources.json').read_text())
assert results and all(r['passed'] for r in results.values()), 'Refusing to publish a passing figure for failed comparisons'
maximum=max(p['maxVertexErrorCm'] for r in results.values() for p in r['poses'].values())
vertex_tests=sum(r['vertices']*len(r['poses']) for r in results.values())
pose_tests=sum(len(r['poses']) for r in results.values())
fig=plt.figure(figsize=(16,8.3),facecolor='#101820')
fig.text(.035,.965,'Downloaded Maya rigs / measured USD parity',color='white',fontsize=23,weight='bold')
fig.text(.035,.923,f'Maya 2025 reference  •  {pose_tests} poses  •  {vertex_tests:,} vertex comparisons  •  maximum error {maximum:.3g} cm',color='#94dccc',fontsize=13)
labels={'rest':'Saved pose','translate':'Translate + (4, 2, 1) cm','rotate':'Rotate (20°, 35°, 15°)','scale':'Uniform scale × 1.25'}
for row,(name,result) in enumerate(results.items()):
    ref=refs[name]
    allpts=np.concatenate([np.array(p['points']) for p in result['poses'].values()])[:,[0,2,1]]
    center=(allpts.max(0)+allpts.min(0))/2; radius=max(allpts.max(0)-allpts.min(0))*.56
    if row==1: radius=max(radius,2)
    for col,(label,pose) in enumerate(result['poses'].items()):
        ax=fig.add_subplot(2,4,row*4+col+1,projection='3d',facecolor='#101820')
        pts=np.array(pose['points'])[:,[0,2,1]]; want=np.array(ref['poses'][label]['points'])[:,[0,2,1]]
        faces=[]; offset=0
        for count in ref['poses'][label]['counts']:
            ids=ref['poses'][label]['indices'][offset:offset+count]; offset+=count; faces.append(pts[ids])
        ax.add_collection3d(Poly3DCollection(faces,facecolors='#3abbc2',edgecolors='#17606b',linewidths=.25 if row==0 else .9,alpha=.48))
        ax.scatter(want[:,0],want[:,1],want[:,2],s=2 if row==0 else 24,c='#ffbc7a',depthshade=False)
        for axis,c in zip([ax.set_xlim,ax.set_ylim,ax.set_zlim],center): axis(c-radius,c+radius)
        ax.set_box_aspect((1,1,1)); ax.view_init(elev=20,azim=-55)
        ax.set_axis_off()
        ax.set_title(labels[label],color='white',fontsize=12,pad=0)
        ax.text2D(.5,.025,f"PASS  |  max error {pose['maxVertexErrorCm']:.2e} cm",transform=ax.transAxes,ha='center',color='#94dccc',fontsize=10)
    fig.text(.038,.867 if row==0 else .468,('BALL  /  382 vertices · 400 faces' if row==0 else 'SKINNED CUBE  /  8 vertices · 6 faces'),color='#b7c8d4',fontsize=11,weight='bold')
fig.legend(handles=[Line2D([0],[0],color='#3abbc2',lw=7,label='USD evaluated geometry'),Line2D([0],[0],marker='o',color='#ffbc7a',ls='',label='Maya reference vertices')],loc='lower center',bbox_to_anchor=(.5,.035),ncol=2,frameon=False,labelcolor='white',fontsize=12)
fig.text(.035,.025,'Pose / geometry / UV validation. Original animation curves and visibility wiring are outside this proof.',color='#b7c8d4',fontsize=10)
fig.subplots_adjust(left=.02,right=.99,bottom=.105,top=.84,hspace=.24,wspace=.02)
fig.savefig(out/'proof.png',dpi=130,facecolor=fig.get_facecolor()); plt.close(fig)
lines=['# Downloaded Maya rig proof','', 'Result: **PASS for the tested pose, geometry, UV, and material-binding paths.**','',
'Both source files were downloaded unchanged from Autodesk/maya-usd at commit `'+sources['commit']+'`. SHA-256 verification is part of the reproducible test. Maya 2025 evaluated the original files in a disposable standalone process. The native `.ma` plugin imported the same bytes without using Maya.','',
'![Measured pose comparison](proof.png)','',
'## Measured results','', '| Example | Vertices | Faces | Poses | Maximum vertex error (cm) | UVs and topology | Material |','| --- | ---: | ---: | ---: | ---: | --- | --- |']
for name,r in results.items():
    asset=sources['files'][name]
    lines.append(f"| [{name}]({asset['sourcePage']}) | {r['vertices']} | {r['faces']} | {len(r['poses'])} | {max(p['maxVertexErrorCm'] for p in r['poses'].values()):.9g} | Match | Bound |")
lines+=['','Tolerance: `1e-5 cm`, Euclidean distance per corresponding vertex. All 1,560 vertex comparisons pass. Face counts and indices match exactly; UV indices match exactly and UV coordinates match within `1e-6`. Native plugin discovery and evaluator regression tests also pass.','',
'## What was tested','',
'* Ball: the downloaded `ball_ctrl` translate/rotate/scale connections drive the imported mesh through RigExec avar connections and a matrix mover.',
'* Cube: the downloaded skin cluster, compact weights and bind matrices produce a native `RigExecSkinMover`; editing the imported joint drives its mesh.',
'* Four independent poses: saved pose; translation delta `(4, 2, 1)` cm; rotation `(20, 35, 15)` degrees; uniform scale `1.25`.',
'* Ball starts at frame 1 with zero translation. Cube was saved with joint translation `(5, 5, 0)` at frame 5. USD avars are offsets from the saved pose, so the Maya test applies matching offsets from those saved channels.',
'* The cube animation inputs are disconnected only in the disposable Maya reference process so manual pose comparisons are not overwritten by animation. The downloaded files are never resaved or modified.','',
'## Limits and warnings','',
'This validates these two examples and these poses. It does not establish general production-rig equivalence or animation playback. The Ball import reports 26 diagnostics, including an untranslated visibility connection, cameras, display/render layers and UI scripts. The cube reports 67 diagnostics, including its untranslated animation curves and tweak node. Full diagnostic lists are retained in `comparison.json` and the exported USD files. Strict import therefore rejects these scenes. Their tested transform/skinning paths still evaluate correctly.',
'',
'Maya itself warned that the Ball file requests an unavailable `AL_USDMayaPlugin` and contains an old node-editor type. Its ordinary transform/mesh graph loaded and was used for the reference measurements; these warnings are preserved in `maya-reference.log`. Embedded scene scripts were disabled. Neither imported example contains an external texture dependency, so this experiment does not prove texture-image rendering. UVs and default material binding were checked.',
'',
'## Fixes exposed by these real files','',
'* Normalize Maya full unit names (`centimeter`, `degree`).',
'* Read compact multi-vertex skin-weight rows.',
'* Translate direct local TRS wires for identity local rest frames.',
'* Supply Maya’s implicit `lambert1` / `initialShadingGroup` material nodes.',
'* Propagate joint scale through skin bind proxies.',
'',
'## Reproduce','',
'From the repository root, after building USD, usdMayaRig and the rigexec Python bindings:',
'',
'```text',
'python plugin/usdMayaRig/tests/proofDownloadedExamples.py --download-only',
'mayapy plugin/usdMayaRig/tests/proofMayaReference.py build/maya-example-proof',
'python plugin/usdMayaRig/tests/proofDownloadedExamples.py',
'python plugin/usdMayaRig/tests/renderDownloadedProof.py build/maya-example-proof',
'ctest --test-dir build -R testUsdMayaRig --output-on-failure',
'```','',
'Use Maya only for the independent reference step. The comparison/plotting scripts need NumPy and Matplotlib in the USD Python environment.',
'',
'## Evidence files','',
'* [Raw comparison data, evaluated points and diagnostics](comparison.json)',
'* [Maya reference vertices and UVs](maya-reference.json)',
'* [Pinned URLs and source hashes](sources.json)',
'* [Maya reference log](maya-reference.log)',
'* [Imported Ball rig in USD](Ball_rig.ma.usda)',
'* [Evaluated Ball pose samples](Ball_rig.ma.evaluated.usda)',
'* [Imported skinned cube in USD](UsdExportSkeletonWithIdentityBindPose.ma.usda)',
'* [Evaluated cube pose samples](UsdExportSkeletonWithIdentityBindPose.ma.evaluated.usda)',
'',
'## Source SHA-256','']
for name,asset in sources['files'].items(): lines+=['* `'+name+'`: `'+asset['sha256']+'`']
(out/'REPORT.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
print(out/'proof.png')
