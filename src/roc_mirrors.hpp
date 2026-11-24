/*
  ISC License

  Copyright (c) 2026, mergerfs contributors

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
  Cloud mirrors of ROC branches.

  A rules file (`roc.mirrors-file`) maps directories inside ROC
  branches to directories of other mounts (typically rclone mounts of
  cloud drives) holding copies of their files, possibly partial:

    # local directory (in a ROC branch)   mirror directory
    /mnt/disk1/archive/Films              /mnt/gdrive/Films
    /mnt/disk1/archive/Films              /mnt/onedrive/Movies
    /mnt/disk1/archive                    /mnt/b2/disk1

  Spaces in paths are written \040. Rule order is priority order.

  Every mirror directory is indexed like a ROC branch (same files in
  `index.path`, loaded at mount, rescanned only on request). From both
  indexes a coverage table lists, for every file of a ROC branch, the
  mirrors holding a copy of the same size.

  Each physical disk is "sleeping", "waking" or "active" (default).
  While a disk is not active, opening a covered file of one of its ROC
  branches opens the first mirror copy whose real size still matches;
  anything else is read from the disk, which wakes it. An external
  tool marks disks sleeping (`roc-sleep`).

  Waking (roc-wake, or a read the mirrors could not serve) first spins
  the disk up from a background thread ("waking"); once it answered
  the disk is "active". Files open on a mirror then move to the disk
  at their next read, which therefore never waits for a spin up. A
  mirror read error moves the file to the disk right away and retries
  the read there. Reads use explicit offsets, so moving between two
  reads is seamless for the application (copies must be identical:
  only their size is checked).
*/

#pragma once

#include "branch.hpp"
#include "branch_index.hpp"
#include "branches.hpp"
#include "fs_path.hpp"

#include <cstdint>
#include <cstddef>
#include <memory>

#include <sys/types.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


struct BranchIndexSlot;
class FileInfo;

struct RocCoverage
{
  struct Target
  {
    std::string                      sub;    // rule's directory, relative to the branch
    std::string                      mirror; // mirror directory
    std::shared_ptr<BranchIndexSlot> slot;   // mirror's index
  };

  // the branch index this table was computed from: a table is only
  // valid with that exact index
  std::shared_ptr<const BranchIndex::Index> roc;
  std::vector<Target>                       targets;
  // entry position -> targets holding an identical copy, in order
  std::unordered_map<uint32_t,std::vector<uint16_t>> by_entry;

  uint64_t files         = 0;
  uint64_t bytes         = 0;
  uint64_t covered_files = 0;
  uint64_t covered_bytes = 0;
};

namespace RocMirrors
{
  struct Rule
  {
    std::string local;
    std::string mirror;
  };

  // Parse a rules file's content. Returns 0 or -EINVAL with `*err_`
  // describing the first bad line.
  int parse(std::string_view   text,
            std::vector<Rule> *rules,
            std::string       *err);

  // (Re)read `roc.mirrors-file`, set up the mirrors it names and load
  // or scan their indexes. `force_` rereads even if the file name did
  // not change.
  int  reload(const Branches::Ptr &branches,
              bool                 force);
  // Load the rules once and start the mirrors not yet indexed.
  void refresh(const Branches::Ptr &branches);
  // every mirror directory and its index slot
  std::vector<std::pair<std::string,std::shared_ptr<BranchIndexSlot>>>
  mirror_slots();

  // Rescan the mirror whose directory is `which_` ("all": every one).
  int  rescan(std::string_view which);

  // Disable / enable mirrors at or below directory `which_` ("all":
  // every one), e.g. a cloud mount found unreachable by an external
  // monitor. A disabled mirror is never opened; its index and the
  // coverage are kept so enabling it is immediate. Disabling a mount
  // also covers mirrors added below it later. -ENOENT if no current
  // mirror matches.
  int  set_enabled(std::string_view which,
                   bool             enabled);
  bool enabled(const std::string &mirror);
  // Recompute the coverage tables (an index changed).
  void index_changed();

  // `which_`: a branch path, "disk:<name>" or "all".
  int  set_sleeping(const Branches::Ptr &branches,
                    std::string_view     which,
                    bool                 sleeping);
  bool sleeping(const Branch &branch);

  // Mirror copies of `fusepath_` on `branch_`, in the order they would
  // be tried. Empty when not covered.
  std::vector<std::string> candidates(const Branch   &branch,
                                      const fs::path &fusepath);

  // Called by open for a file on a ROC branch, opened read only,
  // following `roc.policy`. Returns 1 with `*fd_` / `*mirror_` set when
  // a mirror copy was opened, 0 when the caller must open the disk's
  // file (the disk is then woken if needed), or -EAGAIN when the
  // policy forbids reading the disk and no copy is usable.
  int  open(const Branch   &branch,
            const fs::path &fusepath,
            int             flags,
            int            *fd,
            std::string    *mirror);

  // Read of an open file of a ROC branch. For a file open on a mirror:
  // moves it to the disk first if the disk is active, and on a mirror
  // read error moves it to the disk and retries there.
  ssize_t read(FileInfo *fi,
               char     *buf,
               size_t    size,
               off_t     offset);

  // "sleeping", "waking" or "active"
  std::string disk_state(const Branch &branch);

  std::string coverage_str(const RocCoverage &coverage);
  std::string status(const Branches::Ptr &branches);

  // forget rules, mirrors and disk states (tests)
  void reset();
}
