"""Capture the original rhino in Maya, without disconnecting or saving its graph."""
import hashlib, json, pathlib, sys, traceback
import maya.standalone
maya.standalone.initialize(name='python')
import maya.cmds as cmds
import maya.api.OpenMaya as om
out=pathlib.Path(sys.argv[1]).resolve()
manifest=json.loads(pathlib.Path(__file__).with_name('characterExample.json').read_text())
digest=hashlib.sha256((out/manifest['file']).read_bytes()).hexdigest()
assert digest==manifest['sha256'], 'Reference source differs from pinned download'
result={'mayaVersion':cmds.about(version=True),'sha256':digest,'poses':{},'errors':[]}
try:
    cmds.file(str(out/'rhino_rig_001.ma'),open=True,force=True,ignoreVersion=True,executeScriptNodes=False,prompt=False)
    cmds.evaluationManager(mode='off')
    mesh_names=cmds.listRelatives('model_grp',allDescendents=True,type='mesh',fullPath=True)
    mesh_names=[m for m in mesh_names if not cmds.getAttr(m+'.intermediateObject')]
    result['meshes']=mesh_names
    curve_names=cmds.ls(type='nurbsCurve',long=True)
    def control_snapshot():
        curves={}
        for name in curve_names:
            sel=om.MSelectionList();sel.add(name);fn=om.MFnNurbsCurve(sel.getDagPath(0))
            parent=cmds.listRelatives(name,parent=True,fullPath=True)[0]
            curves[name]={'points':[[p.x,p.y,p.z] for p in fn.cvPositions(om.MSpace.kWorld)],
                'control':parent,'matrix':cmds.xform(parent,q=True,ws=True,m=True)}
        return curves
    def snapshot():
        meshes={}
        for mesh in mesh_names:
            sel=om.MSelectionList();sel.add(mesh);fn=om.MFnMesh(sel.getDagPath(0))
            counts,indices=fn.getVertices(); us,vs=fn.getUVs();uvcounts,uvids=fn.getAssignedUVs()
            meshes[mesh]={'points':[[p.x,p.y,p.z] for p in fn.getPoints(om.MSpace.kWorld)],'counts':list(counts),'indices':list(indices),'uv':list(zip(us,vs)),'uvIndices':list(uvids)}
        return meshes
    tests=[('saved_pose',None,None,0),('world_translate','world_cnt_001','tx',10),('head_turn','cn_head_jnt_001_cnt','rz',15),('front_foot_ik','lf_ft_foot_001_cnt','ty',10),('tail_bend','cn_tail_jnt_003_cnt','ry',20)]
    for label,driver,channel,delta in tests:
        baseline=None
        try:
            if driver:
                baseline=cmds.getAttr(driver+'.'+channel)
                if not cmds.getAttr(driver+'.'+channel,settable=True): raise RuntimeError('Control is not editable: '+driver+'.'+channel)
                cmds.setAttr(driver+'.'+channel,baseline+delta)
            result['poses'][label]={'driver':driver,'channel':channel,'delta':delta,'baseline':baseline,'meshes':snapshot(),'curves':control_snapshot()}
            print(label,'captured',flush=True)
        except Exception: result['errors'].append({'pose':label,'error':traceback.format_exc()})
        finally:
            if driver and baseline is not None: cmds.setAttr(driver+'.'+channel,baseline)
except Exception: result['errors'].append(traceback.format_exc())
(out/'maya-reference.json').write_text(json.dumps(result))
print(json.dumps({'meshes':result.get('meshes'),'errors':result['errors']}),flush=True)
maya.standalone.uninitialize()
sys.exit(1 if result['errors'] else 0)
