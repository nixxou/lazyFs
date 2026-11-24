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
  Immutable metadata index of a branch whose filesystem is mounted
  read-only.

  Because the filesystem cannot change while mounted read-only the
  index, built once by a full scan, stays exact. It answers getattr,
  readdir and readlink style questions without touching the
  underlying device, including reliable negative answers.

  Layout (identical in memory and on disk, see Header):

    entries[] : one record per file / dir / symlink / special file.
                The children of a directory are contiguous and sorted
                by name (byte order). entries[0] is the branch root.
    dirs[]    : one record per directory: its entry, its children
                slice and its full relative path. dirs[0] is the root.
    hash[]    : open addressing table, directory path -> dirs[] index
                (value is index + 1, 0 means empty).
    paths     : directory paths (no leading or trailing '/', root = "").
    names     : entry names and NUL terminated symlink targets.

  Nested mount points (any entry listed in mountinfo below the branch,
  or a directory on another device) are flagged LIVE: nothing is
  indexed under them and lookups at or below them must go to the
  filesystem. So are paths going through a symlink, which the kernel
  would follow.

  An Index is never modified after construction so concurrent readers
  need no locking.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>


namespace BranchIndex
{
  constexpr uint32_t VERSION     = 2;
  constexpr uint64_t HEADER_SIZE = 4096;
  constexpr uint64_t ALIGNMENT   = 4096;

  namespace Flag
  {
    constexpr uint16_t LIVE = 0x0001;
  }

  struct Entry
  {
    uint32_t name_off;
    uint16_t name_len;
    uint16_t flags;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    // dir: index in dirs[]
    // symlink: offset of NUL terminated target in names
    // chr/blk: encoded rdev
    uint32_t aux;
    uint32_t mtime_ns;
    uint32_t ctime_ns;
    uint32_t _pad;
    uint64_t size;
    uint64_t blocks;
    uint64_t ino;
    int64_t  mtime_s;
    int64_t  ctime_s;
  };
  static_assert(sizeof(Entry) == 80);

  struct Dir
  {
    uint32_t entry;
    uint32_t first_child;
    uint32_t child_count;
    uint32_t path_off;
    uint32_t path_len;
    uint32_t flags;
  };
  static_assert(sizeof(Dir) == 24);

  struct Section
  {
    uint64_t off;
    uint64_t size;
  };

  struct Header
  {
    char     magic[8];
    uint32_t version;
    uint32_t endian;
    uint64_t header_size;
    uint64_t file_size;
    uint64_t signature;
    uint64_t fsid;
    uint64_t blksize;
    int64_t  created;
    uint64_t entries_count;
    uint64_t dirs_count;
    uint64_t hash_count;
    Section  entries;
    Section  dirs;
    Section  hash;
    Section  paths;
    Section  names;
  };
  static_assert(sizeof(Header) <= HEADER_SIZE);

  enum class Result
    {
      FOUND,      // *entry_ / *children_ set
      NOT_FOUND,  // ENOENT, without touching the filesystem
      NOT_DIR,    // ENOTDIR, a path component is not a directory
      LIVE        // at or below a mount point: ask the filesystem
    };

  class Index
  {
  public:
    Index();
    ~Index();

    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;

  public:
    Result lookup(std::string_view   path,
                  const Entry      **entry) const;
    Result list(std::string_view          path,
                std::span<const Entry>   *children) const;

    std::string_view name(const Entry &entry) const;
    uint32_t         index_of(const Entry &entry) const
    { return static_cast<uint32_t>(&entry - _entries.data()); }
    std::string_view symlink_target(const Entry &entry) const;
    void             to_stat(const Entry &entry,
                             struct stat *st) const;

  public:
    uint64_t signature() const { return _signature; }
    uint64_t fsid() const { return _fsid; }
    dev_t    dev() const { return _dev; }
    void     set_dev(dev_t dev_) { _dev = dev_; }
    size_t   entries_count() const { return _entries.size(); }
    size_t   dirs_count() const { return _dirs.size(); }
    bool     mapped() const { return (_map != nullptr); }
    // bytes needed to hold the index (file size when serialized)
    uint64_t byte_size() const;

    // Lock up to `budget_` bytes of a mapped index in RAM, most
    // useful sections first: header, hash, dirs, paths, entries,
    // names. Returns the number of bytes locked or -errno.
    int64_t  mlock(uint64_t budget);

  private:
    int  _find_dir(std::string_view path) const;
    int  _find_child(const Dir &dir, std::string_view name) const;
    Result _resolve_missing_parent(std::string_view parent) const;

  private:
    friend class Builder;
    friend int load(const std::string&, std::shared_ptr<Index>*);
    friend int save(const Index&, const std::string&, bool, bool*);

    // owned storage (index built in memory)
    std::vector<Entry>    _entries_v;
    std::vector<Dir>      _dirs_v;
    std::vector<uint32_t> _hash_v;
    std::string           _paths_v;
    std::string           _names_v;

    // mapped storage (index loaded from file)
    void   *_map;
    size_t  _map_size;
    size_t  _locked;

    // views over whichever storage is in use
    std::span<const Entry>    _entries;
    std::span<const Dir>      _dirs;
    std::span<const uint32_t> _hash;
    std::string_view          _paths;
    std::string_view          _names;

    uint64_t _signature;
    uint64_t _fsid;
    uint64_t _blksize;
    int64_t  _created;
    dev_t    _dev;
  };

  // Scan the branch at `path_` and build an in memory index.
  // Fails (returns -errno) on any error other than an entry vanishing
  // during the scan: a partial index would give false negatives.
  // `mountinfo_` lists the mount points to treat as LIVE.
  int build(const std::string       &path,
            std::shared_ptr<Index>  *index,
            const std::string       &mountinfo = "/proc/self/mountinfo");

  // Mount points strictly below `root_` (canonical path), relative to
  // it, read from a mountinfo file.
  std::vector<std::string> mountpoints_below(const std::string &root,
                                             const std::string &mountinfo);

  // Write `index_` to `filepath_` atomically (tmp + fsync + rename),
  // mode 0600. With `write_if_changed_` the existing file is kept when
  // it holds the same signature. `*written_` tells if a write happened.
  int save(const Index       &index,
           const std::string &filepath,
           bool               write_if_changed,
           bool              *written);

  // Map and fully validate (bounds and signature) an index file.
  // -EACCES if it is not owned by us or is accessible to others.
  int load(const std::string       &filepath,
           std::shared_ptr<Index>  *index);

  // File name used for a branch: filesystem id plus a hash of the
  // branch's canonical path, unique even for several branches on one
  // filesystem.
  std::string filename(const std::string &branch_path);
}
