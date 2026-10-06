"""Interactive rhino control check; leaves usdview open, source file unchanged.

Briefly edits the head control by 15 degrees at the current frame, verifies
curve and body motion, then restores it. Opens the exported USD only.
The whole-character deformation test remains a separate, currently failing test.
"""
import argparse, os, sys, pathlib, json, traceback
repo=pathlib.Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=pathlib.Path,default=repo/'build/maya-character-proof')
out=parser.parse_args().output.resolve()
usd=repo.parent/'usd-install'
os.environ['PATH']=os.pathsep.join([str(repo/'build'),str(usd/'bin'),str(usd/'lib'),os.environ.get('PATH','')])
plugin_paths=[repo/'build/usd/rigExecSchema/resources',repo/'build/usd/rigExecImaging/resources',os.environ.get('USDMAYARIG_PLUGIN_RESOURCES',repo/'build/plugin/usdMayaRig/usd/usdMayaRig/resources'),repo/'plugin/rigExecUsdview',repo/'build/python/UsdNoodles',repo/'plugin/touchPose',repo/'plugin/shapeEditor',repo/'plugin/usdMayaRig/usdview']
os.environ['PXR_PLUGINPATH_NAME']=os.pathsep.join(map(str,plugin_paths))
os.environ['TOUCHPOSE_PLUGIN_DIR']=str(repo/'plugin/touchPose')
os.environ['RIGEXEC_USD_INSTALL']=str(usd)
sys.path.insert(0,str(repo/'tests/python'))
from rigexec_test_env import SetupPluginTest
SetupPluginTest(repo/'build')
for p in [repo/'build/python',repo/'plugin/rigExecUsdview',repo/'plugin/touchPose',repo/'plugin/shapeEditor',repo/'plugin/usdMayaRig/usdview']:
    sys.path.insert(0,str(p))
from pxr import Usdviewq, Plug, UsdGeom
from pxr.Usdviewq.qt import QtCore

class InspectLauncher(Usdviewq.Launcher):
    def LaunchProcess(self,args,app,controller):
        def prepare():
            status={}
            try:
                api=controller._usdviewApi
                stage=api.stage
                stage.SetEditTarget(stage.GetSessionLayer())
                status['stage']=stage.GetRootLayer().identifier
                status['deltaMushOperators']=sum(p.GetTypeName()=='RigExecDeltaMushMover' for p in stage.Traverse())
                assert status['deltaMushOperators']==1
                status['plugins']=[p.name for p in Plug.Registry().GetAllPlugins() if p.isLoaded and ('rig' in p.name.lower() or 'noodle' in p.name.lower())]
                body=next(p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh) and (p.GetCustomDataByKey('maya:name') or '').endswith('|rhino_body_geo_Shape'))
                api.dataModel.selection.setPrim(body)
                controller._frameSelection()
                head=next(p for p in stage.Traverse() if p.GetTypeName()=='RigExecControl' and (p.GetCustomDataByKey('maya:name') or '').endswith('|cn_head_jnt_001_cnt'))
                shape=stage.GetPrimAtPath(head.GetRelationship('maya:controlShapes').GetTargets()[0])
                api.dataModel.selection.setPrim(shape)
                app.processEvents()
                status['curvePickSelectsControl']=head in list(api.selectedPrims)
                assert status['curvePickSelectsControl'], 'Picking curve did not select its control'
                status['selectedControl']=str(head.GetPath())
                status['commands']={}
                for name in ['reactivate','avarEditor','execStack']:
                    cmd=controller._plugRegistry.getCommandPlugin('RigExecUsdviewContainer.'+name)
                    status['commands'][name]=bool(cmd)
                    if cmd: cmd.run()
                import execStackUI, avarEditorUI
                stack=execStackUI.ExecStackPanel.GetInstance(api)
                avars=avarEditorUI.AvarEditorPanel.GetInstance(api,None)
                status['executionStackStatus']=stack._status.text()
                status['executionStackMovers']=len(stack._movers)
                status['avarRows']=len(avars._rows)
                # Exercise a real editor change, then restore the session.
                import rigexec
                from execStackUI import _CompilableStage
                oracle=rigexec.Rig(_CompilableStage(stage),'/Rig');oracle.compile()
                time=api.dataModel.currentFrame
                evaluation_time=-1 if time.IsDefault() else time.GetValue()
                status['editorTime']=evaluation_time
                before=oracle.evaluate(evaluation_time)
                avars.SetValue('avars:rz',15.0)
                app.processEvents()
                after=oracle.evaluate(evaluation_time)
                prop=str(shape.GetPath())+'.points'
                status['editorMovesCurve']=before.moved_property(prop)!=after.moved_property(prop)
                body_prop=str(body.GetPath())+'.points'
                status['editorMovesBody']=before.moved_property(body_prop)!=after.moved_property(body_prop)
                avars.SetValue('avars:rz',0.0)
                assert status['editorMovesCurve'], 'Editor did not move the selected curve'
                assert status['editorMovesBody'], 'Editor did not deform the bound body'
                status['diagnostics']=len(stage.GetDefaultPrim().GetAttribute('maya:diagnostics').Get())
                controller._mainWindow.setWindowTitle('usdview - Native usdRig Rhino | deltaMush enabled; full parity incomplete')
                controller._mainWindow.resize(1400,950)
                controller._mainWindow.show()
                controller._mainWindow.raise_()
                status['visible']=controller._mainWindow.isVisible()
                QtCore.QTimer.singleShot(2000,lambda: controller._mainWindow.grab().save(str(out/'usdview-inspection.png')))
            except Exception:
                status['error']=traceback.format_exc()
                print(status['error'],flush=True)
            (out/'usdview-inspection.json').write_text(json.dumps(status,indent=2))
            print(json.dumps(status,indent=2),flush=True)
        QtCore.QTimer.singleShot(1800,prepare)
        super().LaunchProcess(args,app,controller)


sys.argv=[sys.argv[0],str(out/'rhino_imported.usda'),'--defaultsettings']
InspectLauncher().Run()
