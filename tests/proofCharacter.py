"""Pinned full-character parity test. Nonzero exit means a measured failure.

python proofCharacter.py --download-only
mayapy proofCharacterMaya.py <build/maya-character-proof>
python proofCharacter.py
python renderCharacterProof.py

Requires the built usdMayaRig plugin, rigexec Python module and numpy.
"""
import argparse, collections, hashlib, json, os, pathlib, re, sys, time, traceback
import urllib.request

sidecar=pathlib.Path(__file__).resolve().parents[1]
repo=sidecar.parent/'usdRig'
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=pathlib.Path,default=repo/'build/maya-character-proof')
parser.add_argument('--download-only',action='store_true')
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
manifest=json.loads(pathlib.Path(__file__).with_name('characterExample.json').read_text())
src=out/manifest['file']
if args.download_only and not src.exists():
    src.write_bytes(urllib.request.urlopen(manifest['url'],timeout=60).read())
if not src.exists() or hashlib.sha256(src.read_bytes()).hexdigest()!=manifest['sha256']:
    raise RuntimeError('Missing or modified pinned character download: '+str(src))
(out/'sources.json').write_text(json.dumps(manifest,indent=2)+'\n')
if args.download_only:
    print('Pinned character verified:',src)
    sys.exit(0)
os.environ['PATH']=os.pathsep.join([str(repo/'build'),str(repo.parent/'usd-install/lib'),str(repo.parent/'usd-install/bin'),os.environ.get('PATH','')])
sys.path.insert(0,str(repo/'tests/python'))
from rigexec_test_env import SetupPluginTest
SetupPluginTest(repo/'build')
from pxr import Plug,Usd
Plug.Registry().RegisterPlugins(os.environ.get('USDMAYARIG_PLUGIN_RESOURCES',str(sidecar/'build/usd/usdMayaRig/resources')))
Plug.Registry().RegisterPlugins(str(repo/'plugin/rigExecSchema/resources'))
sys.path.insert(0,str(repo/'build/python'))
import rigexec,numpy as np
from pxr import Sdf,UsdGeom,UsdShade
ref=json.loads((out/'maya-reference.json').read_text()); result={'poses':{},'toleranceCm':1e-4}
assert ref['sha256']==manifest['sha256'] and not ref['errors'], 'Maya reference must be complete and match the pinned source'
result['sha256']=manifest['sha256']
started=time.perf_counter()
s=Usd.Stage.Open(str(out/'rhino_rig_001.ma'))
import_result={'opened':bool(s),'openSeconds':time.perf_counter()-started,'sha256':manifest['sha256'],'bytes':src.stat().st_size,
    'sourceNodeCounts':dict(collections.Counter(re.findall(r'^createNode (\w+)',src.read_text(encoding='cp1252'),re.M))),
    'diagnostics':list(s.GetDefaultPrim().GetAttribute('maya:diagnostics').Get()),
    'primCounts':dict(collections.Counter(str(p.GetTypeName()) for p in s.Traverse()))}
(out/'import-result.json').write_text(json.dumps(import_result,indent=2))
s.GetRootLayer().Export(str(out/'rhino_imported.usda'))
by_name={p.GetCustomDataByKey('maya:name'):p for p in s.Traverse() if p.GetCustomDataByKey('maya:name') and str(p.GetPath()).startswith('/Rig/Dag')}
meshes={p.GetCustomDataByKey('maya:name'):p for p in s.Traverse() if p.IsA(UsdGeom.Mesh)}
curves={p.GetCustomDataByKey('maya:name'):p for p in s.Traverse() if p.IsA(UsdGeom.NurbsCurves)}
controlShapes={shape['name']:(control,shape) for control in by_name.values() for shape in (control.GetCustomDataByKey('maya:controlShapes') or {}).values()}
overlay=Sdf.Layer.CreateAnonymous('character-proof.usda');s.GetSessionLayer().subLayerPaths.append(overlay.identifier);s.SetEditTarget(overlay)
r=rigexec.Rig(s,'/Rig')
try:
    r.compile(); result['compiled']=True
except Exception: result['compiled']=False;result['compileError']=traceback.format_exc()
for label,expected in ref['poses'].items():
    item={'meshes':{},'curves':{}}; attribute=None
    try:
        if expected['driver']:
            p=next(p for name,p in by_name.items() if name.split('|')[-1]==expected['driver'])
            attribute=p.GetAttribute('avars:'+expected['channel']); attribute.Set(float(expected['delta']))
        started=time.perf_counter();pose=r.evaluate(-1);item['seconds']=time.perf_counter()-started
        item['valid']=pose.valid; item['evaluatorDiagnostics']=list(pose.diagnostics)
        moved=pose.moved_properties()
        for maya_name,wanted in expected.get('curves',{}).items():
            if maya_name in controlShapes:
                control,shape=controlShapes[maya_name]
                matrix=np.array(pose.control_frame(str(control.GetPath())).to_matrix4()).reshape(4,4)
                shift=np.eye(4);shift[3,:3]=-np.array(control.GetCustomDataByKey('maya:rotatePivot') or (0,0,0))
                matrix=shift@matrix
                local=np.array(shape['points'],dtype=float)
                points=(np.c_[local,np.ones(len(local))]@matrix)[:,:3]
            else:
                prim=curves[maya_name];prop=str(prim.GetPath())+'.points'
                points=np.array(moved[prop] if prop in moved else UsdGeom.NurbsCurves(prim).GetPointsAttr().Get(),dtype=float)
            target=np.array(wanted['points'],dtype=float)
            owner=by_name[wanted['control']]
            frame=np.array(pose.control_frame(str(owner.GetPath())).to_matrix4()).reshape(4,4)
            shift=np.eye(4);shift[3,:3]=-np.array(owner.GetCustomDataByKey('maya:rotatePivot') or (0,0,0))
            frame=shift@frame
            matrix_error=float(abs(frame-np.array(wanted['matrix']).reshape(4,4)).max())
            error=float(np.linalg.norm(points-target,axis=1).max())
            item['curves'][maya_name]={'maxCvErrorCm':error,'maxControlMatrixError':matrix_error,'passed':error<=result['toleranceCm'] and matrix_error<=result['toleranceCm']}
        for maya_name,wanted in expected['meshes'].items():
            if maya_name not in meshes:
                item['meshes'][maya_name]={'missing':True};continue
            prim=meshes[maya_name];mesh=UsdGeom.Mesh(prim); prop=str(prim.GetPath())+'.points'
            points=np.array(moved[prop] if prop in moved else mesh.GetPointsAttr().Get(),dtype=float)
            target=np.array(wanted['points'],dtype=float)
            if points.shape!=target.shape:
                item['meshes'][maya_name]={'shapeMismatch':[list(points.shape),list(target.shape)]};continue
            errors=np.linalg.norm(points-target,axis=1)
            rest=np.array(ref['poses']['saved_pose']['meshes'][maya_name]['points'])
            material=UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()[0]
            record={'vertices':len(points),'faces':len(wanted['counts']),'maxErrorCm':float(errors.max()),'rmsErrorCm':float(np.sqrt(np.mean(errors**2))),'verticesWithinTolerance':int((errors<=result['toleranceCm']).sum()),'mayaMaxMotionCm':float(np.linalg.norm(target-rest,axis=1).max()),'evaluatedMover':prop in moved,'topologyMatches':list(mesh.GetFaceVertexCountsAttr().Get())==wanted['counts'] and list(mesh.GetFaceVertexIndicesAttr().Get())==wanted['indices'],'material':str(material.GetPath()) if material else None,'points':points.tolist()}
            if label!='saved_pose' and maya_name in result['poses']['saved_pose']['meshes']:
                usd_rest=np.array(result['poses']['saved_pose']['meshes'][maya_name]['points'])
                record['usdMaxMotionCm']=float(np.linalg.norm(points-usd_rest,axis=1).max())
            if label=='saved_pose':
                record['counts']=list(mesh.GetFaceVertexCountsAttr().Get())
                record['indices']=list(mesh.GetFaceVertexIndicesAttr().Get())
            record['passed']=record['maxErrorCm']<=result['toleranceCm'] and record['topologyMatches']
            item['meshes'][maya_name]=record
        item['passed']=item['valid'] and len(item['meshes'])==len(expected['meshes']) and all(m.get('passed',False) for m in item['meshes'].values()) and all(c['passed'] for c in item['curves'].values())
    except Exception: item['error']=traceback.format_exc();item['passed']=False
    finally:
        if attribute: attribute.Set(0.0)
    result['poses'][label]=item
    print(label,'PASS' if item['passed'] else 'FAIL',[(n.split('|')[-1],m.get('maxErrorCm')) for n,m in item['meshes'].items()],flush=True)
result['passed']=all(p['passed'] for p in result['poses'].values()) and len(result['poses'])==5
(out/'comparison.json').write_text(json.dumps(result))
sys.exit(0 if result['passed'] else 1)
