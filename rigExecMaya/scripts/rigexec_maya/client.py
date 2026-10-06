"""
The Maya side of the worker connection: starts the worker, describes the rig,
and evaluates frames.

No USD and no Maya imports. The worker needs RigExec's own USD build, which
cannot be loaded into Maya, so it runs as a child process with an
environment built from the RigExec checkout's helper (bin/_env.bat or
bin/_env.sh). Maya's Python and USD settings are removed from that
environment first: inherited, they would point the worker at Maya's
interpreter and Maya's USD plugins.
"""

import os
import subprocess
import sys
import threading

from . import protocol

# Environment that belongs to Maya's process and must not reach the worker.
# Maya's USD is announced through PXR_* (PXR_USD_WINDOWS_DLL_PATH alone makes
# the worker's USD load Maya's same-named libraries), so every PXR_ variable
# goes, along with Maya's Python settings and its USD and MaterialX homes.
_SCRUBBED = ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "PYTHONUSERBASE",
             "USD_LOCATION", "MATERIALX_SEARCH_PATH")
_SCRUBBED_PREFIXES = ("PXR_", "MAYAUSD_", "MAYA_USD_", "MAYA_PXR_")


def _WorkerEnvironment():
    env = {}
    for key, value in os.environ.items():
        upper = key.upper()
        if upper in _SCRUBBED or upper.startswith(_SCRUBBED_PREFIXES):
            continue
        env[key] = value
    # Maya puts its own bin directories (TBB, Python, Qt) at the front of
    # PATH; the worker resolves libraries from the RigExec install instead.
    for key in env:
        if key.upper() == "PATH":
            env[key] = os.pathsep.join(
                entry for entry in env[key].split(os.pathsep)
                if "autodesk" not in entry.lower())
    return env


def RigExecRoot():
    """The usdRig checkout staged beside this sidecar, overridable with
    RIGEXEC_ROOT."""
    explicit = os.environ.get("RIGEXEC_ROOT")
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, os.pardir, os.pardir,
                                         os.pardir, os.pardir, "usdRig"))


def _Command(stagePath, rigPath):
    here = os.path.dirname(os.path.abspath(__file__))
    worker = os.path.normpath(os.path.join(here, os.pardir, os.pardir,
                                           "worker"))
    args = [RigExecRoot(), stagePath] + ([rigPath] if rigPath else [])
    if sys.platform == "win32":
        return [os.path.join(worker, "launch_worker.bat")] + args
    return ["/bin/sh", os.path.join(worker, "launch_worker.sh")] + args


class WorkerError(RuntimeError):
    pass


class Client:
    """One worker process serving one rig."""

    def __init__(self, stagePath, rigPath=None, log=None):
        command = _Command(stagePath, rigPath)
        env = _WorkerEnvironment()
        self._log = log
        self._process = subprocess.Popen(
            command, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self._stderr = []
        threading.Thread(target=self._DrainStderr, daemon=True).start()
        self._lock = threading.Lock()
        ready = protocol.ReadMessage(self._process.stdout)
        if ready != b"ready":
            self.Close()
            raise WorkerError(self._Failure(ready))
        self.description = self._Request(protocol.OP_DESCRIBE, json=True)
        self.layout = protocol.FrameLayout(self.description)

    def _DrainStderr(self):
        for line in iter(self._process.stderr.readline, b""):
            text = line.decode("utf-8", "replace").rstrip()
            self._stderr.append(text)
            del self._stderr[:-200]
            if self._log is not None and not text.startswith("Warning:"):
                self._log(text)

    def _Failure(self, reply):
        if reply and reply[:1] == protocol.ERROR:
            return reply[1:].decode("utf-8", "replace")
        tail = "\n".join(self._stderr[-20:])
        return "the RigExec worker did not start%s" % (
            ":\n" + tail if tail else "")

    def _Request(self, payload, json=False):
        with self._lock:
            if self._process.poll() is not None:
                raise WorkerError("the RigExec worker has exited")
            protocol.WriteMessage(self._process.stdin, payload)
            reply = protocol.ReadMessage(self._process.stdout)
        if reply is None or reply[:1] == protocol.ERROR:
            raise WorkerError(self._Failure(reply))
        return protocol.UnpackJson(reply) if json else reply

    def Evaluate(self, time, values):
        """One frame's bytes, laid out as self.layout describes."""
        return self._Request(protocol.PackEvaluate(time, values))

    def Match(self, time, values, attr, value):
        """The channel values that set the switch `attr` (a rig attribute
        path) to `value` without moving the pose: {attribute path: value},
        the switch included."""
        return self._Request(protocol.PackMatch(time, values, attr, value),
                             json=True)

    def Close(self):
        process, self._process = self._process, None
        if process is None:
            return
        try:
            if process.poll() is None:
                protocol.WriteMessage(process.stdin, protocol.OP_QUIT)
                process.wait(5)
        except Exception:
            pass
        for stream in (process.stdin, process.stdout):
            try:
                stream.close()
            except Exception:
                pass
        if process.poll() is None:
            process.kill()

    def __del__(self):
        if getattr(self, "_process", None) is not None:
            self.Close()
