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

#include "roc_mirrors.hpp"

#include "branch_index_mgr.hpp"
#include "config.hpp"
#include "syslog.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>


namespace
{
  using Clock = std::chrono::steady_clock;

  // a mirror failing with anything but "no such file" is skipped for
  // that long: a dead rclone mount must not slow every open down
  constexpr auto BACKOFF = std::chrono::seconds(60);

  std::mutex                                            g_mutex;
  std::shared_ptr<const std::vector<RocMirrors::Rule>>  g_rules;
  std::map<std::string,std::shared_ptr<BranchIndexSlot>> g_mirrors;
  std::string                                           g_loaded_file;
  bool                                                  g_loaded = false;

  std::mutex g_coverage_mutex;

  std::shared_mutex          g_disks_mutex;
  std::map<std::string,bool> g_sleeping;

  std::mutex                           g_backoff_mutex;
  std::map<std::string,Clock::time_point> g_backoff;

  std::string
  _unescape(const std::string_view s_)
  {
    std::string rv;

    for(size_t i = 0; i < s_.size(); i++)
      {
        if((s_[i] == '\\') && ((i + 3) < s_.size() + 0) &&
           (s_[i+1] >= '0') && (s_[i+1] <= '7') &&
           (s_[i+2] >= '0') && (s_[i+2] <= '7') &&
           (s_[i+3] >= '0') && (s_[i+3] <= '7'))
          {
            rv += (char)(((s_[i+1] - '0') << 6) |
                         ((s_[i+2] - '0') << 3) |
                         (s_[i+3] - '0'));
            i += 3;
            continue;
          }
        rv += s_[i];
      }

    return rv;
  }

  std::string
  _normalize(std::string p_)
  {
    while((p_.size() > 1) && (p_.back() == '/'))
      p_.pop_back();
    return p_;
  }

  std::string_view
  _trim_slashes(std::string_view p_)
  {
    while(!p_.empty() && (p_.front() == '/'))
      p_.remove_prefix(1);
    while(!p_.empty() && (p_.back() == '/'))
      p_.remove_suffix(1);
    return p_;
  }

  // `path_` relative to `sub_` if inside it, else nullopt-like false
  bool
  _relative_to(const std::string_view  path_,
               const std::string_view  sub_,
               std::string_view       *rel_)
  {
    if(sub_.empty())
      {
        *rel_ = path_;
        return true;
      }
    if(path_.size() <= sub_.size())
      return false;
    if(path_.compare(0,sub_.size(),sub_) != 0)
      return false;
    if(path_[sub_.size()] != '/')
      return false;

    *rel_ = path_.substr(sub_.size() + 1);

    return true;
  }

  // rule `local_` relative to branch `branch_` ("" = branch root)
  bool
  _rule_sub(const std::string &local_,
            const std::string &branch_,
            std::string       *sub_)
  {
    if(local_ == branch_)
      {
        sub_->clear();
        return true;
      }
    if((local_.size() > branch_.size() + 1) &&
       (local_.compare(0,branch_.size(),branch_) == 0) &&
       ((local_[branch_.size()] == '/') || (branch_ == "/")))
      {
        *sub_ = local_.substr(branch_.size() + ((branch_ == "/") ? 0 : 1));
        return true;
      }
    return false;
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

  bool
  _in_backoff(const std::string &mirror_)
  {
    std::lock_guard<std::mutex> lk(g_backoff_mutex);

    auto it = g_backoff.find(mirror_);
    if(it == g_backoff.end())
      return false;
    if(Clock::now() < it->second)
      return true;
    g_backoff.erase(it);

    return false;
  }

  void
  _set_backoff(const std::string &mirror_,
               const std::string &path_,
               const int          err_)
  {
    {
      std::lock_guard<std::mutex> lk(g_backoff_mutex);
      g_backoff[mirror_] = Clock::now() + BACKOFF;
    }

    SysLog::warning("roc: mirror `{}` skipped for {}s: open of `{}`"
                    " failed: {}",
                    mirror_,
                    std::chrono::duration_cast<std::chrono::seconds>(BACKOFF).count(),
                    path_,strerror(err_));
  }

  std::shared_ptr<const std::string>
  _disk(const Branch &branch_)
  {
    if(!branch_.index_slot)
      return {};
    return branch_.index_slot->disk.load();
  }

  struct Candidate
  {
    std::string path;
    std::string mirror;
  };

  // mirror copies of a file, from the coverage table, plus the size
  // they must have
  bool
  _candidates(const Branch           &branch_,
              const fs::path         &fusepath_,
              std::vector<Candidate> *out_,
              uint64_t               *size_)
  {
    BranchIndex::Result r;
    const BranchIndex::Entry *e = nullptr;
    std::shared_ptr<const RocCoverage> cov;
    std::shared_ptr<const BranchIndex::Index> idx;
    const std::string_view fp = ::_trim_slashes(fusepath_.native());

    if(!branch_.roc() || !branch_.index_slot)
      return false;

    idx = BranchIndexMgr::get(branch_);
    if(!idx)
      return false;
    cov = branch_.index_slot->coverage.load();
    if(!cov || (cov->roc != idx))
      return false;

    r = idx->lookup(fp,&e);
    if((r != BranchIndex::Result::FOUND) || !S_ISREG(e->mode))
      return false;

    auto it = cov->by_entry.find(idx->index_of(*e));
    if(it == cov->by_entry.end())
      return false;

    for(const uint16_t k : it->second)
      {
        std::string_view rel;
        const RocCoverage::Target &t = cov->targets[k];

        if(!::_relative_to(fp,t.sub,&rel))
          continue;

        out_->push_back({t.mirror + '/' + std::string(rel),t.mirror});
      }

    *size_ = e->size;

    return !out_->empty();
  }

  void
  _wake(const Branch   &branch_,
        const fs::path &fusepath_)
  {
    auto disk = ::_disk(branch_);
    if(!disk)
      return;

    {
      std::unique_lock<std::shared_mutex> lk(g_disks_mutex);
      auto it = g_sleeping.find(*disk);
      if((it == g_sleeping.end()) || !it->second)
        return;
      it->second = false;
    }

    SysLog::info("roc: {} active again: `{}` is not available on a mirror",
                 *disk,fusepath_.string());
  }

  void
  _compute(const Branch                                         &branch_,
           const std::vector<RocMirrors::Rule>                  &rules_,
           const std::map<std::string,std::shared_ptr<BranchIndexSlot>> &mirrors_)
  {
    std::vector<std::string> dirs;
    std::shared_ptr<RocCoverage> cov;
    std::shared_ptr<const BranchIndex::Index> idx;
    std::vector<std::shared_ptr<const BranchIndex::Index>> midx;
    const std::string &bp = branch_.path.native();

    idx = BranchIndexMgr::get(branch_);
    if(!branch_.roc() || !idx)
      {
        branch_.index_slot->coverage.store(nullptr);
        return;
      }

    cov = std::make_shared<RocCoverage>();
    cov->roc = idx;

    for(const auto &rule : rules_)
      {
        std::string sub;

        if(cov->targets.size() >= 0xffff)
          break;
        if(!::_rule_sub(rule.local,bp,&sub))
          continue;
        auto it = mirrors_.find(rule.mirror);
        if(it == mirrors_.end())
          continue;

        cov->targets.push_back({sub,rule.mirror,it->second});
        midx.push_back(it->second->index.load());
      }

    // walk the whole branch: totals, and for every regular file the
    // mirrors holding a copy of the same size
    dirs.emplace_back();
    for(size_t i = 0; i < dirs.size(); i++)
      {
        std::span<const BranchIndex::Entry> kids;
        const std::string dir = dirs[i];

        if(idx->list(dir,&kids) != BranchIndex::Result::FOUND)
          continue;

        for(const auto &e : kids)
          {
            std::string child;

            if(e.flags & BranchIndex::Flag::LIVE)
              continue;

            child = (dir.empty() ?
                     std::string(idx->name(e)) :
                     dir + '/' + std::string(idx->name(e)));

            if(S_ISDIR(e.mode))
              {
                dirs.push_back(std::move(child));
                continue;
              }
            if(!S_ISREG(e.mode))
              continue;

            cov->files++;
            cov->bytes += e.size;

            std::vector<uint16_t> hits;
            for(size_t k = 0; k < cov->targets.size(); k++)
              {
                std::string_view rel;
                const BranchIndex::Entry *m = nullptr;

                if(!midx[k])
                  continue;
                if(!::_relative_to(child,cov->targets[k].sub,&rel))
                  continue;
                if(midx[k]->lookup(rel,&m) != BranchIndex::Result::FOUND)
                  continue;
                if(!S_ISREG(m->mode) || (m->size != e.size))
                  continue;

                hits.push_back(k);
              }

            if(hits.empty())
              continue;

            cov->covered_files++;
            cov->covered_bytes += e.size;
            cov->by_entry.emplace(idx->index_of(e),std::move(hits));
          }
      }

    branch_.index_slot->coverage.store(std::move(cov));
  }

  std::string
  _human(const uint64_t v_)
  {
    char buf[32];
    const char *units = "BKMGTPE";
    double d = v_;
    int u = 0;

    while((d >= 1024) && (u < 6))
      {
        d /= 1024;
        u++;
      }
    ::snprintf(buf,sizeof(buf),(u ? "%.1f%c" : "%.0f%c"),d,units[u]);

    return buf;
  }

  std::string
  _pct(const uint64_t a_,
       const uint64_t b_)
  {
    char buf[16];
    ::snprintf(buf,sizeof(buf),"%.1f%%",(b_ ? (100.0 * a_ / b_) : 0.0));
    return buf;
  }
}

int
RocMirrors::parse(std::string_view   text_,
                  std::vector<Rule> *rules_,
                  std::string       *err_)
{
  int lineno = 0;
  std::istringstream in{std::string(text_)};
  std::string line;

  rules_->clear();
  while(std::getline(in,line))
    {
      std::string tok;
      std::vector<std::string> toks;

      lineno++;
      if(!line.empty() && (line.back() == '\r'))
        line.pop_back();
      {
        size_t pos = line.find_first_not_of(" \t");
        if((pos == std::string::npos) || (line[pos] == '#'))
          continue;
      }

      std::istringstream ls(line);
      while(ls >> tok)
        toks.push_back(::_unescape(tok));

      if(toks.size() != 2)
        {
          *err_ = ("line " + std::to_string(lineno) +
                   ": expected `<local directory> <mirror directory>`");
          return -EINVAL;
        }
      if((toks[0].front() != '/') || (toks[1].front() != '/'))
        {
          *err_ = ("line " + std::to_string(lineno) +
                   ": paths must be absolute");
          return -EINVAL;
        }

      rules_->push_back({::_normalize(toks[0]),::_normalize(toks[1])});
    }

  return 0;
}

int
RocMirrors::reload(const Branches::Ptr &branches_,
                   const bool           force_)
{
  int rv = 0;
  std::string err;
  std::vector<Rule> rules;
  const std::string file = cfg.roc_mirrors_file;
  std::vector<std::pair<std::string,std::shared_ptr<BranchIndexSlot>>> started;

  {
    std::lock_guard<std::mutex> lk(g_mutex);
    if(!force_ && g_loaded && (file == g_loaded_file))
      return 0;
    g_loaded      = true;
    g_loaded_file = file;
  }

  if(!file.empty())
    {
      std::ifstream in(file);
      if(!in)
        {
          rv = -errno;
          SysLog::error("roc: cannot read mirrors file `{}`: {}",
                        file,strerror(errno));
          return rv;
        }
      std::stringstream ss;
      ss << in.rdbuf();
      rv = RocMirrors::parse(ss.str(),&rules,&err);
      if(rv < 0)
        {
          SysLog::error("roc: mirrors file `{}`: {}; keeping previous rules",
                        file,err);
          return rv;
        }
    }

  {
    std::set<std::string> wanted;
    std::lock_guard<std::mutex> lk(g_mutex);

    for(const auto &r : rules)
      wanted.insert(r.mirror);
    for(auto it = g_mirrors.begin(); it != g_mirrors.end();)
      it = (wanted.count(it->first) ? std::next(it) : g_mirrors.erase(it));
    for(const auto &m : wanted)
      if(!g_mirrors.count(m))
        {
          auto slot = std::make_shared<BranchIndexSlot>();
          g_mirrors.emplace(m,slot);
          started.emplace_back(m,slot);
        }

    g_rules = std::make_shared<const std::vector<Rule>>(std::move(rules));

    for(const auto &r : *g_rules)
      {
        bool found = false;
        for(const auto &b : *branches_)
          {
            std::string sub;
            if(b.roc() && ::_rule_sub(r.local,b.path.native(),&sub))
              found = true;
          }
        if(!found)
          SysLog::warning("roc: mirror rule `{}` -> `{}`: `{}` is not in a"
                          " ROC branch",r.local,r.mirror,r.local);
      }
  }

  for(const auto &[path,slot] : started)
    BranchIndexMgr::start(path,slot,false);

  RocMirrors::index_changed();

  return 0;
}

void
RocMirrors::refresh(const Branches::Ptr &branches_)
{
  std::vector<std::pair<std::string,std::shared_ptr<BranchIndexSlot>>> idle;

  RocMirrors::reload(branches_,false);

  {
    std::lock_guard<std::mutex> lk(g_mutex);
    for(const auto &[path,slot] : g_mirrors)
      if(slot->state.load() == BranchIndexSlot::IDLE)
        idle.emplace_back(path,slot);
  }

  for(const auto &[path,slot] : idle)
    BranchIndexMgr::start(path,slot,false);
}

int
RocMirrors::rescan(std::string_view which_)
{
  std::vector<std::pair<std::string,std::shared_ptr<BranchIndexSlot>>> todo;
  const bool all = (which_.empty() || (which_ == "all"));

  {
    std::lock_guard<std::mutex> lk(g_mutex);
    for(const auto &[path,slot] : g_mirrors)
      if(all || (path == ::_normalize(std::string(which_))))
        todo.emplace_back(path,slot);
  }

  for(const auto &[path,slot] : todo)
    BranchIndexMgr::start(path,slot,true);

  return (todo.empty() ? -ENOENT : 0);
}

void
RocMirrors::index_changed()
{
  std::shared_ptr<const std::vector<Rule>> rules;
  std::map<std::string,std::shared_ptr<BranchIndexSlot>> mirrors;
  std::lock_guard<std::mutex> clk(g_coverage_mutex);
  Branches::Ptr branches = cfg.branches;

  {
    std::lock_guard<std::mutex> lk(g_mutex);
    rules   = g_rules;
    mirrors = g_mirrors;
  }
  if(!rules)
    rules = std::make_shared<const std::vector<Rule>>();

  for(const auto &branch : *branches)
    {
      if(!branch.index_slot)
        continue;
      ::_compute(branch,*rules,mirrors);
    }
}

int
RocMirrors::set_sleeping(const Branches::Ptr &branches_,
                         std::string_view     which_,
                         const bool           sleeping_)
{
  std::set<std::string> keys;

  if((which_ == "all") || which_.empty())
    {
      for(const auto &b : *branches_)
        if(b.roc())
          if(auto d = ::_disk(b))
            keys.insert(*d);
    }
  else if(which_.starts_with("disk:") || which_.starts_with("dev:"))
    {
      keys.insert(std::string(which_));
    }
  else
    {
      const std::string p = ::_normalize(std::string(which_));
      for(const auto &b : *branches_)
        if(b.roc() && (b.path.native() == p))
          if(auto d = ::_disk(b))
            keys.insert(*d);
    }

  if(keys.empty())
    return -ENOENT;

  {
    std::unique_lock<std::shared_mutex> lk(g_disks_mutex);
    for(const auto &k : keys)
      g_sleeping[k] = sleeping_;
  }

  for(const auto &k : keys)
    SysLog::info("roc: {} marked {}",k,(sleeping_ ? "sleeping" : "active"));

  return 0;
}

bool
RocMirrors::sleeping(const Branch &branch_)
{
  auto disk = ::_disk(branch_);
  if(!disk)
    return false;

  std::shared_lock<std::shared_mutex> lk(g_disks_mutex);
  auto it = g_sleeping.find(*disk);

  return ((it != g_sleeping.end()) && it->second);
}

std::vector<std::string>
RocMirrors::candidates(const Branch   &branch_,
                       const fs::path &fusepath_)
{
  uint64_t size;
  std::vector<Candidate> c;
  std::vector<std::string> rv;

  if(::_candidates(branch_,fusepath_,&c,&size))
    for(auto &x : c)
      rv.push_back(std::move(x.path));

  return rv;
}

bool
RocMirrors::open(const Branch   &branch_,
                 const fs::path &fusepath_,
                 const int       flags_,
                 int            *fd_)
{
  uint64_t size = 0;
  std::vector<Candidate> c;

  if(!branch_.roc() || !RocMirrors::sleeping(branch_))
    return false;

  if(::_candidates(branch_,fusepath_,&c,&size))
    {
      for(const auto &cand : c)
        {
          int fd;
          struct stat st;

          if(::_in_backoff(cand.mirror))
            continue;

          fd = ::open(cand.path.c_str(),flags_|O_CLOEXEC);
          if(fd < 0)
            {
              // gone or changed since the mirror was scanned: next one
              if((errno != ENOENT) && (errno != ENOTDIR))
                ::_set_backoff(cand.mirror,cand.path,errno);
              continue;
            }

          // the copy must still be the one the table was built from
          if((::fstat(fd,&st) < 0) ||
             !S_ISREG(st.st_mode) ||
             (static_cast<uint64_t>(st.st_size) != size))
            {
              ::close(fd);
              continue;
            }

          *fd_ = fd;
          return true;
        }
    }

  // the caller reads the disk: it is spinning from now on
  ::_wake(branch_,fusepath_);

  return false;
}

std::string
RocMirrors::coverage_str(const RocCoverage &cov_)
{
  return ("mirrored=" + std::to_string(cov_.covered_files) + "/" +
          std::to_string(cov_.files) + " files (" +
          ::_pct(cov_.covered_files,cov_.files) + "), " +
          ::_human(cov_.covered_bytes) + "/" + ::_human(cov_.bytes) + " (" +
          ::_pct(cov_.covered_bytes,cov_.bytes) + ")");
}

std::string
RocMirrors::status(const Branches::Ptr &branches_)
{
  std::string rv;
  std::set<std::string> disks;
  std::map<std::string,std::shared_ptr<BranchIndexSlot>> mirrors;

  {
    std::lock_guard<std::mutex> lk(g_mutex);
    mirrors = g_mirrors;
  }

  for(const auto &[path,slot] : mirrors)
    {
      auto idx = slot->index.load();

      rv += "mirror " + path + ": " + ::_state_str(slot->state.load());
      if(idx)
        rv += " entries=" + std::to_string(idx->entries_count());
      if(::_in_backoff(path))
        rv += " backoff";
      rv += '\n';
    }

  for(const auto &b : *branches_)
    if(b.roc())
      if(auto d = ::_disk(b))
        disks.insert(*d);

  {
    std::shared_lock<std::shared_mutex> lk(g_disks_mutex);
    for(const auto &[k,v] : g_sleeping)
      disks.insert(k);
    for(const auto &k : disks)
      {
        auto it = g_sleeping.find(k);
        bool s = ((it != g_sleeping.end()) && it->second);
        rv += k + ": " + (s ? "sleeping" : "active") + '\n';
      }
  }

  return rv;
}

void
RocMirrors::reset()
{
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_rules.reset();
    g_mirrors.clear();
    g_loaded = false;
    g_loaded_file.clear();
  }
  {
    std::unique_lock<std::shared_mutex> lk(g_disks_mutex);
    g_sleeping.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_backoff_mutex);
    g_backoff.clear();
  }
}
