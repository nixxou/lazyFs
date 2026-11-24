# lazyFS

**lazyFS** is a fork of [mergerfs](https://github.com/trapexit/mergerfs),
the FUSE union filesystem, that lets archive drives **stay asleep**.

mergerfs forwards every metadata request (listing a directory, `stat`,
looking up which drive holds a file) to the drives, so any media
scanner or `ls -R` spins all of them up. lazyFS answers those requests
from an index instead, and can even read files from cloud copies while
a drive sleeps.

Everything else is mergerfs: same branches, policies and options. lazyFS
installs **alongside** mergerfs (its binaries, mount helper, man page and
filesystem type are all named `lazyfs`).

> Status: experimental. Covered by unit and integration tests, not yet
> field tested with real FUSE mounts, sleeping drives or rclone. Feedback
> welcome.


## Features

### ROC branches: read-only, cached metadata

```
/mnt/ssd:/mnt/disk1/archive=ROC:/mnt/disk2/archive=ROC
```

A branch in mode `ROC` behaves like `RO` (mergerfs never modifies it,
files can't be opened for writing) and its metadata is answered from an
index:

* `getattr` / `statx` / lookups, `readdir`, `readlink`, `statfs`, the
  branch searches of every policy: **no drive access**, including for
  files that do not exist (`folder.jpg`, subtitles, `.nfo`...).
* The index is built once by a background scan (one scan at a time per
  physical disk), stored in `index.path`, memory mapped, and **reused at
  the next mount without touching the drive**.
* The index is trusted: after changing a ROC branch outside lazyFS,
  request a rescan.

```
setfattr -n user.mergerfs.cmd.index-rescan -v /mnt/disk1/archive /mnt/pool/.mergerfs
getfattr --only-values -n user.mergerfs.index-status /mnt/pool/.mergerfs
```

Lookups take about 100 ns. Indexes use about 100 bytes per file.

[Documentation](mkdocs/docs/config/index.md)

### Cloud mirrors

A rules file maps directories of ROC branches to other mounts (typically
rclone mounts of cloud drives) holding copies of their files, possibly
partial, several clouds per directory if needed:

```
# local directory (in a ROC branch)   mirror directory
/mnt/disk1/archive/Films              /mnt/gdrive/Films
/mnt/disk1/archive/Films              /mnt/onedrive/Movies
/mnt/disk1/archive                    /mnt/b2/disk1
```

* Mirrors are indexed too; only copies of the **same size** as the
  drive's file are used.
* An external tool marks a disk sleeping (`roc-sleep`). While it sleeps,
  opening a mirrored file reads the cloud copy; anything else is read
  from the drive, which wakes it.
* When a drive wakes up, files open on a cloud move to it at their next
  read, and a cloud failing mid read moves the file to the drive and
  retries: **no error for the application**, a film keeps playing.
* Mirrors can be disabled and enabled at runtime by a monitor
  (`roc-mirror-disable /mnt/gdrive`).

[Documentation](mkdocs/docs/config/roc-mirrors.md)

### One pool, several mounts, several policies

Mount the same pool several times with different `roc.policy` and point
each application at its mount:

| policy | uses |
|---|---|
| `disk` | always the master copy (backups, verification) |
| `wake` | clouds while the drive sleeps, else the drive (default) |
| `no-wake` | clouds while the drive sleeps, never wakes it (`EAGAIN`) |
| `mirror` | clouds only |

Mounts share the indexes: one scans, the others use `index.scan=false`
and reload index files automatically when they change.


## Example

```
# /etc/fstab
/mnt/ssd:/mnt/disk1/archive=ROC:/mnt/disk2/archive=ROC  /mnt/pool       lazyfs  cache.files=off,category.create=ff,index.path=/var/lib/lazyfs/index,roc.mirrors-file=/etc/lazyfs/mirrors  0 0
/mnt/ssd:/mnt/disk1/archive=ROC:/mnt/disk2/archive=ROC  /mnt/pool-plex  lazyfs  cache.files=off,index.path=/var/lib/lazyfs/index,index.scan=false,roc.policy=no-wake,roc.mirrors-file=/etc/lazyfs/mirrors  0 0
```

```
install -d -m 0700 /var/lib/lazyfs/index
```

Mount archive drives `ro,noatime`, or at least `noatime`, and keep new
data on the SSD until a script moves it to the archives and requests a
rescan.


## Building and installing

Same as mergerfs:

```
make
sudo make install
```

Installed files: `lazyfs`, `lazyfs-fusermount`, `fsck.lazyfs`,
`lazyfs.collect-info`, `/sbin/mount.lazyfs`, `lazyfs.1`,
`/usr/lib/lazyfs/preload.so`. The filesystem type is `fuse.lazyfs`. The
runtime control interface is unchanged (`.mergerfs` control file,
`user.mergerfs.*` attributes), so mergerfs tools keep working.

Unit tests: `make tests && ./build/tests`.


## Documentation

* lazyFS additions: [ROC branches](mkdocs/docs/config/index.md),
  [cloud mirrors](mkdocs/docs/config/roc-mirrors.md)
* Everything else: the [mergerfs documentation](https://trapexit.github.io/mergerfs/)
  and the original [mergerfs README](README.mergerfs.md)


## Credits and license

lazyFS is built on [mergerfs](https://github.com/trapexit/mergerfs) by
Antonio SJ Musumeci. Please report lazyFS issues here, not to mergerfs.

* mergerfs and the lazyFS additions: ISC license, see [LICENSE](LICENSE).
* `vendored/libfuse`: LGPL-2.1 / GPL-2 (see its `COPYING` files); the
  other vendored libraries keep their own licenses.
