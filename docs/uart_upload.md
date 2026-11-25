# Uploading a plan over the UART / shell

A plan does not have to be compiled into the firmware. The host compiles it to the IR v1 wire
format (versioned, CRC-16) and you paste the lines into the device shell; the device decodes
into one ephemeral slot named `uploaded` and runs the *same* admission as for an embedded plan.

```bash
planoszc compile examples/missions/04_parallel_overlap_accept.yaml --emit uart
```

```text
planosz load begin
planosz load data UFoBABIAAAD0AQAABBVkZW1vLnBhcmFsbGVsX292ZXJsYXAEdm9sdA...
planosz load data AQAAAAAeAAAADXE=
planosz load end
planosz run uploaded          # admit, then execute (or: planosz admit uploaded)
```

`load end` prints one `load` trace record (`{"seq":0,"type":"load","plan":"demo.parallel_overlap",
"status":"OK",...,"bytes":155}`); a bad upload prints `"status":"REJECT","code":"REJECT_UNSUPPORTED_IR"`
with a `detail` (`crc mismatch`, `unsupported version`, `truncated`, ...) and the slot stays empty.

## Format (v1)

| Field | Bytes |
|---|---|
| magic `'P' 'Z'`, version (=1), flags (=0) | 4 |
| permission mask u32, deadline_ms u32, n_steps u8, plan id length u8 + id (<= 31, `[A-Za-z0-9_.-]`) | 10 + id |
| per step: id length + id (<= 23), skill u8, deps u32, confirm u8, when_step i8, when_op u8, when_value_milli i32, n_args u8 + args i32 each, host wcet u32 (advisory) | variable |
| CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over everything before it, little endian | 2 |

At most 32 steps, 4 args per step, 2048 bytes in total; each `load data` line carries <= 144 bytes
(192 base64 characters) so it fits the shell line buffer. Resource masks are **not** on the wire,
and the host WCET is never used for a decision.

## Try the whole thing without hardware

```bash
make zephyr-setup                # once
make native-sim-shell            # stdin/stdout shell image
PATH=$PWD/.venv/bin:$PATH tools/ci/native_sim_shell.sh
```

The script uploads an honest plan (accepted, overlapped, replay-checked), a corrupted copy
(`REJECT_UNSUPPORTED_IR`, nothing admitted), a deadline-infeasible plan whose IR claims every step
takes 1 ms (`REJECT_DEADLINE_INFEASIBLE`, `ir_wcet_mismatch` reported) and a conflicting plan with
stripped masks (`REJECT_RESOURCE_CONFLICT`), then runs `planoszc replay --extract` on the console.
