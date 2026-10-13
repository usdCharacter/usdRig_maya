#!/bin/sh
# Runs the export under the usdRig checkout's own Python and USD.
# Usage: launch_export.sh <usdRig root> <stage> <out.rigexec> <out.json> [<rig prim path>]
root="$1"
shift
. "$root/bin/_env.sh" || exit 1
exec "$PY" "$(dirname "$0")/rigexec_maya_export.py" "$@"
