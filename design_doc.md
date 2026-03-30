# Mini-UnionFS — Design Document

## 1. Overview

Mini-UnionFS is a userspace union filesystem built with FUSE (Filesystem in
Userspace). It merges two directories — a read-only `lower_dir` and a
read-write `upper_dir` — into a single unified mount point. This mirrors how
Docker overlay filesystems work.

---

## 2. Architecture

```
  User process (ls, cat, echo ...)
        |
  Kernel VFS layer
        |
  FUSE kernel module  <----> /dev/fuse
        |
  mini_unionfs (userspace daemon)
        |
   +-----------+-----------+
   |                       |
upper_dir/           lower_dir/
(read-write)         (read-only)
```

The FUSE daemon receives every filesystem call (open, read, write, unlink,
mkdir, etc.) and resolves it against the two layers using a fixed priority
rule: **upper_dir always wins over lower_dir**.

---

## 3. Data Structures

### 3.1 Global State

```c
struct mini_unionfs_state {
    char *lower_dir;   /* absolute path to read-only base layer */
    char *upper_dir;   /* absolute path to read-write top layer  */
};
```

This struct is allocated once in `main()` and passed to FUSE via the
`private_data` field. Every callback retrieves it with:

```c
#define STATE ((struct mini_unionfs_state *)fuse_get_context()->private_data)
```

### 3.2 No additional in-memory structures

All state is reflected directly on disk. The upper_dir and lower_dir are the
source of truth. There is no in-memory inode table or name cache.

---

## 4. Path Resolution Algorithm

Every FUSE operation calls `resolve_path(path, out_path)` first.

```
Input:  virtual path (e.g. "/config.txt")
Output: real path on disk + which layer it came from

Steps:
  1. Build whiteout path: upper_dir/<dir>/.wh.<filename>
     If this file EXISTS → return -ENOENT  (file is "deleted")

  2. Build upper path:    upper_dir/<path>
     If this EXISTS       → return 1 (upper), set out_path

  3. Build lower path:    lower_dir/<path>
     If this EXISTS       → return 0 (lower), set out_path

  4. Return -ENOENT (file not found in either layer)
```

Return value semantics:
- `1`      → found in upper_dir
- `0`      → found in lower_dir
- `-ENOENT`→ not found (or whiteout'd)

---

## 5. Copy-on-Write (CoW)

**Trigger:** A write/append is requested on a file that exists only in
lower_dir.

**Mechanism** (`copy_file_to_upper`):
1. Build the destination path in upper_dir (mirroring the virtual path).
2. `mkdir_p()` all parent directories in upper_dir so the path exists.
3. Open the source file from lower_dir (read-only).
4. Open the destination in upper_dir (write, create, truncate).
5. Copy all bytes in 64 KB chunks using `read()`/`write()`.
6. Preserve the original file mode (`st.st_mode`).

After CoW, the upper copy is opened for writing. The lower_dir file is
**never modified**.

**Where it fires:** `unionfs_open()` detects `O_WRONLY`, `O_RDWR`, or
`O_APPEND` flags. If `resolve_path` returned 0 (lower only), CoW is
triggered before returning.

---

## 6. Whiteout Mechanism

**Purpose:** Simulate deletion of a lower_dir file without modifying
lower_dir.

**Whiteout filename format:**
```
upper_dir/<same directory>/.wh.<original filename>
```
Example: deleting virtual `/etc/config.txt` creates:
```
upper_dir/etc/.wh.config.txt   (empty file, 0 bytes)
```

**On deletion** (`unionfs_unlink`):
- If file exists in upper_dir → physically `unlink()` it.
- If file exists only in lower_dir → create the `.wh.` marker in upper_dir.

**On reads/lookups** (`resolve_path`):
- Before checking upper or lower, check for `.wh.<name>` in upper_dir.
- If found → return `-ENOENT` immediately, hiding the file.

**On directory listing** (`unionfs_readdir`):
- For each entry in lower_dir, check if `upper_dir/<dir>/.wh.<name>` exists.
- If yes → skip this entry (it is hidden).
- Whiteout files themselves (`.wh.*`) are never shown to the user.

---

## 7. Directory Operations

### mkdir
Always creates the new directory inside upper_dir. The lower_dir is never
touched.

### rmdir
- If directory exists in upper_dir → `rmdir()` it physically.
- If directory exists only in lower_dir → create a `.wh.` whiteout marker
  file in upper_dir to mark it as deleted.

### readdir
Performs a two-pass merge:
1. Emit all entries from upper_dir (excluding `.wh.*` files).
2. Emit entries from lower_dir, skipping:
   - Entries that also exist in upper_dir (already listed).
   - Entries that have a whiteout marker in upper_dir.

---

## 8. Edge Cases Handled

| Scenario | Handling |
|---|---|
| File in both layers | upper_dir version shown; lower ignored |
| Write to lower-only file | CoW copies to upper first |
| Delete lower-only file | Whiteout marker created in upper |
| Delete upper-copy of CoW file | Physical unlink of upper copy; lower re-appears |
| Create new file | Always written to upper_dir |
| List directory | Two-pass merge; whiteouts filtered |
| Nested directories | `mkdir_p` ensures parent path exists in upper |
| Whiteout marker appears in ls | `.wh.*` files are always hidden from readdir |

---

## 9. Build & Usage

```bash
# Build
make

# Run (in foreground for debugging)
./mini_unionfs <lower_dir> <upper_dir> <mountpoint>

# Unmount
fusermount -u <mountpoint>

# Run tests
chmod +x test_unionfs.sh
./test_unionfs.sh
```

---

## 10. Limitations (Scope of This Project)

- No support for hard links, symlinks, or extended attributes.
- No file locking.
- No persistent inode numbering (getattr returns whatever lstat gives).
- Single-threaded FUSE (no `-o allow_other` multi-user support).
- `rmdir` whiteout does not recursively hide sub-entries of a lower dir.
