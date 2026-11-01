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

## THRD v2 (2026-10-26, after ?)

- NcThread.tib_base, frame_top: a thread's stack keeps the Xbox kernel's FX save area (0x210 bytes) under its top; the TIB's StackBase (fs:[4]) and the thread's frames sit under it (v1 states: 0, the stack's old layout, kept)
- upgraded: v1..v1 states load

## KERN v2 (2026-10-26, after ?)

- NcKernel.handles_more: the handle table grows from 4096 slots to 16384 (Super Monkey Ball Deluxe keeps ~6100 files open); its slots past 4096 (v1 states: none, all free)
- upgraded: v1..v1 states load

## KERN v3 (2026-10-29, after 113a34e7e7)

- the kernel pool's first 64 KiB range sits just above the kernel (physical 0x50000), not 512 KiB under the top reserve (Burnout 2 lays out 0x2377000-0x3F00000 at fixed addresses); a state's boot puts it under the state's process object (v1, v2 states, and states saved since by a run that loaded one: where it was)
- upgraded: v1..v2 states load

## CHIH v1 (2026-11-01, after 2c68bb7664)

- first version

## THRD v3 (2026-11-01, after 2c68bb7664)

- NcThread.has_wait_irql, wait_irql: the IRQL a thread blocked at, given back when it runs again (a Chihiro's USB driver waits at DISPATCH_LEVEL); v2 states: none, the thread runs at PASSIVE_LEVEL
- upgraded: v1..v2 states load
