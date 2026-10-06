"""Download pinned examples, or compare the native importer with Maya results.

python proofDownloadedExamples.py --download-only
mayapy proofMayaReference.py <build/maya-example-proof>
python proofDownloadedExamples.py

The compare step needs the built rigexec Python module and numpy. Returns a
nonzero status for any numerical mismatch; unsupported graph data stays reported.
"""
import argparse, hashlib, json, os, pathlib, sys, traceback, urllib.request
repo=pathlib.Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=pathlib.Path,default=repo/'build/maya-example-proof')
parser.add_argument('--download-only',action='store_true')
args=parser.parse_args()
out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
manifest=json.loads(pathlib.Path(__file__).with_name('downloadedExamples.json').read_text())
for name,asset in manifest['files'].items():
    path=out/name
    if args.download_only and not path.exists():
        path.write_bytes(urllib.request.urlopen(asset['url'],timeout=60).read())
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest()!=asset['sha256']:
        raise RuntimeError('Missing or modified download: '+str(path))
(out/'sources.json').write_text(json.dumps(manifest,indent=2)+'\n')
if args.download_only:
    print('Pinned downloads verified:',out)
    sys.exit(0)
os.environ['PATH']=os.pathsep.join([str(repo/'build'),str(repo.parent/'usd-install/lib'),str(repo.parent/'usd-install/bin'),os.environ.get('PATH','')])
sys.path.insert(0,str(repo/'tests/python'))
from rigexec_test_env import SetupPluginTest
SetupPluginTest(repo/'build')
sys.path.insert(0,str(repo/'build/python'))
from pxr import Plug, Usd
Plug.Registry().RegisterPlugins(os.environ.get('USDMAYARIG_PLUGIN_RESOURCES',str(repo/'build/plugin/usdMayaRig/usd/usdMayaRig/resources')))
Plug.Registry().RegisterPlugins(str(repo/'plugin/rigExecSchema/resources'))
import rigexec, numpy as np, hashlib, collections, os
from pxr import Sdf,UsdGeom,UsdShade
reference=json.loads((out/'maya-reference.json').read_text())
results={}
for name,ground in reference.items():
    try:
        s=Usd.Stage.Open(str(out/name)); assert s
        s.GetRootLayer().Export(str(out/(name+'.usda')))
        diagnostics=list(s.GetDefaultPrim().GetAttribute('maya:diagnostics').Get())
        driver=next(p for p in s.Traverse() if p.GetCustomDataByKey('maya:name') and p.GetCustomDataByKey('maya:name').split('|')[-1]==ground['driver'] and p.GetTypeName() in ['RigExecControl','RigExecJoint'])
        mesh=next(p for p in s.Traverse() if p.IsA(UsdGeom.Mesh) and (p.GetCustomDataByKey('maya:name') or '').split('|')[-1]==ground['mesh'])
        topology=UsdGeom.Mesh(mesh); counts=list(topology.GetFaceVertexCountsAttr().Get()); indices=list(topology.GetFaceVertexIndicesAttr().Get())
        uv=UsdGeom.PrimvarsAPI(mesh).GetPrimvar('st'); material=UsdShade.MaterialBindingAPI(mesh).ComputeBoundMaterial()[0]
        overlay=Sdf.Layer.CreateAnonymous('proof.usda'); s.GetSessionLayer().subLayerPaths.append(overlay.identifier); s.SetEditTarget(overlay)
        r=rigexec.Rig(s,'/Rig'); r.compile()
        pose_results={}
        evaluated=Usd.Stage.CreateInMemory()
        em=UsdGeom.Mesh.Define(evaluated,'/Mesh'); em.CreateFaceVertexCountsAttr(counts); em.CreateFaceVertexIndicesAttr(indices); em.CreateSubdivisionSchemeAttr('none')
        evaluated.SetDefaultPrim(em.GetPrim()); UsdGeom.SetStageMetersPerUnit(evaluated,0.01); UsdGeom.SetStageUpAxis(evaluated,'Y')
        for frame,(label,expected) in enumerate(ground['poses'].items(),1):
            for c in ['tx','ty','tz','rx','ry','rz','sx','sy','sz']:
                driver.GetAttribute('avars:'+c).Set(float(1 if c.startswith('s') else 0))
            for c,v in expected['channels'].items(): driver.GetAttribute('avars:'+c).Set(float(v))
            pose=r.evaluate(-1)
            assert pose.valid
            got=np.array(pose.moved_property(str(mesh.GetPath())+'.points'),dtype=float)
            want=np.array(expected['points'],dtype=float)
            error=np.linalg.norm(got-want,axis=1)
            pose_results[label]={'maxVertexErrorCm':float(error.max()),'rmsVertexErrorCm':float(np.sqrt(np.mean(error**2))),'points':got.tolist(),'channels':expected['channels'],'valid':pose.valid,'evaluatorDiagnostics':list(pose.diagnostics)}
            em.GetPointsAttr().Set(got.tolist(),frame)
        evaluated.SetStartTimeCode(1); evaluated.SetEndTimeCode(4); evaluated.GetRootLayer().Export(str(out/(name+'.evaluated.usda')))
        result={'sha256':hashlib.sha256((out/name).read_bytes()).hexdigest(),'primTypes':dict(collections.Counter(str(p.GetTypeName()) for p in s.Traverse())),'driver':str(driver.GetPath()),'mesh':str(mesh.GetPath()),'vertices':len(ground['poses']['rest']['points']),'faces':len(counts),'topologyMatchesMaya':counts==ground['poses']['rest']['counts'] and indices==ground['poses']['rest']['indices'],'uvValues':len(uv.Get()) if uv else 0,'uvIndices':len(uv.GetIndices()) if uv else 0,'material':str(material.GetPath()) if material else None,'diagnostics':diagnostics,'poses':pose_results}
        result['uvMatchesMaya']=bool(np.allclose(np.array(uv.Get()),np.array(ground['poses']['rest']['uv']),rtol=0,atol=1e-6)) and list(uv.GetIndices())==ground['poses']['rest']['uvIndices']
        result['passed']=result['uvMatchesMaya'] and bool(material) and result['topologyMatchesMaya'] and all(p['maxVertexErrorCm']<1e-5 for p in pose_results.values())
        results[name]=result
        print(name, 'PASS' if result['passed'] else 'FAIL', [(n,p['maxVertexErrorCm']) for n,p in pose_results.items()],flush=True)
    except Exception:
        results[name]={'error':traceback.format_exc(),'passed':False}; print(results[name]['error'],flush=True)
(out/'comparison.json').write_text(json.dumps(results,indent=2))

assert all(v['passed'] for v in results.values()), 'One or more downloaded rigs failed parity'
