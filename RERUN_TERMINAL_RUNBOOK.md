# OS-Jackfruit Full Rerun Runbook (Terminal A + Terminal B)

This file is a clean, copy-paste sequence to rerun everything from the beginning.

Important:
- Run all commands from `/home/vismayhs/Downloads/OS_Jackfruit` unless a step says otherwise.
- Use `make` for runtime tests. Do not use `make ci` before container tests (it rebuilds workloads without static linking).
- If a command asks for sudo password, enter it.

---

## 0) One-Time Reset Before Rerun (single terminal)

Run these first to avoid stale state from previous runs.

```bash
cd /home/vismayhs/Downloads/OS_Jackfruit

# Stop any old supervisor if still running
sudo pkill -f "/home/vismayhs/Downloads/OS_Jackfruit/boilerplate/engine supervisor" 2>/dev/null || true

# Unload monitor if loaded (ignore errors if not loaded)
sudo rmmod monitor 2>/dev/null || true

# Clear runtime artifacts
sudo rm -f /tmp/engine.sock
sudo rm -rf /tmp/engine_logs

# If previous runs leaked proc mounts into rootfs/*/proc, unmount them first.
for d in rootfs-base rootfs-alpha rootfs-beta rootfs-gamma rootfs-high rootfs-low; do
  while findmnt -rn "$PWD/$d/proc" >/dev/null 2>&1; do
    sudo umount -l "$d/proc" || break
  done
done

# Remove old rootfs trees for a truly fresh run.
# Use sudo because previous container runs can leave root-owned files inside.
sudo rm -rf rootfs-base rootfs-alpha rootfs-beta rootfs-gamma rootfs-high rootfs-low
```

Expected output:
- Usually no output, or harmless "No such process"/"Module monitor is not currently loaded" style behavior.
- After this, there should be no active supervisor and no stale socket/log directory.

---

## 1) Build Correctly (single terminal)

```bash
cd /home/vismayhs/Downloads/OS_Jackfruit
make -C boilerplate clean
make -C boilerplate
```

Expected output:
- `engine`, workloads, and `monitor.ko` are built.
- Warnings like "compiler differs" or "Skipping BTF generation ..." are normal for kernel module builds.

Verify workloads are statically linked (this is critical for Alpine rootfs):

```bash
file boilerplate/memory_hog boilerplate/cpu_hog boilerplate/io_pulse
```

Expected output:
- Each workload line should include `statically linked`.

---

## 2) Prepare Root Filesystems (single terminal)

```bash
cd /home/vismayhs/Downloads/OS_Jackfruit

# Download Alpine minirootfs only if tarball is not already present
if [ ! -f alpine-minirootfs-3.20.3-x86_64.tar.gz ]; then
  wget -O alpine-minirootfs-3.20.3-x86_64.tar.gz \
    https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
fi

mkdir -p rootfs-base
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Copy workloads into base rootfs
cp boilerplate/memory_hog rootfs-base/
cp boilerplate/cpu_hog rootfs-base/
cp boilerplate/io_pulse rootfs-base/

# Clone per-container rootfs copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-gamma
cp -a rootfs-base rootfs-high
cp -a rootfs-base rootfs-low
```

Expected output:
- No errors.
- Rootfs folders exist and contain executables.

Quick verify:

```bash
for d in rootfs-base rootfs-alpha rootfs-beta rootfs-gamma rootfs-high rootfs-low; do
  test -x "$d/memory_hog" && test -x "$d/cpu_hog" && test -x "$d/io_pulse" && echo "OK $d"
done
```

Expected output:
- `OK ...` for all six rootfs folders.

---

## 3) Terminal A (Supervisor + Kernel Module)

Open Terminal A and run:

```bash
cd /home/vismayhs/Downloads/OS_Jackfruit/boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh

cd /home/vismayhs/Downloads/OS_Jackfruit
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor
sudo dmesg | grep container_monitor | tail -n 10

sudo ./boilerplate/engine supervisor ./rootfs-base
```

Expected output:
- Environment script shows `Preflight passed.`
- `/dev/container_monitor` exists.
- dmesg includes `container_monitor: loaded, /dev/container_monitor ready`.
- Supervisor prints:
  - `[engine] Supervisor ready. Socket: /tmp/engine.sock`

Keep Terminal A running (do not close it) while testing from Terminal B.

---

## 4) Terminal B (CLI Tests in Order)

Open Terminal B and run in this exact order.

### 4.1 Usage Contract

```bash
cd /home/vismayhs/Downloads/OS_Jackfruit
./boilerplate/engine
echo $?
```

Expected output:
- Usage text shown.
- Exit code is `1`.

### 4.2 Start Two Containers

```bash
sudo ./boilerplate/engine start alpha ./rootfs-alpha /memory_hog --soft-mib 64 --hard-mib 128
sudo ./boilerplate/engine start beta  ./rootfs-beta  "/cpu_hog 20" --soft-mib 64 --hard-mib 128 --nice 5
sudo ./boilerplate/engine ps
```

Expected output:
- First two commands print `Container started`.
- `ps` shows `alpha` and `beta` entries.
- No `exit=127` and no `not found` errors.

### 4.3 Rootfs Uniqueness Check

```bash
sudo ./boilerplate/engine start uniq1 ./rootfs-gamma "sleep 120"
sudo ./boilerplate/engine start uniq2 ./rootfs-gamma "sleep 120"
sudo ./boilerplate/engine stop uniq1
```

Expected output:
- `uniq1`: `Container started`
- `uniq2`: `ERROR: failed to start container` (expected, same live rootfs)
- stop: `Stopped`

### 4.4 Namespace and /proc Check

```bash
sudo ./boilerplate/engine run nscheck ./rootfs-gamma "echo pid=\$\$; mount | grep ' on /proc '; ps -o pid,comm | head -n 5"
echo $?
sudo ./boilerplate/engine logs nscheck
```

Expected output:
- Exit code `0`.
- Logs include `pid=1` and a mounted `/proc` line.

### 4.5 Stdout + Stderr Logging

```bash
sudo ./boilerplate/engine run iolog ./rootfs-gamma "echo out-1; echo err-1 >&2; echo out-2; echo err-2 >&2"
echo $?
sudo ./boilerplate/engine logs iolog
```

Expected output:
- Exit code `0`.
- Logs show all four lines (`out-*` and `err-*`).

### 4.6 Foreground Signal Forwarding

```bash
sudo ./boilerplate/engine run fg ./rootfs-gamma "while true; do echo tick; sleep 1; done"
```

Press `Ctrl+C` after 3-5 seconds, then run:

```bash
echo $?
sudo ./boilerplate/engine ps | grep fg
```

Expected output:
- Exit code usually `137` after Ctrl+C.
- `ps` shows `fg` in `stopped` state.

### 4.7 Soft + Hard Memory Limit Enforcement

```bash
sudo ./boilerplate/engine start memtest ./rootfs-gamma "/memory_hog 16 200" --soft-mib 32 --hard-mib 64
sleep 8
sudo dmesg | grep container_monitor | tail -n 40
sudo ./boilerplate/engine ps | grep memtest
sudo ./boilerplate/engine logs memtest | tail -n 30
```

Expected output:
- dmesg includes `[SOFT LIMIT]` and then `[HARD LIMIT]` lines for `memtest`.
- `ps` should show `memtest` as `hard_limit_killed`.

### 4.8 Logging Under Burst Load

```bash
sudo ./boilerplate/engine run burst ./rootfs-gamma "i=1; while [ \$i -le 2000 ]; do echo line-\$i; i=\$((i+1)); done"
echo $?
sudo ./boilerplate/engine logs burst | tail -n 20
```

Expected output:
- Exit code `0`.
- Tail shows near-final lines like `line-198x ... line-2000`.

### 4.9 Scheduler Priority Experiment

```bash
sudo ./boilerplate/engine start high ./rootfs-high "/cpu_hog 8" --soft-mib 64 --hard-mib 128 --nice -10
sudo ./boilerplate/engine start low  ./rootfs-low  "/cpu_hog 8" --soft-mib 64 --hard-mib 128 --nice 10
sleep 10
sudo ./boilerplate/engine logs high | tail -n 20
sudo ./boilerplate/engine logs low  | tail -n 20
sudo ./boilerplate/engine ps
```

Expected output:
- Both start commands print `Container started`.
- Logs contain `cpu_hog alive ...` and `cpu_hog done ...` lines.
- No `/cpu_hog: not found` errors.

---

## 5) Teardown (A then B)

### 5.1 Terminal A

Press `Ctrl+C` in Terminal A (supervisor window).

Expected output:
- `[engine] Shutting down...`
- `[engine] Clean exit.`

### 5.2 Terminal B

```bash
cd /home/vismayhs/Downloads/OS_Jackfruit

for id in alpha beta memtest high low fg nscheck iolog uniq1 uniq2 burst; do
  sudo ./boilerplate/engine stop "$id"
done

ps aux | grep -E "engine supervisor|[d]efunct"
sudo dmesg | grep container_monitor | tail -n 50
sudo rmmod monitor
lsmod | grep monitor || echo "monitor unloaded"
```

Expected output:
- If supervisor already exited, stop commands may print `connect (is supervisor running?): No such file or directory` (this is acceptable during teardown).
- Final line should be `monitor unloaded`.

---

## 6) Screenshot Checklist (what to capture)

Take these 8 screenshots while rerunning:

1. Environment check passed + dependency context.
2. Module loaded (`/dev/container_monitor`) + supervisor ready message.
3. `start alpha/beta` + `ps` showing tracked containers.
4. `logs iolog` (or alpha logs) showing captured stdout/stderr.
5. dmesg showing soft-limit and hard-limit events for memtest.
6. `ps` metadata showing correct states (including `hard_limit_killed`/`stopped`).
7. Scheduler experiment output (`high` vs `low` logs, no not-found errors).
8. Clean shutdown (`Shutting down`, `Clean exit`, module unloaded).

---

## 7) If You Need to Run Everything Again

Yes, you should reset first every time:
- Stop supervisor and unload module.
- Remove `/tmp/engine.sock` and `/tmp/engine_logs`.
- Rebuild with `make -C boilerplate clean && make -C boilerplate`.
- Recreate rootfs directories and recopy workloads.

If you see `Permission denied` while deleting rootfs folders, use:

```bash
sudo rm -rf rootfs-base rootfs-alpha rootfs-beta rootfs-gamma rootfs-high rootfs-low
```

If you see `Operation not permitted`, it usually means a leftover `proc` mount exists.
Run this first, then delete:

```bash
cd /home/vismayhs/Downloads/OS_Jackfruit
for d in rootfs-base rootfs-alpha rootfs-beta rootfs-gamma rootfs-high rootfs-low; do
  while findmnt -rn "$PWD/$d/proc" >/dev/null 2>&1; do
    sudo umount -l "$d/proc" || break
  done
done
sudo rm -rf rootfs-base rootfs-alpha rootfs-beta rootfs-gamma rootfs-high rootfs-low
```

You do not need to re-download the Alpine tarball if `alpine-minirootfs-3.20.3-x86_64.tar.gz` is already present.