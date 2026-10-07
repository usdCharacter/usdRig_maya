"""
Exports a rig for Maya: runs export/rigexec_maya_export.py once, under the
usdRig checkout's own Python and USD, and reads back its description.

The export bakes the rig to a poseable .rigexec, which the rigExecRig node
plays in Maya's own process, and describes what Maya builds around it. It
can't run inside Maya: Maya's USD has the same library names as usdRig's
and an older OpenExec. So it runs as a child process, with Maya's Python
and USD settings removed from its environment (inherited, they would point
it at Maya's interpreter and Maya's USD plugins).

No USD imports here.
"""

import hashlib
import json
import os
import subprocess
import sys

# Environment that belongs to Maya's process and must not reach the export.
# Maya's USD is announced through PXR_* (PXR_USD_WINDOWS_DLL_PATH alone makes
# usdRig's USD load Maya's same-named libraries), so every PXR_ variable
# goes, along with Maya's Python settings and its USD and MaterialX homes.
_SCRUBBED = ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "PYTHONUSERBASE",
             "USD_LOCATION", "MATERIALX_SEARCH_PATH")
_SCRUBBED_PREFIXES = ("PXR_", "MAYAUSD_", "MAYA_USD_", "MAYA_PXR_")

_HERE = os.path.dirname(os.path.abspath(__file__))
_MODULE = os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir))


def RigExecRoot():
    """The usdRig checkout staged beside this sidecar, overridable with
    RIGEXEC_ROOT."""
    explicit = os.environ.get("RIGEXEC_ROOT")
    if explicit:
        return explicit
    return os.path.normpath(os.path.join(_MODULE, os.pardir, os.pardir,
                                         "usdRig"))


def CacheDirectory():
    """Where exported rigs are kept: RIGEXEC_MAYA_CACHE, or cache/ in this
    module."""
    return os.environ.get("RIGEXEC_MAYA_CACHE") or os.path.join(_MODULE,
                                                                "cache")


def _Environment():
    env = {}
    for key, value in os.environ.items():
        upper = key.upper()
        if upper in _SCRUBBED or upper.startswith(_SCRUBBED_PREFIXES):
            continue
        env[key] = value
    # Maya puts its own bin directories (TBB, Python, Qt) at the front of
    # PATH; the export resolves libraries from usdRig's build instead.
    for key in env:
        if key.upper() == "PATH":
            env[key] = os.pathsep.join(
                entry for entry in env[key].split(os.pathsep)
                if "autodesk" not in entry.lower())
    return env


def Export(stagePath, rigPath=None):
    """Bakes and describes the rig at `stagePath`; the description, with
    `rigFile` naming the bake."""
    stagePath = os.path.abspath(stagePath).replace("\\", "/")
    cache = CacheDirectory()
    os.makedirs(cache, exist_ok=True)
    key = hashlib.sha1((stagePath + "|" + (rigPath or "")).encode("utf-8"))
    base = os.path.join(cache, "%s_%s" % (
        os.path.splitext(os.path.basename(stagePath))[0],
        key.hexdigest()[:10]))
    rigFile, jsonFile = base + ".rigexec", base + ".json"
    launcher = os.path.join(_MODULE, "export", "launch_export")
    if sys.platform == "win32":
        command = [launcher + ".bat"]
    else:
        command = ["/bin/sh", launcher + ".sh"]
    command += [RigExecRoot(), stagePath, rigFile, jsonFile]
    if rigPath:
        command.append(rigPath)
    result = subprocess.run(
        command, env=_Environment(), stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode != 0 or not os.path.exists(jsonFile):
        output = result.stdout.decode("utf-8", "replace").splitlines()
        tail = "\n".join(line for line in output
                         if not line.startswith("Warning:"))[-3000:]
        raise RuntimeError("exporting %s failed:\n%s" % (stagePath, tail))
    with open(jsonFile) as source:
        return json.load(source)
