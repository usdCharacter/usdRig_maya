"""
Wire format between the Maya client and the RigExec worker.

Pure Python with no USD or Maya imports: the worker runs it under the
interpreter that owns the rig's USD build, the client under Maya's. The two
processes share no objects, only these framed byte messages over the
worker's stdin and stdout.

Every message is a little-endian u32 length followed by that many bytes.
The first byte of a request names the operation:

    D            describe the rig; the reply is UTF-8 JSON
    E t n v[n]   evaluate at time t (f64) with n (u32) channel values (f64)
                 in describe order; the reply is the frame (see Frame)
    M json       switch IK/FK or a space without moving anything: the
                 request names the switch attribute, its new value, the
                 time and every channel value; the reply is UTF-8 JSON of
                 the channel values to set
    Q            quit

A reply that starts with b"!" is an error, the rest UTF-8 text.
"""

import json
import struct

OP_DESCRIBE = b"D"
OP_EVALUATE = b"E"
OP_MATCH = b"M"
OP_QUIT = b"Q"
ERROR = b"!"

_LEN = struct.Struct("<I")
_EVAL_HEADER = struct.Struct("<dI")


def WriteMessage(stream, payload):
    stream.write(_LEN.pack(len(payload)))
    stream.write(payload)
    stream.flush()


def ReadMessage(stream):
    """The next message's bytes, or None at end of stream."""
    header = _ReadExactly(stream, _LEN.size)
    if header is None:
        return None
    (size,) = _LEN.unpack(header)
    payload = _ReadExactly(stream, size)
    if payload is None:
        raise EOFError("stream ended inside a message")
    return payload


def _ReadExactly(stream, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            if remaining == size:
                return None
            raise EOFError("stream ended inside a message")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def PackEvaluate(time, values):
    return (OP_EVALUATE + _EVAL_HEADER.pack(float(time), len(values)) +
            struct.pack("<%dd" % len(values), *values))


def UnpackEvaluate(payload):
    time, count = _EVAL_HEADER.unpack_from(payload, 1)
    offset = 1 + _EVAL_HEADER.size
    values = struct.unpack_from("<%dd" % count, payload, offset)
    return time, values


def PackMatch(time, values, attr, value):
    return OP_MATCH + PackJson({"time": float(time), "values": list(values),
                                "attr": attr, "value": float(value)})


def PackJson(obj):
    return json.dumps(obj, separators=(",", ":")).encode("utf-8")


def UnpackJson(payload):
    return json.loads(payload.decode("utf-8"))


class FrameLayout:
    """
    Byte offsets of one evaluated frame, fixed by the description.

    The frame is, in order: every mesh's points as f32 xyz in mesh order,
    then every control's ASSET-space matrix as 16 f64 (row-major, USD
    row-vector convention, which is also Maya's; the description's
    assetToWorld places the asset), then every mesh's world matrix the same
    way.
    """

    def __init__(self, description):
        self.pointCounts = [mesh["pointCount"] for mesh in description["meshes"]]
        self.controlCount = len(description["controls"])
        self.meshCount = len(description["meshes"])
        self.pointOffsets = []
        offset = 0
        for count in self.pointCounts:
            self.pointOffsets.append(offset)
            offset += count * 12
        self.controlMatricesOffset = offset
        offset += self.controlCount * 128
        self.meshMatricesOffset = offset
        offset += self.meshCount * 128
        self.size = offset
