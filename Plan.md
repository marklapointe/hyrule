# Hyrule Kernel Module - Plan

## Goal
Fix the kernel panic occurring during the self-unload sequence triggered by Link's death (HP <= 0). Ensure the module unloads gracefully and provides educational value through documented stubs.

## Current State
- The module implements a self-unload mechanism in `hyrule_death_worker` using `kern_kldunload`.
- A 1-second delay is implemented via `taskqueue_enqueue_timeout` to allow the write operation to complete.
- Educational stubs for all standard character device operations (`ioctl`, `poll`, `mmap`, etc.) have been added to `hyrule.c` and `hyrule_map.c`.
- The `link_death` test causes a kernel panic, likely due to the taskqueue thread attempting to return to unmapped module memory or accessing the unmapped `hyrule_death_task` structure after unload.

## Accomplishments
- Implemented `hyrule_death_worker` and integrated it with Link's health stat.
- Added comprehensive device operation stubs with educational comments.
- Identified the likely cause of the kernel panic (return to unmapped memory).
- Verified that the `MOD_UNLOAD` event is reached before the panic.

## Done:
- Created `test.sh` to automate testing and append to `last.log`.
- Simplified `Makefile` to avoid installation issues during testing.

To-Do
- Refactor the self-unload mechanism to avoid returning to unmapped memory.
- Experiment with heap-allocated task structures to see if it mitigates the panic.
- Investigate if `kthread_add` or a similar dedicated thread approach can be used safely.
- Run tests and append all output to `last.log`.
- Finalize documentation in the code.
