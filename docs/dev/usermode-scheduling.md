# Usermode Scheduling Notes

K64 usermode is in transition. This document describes the current model precisely so future scheduler work can move forward without accidentally reintroducing the old wait-only behavior.

## Current Model

K64 has:

- Ring-3 ELF entry through `iretq`.
- `int 0x80` syscall entry.
- A service-call-only public ABI at syscall `25`.
- A kernel process table for `/ex` programs.
- Parent/child process identity.
- `proc.spawn`, `proc.wait`, `proc.info`, and `proc.exit`.
- Per-process fd tables for the active process record.
- Cooperative scheduler points through `sched.yield` and `sched.sleep`.

As of v0.3.32, spawned children are not tied only to blocking wait. A parent can:

1. call `proc.spawn`
2. receive a child PID immediately
3. call `sched.sleep` or `sched.yield`
4. allow the queued child to run
5. later call `proc.wait` and collect the exit code

The `backgroundspawn.elf` test exercises exactly that path.

## Why It Is Not Full Preemption Yet

The remaining architectural constraint is `active_ctx` in `k64_usermode.c`. It is still a single global description of the currently entered user address space and return context. That makes it safe to run one user context at a time, including nested child execution, but not yet safe for arbitrary timer-preemptive switching between many user processes.

Full preemptive Ring-3 scheduling needs:

- one saved trap frame per user process
- one kernel stack per user process or per user task
- CR3 stored per runnable user task
- fd table access through the current process, not the global active context
- service-call scratch buffers that are process-local or call-local
- timer IRQ logic that can save a user frame and restore a different user frame
- fault handling that kills only the faulting process and then schedules another runnable task

Until those exist, K64 should not claim CPU-bound user processes are preemptible.

## Where Cooperative Progress Happens

Current cooperative child progress happens in three places:

- `sched.yield`: runs one queued child belonging to the current process before yielding.
- `sched.sleep`: runs one queued child belonging to the current process before parking/sleeping.
- shell/service poll: runs queued orphan or returned-to-shell children when no user context is active.

Blocking `proc.wait` still has a direct child execution path for the requested PID. That keeps old process tests deterministic while allowing newer tests to prove background progress before wait.

## Failure Modes To Avoid

Do not run arbitrary child usermode directly from an interrupt frame. A previous experiment created scheduler tasks for spawned children and let the timer pick them. That exposed the missing per-user trap-frame model and could corrupt the IRQ return frame. The correct next step is not more ad hoc task entry; it is a real user task object with saved user registers and a kernel stack.

Do not let service handlers receive raw user pointers. Even when the caller is a user process, the syscall layer must copy request bytes into bounded kernel buffers before dispatch.

Do not let `waitpid` reap a process that is not a direct child. Parent-child ownership is part of the current process security boundary.

## Next Correct Step

The next robust scheduler milestone should add:

1. `k64_user_trap_frame_t` storage per process.
2. A user task kind in the scheduler.
3. A per-user-task kernel stack.
4. A syscall return path that can return to a selected user process, not necessarily the process that entered the syscall.
5. Timer IRQ save/restore for user mode.
6. A retirement plan for global `active_ctx`.

Once that exists, `spawn()` can create a runnable user task directly, and the cooperative queue bridge can be removed.
