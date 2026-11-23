# ROC branches (read-only, cached metadata)

* branch mode `ROC`: `/mnt/disk1/archive=ROC`
* `index.path=PATH`: where indexes are stored. Defaults to empty (kept
  in memory, every mount rescans).
* `index.write-if-changed=true|false`: defaults to `true`
* `index.mlock=SIZE`: defaults to `0` (nothing locked)

Every metadata request sent to mergerfs (`getattr`, `readdir`, looking
up which branch holds a file, etc.) is normally forwarded to the
branches, which can spin up sleeping hard drives even when nobody
reads any file data. See [Limiting drive
spinup](../faq/limit_drive_spinup.md).

A branch in mode `ROC` behaves exactly like `RO` (excluded from
`create` and `action` policies, so mergerfs never modifies it) and its
metadata is answered from an index of the branch instead of the drive.
Answers include reliable negative ones: looking up a file which does
not exist on a ROC branch does not touch it.

**The index is trusted.** It is the user's job to make sure a ROC
branch does not change behind mergerfs' back, or to request a rescan
after it did (see [Runtime control](#runtime-control)). Changes made
directly on the branch are not visible through mergerfs until then.

This makes it possible to keep, on one drive, a static directory as a
ROC branch next to another directory receiving new data as a normal
branch. Note that a normal branch on the same drive is still queried
directly: listing a directory which also exists there, or looking up a
file which exists nowhere, reads that drive. Keeping incoming data on
an SSD or an always spinning drive avoids that.


## What is served from the index

* `getattr` / `statx` / `lookup`
* the existence checks done by all policies
* `readdir` (all `func.readdir` modes)
* `readlink`
* `statfs` with `statfs=full`
* `getxattr` / `listxattr`: ROC branches report **no extended
  attributes**

Opening a file, reading it, still goes to the branch and will spin up
its drive.


## Lifecycle

1. At mount, and whenever the configuration changes, a ROC branch
   whose index file exists in `index.path` and is valid uses it
   directly, **without touching the branch**. Index files are named
   after the filesystem id and the branch path, so a different drive
   mounted at the same place is not mistaken for the old one.
2. Otherwise, and on every rescan, the branch is scanned in the
   background. Branches on the same physical disk (several
   partitions, or several directories of one filesystem) are scanned
   one after the other so the drive does not seek between two scans;
   different disks are scanned in parallel. Until its index is ready
   a branch is queried normally (state `queued` then `scanning` in
   `index-status`).
3. The index is written to `index.path` (atomically: temporary file,
   `fsync`, `rename`) and memory mapped, so the kernel can drop unused
   pages under memory pressure and read them back from that file
   rather than from the branch. `index.write-if-changed=true` skips
   rewriting a file whose content signature did not change, avoiding
   needless SSD writes. Files are fully validated when loaded and a
   damaged one is rewritten.

`index.path` must be on storage which is always available, such as the
system SSD. Never put it on one of the branches or inside the pool.

`index.mlock=SIZE` (per branch, e.g. `64M`) locks part of each mapped
index in RAM, most useful parts first (directory table, then entries,
then names). It has no effect without `index.path`. To cap the memory
used instead, run mergerfs in a cgroup with `MemoryHigh=` (systemd);
avoid `MemoryMax=`, which can kill mergerfs.


## Restrictions

* Indexes are not used with `follow-symlinks` other than `never` (the
  default), `symlinkify=true` or `posix-acl=true`: ACLs are not indexed
  and hiding them would weaken permission checks. Changing one of
  these options at runtime bypasses indexes immediately.
* Anything mounted inside a ROC branch (directories or files, bind
  mounts included, as listed in `/proc/self/mountinfo` at scan time) is
  not indexed: anything at or below it is always queried live. So are
  paths going through a symlink.
* `atime` is not indexed; `mtime` is reported instead.
* Only Linux is supported.


## Runtime control

```
# state of every ROC branch
getfattr -n user.mergerfs.index-status /mnt/pool/.mergerfs

# rescan one branch (by its path) or all of them
setfattr -n user.mergerfs.cmd.index-rescan -v /mnt/disk1/archive /mnt/pool/.mergerfs
setfattr -n user.mergerfs.cmd.index-rescan -v all /mnt/pool/.mergerfs
```

While a branch is being rescanned it is queried normally.

A typical setup keeps new data on an SSD or an always-on drive and a
script moves it to the archive directories on a schedule, then
requests a rescan of the branches it changed.


## Example

```
mergerfs -o index.path=/var/lib/mergerfs/index \
  /mnt/ssd:/mnt/disk1/archive=ROC:/mnt/disk2/archive=ROC /mnt/pool
```
