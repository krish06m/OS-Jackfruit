# Repository Compliance Audit

This file checks the current repository against [project-guide.md](project-guide.md) and records the places where the implementation is not fully following the assignment requirements.

I also scanned the tracked repo files for AI-related wording. No matches were found for the requested terms.

The implementation has since been updated to address the major issues listed below; keep this file as the pre-fix audit trail.

## Not Fully Followed

### 1. The CLI contract does not match the required syntax

The guide requires:

`engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]`

The current parser in [boilerplate/engine.c](boilerplate/engine.c#L470) does not implement that contract. It treats `argv[2]` as the command string, and it reads `argv[3]` and `argv[4]` as raw numbers instead of parsing named flags. The `--nice` option is not handled at all.

Why this matters: the assignment expects the CLI itself to accept the documented interface. A grader or another student following the guide will send commands that this code does not understand.

### 2. Per-container writable rootfs isolation is missing

The guide says each live container must use its own writable rootfs copy. In this repo, the supervisor takes one rootfs argument at startup in [boilerplate/engine.c](boilerplate/engine.c#L566) and reuses that same path for every container.

The child does call `chroot(ca->rootfs)` in [boilerplate/engine.c](boilerplate/engine.c#L272), but the current control path never passes a unique rootfs per container from the CLI into `do_start`.

Why this matters: the project requirement is container-specific filesystem isolation, not one shared rootfs for all containers.

### 3. `run` does not return the container exit status

The guide says `run` should block until the container exits and return the exit status, or `128 + signal` when signaled.

The current code waits in the supervisor in [boilerplate/engine.c](boilerplate/engine.c#L360), stores `exit_status`, and then only writes `Container finished` to the CLI client in [boilerplate/engine.c](boilerplate/engine.c#L484). The client never receives the actual numeric exit code.

Why this matters: scripts and graders cannot tell whether the container exited cleanly or was killed by a signal.

### 4. Manual stop and hard-limit kill are not distinguished correctly

The guide requires a `stop_requested` flag so the supervisor can tell the difference between a manual stop and a hard-limit kill.

The current `do_stop` path in [boilerplate/engine.c](boilerplate/engine.c#L419) sends `SIGTERM` and then escalates to `SIGKILL`, but it never records a stop-request flag. The reaper in [boilerplate/engine.c](boilerplate/engine.c#L112) marks any `SIGKILL` exit as `ST_KILLED` in [boilerplate/engine.c](boilerplate/engine.c#L121). That means a forced shutdown from `stop` can be misclassified as a hard-limit kill.

Why this matters: the assignment explicitly wants metadata to distinguish `stopped` from `hard_limit_killed`.

### 5. `ps` metadata is incomplete

The guide says the supervisor must track at least the container ID, host PID, start time, state, memory limits, log path, and an exit reason or exit status.

The `ps` output in [boilerplate/engine.c](boilerplate/engine.c#L373) shows name, PID, state, soft limit, hard limit, and log path, but it does not expose the recorded exit status or a final reason field. The in-memory `exit_status` field exists in [boilerplate/engine.c](boilerplate/engine.c#L59), but the user-facing metadata view does not print it.

Why this matters: the project asks for lifecycle metadata that can explain how each container ended.

### 6. `/proc` mounting is fragile in the child setup

The child mounts proc in [boilerplate/engine.c](boilerplate/engine.c#L268) before it enters the container root with `chroot` in [boilerplate/engine.c](boilerplate/engine.c#L272).

Why this matters: the assignment expects `/proc` to work inside the container after the filesystem switch. Mounting before `chroot` is fragile and can leave `/proc` unavailable or visible in the wrong place depending on the rootfs layout. A safer design is to mount after the final root is established, or to use `pivot_root`.

### 7. The kernel timer callback is not safe as written

The memory monitor performs periodic checks in [boilerplate/monitor.c](boilerplate/monitor.c#L80).

That function takes `container_mutex` inside a timer callback. In kernel space, timer callbacks run in atomic context and cannot sleep, so using a mutex there is not safe. The design should be reworked around a workqueue or another deferred context if a mutex is required.

Why this matters: this is a kernel-context correctness issue, not just a style choice.

### 8. `MONITOR_QUERY` is only partially implemented

The ioctl ABI includes a query path, but the current implementation in [boilerplate/monitor.c](boilerplate/monitor.c#L173) never sets `hard_limit_exceeded` to anything other than `0` in [boilerplate/monitor.c](boilerplate/monitor.c#L189).

Why this matters: if user space ever relies on the query ioctl for reporting, it will not get a complete hard-limit status.

### 9. The README documentation does not match the required CLI

The repository README describes a simplified command shape and examples that do not match the assignment syntax in [project-guide.md](project-guide.md). That is another reason the repo is misleading for someone trying to follow the spec from scratch.

Why this matters: even if the code were close, the documentation would still cause students to use the wrong interface.

## What Looks Followed

The broad architecture is present: a long-running supervisor, a CLI client over a UNIX domain socket, per-container stdout/stderr pipes feeding a bounded log buffer, and a kernel module exposed through `/dev/container_monitor` with ioctl registration.

The build flow also looks aligned with the grading setup: [boilerplate/Makefile](boilerplate/Makefile) has a `ci` target for user-space compilation only, and [boilerplate/environment-check.sh](boilerplate/environment-check.sh) validates the expected VM environment.

## Bottom Line

This repo follows the assignment at the architecture level, but it does not fully follow the required interface and lifecycle semantics. The biggest gaps are the CLI syntax, per-container rootfs handling, `run` exit-status behavior, stop-vs-kill attribution, and the kernel timer safety issue.

If your teacher said to "change the algorithms and the system calls", this repository already does that in a broad sense by using `clone`, `chroot`, `mount`, pipes, sockets, ioctl, `waitpid`, and `kill`, but the assignment-level behavior is still incomplete in the areas listed above.