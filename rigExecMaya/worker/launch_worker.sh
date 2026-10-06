#!/bin/sh
# Starts the RigExec worker under the RigExec checkout's own Python and USD.
# Usage: launch_worker.sh <rig root> <stage> [<rig prim path>]
root="$1"
shift
. "$root/bin/_env.sh" || exit 1
exec "$PY" "$(dirname "$0")/rigexec_maya_worker.py" "$@"
