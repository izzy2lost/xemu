# Savestate changelog

One entry per section version (tools/native/statelock.py).

## ACI, APU, CACHE, FILE, KERN, MISC, MM, NV2A, PAGE, PBD, RAM, THRD, TIMR, USB, VM v1 (2026-10-20)

- first version

## FILE v2 (2026-10-21, after ba8ad66cc6)

- NcFile.del: a file deleted through a handle stays until the last handle on it closes (v1 states: a file deleted while open reopens empty)
- upgraded: v1..v1 states load

## MODS v1 (2026-10-21, after 6abc1f8db4)

- first version

## TIMR v2 (2026-10-22, after ed4707c8ae)

- a timer's APC routine, context and thread (NtSetTimerEx's TimerApcRoutine, delivered at expiry)
- upgraded: v1..v1 states load

## FILE v3 (2026-10-23, after e9b90ac2d6)

- NcFile.no_buffering: a handle opened with FILE_NO_INTERMEDIATE_BUFFERING; only its writes pend (v2 states: buffered, writes complete in the call)
- upgraded: v1..v2 states load

## MM v2 (2026-10-26, after 1b782ed02b)

- Contig.protect: a contiguous block's ProtectionType, MmQueryAddressProtect's answer (v1 states: read-write)
- upgraded: v1..v1 states load
