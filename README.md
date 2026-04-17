# Mini-UnionFS

A simplified Union Filesystem in userspace using FUSE, implementing
Docker-style layer stacking with Copy-on-Write and whiteout deletions.

## Quick Start

```bash
# 1. Install dependencies (run once on each machine)
sudo apt update
sudo apt install -y libfuse-dev fuse build-essential git

# 2. Build
make

# 3. Create test directories
mkdir -p /tmp/lower /tmp/upper /tmp/mnt
echo "hello from lower" > /tmp/lower/test.txt

# 4. Mount
./mini_unionfs /tmp/lower /tmp/upper /tmp/mnt

# 5. Use it
ls /tmp/mnt
cat /tmp/mnt/test.txt

# 6. Unmount
fusermount -u /tmp/mnt
```

## Running Tests

```bash
chmod +x test_unionfs.sh
./test_unionfs.sh
```

## Team

| Member | Environment | Responsibility |
|--------|-------------|----------------|
| M1 | Mac / UTM Ubuntu | Core FUSE, CoW, read path, Makefile |
| M2 | Mac / UTM Ubuntu | Whiteout, unlink, mkdir, create |
| W1 | Win / VirtualBox Ubuntu | Test suite, env setup |
| W2 | Win / WSL Ubuntu | Design doc, README, integration |

---

# SETUP.md — Per-machine environment setup

## All machines (Ubuntu 22.04)

```bash
sudo apt update
sudo apt install -y libfuse-dev fuse build-essential git
```

Verify FUSE is available:
```bash
ls /dev/fuse        # should exist
modinfo fuse        # should show module info
```

## Mac users (UTM with Ubuntu 22.04)

- Work entirely inside the UTM virtual machine terminal.
- Do NOT install macFUSE on the Mac host — it is not needed.
- Open UTM → start your Ubuntu VM → open a terminal inside it.
- All git, make, and ./mini_unionfs commands run inside the VM.

Recommended: enable shared clipboard in UTM so you can paste commands.

## Windows users (VirtualBox with Ubuntu 22.04)

- Open VirtualBox → start Ubuntu VM → open a terminal inside it.
- All work happens inside the VM. Windows host needs nothing installed.
- Optional: install Guest Additions for shared clipboard.

```bash
# Inside the VM:
sudo apt install -y virtualbox-guest-utils   # for clipboard
```

## Windows users (WSL2 — Ubuntu 22.04)

WSL2 supports FUSE but needs a check first:

```bash
uname -r    # should show 5.15.x or higher
ls /dev/fuse  # must exist
```

If `/dev/fuse` is missing, WSL2 FUSE is not enabled. In that case,
switch to using the VirtualBox VM instead for this project.

If it exists, proceed normally — all commands are the same.
