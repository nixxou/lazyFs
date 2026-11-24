# ROC cloud mirrors

* `roc.mirrors-file=PATH`: rules file. Defaults to empty (no mirrors).
* `roc.policy=disk|wake|no-wake|mirror`: how this mount reads mirrored
  files. Defaults to `wake`.

[ROC branches](index.md) answer metadata from their index, so a
sleeping drive is not woken by listings or lookups. Reading a file
still needs the drive. If copies of some of its files exist elsewhere,
typically on cloud drives mounted with rclone, mergerfs can read those
copies instead while the drive sleeps.


## Rules

The rules file maps directories inside ROC branches to directories
holding copies of their files. One rule per line, `#` starts a comment,
spaces in paths are written `\040`:

```
# local directory (in a ROC branch)   mirror directory
/mnt/disk1/archive/Films              /mnt/gdrive/Films
/mnt/disk1/archive/Films              /mnt/onedrive/Movies
/mnt/disk1/archive/Series             /mnt/gdrive2/Series
/mnt/disk1/archive                    /mnt/b2/disk1
/mnt/disk2/archive/Music              /mnt/gdrive/Music
```

* A mirror holds the same tree as the local directory, possibly only
  part of it.
* Several rules may cover the same directory, a rule may cover the
  whole branch or a single sub directory, rules may overlap.
* **Rule order is priority order**: for a file, every rule whose local
  directory contains it is tried, first line first.


## Mirror indexes and coverage

Every mirror directory gets an index, exactly like a ROC branch: stored
in `index.path`, loaded at mount without touching the mirror, rescanned
only on request. From the branch and mirror indexes mergerfs keeps,
for every file of a ROC branch, the list of mirrors holding **a copy
of the same size**. Only those are ever used: files absent from the
drive, or of another size, are ignored.

Size is the only check: a different file of exactly the same size
would be taken as a copy. Content is never compared, that would mean
reading both.

`index-status` shows for each ROC branch how much of it is mirrored:

```
/mnt/disk1/archive: ready disk:sda ... mirrored=41210/48211 files (85.5%), 3.1T/3.4T (91.2%)
mirror /mnt/gdrive/Films: ready entries=20188
disk:sda: sleeping
```


## Sleeping, waking and active disks

Each physical disk is `sleeping`, `waking` or `active` (default):

* **active**: files are read from the disk.
* **sleeping** and **waking**: opening a file of a ROC branch on that
  disk opens the first mirror copy whose current size still matches.
  If there is none (not mirrored, copy changed or removed since the
  mirror was scanned, mirror failing) the disk is read, and the disk
  starts waking.
* **waking**: mergerfs spins the disk up from a background thread
  (reading a block of one of its files). Once the disk answered it is
  **active**. Meanwhile opens keep using the mirrors, so none of them
  waits for the spin up.

### Moving open files to the disk

A file open on a mirror moves to the disk, without the application
noticing:

* **at its next read once the disk is active** (after `roc-wake`, or
  after the disk was woken by another file). A film being played
  continues from the disk, at the same position;
* **right away if a mirror read fails** (network cut, rclone error):
  the mirror is put aside for 60 seconds and the same read is retried
  on the disk. The application sees a slow read, the time the disk
  takes to spin up, not an error.

This works because reads carry their offset: the descriptor is
replaced in place between two reads. Files open on a mirror are
therefore never set up for `passthrough.io`, whose reads would bypass
mergerfs.

**Copies must be identical.** Only sizes are compared: a copy of the
same size but different content would mix both versions in the data
read when the file moves to the disk.

mergerfs does not watch the drives itself. An external tool (for
example one built around `hd-idle`, or a script checking
`/sys/block/sdX/stat`) marks disks sleeping:

```
setfattr -n user.mergerfs.cmd.roc-sleep -v disk:sda /mnt/pool/.mergerfs
setfattr -n user.mergerfs.cmd.roc-sleep -v /mnt/disk1/archive /mnt/pool/.mergerfs
setfattr -n user.mergerfs.cmd.roc-sleep -v all /mnt/pool/.mergerfs
setfattr -n user.mergerfs.cmd.roc-wake  -v disk:sda /mnt/pool/.mergerfs
```

A branch path selects the disk holding it (all ROC branches of that
disk follow). The disk names are the ones shown by `index-status`.

A mirror whose open fails with anything but "no such file" (a dead
rclone mount, I/O errors) is skipped for 60 seconds so every open does
not wait for it.

Metadata (`stat`, including on open files) always comes from the
branch's index, so a file read from a mirror looks exactly like the
branch's.


## Read policy per mount

`roc.policy` decides, for one mergerfs mount, where files of ROC
branches are read:

| policy | disk active | disk sleeping, file mirrored | disk sleeping, file not mirrored |
|---|---|---|---|
| `disk` | disk | disk (woken) | disk (woken) |
| `wake` (default) | disk | mirror | disk (woken) |
| `no-wake` | disk | mirror | `EAGAIN`, the disk keeps sleeping |
| `mirror` | mirror | mirror | `EAGAIN` |

With `no-wake` a mirror failing while the disk sleeps returns the read
error instead of moving the file to the disk. With `mirror` files are
never moved to the disk.

To give applications different policies, mount the same pool several
times and point each application at its mount:

```
# general use
mergerfs -o index.path=/var/lib/mergerfs/index,roc.mirrors-file=/etc/mergerfs/mirrors \
  /mnt/ssd:/mnt/disk1/archive=ROC /mnt/pool
# Plex: mirrors, never wakes a disk; reuses the indexes above
mergerfs -o index.path=/var/lib/mergerfs/index,index.scan=false,roc.policy=no-wake,roc.mirrors-file=/etc/mergerfs/mirrors \
  /mnt/ssd:/mnt/disk1/archive=ROC /mnt/pool-plex
# backups: always the master copy
mergerfs -o index.path=/var/lib/mergerfs/index,index.scan=false,roc.policy=disk \
  /mnt/ssd:/mnt/disk1/archive=ROC /mnt/pool-backup
```

Sharing indexes between mounts is described in [ROC
branches](index.md#several-mounts). Disk states (`roc-sleep`,
`roc-wake`) belong to each mount: send them to every mount.


## Runtime control

```
# rules file changed
setfattr -n user.mergerfs.cmd.roc-mirrors-reload -v 1 /mnt/pool/.mergerfs

# a mirror changed: rescan it (its directory as in the rules) or all
setfattr -n user.mergerfs.cmd.roc-mirror-rescan -v /mnt/gdrive/Films /mnt/pool/.mergerfs
setfattr -n user.mergerfs.cmd.roc-mirror-rescan -v all /mnt/pool/.mergerfs
```

### Disabling mirrors

An external monitor can take mirrors out of use, for example a cloud
mount it found unreachable, and put them back:

```
setfattr -n user.mergerfs.cmd.roc-mirror-disable -v /mnt/gdrive /mnt/pool/.mergerfs
setfattr -n user.mergerfs.cmd.roc-mirror-enable  -v /mnt/gdrive /mnt/pool/.mergerfs
setfattr -n user.mergerfs.cmd.roc-mirror-disable -v all /mnt/pool/.mergerfs
```

* The value is a directory: every mirror at or below it is affected,
  so disabling a mount covers all the rules pointing into it,
  including ones added by a later rules reload.
* A disabled mirror is never opened; files only it holds are read from
  the disk (waking it). Its index and the coverage are kept, so
  enabling it takes effect immediately. Enabling also clears the 60
  seconds error backoff.
* Enabling a sub directory does not override a disabled parent:
  enable the directory that was disabled.
* The value must match at least one current mirror (`ENOENT`
  otherwise). The state survives rules reloads, not mergerfs restarts.
* `index-status` marks such mirrors `disabled`.

A minimal monitor:

```
#!/bin/sh
CTRL=/mnt/pool/.mergerfs
for m in /mnt/gdrive /mnt/onedrive; do
  if timeout 10 stat -t "$m/.alive" >/dev/null 2>&1; then
    setfattr -n user.mergerfs.cmd.roc-mirror-enable -v "$m" "$CTRL"
  else
    setfattr -n user.mergerfs.cmd.roc-mirror-disable -v "$m" "$CTRL"
  fi
done
```

A broken rules file is rejected as a whole and the previous rules stay
in use. Mirrors are not on the drives: scanning them wakes no disk,
but large cloud drives can take long to list.

rclone tip: a long `--dir-cache-time` and a VFS cache make opening a
mirror copy cheap.
