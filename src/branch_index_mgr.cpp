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

#include "branch_index_mgr.hpp"

#include "config.hpp"
#include "fs_exists.hpp"
#include "syslog.hpp"

#include "fuse_kernel.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>

#include <sys/sysmacros.h>


namespace
{
  // STATX_TYPE ... STATX_BLOCKS: everything but btime / mnt_id / ...
  constexpr uint32_t STATX_BASIC = 0x000007ffU;

  bool
  _config_allows_index()
  {
    return ((cfg.follow_symlinks == FollowSymlinks::ENUM::NEVER) &&
            !cfg.symlinkify &&
            !cfg.posix_acl);
  }

  bool
  _eligible(const Branch &branch_)
  {
    return (branch_.roc() && ::_config_allows_index());
  }

  const char*
  _state_str(const int state_)
  {
    switch(state_)
      {
      case BranchIndexSlot::IDLE:       return "idle";
      case BranchIndexSlot::INELIGIBLE: return "not-indexed";
      case BranchIndexSlot::QUEUED:     return "queued";
      case BranchIndexSlot::SCANNING:   return "scanning";
      case BranchIndexSlot::READY:      return "ready";
      case BranchIndexSlot::FAILED:     return "failed";
      }
    return "unknown";
  }

  // Invalidate the slot's index and start a new generation. Any job
  // of an older generation can no longer publish.
  uint64_t
  _invalidate(BranchIndexSlot &slot_,
              const int        state_)
  {
    std::lock_guard<std::mutex> lk(slot_.mutex);

    slot_.index.store(nullptr);
    slot_.state = state_;

    return ++slot_.gen;
  }

  bool
  _set_state(BranchIndexSlot &slot_,
             const uint64_t   gen_,
             const int        state_)
  {
    std::lock_guard<std::mutex> lk(slot_.mutex);

    if(slot_.gen.load() != gen_)
      return false;
    slot_.state = state_;

    return true;
  }

  bool
  _publish(BranchIndexSlot                            &slot_,
           const uint64_t                              gen_,
           std::shared_ptr<const BranchIndex::Index>   idx_)
  {
    std::lock_guard<std::mutex> lk(slot_.mutex);

    if(slot_.gen.load() != gen_)
      return false;

    slot_.index.store(std::move(idx_));
    slot_.published_gen = gen_;
    slot_.state         = BranchIndexSlot::READY;

    return true;
  }

  struct Job
  {
    fs::path                         path;
    std::shared_ptr<BranchIndexSlot> slot;
    uint64_t                         gen;
    bool                             force_scan;
    std::string                      index_dir;
    bool                             write_if_changed;
    uint64_t                         mlock;
  };

  // Finish a mapped index: device id of the branch and optional mlock.
  void
  _prepare_mapped(BranchIndex::Index &idx_,
                  const Job          &job_,
                  const std::string  &file_)
  {
    struct stat st;

    if(::stat(job_.path.c_str(),&st) == 0)
      idx_.set_dev(st.st_dev);

    if(job_.mlock)
      {
        int64_t locked = idx_.mlock(job_.mlock);
        if(locked < 0)
          SysLog::warning("index: mlock of `{}` failed: {}",
                          file_,strerror(-locked));
      }
  }

  // Use the existing index file without touching the branch. Returns
  // true when it was published.
  bool
  _try_load(const Job         &job_,
            const std::string &file_)
  {
    int rv;
    std::shared_ptr<BranchIndex::Index> idx;

    rv = BranchIndex::load(file_,&idx);
    if(rv < 0)
      {
        if(rv != -ENOENT)
          SysLog::warning("index: `{}` unusable ({}), rescanning `{}`",
                          file_,strerror(-rv),job_.path.string());
        return false;
      }

    ::_prepare_mapped(*idx,job_,file_);
    if(!::_publish(*job_.slot,job_.gen,idx))
      return true;

    SysLog::info("index: branch `{}` ready from `{}`: {} entries",
                 job_.path.string(),file_,idx->entries_count());

    return true;
  }

  void
  _scan(const Job &job_)
  {
    int rv;
    std::string file;
    std::shared_ptr<BranchIndex::Index> idx;
    const auto start = std::chrono::steady_clock::now();

    if(!job_.index_dir.empty())
      file = (fs::path(job_.index_dir) /
              BranchIndex::filename(job_.path)).string();

    if(!job_.force_scan && !file.empty() && ::_try_load(job_,file))
      return;

    if(!::_set_state(*job_.slot,job_.gen,BranchIndexSlot::SCANNING))
      return;

    SysLog::info("index: scanning branch `{}`",job_.path.string());

    rv = BranchIndex::build(job_.path,&idx);
    if(rv < 0)
      {
        SysLog::error("index: scan of `{}` failed: {}",
                      job_.path.string(),strerror(-rv));
        ::_set_state(*job_.slot,job_.gen,BranchIndexSlot::FAILED);
        return;
      }

    if(!file.empty())
      {
        bool written = false;
        std::shared_ptr<BranchIndex::Index> mapped;

        rv = BranchIndex::save(*idx,file,job_.write_if_changed,&written);
        if(rv == 0)
          rv = BranchIndex::load(file,&mapped);

        // an existing file with the right signature in its header but
        // damaged content: rewrite it
        if((rv < 0) && !written)
          {
            rv = BranchIndex::save(*idx,file,false,&written);
            if(rv == 0)
              rv = BranchIndex::load(file,&mapped);
          }

        if(rv < 0)
          {
            SysLog::warning("index: cannot use file `{}` for `{}` ({}),"
                            " keeping the index in memory",
                            file,job_.path.string(),strerror(-rv));
          }
        else
          {
            mapped->set_dev(idx->dev());
            ::_prepare_mapped(*mapped,job_,file);
            idx = std::move(mapped);
          }
      }

    if(!::_publish(*job_.slot,job_.gen,idx))
      return;

    SysLog::info("index: branch `{}` ready: {} entries, {} dirs, {} ms",
                 job_.path.string(),
                 idx->entries_count(),
                 idx->dirs_count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
  }

  struct DiskQueue
  {
    std::deque<Job> jobs;
    bool            running = false;
  };

  std::mutex                      g_queues_mutex;
  std::map<std::string,DiskQueue> g_queues;

  // One worker per disk: drains that disk's queue then exits.
  void
  _disk_worker(const std::string key_)
  {
    for(;;)
      {
        Job job;

        {
          std::lock_guard<std::mutex> lk(g_queues_mutex);
          DiskQueue &q = g_queues[key_];
          if(q.jobs.empty())
            {
              q.running = false;
              return;
            }
          job = std::move(q.jobs.front());
          q.jobs.pop_front();
        }

        // superseded by a later rescan or configuration change
        if(job.slot->gen.load() != job.gen)
          continue;

        ::_scan(job);
      }
  }

  void
  _start(const Branch &branch_,
         const bool    force_scan_)
  {
    Job job;
    bool spawn;
    std::string key;

    job.path             = branch_.path;
    job.slot             = branch_.index_slot;
    job.force_scan       = force_scan_;
    job.index_dir        = cfg.index_path;
    job.write_if_changed = cfg.index_write_if_changed;
    job.mlock            = cfg.index_mlock;

    // the branch answers directly until the new index is published
    job.gen = ::_invalidate(*job.slot,BranchIndexSlot::QUEUED);

    // once per job, never on the request path
    key = BranchIndexMgr::disk_key(branch_.path);
    job.slot->disk.store(std::make_shared<const std::string>(key));

    {
      std::lock_guard<std::mutex> lk(g_queues_mutex);
      DiskQueue &q = g_queues[key];
      q.jobs.push_back(std::move(job));
      spawn = !q.running;
      q.running = true;
    }

    if(spawn)
      std::thread(::_disk_worker,key).detach();
  }

  void
  _drop(const Branch &branch_)
  {
    if(branch_.index_slot->state == BranchIndexSlot::INELIGIBLE)
      return;

    ::_invalidate(*branch_.index_slot,BranchIndexSlot::INELIGIBLE);
  }

  fs::path
  _realpath(const fs::path &p_)
  {
    std::error_code ec;
    fs::path rv = std::filesystem::canonical(p_,ec);
    return (ec ? fs::path() : rv);
  }

  BranchIndex::Result
  _lookup(const Branch                               &branch_,
          const fs::path                             &fusepath_,
          std::shared_ptr<const BranchIndex::Index>  *idx_,
          const BranchIndex::Entry                  **entry_)
  {
    *idx_ = BranchIndexMgr::get(branch_);
    if(!*idx_)
      return BranchIndex::Result::LIVE;

    return (*idx_)->lookup(fusepath_.native(),entry_);
  }

  int
  _errno(const BranchIndex::Result r_)
  {
    switch(r_)
      {
      case BranchIndex::Result::FOUND:     return 0;
      case BranchIndex::Result::NOT_DIR:   return -ENOTDIR;
      case BranchIndex::Result::NOT_FOUND:
      default:                             return -ENOENT;
      }
  }
}

std::shared_ptr<const BranchIndex::Index>
BranchIndexMgr::get(const Branch &branch_)
{
  if(!branch_.index_slot || !branch_.roc())
    return {};
  if(!::_config_allows_index())
    return {};

  return branch_.index_slot->index.load(std::memory_order_acquire);
}

void
BranchIndexMgr::refresh(const Branches::Ptr &branches_)
{
  for(const auto &branch : *branches_)
    {
      int state;

      if(!branch.index_slot)
        continue;

      if(!::_eligible(branch))
        {
          ::_drop(branch);
          continue;
        }

      state = branch.index_slot->state.load();
      if((state == BranchIndexSlot::IDLE) ||
         (state == BranchIndexSlot::INELIGIBLE))
        ::_start(branch,false);
    }
}

int
BranchIndexMgr::rescan(const Branches::Ptr &branches_,
                       std::string_view     which_)
{
  int matched = 0;
  const bool all = (which_.empty() || (which_ == "all"));

  for(const auto &branch : *branches_)
    {
      if(!branch.index_slot || !branch.roc())
        continue;
      if(!all && (branch.path.native() != which_))
        continue;

      matched++;
      if(::_eligible(branch))
        ::_start(branch,true);
      else
        ::_drop(branch);
    }

  return (matched ? 0 : -ENOENT);
}

std::string
BranchIndexMgr::status(const Branches::Ptr &branches_)
{
  std::string rv;

  for(const auto &branch : *branches_)
    {
      int state;
      std::shared_ptr<const std::string> disk;
      std::shared_ptr<const BranchIndex::Index> idx;

      if(!branch.index_slot || !branch.roc())
        continue;

      state = branch.index_slot->state.load();
      idx   = branch.index_slot->index.load();
      disk  = branch.index_slot->disk.load();

      rv += branch.path.native();
      rv += ": ";
      rv += ::_state_str(state);
      if(disk)
        rv += " " + *disk;
      if(idx)
        {
          rv += " entries=" + std::to_string(idx->entries_count());
          rv += " dirs=" + std::to_string(idx->dirs_count());
          rv += " bytes=" + std::to_string(idx->byte_size());
          rv += (idx->mapped() ? " mapped" : " memory");
        }
      rv += '\n';
    }

  return rv;
}

std::string
BranchIndexMgr::disk_key(const dev_t     dev_,
                         const fs::path &sysfs_)
{
  fs::path dev;
  const std::string majmin = (std::to_string(::major(dev_)) + ':' +
                              std::to_string(::minor(dev_)));

  // /sys/dev/block/MAJ:MIN -> /sys/devices/.../block/sda/sda1
  dev = ::_realpath(sysfs_ / "dev" / "block" / majmin);
  if(dev.empty())
    return ("dev:" + majmin);

  // bounded: device stacks are never this deep
  for(int depth = 0; depth < 8; depth++)
    {
      std::error_code ec;
      std::vector<fs::path> slaves;

      // a partition: its parent directory is the whole disk
      if(std::filesystem::exists(dev / "partition",ec))
        {
          dev = dev.parent_path();
          continue;
        }

      // dm / md: follow a single underlying device
      for(const auto &de : std::filesystem::directory_iterator(dev / "slaves",ec))
        slaves.push_back(de.path());
      if(slaves.size() != 1)
        break;

      fs::path next = ::_realpath(slaves[0]);
      if(next.empty() || (next == dev))
        break;
      dev = next;
    }

  return ("disk:" + dev.filename().string());
}

std::string
BranchIndexMgr::disk_key(const fs::path &path_,
                         const fs::path &sysfs_)
{
  struct stat st;

  if(::stat(path_.c_str(),&st) < 0)
    return ("path:" + path_.string());

  return BranchIndexMgr::disk_key(st.st_dev,sysfs_);
}

bool
BranchIndexMgr::lstat(const Branch   &branch_,
                      const fs::path &fusepath_,
                      struct stat    *st_,
                      int            *rv_)
{
  BranchIndex::Result r;
  const BranchIndex::Entry *e = nullptr;
  std::shared_ptr<const BranchIndex::Index> idx;

  r = ::_lookup(branch_,fusepath_,&idx,&e);
  if(r == BranchIndex::Result::LIVE)
    return false;

  *rv_ = ::_errno(r);
  if(r == BranchIndex::Result::FOUND)
    idx->to_stat(*e,st_);

  return true;
}

bool
BranchIndexMgr::statx(const Branch      &branch_,
                      const fs::path    &fusepath_,
                      struct fuse_statx *stx_,
                      int               *rv_)
{
  struct stat st;

  if(!BranchIndexMgr::lstat(branch_,fusepath_,&st,rv_))
    return false;
  if(*rv_ < 0)
    return true;

  ::memset(stx_,0,sizeof(*stx_));
  stx_->mask          = STATX_BASIC;
  stx_->blksize       = st.st_blksize;
  stx_->nlink         = st.st_nlink;
  stx_->uid           = st.st_uid;
  stx_->gid           = st.st_gid;
  stx_->mode          = st.st_mode;
  stx_->ino           = st.st_ino;
  stx_->size          = st.st_size;
  stx_->blocks        = st.st_blocks;
  stx_->atime.tv_sec  = st.st_atim.tv_sec;
  stx_->atime.tv_nsec = st.st_atim.tv_nsec;
  stx_->mtime.tv_sec  = st.st_mtim.tv_sec;
  stx_->mtime.tv_nsec = st.st_mtim.tv_nsec;
  stx_->ctime.tv_sec  = st.st_ctim.tv_sec;
  stx_->ctime.tv_nsec = st.st_ctim.tv_nsec;
  stx_->rdev_major    = ::major(st.st_rdev);
  stx_->rdev_minor    = ::minor(st.st_rdev);
  stx_->dev_major     = ::major(st.st_dev);
  stx_->dev_minor     = ::minor(st.st_dev);

  return true;
}

bool
BranchIndexMgr::readlink(const Branch   &branch_,
                         const fs::path &fusepath_,
                         char           *buf_,
                         const size_t    bufsize_,
                         ssize_t        *rv_)
{
  BranchIndex::Result r;
  std::string_view target;
  const BranchIndex::Entry *e = nullptr;
  std::shared_ptr<const BranchIndex::Index> idx;

  r = ::_lookup(branch_,fusepath_,&idx,&e);
  if(r == BranchIndex::Result::LIVE)
    return false;
  if(r != BranchIndex::Result::FOUND)
    return (*rv_ = ::_errno(r),true);
  if(!S_ISLNK(e->mode))
    return (*rv_ = -EINVAL,true);

  // same contract as readlink(2): not NUL terminated, truncated
  target = idx->symlink_target(*e);
  *rv_   = std::min(target.size(),bufsize_);
  ::memcpy(buf_,target.data(),*rv_);

  return true;
}

bool
fs::exists(const Branch   &branch_,
           const fs::path &fusepath_,
           struct stat    *st_)
{
  int rv;

  if(BranchIndexMgr::lstat(branch_,fusepath_,st_,&rv))
    return (rv == 0);

  return fs::exists(branch_.path,fusepath_,st_);
}

bool
fs::exists(const Branch   &branch_,
           const fs::path &fusepath_)
{
  struct stat st;

  return fs::exists(branch_,fusepath_,&st);
}
