/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "MmappedFileMonitor.h"

#include "RecordSession.h"
#include "RecordTask.h"
#include "log.h"

using namespace std;

namespace rr {

MmappedFileMonitor::MmappedFileMonitor(Task* t, int fd) {
  ASSERT(t, !t->session().is_replaying());
  extant_ = true;
  dead_ = false;
  auto stat = t->stat_fd(fd);
  device_ = stat.st_dev;
  inode_ = stat.st_ino;
}

MmappedFileMonitor::MmappedFileMonitor(Task* t, EmuFile::shr_ptr f) {
  ASSERT(t, t->session().is_replaying());

  extant_ = !!f;
  dead_ = false;
  if (!extant_) {
    // Our only role is to disable syscall buffering for this fd.
    return;
  }

  device_ = f->device();
  inode_ = f->inode();
}

void MmappedFileMonitor::did_write(Task* t, const std::vector<Range>& ranges,
                                   int64_t offset) {
  // If there are no remaining mappings that we care about, those can't reappear
  // without going through mmap again, at which point this will be reset to
  // false.
  if (dead_) {
    return;
  }

  if (!extant_ || ranges.empty()) {
    return;
  }

  // Dead until proven otherwise
  dead_ = true;

  bool is_replay = t->session().is_replaying();
  for (auto v : t->session().vms()) {
    for (auto m : v->maps()) {
      auto km = m.map;

      if (is_replay) {
        if (!m.emu_file || m.emu_file->device() != device_ ||
            m.emu_file->inode() != inode_) {
          continue;
        }
      } else {
        if (km.device() != device_ || km.inode() != inode_) {
          continue;
        }
      }

      dead_ = false;

      ASSERT(t, offset >= 0)
          << "Only pwrite/pwritev supported on MAP_SHARED file descriptors";
      // XXX to fix that, we'd have to grab the file offset from
      // /proc/<pid>/fdinfo.

      // stat matches.
      ASSERT(t, km.flags() & MAP_SHARED);
      uint64_t mapping_offset = km.file_offset_bytes();
      int64_t local_offset = offset;
      for (auto r : ranges) {
        remote_ptr<void> start = km.start() + local_offset - mapping_offset;
        MemoryRange mr(start, r.length);
        if (km.intersects(mr)) {
          if (is_replay) {
            // If we're writing beyond the EmuFile's end, resize it.
            m.emu_file->ensure_size(local_offset + r.length);
          } else {
            // We will record multiple writes if the file is mapped multiple
            // times. This is inefficient, but not wrong.
            static_cast<RecordTask*>(t)->record_remote(km.intersect(mr));
          }
        }
        local_offset += r.length;
      }
    }
  }
}

} // namespace rr
