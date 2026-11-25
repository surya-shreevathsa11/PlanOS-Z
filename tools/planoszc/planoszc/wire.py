"""IR wire format v1: the compact binary blob uploaded to a device over UART / the shell.

Layout (little-endian). The device re-derives WCET and resource claims itself, so the blob
carries NO resource masks; `host_wcet_ms` is advisory (the device only reports mismatches).

    'P' 'Z' | version u8 | flags u8 (=0) | perm_mask u32 | deadline_ms u32 |
    n_steps u8 | plan_id_len u8 | plan_id
    n_steps x { id_len u8 | id | skill u8 | deps_mask u32 | confirm u8 | when_step i8 |
                when_op u8 | when_value_milli i32 | n_args u8 | args i32 x n_args | host_wcet_ms u32 }
    crc16 u16   (CRC-16/CCITT-FALSE over every preceding byte)

Mirrored by runtime/src/ingress/ir_wire.c (tests/host cross-check both directions).
"""

from __future__ import annotations

import base64
import struct

from .compile import IR_VERSION, MAX_ARGS, MAX_STEPS

MAGIC = b"PZ"
PLAN_ID_MAX = 31  # chars; device buffer is 32 incl. NUL
STEP_ID_MAX = 23  # chars; device buffer is 24 incl. NUL
UPLOAD_MAX_BYTES = 2048
CHUNK_BYTES = 144  # 192 base64 chars per `planosz load data` line (fits a 512-byte shell buffer)


class WireError(Exception):
    pass


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ir: dict, *, version: int | None = None, flags: int = 0) -> bytes:
    """IR (the dict produced by compile_plan) -> wire bytes. `version` is overridable so tests
    can build a deliberately unsupported blob."""
    plan_id = ir["plan_id"].encode()
    if not 1 <= len(plan_id) <= PLAN_ID_MAX:
        raise WireError(f"plan_id must be 1..{PLAN_ID_MAX} bytes for the device slot")
    steps = ir["steps"]
    if not 1 <= len(steps) <= MAX_STEPS:
        raise WireError(f"1..{MAX_STEPS} steps required")
    out = bytearray(MAGIC)
    out += struct.pack("<BBII", IR_VERSION if version is None else version, flags, ir["perm_mask"],
                       ir["deadline_ms"])
    out += struct.pack("<BB", len(steps), len(plan_id)) + plan_id
    for s in steps:
        sid = s["id"].encode()
        if not 1 <= len(sid) <= STEP_ID_MAX:
            raise WireError(f"step id '{s['id']}' must be 1..{STEP_ID_MAX} bytes for the device slot")
        args = s["args"]
        if len(args) > MAX_ARGS:
            raise WireError(f"step '{s['id']}': more than {MAX_ARGS} args")
        out += struct.pack("<B", len(sid)) + sid
        out += struct.pack("<BIBbBiB", s["skill"], s["deps_mask"], int(s["confirm"]), s["when_step"],
                           s["when_op"], s["when_value_milli"], len(args))
        out += struct.pack(f"<{len(args)}i", *args)
        out += struct.pack("<I", s["wcet_ms"] & 0xFFFFFFFF)
    out += struct.pack("<H", crc16_ccitt(bytes(out)))
    if len(out) > UPLOAD_MAX_BYTES:
        raise WireError(f"blob is {len(out)} bytes; device upload buffer is {UPLOAD_MAX_BYTES}")
    return bytes(out)


def decode(blob: bytes) -> dict:
    """Wire bytes -> IR-shaped dict (masks are reported as 0: they are not on the wire)."""
    if len(blob) < 16 or blob[:2] != MAGIC:
        raise WireError("bad magic")
    if struct.unpack_from("<H", blob, len(blob) - 2)[0] != crc16_ccitt(blob[:-2]):
        raise WireError("crc mismatch")
    version, flags, perm, deadline = struct.unpack_from("<BBII", blob, 2)
    if version != IR_VERSION or flags != 0:
        raise WireError(f"unsupported version/flags {version}/{flags}")
    n_steps, idlen = struct.unpack_from("<BB", blob, 12)
    pos = 14
    plan_id = blob[pos : pos + idlen].decode()
    pos += idlen
    steps = []
    for _ in range(n_steps):
        (sl,) = struct.unpack_from("<B", blob, pos)
        pos += 1
        sid = blob[pos : pos + sl].decode()
        pos += sl
        skill, deps, confirm, wstep, wop, wval, nargs = struct.unpack_from("<BIBbBiB", blob, pos)
        pos += struct.calcsize("<BIBbBiB")
        args = list(struct.unpack_from(f"<{nargs}i", blob, pos))
        pos += 4 * nargs
        (wcet,) = struct.unpack_from("<I", blob, pos)
        pos += 4
        steps.append({"id": sid, "skill": skill, "deps_mask": deps, "confirm": confirm, "when_step": wstep,
                      "when_op": wop, "when_value_milli": wval, "args": args, "wcet_ms": wcet,
                      "excl_mask": 0, "shared_mask": 0, "mutex_mask": 0})
    if pos != len(blob) - 2:
        raise WireError("trailing bytes")
    return {"ir_version": version, "plan_id": plan_id, "deadline_ms": deadline, "perm_mask": perm,
            "steps": steps}


def shell_lines(blob: bytes, prefix: str = "planosz load") -> list[str]:
    """The shell paste payload: begin / data* / end. Chunks are multiples of 3 bytes so only the
    last base64 chunk carries padding."""
    lines = [f"{prefix} begin"]
    for i in range(0, len(blob), CHUNK_BYTES):
        lines.append(f"{prefix} data {base64.b64encode(blob[i : i + CHUNK_BYTES]).decode()}")
    lines.append(f"{prefix} end")
    return lines
