/*
  ISC License

  Copyright (c) 2026, nixxou

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

/*
  Glue between BranchIndex (src/branch_index.hpp) and mergerfs.

  A branch in mode ROC ("read-only, cached") behaves like RO (mergerfs
  never modifies it) and its metadata is answered from an index. The
  user guarantees the branch does not change behind mergerfs' back, or
  requests a rescan when it did: the index is trusted.

  At mount a ROC branch uses its index file from `index.path` when one
  exists and is valid, without touching the branch. Otherwise, and on
  `index-rescan`, it is scanned in the background (one scan at a time
  per physical disk); meanwhile the branch is queried normally.

  Indexes are not used with `follow-symlinks` other than `never`,
  `symlinkify=true` or `posix-acl=true` (ACLs are not indexed and
  hiding them would weaken permission checks).

  The query helpers return true when the index answered (`*rv_` is
  then 0 or -errno) and false when the caller must ask the filesystem:
  no index for that branch, or a path at or below a nested mount point
  or through a symlink.
*/

#pragma once

#include "branch.hpp"
#include "branch_index.hpp"
#include "branches.hpp"
#include "fs_path.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <sys/stat.h>


struct fuse_statx;
struct RocCoverage;

struct BranchIndexSlot
{
  enum State : int
    {
      IDLE,        // never evaluated
      INELIGIBLE,  // not ROC, or incompatible options
      QUEUED,      // waiting for another scan on the same disk
      SCANNING,
      READY,
      FAILED,      // scan failed, retried only on rescan
      WAITING      // index.scan=false and no usable file yet
    };

  // Serializes invalidation and publication so a superseded scan can
  // never publish its result.
  std::mutex mutex;

  std::atomic<std::shared_ptr<const BranchIndex::Index>> index;
  // physical disk key, computed once when a scan is queued
  std::atomic<std::shared_ptr<const std::string>> disk;
  // index file used for this slot (when index.path is set)
  std::atomic<std::shared_ptr<const std::string>> file;
  // ROC branches: which cloud mirrors hold identical copies of which
  // files (see roc_mirrors.hpp)
  std::atomic<std::shared_ptr<const RocCoverage>> coverage;
  std::atomic<uint64_t> gen{0};
  std::atomic<uint64_t> published_gen{0};
  std::atomic<int>      state{IDLE};
};

namespace BranchIndexMgr
{
  // Index to use for queries on this branch, or null. Cheap: one
  // atomic load plus a few option checks, no filesystem access.
  std::shared_ptr<const BranchIndex::Index> get(const Branch &branch);

  // Load or scan indexes of ROC branches not yet indexed, and drop the
  // ones of branches no longer eligible. Meant for init and after any
  // configuration change.
  void refresh(const Branches::Ptr &branches);

  // Drop and rebuild the index of the branches whose path is `which_`
  // ("" or "all" for every ROC branch). -ENOENT if none matched.
  int  rescan(const Branches::Ptr &branches,
              std::string_view     which);

  // Human readable state of every ROC branch, one line per branch,
  // followed by the cloud mirrors and disk states.
  std::string status(const Branches::Ptr &branches);

  // An index file in `index.path` was written (by this or another
  // mergerfs): reload it into the slots using it, unless they already
  // hold that content or are being rescanned here.
  void file_written(const std::string &file);

  // Load (or scan) the index of an arbitrary directory into `slot_`,
  // through the per disk queues. Used for cloud mirrors.
  void start(const fs::path                         &path,
             const std::shared_ptr<BranchIndexSlot> &slot,
             bool                                    force_scan);

  // Identifies the physical disk holding `path_`: scans of branches
  // with the same key run one after the other so a spinning drive is
  // not made to seek between two scans; different disks are scanned in
  // parallel. Partitions resolve to their disk and single device
  // stacks (dm-crypt, single PV LVM) to the device below; multi device
  // stacks keep their own key. Without sysfs: the filesystem device.
  std::string disk_key(const fs::path &path,
                       const fs::path &sysfs = "/sys");
  std::string disk_key(dev_t           dev,
                       const fs::path &sysfs = "/sys");

  bool lstat(const Branch    &branch,
             const fs::path  &fusepath,
             struct stat     *st,
             int             *rv);
  bool statx(const Branch      &branch,
             const fs::path    &fusepath,
             struct fuse_statx *st,
             int               *rv);
  bool readlink(const Branch   &branch,
                const fs::path &fusepath,
                char           *buf,
                size_t          bufsize,
                ssize_t        *rv);
}

namespace fs
{
  // Branch aware existence checks used by policies: served by the
  // branch's index when it has one.
  bool exists(const Branch   &branch,
              const fs::path &fusepath,
              struct stat    *st);
  bool exists(const Branch   &branch,
              const fs::path &fusepath);
}
