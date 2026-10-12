"""Select the actual imported control when its original Maya curve is picked."""
from pxr import Tf
from pxr.Usdviewq.qt import QtCore
from pxr.Usdviewq.plugin import PluginContainer


class MayaRigContainer(PluginContainer):
    def registerPlugins(self, registry, usdviewApi):
        self._api = usdviewApi
        self._registry = registry
        self._changing = False
        usdviewApi.dataModel.selection.signalPrimSelectionChanged.connect(self._selectionChanged)
        usdviewApi.dataModel.signalStageReplaced.connect(self._stageChanged)
        self._stageChanged()

    def configureView(self, registry, builder):
        # Required by PluginContainer; this container adds no menu items.
        pass

    def _stageChanged(self, *unused):
        # Plugins register before the stage and the RigExec commands exist.
        QtCore.QTimer.singleShot(0, self._showImportedPicker)

    def _showImportedPicker(self):
        stage = self._api.stage
        if not stage:
            return
        imported = any(p.GetAttribute('maya:diagnostics')
                       for p in stage.GetPseudoRoot().GetChildren()
                       if p.GetTypeName() == 'RigExecRoot')
        if not imported:
            return
        try:
            import pickerScene
            if pickerScene.find(stage):
                command = self._registry.getCommandPlugin('RigExecUsdviewContainer.picker')
                if command:
                    command.run()
        except ImportError:
            # The companion can be installed in hosts without RigExec's UI.
            return

    def _selectionChanged(self, *unused):
        if self._changing:
            return
        selected = list(self._api.selectedPrims or [])
        resolved = []
        changed = False
        for prim in selected:
            if not prim or prim.IsPseudoRoot():
                continue
            rel = prim.GetRelationship('maya:control')
            targets = rel.GetTargets() if rel else []
            owner = prim.GetStage().GetPrimAtPath(targets[0]) if len(targets) == 1 else None
            if owner and owner.GetTypeName() == 'RigExecControl':
                resolved.append(owner)
                changed = True
            else:
                resolved.append(prim)
        if not changed:
            return
        self._changing = True
        try:
            selection = self._api.dataModel.selection
            with selection.batchPrimChanges:
                selection.clearPrims()
                seen = set()
                for prim in resolved:
                    if prim.GetPath() not in seen:
                        selection.addPrim(prim)
                        seen.add(prim.GetPath())
        finally:
            self._changing = False


Tf.Type.Define(MayaRigContainer)
