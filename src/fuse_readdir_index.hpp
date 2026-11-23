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

#pragma once

#include "branch_index_mgr.hpp"
#include "fs_dirent64.hpp"
#include "fs_inode.hpp"
#include "fs_path.hpp"
#include "fuse_dirents.hpp"
#include "hashset.hpp"

#include <climits>
#include <cstring>

#include <dirent.h>


namespace ReaddirIndex
{
  // Sentinel fd for readdir implementations which open directories
  // ahead of reading them: "this branch is served by its index".
  constexpr int DEFERRED_FD = INT_MIN;

  static
  inline
  bool
  has_index(const Branch &branch_)
  {
    return !!BranchIndexMgr::get(branch_);
  }

  // Add the entries of `rel_dirpath_` on `branch_` from its index.
  // Returns true when the index answered (`*rv_` = 0 or -errno), false
  // when the directory must be read from the filesystem.
  static
  inline
  bool
  fill(const Branch   &branch_,
       const fs::path &rel_dirpath_,
       HashSet        &names_,
       fuse_dirents_t *dirents_,
       int            *rv_)
  {
    BranchIndex::Result r;
    std::span<const BranchIndex::Entry> children;
    std::shared_ptr<const BranchIndex::Index> idx;
    alignas(fs::dirent64) char buf[sizeof(fs::dirent64) + NAME_MAX + 1];
    fs::dirent64 *d = reinterpret_cast<fs::dirent64*>(buf);

    idx = BranchIndexMgr::get(branch_);
    if(!idx)
      return false;

    r = idx->list(rel_dirpath_.native(),&children);
    switch(r)
      {
      case BranchIndex::Result::LIVE:
        return false;
      case BranchIndex::Result::NOT_FOUND:
        *rv_ = -ENOENT;
        return true;
      case BranchIndex::Result::NOT_DIR:
        *rv_ = -ENOTDIR;
        return true;
      case BranchIndex::Result::FOUND:
        break;
      }

    fs::inode::ReaddirCalc inodecalc(branch_.path,rel_dirpath_);
    auto add = [&](const std::string_view name_,
                   const uint32_t         mode_,
                   const uint64_t         ino_)
    {
      if(name_.size() > NAME_MAX)
        return;
      if(names_.put(name_.data(),name_.size()) == 0)
        return;

      ::memcpy(d->name,name_.data(),name_.size());
      d->name[name_.size()] = '\0';
      d->type   = IFTODT(mode_);
      d->off    = 0;
      d->reclen = sizeof(buf);
      d->ino    = inodecalc.calc(d->name,
                                 name_.size(),
                                 (mode_ & S_IFMT),
                                 ino_);

      fuse_dirents_add(dirents_,d,name_.size());
    };

    // "." and ".." like getdents() would return them
    {
      size_t pos;
      std::string_view dir = rel_dirpath_.native();
      const BranchIndex::Entry *self   = nullptr;
      const BranchIndex::Entry *parent = nullptr;

      while(!dir.empty() && (dir.back() == '/'))
        dir.remove_suffix(1);
      pos = dir.rfind('/');

      idx->lookup(dir,&self);
      idx->lookup(((pos == std::string_view::npos) ?
                   std::string_view() :
                   dir.substr(0,pos)),
                  &parent);
      if(self)
        {
          if(dir.empty() || (parent == nullptr))
            parent = self;
          add(".",self->mode,self->ino);
          add("..",parent->mode,parent->ino);
        }
    }

    for(const auto &e : children)
      add(idx->name(e),e.mode,e.ino);

    *rv_ = 0;

    return true;
  }
}
