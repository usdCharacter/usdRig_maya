"""Run with mayapy, passing the downloaded-example directory.
Scripts embedded in the examples are disabled. Animation inputs are disconnected
only in this disposable Maya process to compare manual offsets from the saved pose.
The downloaded files are never saved or modified.
"""
import os, json, pathlib, traceback, sys
import maya.standalone
maya.standalone.initialize(name='python')
import maya.cmds as cmds
import maya.api.OpenMaya as om
out=pathlib.Path(sys.argv[1]).resolve()
result={}
for name,driver,mesh in [('Ball_rig.ma','ball_ctrl','meshShape'),('UsdExportSkeletonWithIdentityBindPose.ma','joint1','pCubeShape1')]:
    try:
        cmds.file(str(out/name),open=True,force=True,ignoreVersion=True,executeScriptNodes=False,prompt=False)
        cmds.currentTime(5 if name.startswith('UsdExport') else 1)
        for channel in ['tx','ty','tz','rx','ry','rz','sx','sy','sz']:
            plug=driver+'.'+channel
            for src in cmds.listConnections(plug,source=True,destination=False,plugs=True) or []:
                cmds.disconnectAttr(src,plug)
        baseline={c:cmds.getAttr(driver+'.'+c) for c in ['tx','ty','tz','rx','ry','rz','sx','sy','sz']}
        def snapshot():
            sel=om.MSelectionList(); sel.add(mesh)
            fn=om.MFnMesh(sel.getDagPath(0))
            counts,indices=fn.getVertices()
            uv_u,uv_v=fn.getUVs(); uv_counts,uv_ids=fn.getAssignedUVs()
            return {'uv':list(zip(uv_u,uv_v)),'uvCounts':list(uv_counts),'uvIndices':list(uv_ids),'points':[[p.x,p.y,p.z] for p in fn.getPoints(om.MSpace.kWorld)],'counts':list(counts),'indices':list(indices)}
        poses={}
        for label,attrs in [('rest',{}),('translate',{'tx':4,'ty':2,'tz':1}),('rotate',{'rx':20,'ry':35,'rz':15}),('scale',{'sx':1.25,'sy':1.25,'sz':1.25})]:
            for c in ['tx','ty','tz','rx','ry','rz','sx','sy','sz']:
                cmds.setAttr(driver+'.'+c,baseline[c])
            for c,v in attrs.items(): cmds.setAttr(driver+'.'+c,baseline[c]*v if c.startswith('s') else baseline[c]+v)
            poses[label]={'channels':attrs,**snapshot()}
        result[name]={'driver':driver,'mesh':mesh,'baselineChannels':baseline,'poses':poses,'mayaVersion':cmds.about(version=True)}
        print(name,'OK',len(poses['rest']['points']),flush=True)
    except Exception:
        result[name]={'error':traceback.format_exc()}; print(result[name]['error'],flush=True)
(out/'maya-reference.json').write_text(json.dumps(result,indent=2))
maya.standalone.uninitialize()

