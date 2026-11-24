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

#include "branch_index.hpp"

#include "fs_getdents64.hpp"
#include "rapidhash/rapidhash.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <unordered_set>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>


using namespace BranchIndex;

namespace
{
  constexpr char     MAGIC[8]    = {'M','F','S','I','D','X','\0','\1'};
  constexpr uint32_t ENDIAN_MARK = 0x01020304;
  constexpr uint64_t U32_MAX     = std::numeric_limits<uint32_t>::max();
  constexpr size_t   DENTS_BUF   = 128 * 1024;

  struct LinuxDirent64
  {
    uint64_t ino;
    int64_t  off;
    uint16_t reclen;
    uint8_t  type;
    char     name[];
  };

  class ScopedFD
  {
  public:
    explicit ScopedFD(int fd_ = -1) : fd(fd_) {}
    ~ScopedFD() { if(fd >= 0) ::close(fd); }
    ScopedFD(const ScopedFD&) = delete;
    ScopedFD& operator=(const ScopedFD&) = delete;
  public:
    int fd;
  };

  uint64_t
  _align_up(const uint64_t v_)
  {
    return ((v_ + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
  }

  std::string_view
  _trim(std::string_view p_)
  {
    while(!p_.empty() && (p_.front() == '/'))
      p_.remove_prefix(1);
    while(!p_.empty() && (p_.back() == '/'))
      p_.remove_suffix(1);
    return p_;
  }

  uint64_t
  _hash_path(const std::string_view p_)
  {
    return rapidhash(p_.data(),p_.size());
  }

  // Same encoding as the kernel's new_encode_dev(): 12 bit major,
  // 20 bit minor, fits the kernel's internal 32 bit dev_t.
  uint32_t
  _encode_rdev(const dev_t rdev_)
  {
    const uint32_t maj = ::major(rdev_);
    const uint32_t min = ::minor(rdev_);
    return ((min & 0xff) | (maj << 8) | ((min & ~0xffU) << 12));
  }

  dev_t
  _decode_rdev(const uint32_t v_)
  {
    const uint32_t maj = ((v_ & 0xfff00) >> 8);
    const uint32_t min = ((v_ & 0xff) | ((v_ >> 12) & 0xfff00));
    return ::makedev(maj,min);
  }

  void
  _fill_entry(const struct stat &st_,
              Entry             *e_)
  {
    *e_ = Entry{};
    e_->mode     = st_.st_mode;
    e_->uid      = st_.st_uid;
    e_->gid      = st_.st_gid;
    e_->nlink    = ((st_.st_nlink > U32_MAX) ? U32_MAX : st_.st_nlink);
    e_->size     = st_.st_size;
    e_->blocks   = st_.st_blocks;
    e_->ino      = st_.st_ino;
    e_->mtime_s  = st_.st_mtim.tv_sec;
    e_->mtime_ns = st_.st_mtim.tv_nsec;
    e_->ctime_s  = st_.st_ctim.tv_sec;
    e_->ctime_ns = st_.st_ctim.tv_nsec;
    if(S_ISCHR(st_.st_mode) || S_ISBLK(st_.st_mode))
      e_->aux = _encode_rdev(st_.st_rdev);
  }

  uint64_t
  _signature(const std::span<const Entry>     entries_,
             const std::span<const Dir>       dirs_,
             const std::span<const uint32_t>  hash_,
             const std::string_view           paths_,
             const std::string_view           names_)
  {
    uint64_t h;

    h = VERSION;
    h = rapidhash_withSeed(entries_.data(),entries_.size_bytes(),h);
    h = rapidhash_withSeed(dirs_.data(),dirs_.size_bytes(),h);
    h = rapidhash_withSeed(hash_.data(),hash_.size_bytes(),h);
    h = rapidhash_withSeed(paths_.data(),paths_.size(),h);
    h = rapidhash_withSeed(names_.data(),names_.size(),h);

    return h;
  }

  struct Layout
  {
    Section  entries;
    Section  dirs;
    Section  hash;
    Section  paths;
    Section  names;
    uint64_t file_size;
  };

  Layout
  _layout(const uint64_t entries_bytes_,
          const uint64_t dirs_bytes_,
          const uint64_t hash_bytes_,
          const uint64_t paths_bytes_,
          const uint64_t names_bytes_)
  {
    Layout l;
    uint64_t off = HEADER_SIZE;
    auto place = [&](const uint64_t size_)
    {
      Section s{off,size_};
      off = _align_up(off + size_);
      return s;
    };

    l.entries   = place(entries_bytes_);
    l.dirs      = place(dirs_bytes_);
    l.hash      = place(hash_bytes_);
    l.paths     = place(paths_bytes_);
    l.names     = place(names_bytes_);
    l.file_size = off;

    return l;
  }

  int
  _pwrite_all(const int       fd_,
              const void     *buf_,
              size_t          size_,
              off_t           off_)
  {
    const char *p = static_cast<const char*>(buf_);

    while(size_ > 0)
      {
        ssize_t rv;

        rv = ::pwrite(fd_,p,size_,off_);
        if(rv < 0)
          {
            if(errno == EINTR)
              continue;
            return -errno;
          }

        p     += rv;
        size_ -= rv;
        off_  += rv;
      }

    return 0;
  }

  bool
  _section_ok(const Section  &s_,
              const uint64_t  file_size_)
  {
    if(s_.off % ALIGNMENT)
      return false;
    if(s_.off < HEADER_SIZE)
      return false;
    if(s_.off > file_size_)
      return false;
    if(s_.size > (file_size_ - s_.off))
      return false;
    return true;
  }

  std::string
  _dirname(const std::string &p_)
  {
    size_t pos = p_.rfind('/');

    if(pos == std::string::npos)
      return ".";
    if(pos == 0)
      return "/";
    return p_.substr(0,pos);
  }
}

namespace BranchIndex
{
  class Builder
  {
  public:
    int run(const std::string      &path,
            std::shared_ptr<Index> *index,
            const std::string      &mountinfo);

  private:
    int _scan_dir(const size_t dir_idx);
    int _add_name(const std::string_view  name,
                  uint32_t               *off);

  private:
    int                      _root_fd = -1;
    dev_t                    _root_dev = 0;
    std::vector<Entry>       _entries;
    std::vector<Dir>         _dirs;
    std::vector<std::string> _dir_paths;
    std::string              _names;
    std::vector<char>        _buf;
    std::unordered_set<std::string> _mounts;
  };
}

int
Builder::_add_name(const std::string_view  name_,
                   uint32_t               *off_)
{
  if((_names.size() + name_.size() + 1) > U32_MAX)
    return -EOVERFLOW;

  *off_ = _names.size();
  _names.append(name_);

  return 0;
}

int
Builder::_scan_dir(const size_t dir_idx_)
{
  struct Raw
  {
    uint64_t    ino;
    std::string name;
  };
  struct Child
  {
    std::string name;
    struct stat st;
    std::string target;
  };

  int rv;
  std::vector<Raw> raw;
  std::vector<Child> children;
  const std::string parent = _dir_paths[dir_idx_];

  ScopedFD dfd(::openat(_root_fd,
                        (parent.empty() ? "." : parent.c_str()),
                        O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
  if(dfd.fd < 0)
    return -errno;

  for(;;)
    {
      ssize_t nread;

      nread = fs::getdents64(dfd.fd,_buf.data(),_buf.size());
      if(nread < 0)
        return nread;
      if(nread == 0)
        break;

      for(ssize_t pos = 0; pos < nread;)
        {
          auto *d = reinterpret_cast<const LinuxDirent64*>(&_buf[pos]);
          pos += d->reclen;

          if((::strcmp(d->name,".") == 0) || (::strcmp(d->name,"..") == 0))
            continue;

          raw.push_back({d->ino,d->name});
        }
    }

  // stat in inode order: far fewer seeks on spinning disks
  std::sort(raw.begin(),raw.end(),
            [](const Raw &a_, const Raw &b_) { return a_.ino < b_.ino; });

  children.reserve(raw.size());
  for(auto &r : raw)
    {
      Child c;

      rv = ::fstatat(dfd.fd,r.name.c_str(),&c.st,
                     AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT);
      if(rv < 0)
        {
          if(errno == ENOENT)
            continue;
          return -errno;
        }

      if(S_ISLNK(c.st.st_mode))
        {
          size_t bufsize = ((c.st.st_size > 0) ? c.st.st_size + 1 : 256);
          for(;;)
            {
              ssize_t len;

              c.target.resize(bufsize);
              len = ::readlinkat(dfd.fd,r.name.c_str(),c.target.data(),bufsize);
              if(len < 0)
                return -errno;
              if(static_cast<size_t>(len) < bufsize)
                {
                  c.target.resize(len);
                  break;
                }
              bufsize *= 2;
            }
        }

      c.name = std::move(r.name);
      children.push_back(std::move(c));
    }

  std::sort(children.begin(),children.end(),
            [](const Child &a_, const Child &b_) { return a_.name < b_.name; });

  if((_entries.size() + children.size()) >= U32_MAX)
    return -EOVERFLOW;

  _dirs[dir_idx_].first_child = _entries.size();
  _dirs[dir_idx_].child_count = children.size();

  for(auto &c : children)
    {
      Entry e;

      std::string rel = (parent.empty() ? c.name : parent + '/' + c.name);
      const bool is_mount = (_mounts.count(rel) ||
                             (S_ISDIR(c.st.st_mode) &&
                              (c.st.st_dev != _root_dev)));

      ::_fill_entry(c.st,&e);
      rv = _add_name(c.name,&e.name_off);
      if(rv < 0)
        return rv;
      e.name_len = c.name.size();
      if(is_mount)
        e.flags |= Flag::LIVE;

      if(S_ISDIR(c.st.st_mode))
        {
          Dir d{};

          if(_dirs.size() >= U32_MAX)
            return -EOVERFLOW;

          d.entry = _entries.size();
          if(is_mount)
            d.flags |= Flag::LIVE;

          e.aux = _dirs.size();
          _dirs.push_back(d);
          _dir_paths.push_back(std::move(rel));
        }
      else if(S_ISLNK(c.st.st_mode))
        {
          rv = _add_name(c.target,&e.aux);
          if(rv < 0)
            return rv;
          _names.push_back('\0');
        }

      _entries.push_back(e);
    }

  return 0;
}

int
Builder::run(const std::string      &path_,
             std::shared_ptr<Index> *index_,
             const std::string      &mountinfo_)
{
  int rv;
  struct stat st;
  struct statvfs vfs;
  Entry root;
  std::shared_ptr<Index> idx;

  ScopedFD root_fd(::open(path_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
  if(root_fd.fd < 0)
    return -errno;
  _root_fd = root_fd.fd;

  rv = ::fstat(_root_fd,&st);
  if(rv < 0)
    return -errno;
  rv = ::fstatvfs(_root_fd,&vfs);
  if(rv < 0)
    return -errno;

  _root_dev = st.st_dev;
  _buf.resize(DENTS_BUF);

  {
    char *real = ::realpath(path_.c_str(),nullptr);
    std::string root = (real ? real : path_);
    ::free(real);
    for(auto &m : BranchIndex::mountpoints_below(root,mountinfo_))
      _mounts.insert(std::move(m));
  }

  ::_fill_entry(st,&root);
  root.aux = 0;
  _entries.push_back(root);
  _dirs.push_back(Dir{});
  _dir_paths.emplace_back();

  // _dirs grows while scanning: breadth first, the vector is the queue
  for(size_t i = 0; i < _dirs.size(); i++)
    {
      if(_dirs[i].flags & Flag::LIVE)
        continue;
      rv = _scan_dir(i);
      if(rv < 0)
        return rv;
    }

  idx = std::make_shared<Index>();

  for(size_t i = 0; i < _dirs.size(); i++)
    {
      if((idx->_paths_v.size() + _dir_paths[i].size()) > U32_MAX)
        return -EOVERFLOW;
      _dirs[i].path_off = idx->_paths_v.size();
      _dirs[i].path_len = _dir_paths[i].size();
      idx->_paths_v.append(_dir_paths[i]);
    }

  {
    uint64_t n = 2;
    uint64_t mask;

    while(n < (_dirs.size() * 2))
      n <<= 1;
    mask = n - 1;

    idx->_hash_v.assign(n,0);
    for(size_t i = 0; i < _dirs.size(); i++)
      {
        uint64_t h = (::_hash_path(_dir_paths[i]) & mask);
        while(idx->_hash_v[h] != 0)
          h = ((h + 1) & mask);
        idx->_hash_v[h] = (i + 1);
      }
  }

  idx->_entries_v = std::move(_entries);
  idx->_dirs_v    = std::move(_dirs);
  idx->_names_v   = std::move(_names);

  idx->_entries   = idx->_entries_v;
  idx->_dirs      = idx->_dirs_v;
  idx->_hash      = idx->_hash_v;
  idx->_paths     = idx->_paths_v;
  idx->_names     = idx->_names_v;

  idx->_fsid      = vfs.f_fsid;
  idx->_blksize   = st.st_blksize;
  idx->_created   = ::time(NULL);
  idx->_dev       = _root_dev;
  idx->_signature = ::_signature(idx->_entries,idx->_dirs,idx->_hash,
                                 idx->_paths,idx->_names);

  *index_ = std::move(idx);

  return 0;
}

int
BranchIndex::build(const std::string      &path_,
                   std::shared_ptr<Index> *index_,
                   const std::string      &mountinfo_)
{
  Builder b;

  return b.run(path_,index_,mountinfo_);
}

// mountinfo escapes ' ', '\t', '\n' and '\\' as \ooo
static
std::string
_unescape_mountinfo(const std::string_view s_)
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

std::vector<std::string>
BranchIndex::mountpoints_below(const std::string &root_,
                               const std::string &mountinfo_)
{
  FILE *f;
  char *line = nullptr;
  size_t linecap = 0;
  std::string prefix;
  std::vector<std::string> rv;

  prefix = root_;
  while((prefix.size() > 1) && (prefix.back() == '/'))
    prefix.pop_back();
  if(prefix != "/")
    prefix += '/';

  f = ::fopen(mountinfo_.c_str(),"re");
  if(f == nullptr)
    return rv;

  // "36 35 98:0 /mnt1 /mnt/parent rw,noatime master:1 - ext3 ..."
  //                    ^ 5th field: mount point
  while(::getline(&line,&linecap,f) > 0)
    {
      std::string_view l(line);
      size_t pos = 0;

      for(int field = 0; field < 4; field++)
        {
          pos = l.find(' ',pos);
          if(pos == std::string_view::npos)
            break;
          pos++;
        }
      if(pos == std::string_view::npos)
        continue;

      size_t end = l.find(' ',pos);
      std::string mp = ::_unescape_mountinfo(l.substr(pos,end - pos));

      if((mp.size() > prefix.size()) &&
         (mp.compare(0,prefix.size(),prefix) == 0))
        rv.emplace_back(mp.substr(prefix.size()));
    }

  ::free(line);
  ::fclose(f);

  return rv;
}

Index::Index()
  : _map(nullptr),
    _map_size(0),
    _locked(0),
    _signature(0),
    _fsid(0),
    _blksize(0),
    _created(0),
    _dev(0)
{
}

Index::~Index()
{
  if(_map)
    ::munmap(_map,_map_size);
}

uint64_t
Index::byte_size() const
{
  return ::_layout(_entries.size_bytes(),
                   _dirs.size_bytes(),
                   _hash.size_bytes(),
                   _paths.size(),
                   _names.size()).file_size;
}

std::string_view
Index::name(const Entry &e_) const
{
  return _names.substr(e_.name_off,e_.name_len);
}

std::string_view
Index::symlink_target(const Entry &e_) const
{
  if(!S_ISLNK(e_.mode))
    return {};

  const char   *s   = _names.data() + e_.aux;
  const size_t  max = _names.size() - e_.aux;

  return std::string_view(s,::strnlen(s,max));
}

void
Index::to_stat(const Entry &e_,
               struct stat *st_) const
{
  *st_ = {};
  st_->st_dev   = _dev;
  st_->st_ino   = e_.ino;
  st_->st_mode  = e_.mode;
  st_->st_nlink = e_.nlink;
  st_->st_uid   = e_.uid;
  st_->st_gid   = e_.gid;
  st_->st_size  = e_.size;
  st_->st_blksize = _blksize;
  st_->st_blocks  = e_.blocks;
  st_->st_mtim.tv_sec  = e_.mtime_s;
  st_->st_mtim.tv_nsec = e_.mtime_ns;
  st_->st_ctim.tv_sec  = e_.ctime_s;
  st_->st_ctim.tv_nsec = e_.ctime_ns;
  // atime is not indexed: branches are read-only so it is static
  // anyway; report mtime.
  st_->st_atim = st_->st_mtim;
  if(S_ISCHR(e_.mode) || S_ISBLK(e_.mode))
    st_->st_rdev = ::_decode_rdev(e_.aux);
}

int
Index::_find_dir(const std::string_view path_) const
{
  uint64_t i;
  uint64_t mask;

  if(_hash.empty())
    return -1;

  mask = (_hash.size() - 1);
  i    = (::_hash_path(path_) & mask);
  for(size_t n = 0; n < _hash.size(); n++)
    {
      uint32_t v = _hash[i];

      if(v == 0)
        return -1;

      const Dir &d = _dirs[v - 1];
      if(_paths.substr(d.path_off,d.path_len) == path_)
        return (v - 1);

      i = ((i + 1) & mask);
    }

  return -1;
}

int
Index::_find_child(const Dir              &dir_,
                   const std::string_view  name_) const
{
  auto begin = _entries.begin() + dir_.first_child;
  auto end   = begin + dir_.child_count;
  auto it    = std::lower_bound(begin,end,name_,
                                [this](const Entry &e_, std::string_view n_)
                                {
                                  return (name(e_) < n_);
                                });

  if((it == end) || (name(*it) != name_))
    return -1;

  return (it - _entries.begin());
}

// `path_` is a directory path absent from the hash: work out why by
// walking up to the closest indexed ancestor.
Result
Index::_resolve_missing_parent(std::string_view path_) const
{
  for(;;)
    {
      int d;
      size_t pos;
      std::string_view anc;
      std::string_view comp;

      pos  = path_.rfind('/');
      anc  = ((pos == std::string_view::npos) ? std::string_view() : path_.substr(0,pos));
      comp = ((pos == std::string_view::npos) ? path_ : path_.substr(pos + 1));

      d = _find_dir(anc);
      if(d >= 0)
        {
          int c;
          const Dir &dir = _dirs[d];

          if(dir.flags & Flag::LIVE)
            return Result::LIVE;

          c = _find_child(dir,comp);
          if(c < 0)
            return Result::NOT_FOUND;

          const Entry &e = _entries[c];
          if(e.flags & Flag::LIVE)
            return Result::LIVE;
          // lstat() follows symlinks before the last component
          if(S_ISLNK(e.mode))
            return Result::LIVE;
          if(S_ISDIR(e.mode))
            return Result::NOT_FOUND;

          return Result::NOT_DIR;
        }

      if(anc.empty())
        return Result::NOT_FOUND;

      path_ = anc;
    }
}

Result
Index::lookup(std::string_view   path_,
              const Entry      **entry_) const
{
  int d;
  int c;
  size_t pos;
  std::string_view parent;
  std::string_view nm;

  path_ = ::_trim(path_);
  if(path_.empty())
    {
      *entry_ = &_entries[_dirs[0].entry];
      return Result::FOUND;
    }

  pos    = path_.rfind('/');
  parent = ((pos == std::string_view::npos) ? std::string_view() : path_.substr(0,pos));
  nm     = ((pos == std::string_view::npos) ? path_ : path_.substr(pos + 1));

  d = _find_dir(parent);
  if(d < 0)
    return _resolve_missing_parent(parent);

  const Dir &dir = _dirs[d];
  if(dir.flags & Flag::LIVE)
    return Result::LIVE;

  c = _find_child(dir,nm);
  if(c < 0)
    return Result::NOT_FOUND;

  const Entry &e = _entries[c];
  if(e.flags & Flag::LIVE)
    return Result::LIVE;

  *entry_ = &e;

  return Result::FOUND;
}

Result
Index::list(std::string_view         path_,
            std::span<const Entry>  *children_) const
{
  int d;

  path_ = ::_trim(path_);

  d = _find_dir(path_);
  if(d < 0)
    return _resolve_missing_parent(path_);

  const Dir &dir = _dirs[d];
  if(dir.flags & Flag::LIVE)
    return Result::LIVE;

  *children_ = _entries.subspan(dir.first_child,dir.child_count);

  return Result::FOUND;
}

int64_t
Index::mlock(const uint64_t budget_)
{
  uint64_t left;
  uint64_t total;
  const char *base;
  const std::pair<const void*,uint64_t> order[] =
    {
      {_map,HEADER_SIZE},
      {_hash.data(),_hash.size_bytes()},
      {_dirs.data(),_dirs.size_bytes()},
      {_paths.data(),_paths.size()},
      {_entries.data(),_entries.size_bytes()},
      {_names.data(),_names.size()}
    };

  if(_map == nullptr)
    return -EINVAL;
  if(_locked)
    {
      ::munlock(_map,_map_size);
      _locked = 0;
    }

  base  = static_cast<const char*>(_map);
  left  = budget_;
  total = 0;
  for(const auto &[ptr,size] : order)
    {
      uint64_t n;

      if(left == 0)
        break;
      n = std::min(left,size);
      if(n == 0)
        continue;
      if(::mlock(ptr,n) < 0)
        {
          int err = errno;
          if(total)
            ::munlock(base,_map_size);
          return -err;
        }
      left  -= n;
      total += n;
    }

  _locked = total;

  return total;
}

int
BranchIndex::save(const Index       &index_,
                  const std::string &filepath_,
                  const bool         write_if_changed_,
                  bool              *written_)
{
  int rv;
  Header h{};
  Layout l;
  std::string tmp;

  if(written_)
    *written_ = false;

  l = ::_layout(index_._entries.size_bytes(),
                index_._dirs.size_bytes(),
                index_._hash.size_bytes(),
                index_._paths.size(),
                index_._names.size());

  if(write_if_changed_)
    {
      ScopedFD fd(::open(filepath_.c_str(),O_RDONLY|O_CLOEXEC));
      if(fd.fd >= 0)
        {
          Header cur;
          if((::pread(fd.fd,&cur,sizeof(cur),0) == sizeof(cur)) &&
             (::memcmp(cur.magic,MAGIC,sizeof(MAGIC)) == 0) &&
             (cur.version   == VERSION) &&
             (cur.endian    == ENDIAN_MARK) &&
             (cur.signature == index_._signature) &&
             (cur.fsid      == index_._fsid) &&
             (cur.file_size == l.file_size))
            return 0;
        }
    }

  ::memcpy(h.magic,MAGIC,sizeof(MAGIC));
  h.version       = VERSION;
  h.endian        = ENDIAN_MARK;
  h.header_size   = HEADER_SIZE;
  h.file_size     = l.file_size;
  h.signature     = index_._signature;
  h.fsid          = index_._fsid;
  h.blksize       = index_._blksize;
  h.created       = index_._created;
  h.entries_count = index_._entries.size();
  h.dirs_count    = index_._dirs.size();
  h.hash_count    = index_._hash.size();
  h.entries       = l.entries;
  h.dirs          = l.dirs;
  h.hash          = l.hash;
  h.paths         = l.paths;
  h.names         = l.names;

  tmp = filepath_ + ".tmp." + std::to_string(::getpid());

  {
    // the index reveals names, owners and sizes of files in
    // directories other users may not be allowed to list
    ScopedFD fd(::open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600));
    if(fd.fd < 0)
      return -errno;
    if(::fchmod(fd.fd,0600) < 0)
      {
        rv = -errno;
        ::unlink(tmp.c_str());
        return rv;
      }

    const std::pair<const void*,Section> parts[] =
      {
        {&h,{0,sizeof(h)}},
        {index_._entries.data(),l.entries},
        {index_._dirs.data(),l.dirs},
        {index_._hash.data(),l.hash},
        {index_._paths.data(),l.paths},
        {index_._names.data(),l.names}
      };

    for(const auto &[ptr,sec] : parts)
      {
        rv = ::_pwrite_all(fd.fd,ptr,sec.size,sec.off);
        if(rv < 0)
          break;
      }
    if((rv >= 0) && (::ftruncate(fd.fd,l.file_size) < 0))
      rv = -errno;
    if((rv >= 0) && (::fsync(fd.fd) < 0))
      rv = -errno;

    if(rv < 0)
      {
        ::unlink(tmp.c_str());
        return rv;
      }
  }

  if(::rename(tmp.c_str(),filepath_.c_str()) < 0)
    {
      rv = -errno;
      ::unlink(tmp.c_str());
      return rv;
    }

  {
    ScopedFD dfd(::open(::_dirname(filepath_).c_str(),
                        O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    if(dfd.fd >= 0)
      ::fsync(dfd.fd);
  }

  if(written_)
    *written_ = true;

  return 0;
}

int
BranchIndex::load(const std::string      &filepath_,
                  std::shared_ptr<Index> *index_)
{
  void *map;
  struct stat st;
  const Header *h;
  std::shared_ptr<Index> idx;

  {
    ScopedFD fd(::open(filepath_.c_str(),O_RDONLY|O_CLOEXEC));
    if(fd.fd < 0)
      return -errno;
    if(::fstat(fd.fd,&st) < 0)
      return -errno;
    if(!S_ISREG(st.st_mode))
      return -EBADMSG;
    // written by someone else or readable by others: not ours to use
    if((st.st_uid != ::geteuid()) || (st.st_mode & 077))
      return -EACCES;
    if(static_cast<uint64_t>(st.st_size) < HEADER_SIZE)
      return -EBADMSG;

    map = ::mmap(nullptr,st.st_size,PROT_READ,MAP_SHARED,fd.fd,0);
    if(map == MAP_FAILED)
      return -errno;
  }

  idx = std::make_shared<Index>();
  idx->_map      = map;
  idx->_map_size = st.st_size;

  h = static_cast<const Header*>(map);
  if(::memcmp(h->magic,MAGIC,sizeof(MAGIC)) != 0)
    return -EBADMSG;
  if((h->version != VERSION) || (h->endian != ENDIAN_MARK))
    return -EBADMSG;
  if((h->header_size != HEADER_SIZE) ||
     (h->file_size != static_cast<uint64_t>(st.st_size)))
    return -EBADMSG;

  for(const Section *s : {&h->entries,&h->dirs,&h->hash,&h->paths,&h->names})
    if(!::_section_ok(*s,h->file_size))
      return -EBADMSG;

  if((h->entries_count == 0) ||
     (h->entries_count > (h->entries.size / sizeof(Entry))) ||
     (h->entries.size != (h->entries_count * sizeof(Entry))))
    return -EBADMSG;
  if((h->dirs_count == 0) ||
     (h->dirs_count > (h->dirs.size / sizeof(Dir))) ||
     (h->dirs.size != (h->dirs_count * sizeof(Dir))))
    return -EBADMSG;
  if((h->hash_count <= h->dirs_count) ||
     (h->hash_count & (h->hash_count - 1)) ||
     (h->hash.size != (h->hash_count * sizeof(uint32_t))))
    return -EBADMSG;
  if((h->paths.size > U32_MAX) || (h->names.size > U32_MAX))
    return -EBADMSG;

  {
    const char *base = static_cast<const char*>(map);

    idx->_entries = {reinterpret_cast<const Entry*>(base + h->entries.off),
                     h->entries_count};
    idx->_dirs    = {reinterpret_cast<const Dir*>(base + h->dirs.off),
                     h->dirs_count};
    idx->_hash    = {reinterpret_cast<const uint32_t*>(base + h->hash.off),
                     h->hash_count};
    idx->_paths   = {base + h->paths.off,h->paths.size};
    idx->_names   = {base + h->names.off,h->names.size};
  }

  if(::_signature(idx->_entries,idx->_dirs,idx->_hash,
                   idx->_paths,idx->_names) != h->signature)
    return -EBADMSG;

  // Bounds of every reference, so a crafted or damaged file that
  // still matches its signature cannot cause out of bounds reads.
  for(const Entry &e : idx->_entries)
    {
      if((static_cast<uint64_t>(e.name_off) + e.name_len) > idx->_names.size())
        return -EBADMSG;
      if(S_ISDIR(e.mode) && (e.aux >= idx->_dirs.size()))
        return -EBADMSG;
      if(S_ISLNK(e.mode) &&
         ((e.aux >= idx->_names.size()) ||
          (::memchr(idx->_names.data() + e.aux,'\0',
                    idx->_names.size() - e.aux) == nullptr)))
        return -EBADMSG;
    }
  for(const Dir &d : idx->_dirs)
    {
      if(d.entry >= idx->_entries.size())
        return -EBADMSG;
      if(!S_ISDIR(idx->_entries[d.entry].mode))
        return -EBADMSG;
      if((static_cast<uint64_t>(d.first_child) + d.child_count) > idx->_entries.size())
        return -EBADMSG;
      if((static_cast<uint64_t>(d.path_off) + d.path_len) > idx->_paths.size())
        return -EBADMSG;
    }
  if((idx->_dirs[0].entry != 0) || (idx->_dirs[0].path_len != 0))
    return -EBADMSG;
  for(const uint32_t v : idx->_hash)
    if(v > idx->_dirs.size())
      return -EBADMSG;

  ::madvise(map,st.st_size,MADV_RANDOM);

  idx->_signature = h->signature;
  idx->_fsid      = h->fsid;
  idx->_blksize   = h->blksize;
  idx->_created   = h->created;

  *index_ = std::move(idx);

  return 0;
}

// <filesystem id>-<hash of the branch's canonical path>.idx: several
// branches can live on one filesystem (sub directories) and each needs
// its own file. Computed once per save / load, never per request.
std::string
BranchIndex::filename(const std::string &branch_path_)
{
  int rv;
  uint64_t fsid;
  uint64_t pathid;
  struct statvfs vfs;
  std::string canonical;
  char buf[64];

  {
    char *p = ::realpath(branch_path_.c_str(),nullptr);
    if(p)
      {
        canonical = p;
        ::free(p);
      }
    else
      {
        canonical = branch_path_;
        while((canonical.size() > 1) && (canonical.back() == '/'))
          canonical.pop_back();
      }
  }

  rv     = ::statvfs(canonical.c_str(),&vfs);
  fsid   = ((rv == 0) ? static_cast<uint64_t>(vfs.f_fsid) : 0);
  pathid = ::_hash_path(canonical);

  ::snprintf(buf,sizeof(buf),"%016llx-%016llx.idx",
             static_cast<unsigned long long>(fsid),
             static_cast<unsigned long long>(pathid));

  return buf;
}
