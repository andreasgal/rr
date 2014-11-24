/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_ADDRESS_SPACE_H_
#define RR_ADDRESS_SPACE_H_

#include <inttypes.h>
#include <sys/mman.h>

#include <map>
#include <memory>
#include <set>

#include "TraceStream.h"
#include "util.h"

class Session;
class Task;

/**
 * Base class for classes that manage a set of Tasks.
 */
class HasTaskSet {
public:
  typedef std::set<Task*> TaskSet;

  const TaskSet& task_set() const { return tasks; }

  void insert_task(Task* t);
  void erase_task(Task* t);

protected:
  TaskSet tasks;
};

/**
 * PseudoDevices aren't real disk devices, but they differentiate
 * memory mappings when we're trying to decide whether adjacent
 * FileId::NO_DEVICE mappings should be coalesced.
 */
enum PseudoDevice {
  PSEUDODEVICE_NONE = 0,
  PSEUDODEVICE_ANONYMOUS,
  PSEUDODEVICE_HEAP,
  PSEUDODEVICE_SCRATCH,
  PSEUDODEVICE_SHARED_MMAP_FILE,
  PSEUDODEVICE_STACK,
  PSEUDODEVICE_SYSCALLBUF,
  PSEUDODEVICE_VDSO,
};

/**
 * FileIds uniquely identify a file at a point in time (when the file
 * is stat'd).
 */
class FileId {
public:
  static const dev_t NO_DEVICE = 0;
  static const ino_t NO_INODE = 0;

  FileId(PseudoDevice psdev = PSEUDODEVICE_NONE)
      : device(NO_DEVICE), inode(NO_INODE), psdev(psdev) {}
  FileId(const struct stat& st, PseudoDevice psdev = PSEUDODEVICE_NONE)
      : device(st.st_dev), inode(st.st_ino), psdev(psdev) {}
  FileId(dev_t dev, ino_t ino, PseudoDevice psdev = PSEUDODEVICE_NONE)
      : device(dev), inode(ino), psdev(psdev) {}
  FileId(dev_t dev_major, dev_t dev_minor, ino_t ino,
         PseudoDevice psdev = PSEUDODEVICE_NONE);

  /**
   * Return the major/minor ID for the device underlying this
   * file.  If |is_real_device()| is false, return 0
   * (NO_DEVICE).
   */
  dev_t dev_major() const;
  dev_t dev_minor() const;
  /**
   * Return a displayable "real" inode.  If |is_real_device()|
   * is false, return 0 (NO_INODE).
   */
  ino_t disp_inode() const;
  PseudoDevice psuedodevice() const { return psdev; }

  /**
   * Return true iff |this| and |o| are the same "real device"
   * (i.e., same device and inode), or |this| and |o| are
   * ANONYMOUS pseudo-devices.  Results are undefined for other
   * pseudo-devices.
   */
  bool equivalent_to(const FileId& o) const {
    if (psdev != o.psdev) {
      return false;
    }
    if (psdev == PSEUDODEVICE_ANONYMOUS) {
      return true;
    }
    if (dev_major() != o.dev_major()) {
      return false;
    }
    // Allow device minor numbers to vary if the major device is
    // 0. This was observed to be happening on
    // "3.13.0-24-generic #46-Ubuntu SMP" in KVM with btrfs.
    if (dev_major() != 0 && dev_minor() != o.dev_minor()) {
      return false;
    }
    return inode == o.inode;
  }
  /**
   * Return true if this file is/was backed by an external
   * device, as opposed to a transient RAM mapping.
   */
  bool is_real_device() const { return device > NO_DEVICE; }
  const char* special_name() const;

  bool operator<(const FileId& o) const {
    return psdev != o.psdev ? psdev < o.psdev : device != o.device
                                                    ? device < o.device
                                                    : inode < o.inode;
  }

private:
  dev_t device;
  ino_t inode;
  PseudoDevice psdev;
};

/**
 * Describe the mapping of a MappableResource.  This includes the
 * offset of the mapping, its protection flags, the offest within the
 * resource, and more.
 */
struct Mapping {
  /**
   * These are the flags we track internally to distinguish
   * between adjacent segments.  For example, the kernel
   * considers a NORESERVE anonynmous mapping that's adjacent to
   * a non-NORESERVE mapping distinct, even if all other
   * metadata are the same.  See |is_adjacent_mapping()| in
   * task.cc.
   */
  static const int map_flags_mask =
      (MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE | MAP_SHARED | MAP_STACK);
  static const int checkable_flags_mask = (MAP_PRIVATE | MAP_SHARED);

  Mapping() : prot(0), flags(0), offset(0) {}
  Mapping(remote_ptr<void> addr, size_t num_bytes, int prot = 0, int flags = 0,
          off64_t offset = 0)
      : start(addr),
        end(addr + ceil_page_size(num_bytes)),
        prot(prot),
        flags(flags & map_flags_mask),
        offset(offset) {
    assert_valid();
  }
  Mapping(remote_ptr<void> start, remote_ptr<void> end, int prot = 0,
          int flags = 0, off64_t offset = 0)
      : start(start),
        end(end),
        prot(prot),
        flags(flags & map_flags_mask),
        offset(offset) {
    assert_valid();
  }

  Mapping(const Mapping& o)
      : start(o.start),
        end(o.end),
        prot(o.prot),
        flags(o.flags),
        offset(o.offset) {
    assert_valid();
  }
  Mapping operator=(const Mapping& o) {
    memcpy(this, &o, sizeof(*this));
    assert_valid();
    return *this;
  }

  void assert_valid() const {
    assert(end >= start);
    assert(num_bytes() % page_size() == 0);
    assert(!(flags & ~map_flags_mask));
    assert(offset % page_size() == 0);
  }

  /**
   * Return true iff |o| is an address range fully contained by
   * this.
   */
  bool has_subset(const Mapping& o) const {
    return start <= o.start && o.end <= end;
  }

  /**
   * Return true iff |o| and this map at least one shared byte.
   */
  bool intersects(const Mapping& o) const {
    return (start == o.start && o.end == end) ||
           (start <= o.start && o.start < end) ||
           (start < o.end && o.end <= end);
  }

  size_t num_bytes() const {
    ssize_t s = end - start;
    assert(s >= 0);
    return s;
  }

  /**
   * Return the lowest-common-denominator interpretation of this
   * mapping, namely, the one that can be parsed out of
   * /proc/maps.
   */
  Mapping to_kernel() const {
    return Mapping(start, end, prot, flags & checkable_flags_mask, offset);
  }

  /**
   * Dump a representation of |this| to a string in a format
   * similar to the former part of /proc/[tid]/maps.
  */
  std::string str() const {
    char str[200];
    sprintf(str, "%8p-%8p %c%c%c%c %08" PRIx64, (void*)start.as_int(),
            (void*)end.as_int(), (PROT_READ & prot) ? 'r' : '-',
            (PROT_WRITE & prot) ? 'w' : '-', (PROT_EXEC & prot) ? 'x' : '-',
            (MAP_SHARED & flags) ? 's' : 'p', offset);
    return str;
  }

  const remote_ptr<void> start;
  const remote_ptr<void> end;
  const int prot;
  const int flags;
  const off64_t offset;
};
std::ostream& operator<<(std::ostream& o, const Mapping& m);

/**
 * Compare |a| and |b| so that "subset" lookups will succeed.  What
 * does that mean?  If |a| and |b| overlap (intersect), then this
 * comparator considers them equivalent.  That means that if |a|
 * represents one byte within a mapping |b|, then |a| and |b| will be
 * considered equivalent.
 *
 * If |a| and |b| don't overlap, return true if |a|'s start addres is
 * less than |b|'s/
 */
struct MappingComparator {
  bool operator()(const Mapping& a, const Mapping& b) const {
    return a.intersects(b) ? false : a.start < b.start;
  }
};

/**
 * A resource that can be mapped into RAM.  |Mapping| represents a
 * mapping into RAM of a MappableResource.
 */
struct MappableResource {
  MappableResource() : id(FileId()) {}
  MappableResource(const FileId& id) : id(id) {}
  MappableResource(const FileId& id, const std::string& fsname)
      : id(id), fsname(fsname) {}

  bool operator==(const MappableResource& o) const {
    return id.equivalent_to(o.id);
  }
  bool operator!=(const MappableResource& o) const { return !(*this == o); }
  bool is_scratch() const { return PSEUDODEVICE_SCRATCH == id.psuedodevice(); }
  bool is_shared_mmap_file() const {
    return PSEUDODEVICE_SHARED_MMAP_FILE == id.psuedodevice();
  }
  bool is_stack() const { return PSEUDODEVICE_STACK == id.psuedodevice(); }

  /**
   * Return a representation of this resource that would be
   * parsed from /proc/maps if this were mapped.
   */
  MappableResource to_kernel() const {
    return MappableResource(
        FileId(id.dev_major(), id.dev_minor(), id.disp_inode()),
        fsname.c_str());
  }

  /**
   * Dump a representation of |this| to a string in a format
   * similar to the tail part of /proc/[tid]/maps. Some extra
   * informations are put in a '()'.
  */
  std::string str() const {
    char str[200];
    sprintf(str, "%02" PRIx64 ":%02" PRIx64 " %-10ld %s %s", id.dev_major(),
            id.dev_minor(), id.disp_inode(), fsname.c_str(), id.special_name());
    return str;
  }

  static MappableResource anonymous() {
    return FileId(FileId::NO_DEVICE, nr_anonymous_maps++,
                  PSEUDODEVICE_ANONYMOUS);
  }
  static MappableResource heap() {
    return MappableResource(
        FileId(FileId::NO_DEVICE, FileId::NO_INODE, PSEUDODEVICE_HEAP),
        "[heap]");
  }
  static MappableResource scratch(pid_t tid) {
    return MappableResource(
        FileId(FileId::NO_DEVICE, tid, PSEUDODEVICE_SCRATCH), "[scratch]");
  }
  static MappableResource shared_mmap_file(const TraceMappedRegion& file);
  static MappableResource stack(pid_t tid) {
    return MappableResource(FileId(FileId::NO_DEVICE, tid, PSEUDODEVICE_STACK),
                            "[stack]");
  }
  static MappableResource syscallbuf(pid_t tid, int fd, const char* path);

  FileId id;
  /**
   * Some name that this file may have on its underlying file
   * system.  Not used for anything other than labelling mapped
   * segments.
   */
  std::string fsname;

  static ino_t nr_anonymous_maps;
};

struct Breakpoint;

enum TrapType {
  TRAP_NONE = 0,
  // Trap for debugger 'stepi' request.
  TRAP_STEPI,
  // Trap for internal rr purposes, f.e. replaying async
  // signals.
  TRAP_BKPT_INTERNAL,
  // Trap on behalf of a debugger user.
  TRAP_BKPT_USER,
};

// XXX one is tempted to merge Breakpoint and Watchpoint into a single
// entity, but the semantics are just different enough that separate
// objects are easier for now.
class Watchpoint;

enum WatchType {
  // NB: these random-looking enumeration values are chosen to
  // match the numbers programmed into x86 debug registers.
  WATCH_EXEC = 0x00,
  WATCH_WRITE = 0x01,
  WATCH_READWRITE = 0x03
};

enum DebugStatus {
  DS_WATCHPOINT0 = 1 << 0,
  DS_WATCHPOINT1 = 1 << 1,
  DS_WATCHPOINT2 = 1 << 2,
  DS_WATCHPOINT3 = 1 << 3,
  DS_WATCHPOINT_ANY = 0xf,
  DS_SINGLESTEP = 1 << 14,
};

/**
 * Range of memory addresses that can be used as a std::map key.
 */
struct MemoryRange {
  MemoryRange(remote_ptr<void> addr, size_t num_bytes)
      : addr(addr), num_bytes(num_bytes) {}
  MemoryRange(const MemoryRange&) = default;
  MemoryRange& operator=(const MemoryRange&) = default;

  bool operator==(const MemoryRange& o) const {
    return addr == o.addr && num_bytes == o.num_bytes;
  }
  bool operator<(const MemoryRange& o) const {
    return addr != o.addr ? addr < o.addr : num_bytes < o.num_bytes;
  }

  remote_ptr<void> addr;
  size_t num_bytes;
};

/**
 * A distinct watchpoint, corresponding to the information needed to
 * program a single x86 debug register.
 */
struct WatchConfig {
  WatchConfig(remote_ptr<void> addr, size_t num_bytes, WatchType type)
      : addr(addr), num_bytes(num_bytes), type(type) {}
  remote_ptr<void> addr;
  size_t num_bytes;
  WatchType type;
};

/**
 * Models the address space for a set of tasks.  This includes the set
 * of mapped pages, and the resources those mappings refer to.
 */
class AddressSpace : public HasTaskSet {
  friend class Session;
  friend struct VerifyAddressSpace;

public:
  typedef std::map<remote_ptr<uint8_t>, std::shared_ptr<Breakpoint> >
  BreakpointMap;
  typedef std::map<Mapping, MappableResource, MappingComparator> MemoryMap;
  typedef std::shared_ptr<AddressSpace> shr_ptr;
  typedef std::map<MemoryRange, std::shared_ptr<Watchpoint> > WatchpointMap;

  ~AddressSpace();

  /**
   * Call this after a new task has been cloned within this
   * address space.
   */
  void after_clone();

  /**
   * Change the program data break of this address space to
   * |addr|.
   */
  void brk(remote_ptr<void> addr);

  /**
   * Dump a representation of |this| to stderr in a format
   * similar to /proc/[tid]/maps.
   *
   * XXX/ostream-ify me.
   */
  void dump() const;

  /**
   * Return true if this was created as the result of an exec()
   * call, instead of cloned from another address space.
   */
  bool execed() const { return !is_clone; }

  /**
   * Return the path this address space was exec()'d with.
   */
  const std::string& exe_image() const { return exe; }

  /**
   * Assuming the last retired instruction has raised a SIGTRAP
   * and might be a breakpoint trap instruction, return the type
   * of breakpoint set at |ip() - sizeof(breakpoint_insn)|, if
   * one exists.  Otherwise return TRAP_NONE.
   */
  TrapType get_breakpoint_type_for_retired_insn(remote_ptr<uint8_t> ip);

  /**
   * Return the type of breakpoint that's been registered for
   * |addr|.
   */
  TrapType get_breakpoint_type_at_addr(remote_ptr<uint8_t> addr);

  /**
   * Map |num_bytes| into this address space at |addr|, with
   * |prot| protection and |flags|.  The pages are (possibly
   * initially) backed starting at |offset| of |res|.
   */
  void map(remote_ptr<void> addr, size_t num_bytes, int prot, int flags,
           off64_t offset_bytes, const MappableResource& res);

  /**
   * Return the mapping and mapped resource that underly [addr,
   * addr + num_bytes).  There must be exactly one such mapping.
   */
  MemoryMap::value_type mapping_of(remote_ptr<void> addr,
                                   size_t num_bytes) const;

  /**
   * Return the memory map.
   */
  const MemoryMap& memmap() const { return mem; }

  /**
   * Change the protection bits of [addr, addr + num_bytes) to
   * |prot|.
   */
  void protect(remote_ptr<void> addr, size_t num_bytes, int prot);

  /**
   * Move the mapping [old_addr, old_addr + old_num_bytes) to
   * [new_addr, old_addr + new_num_bytes), preserving metadata.
   */
  void remap(remote_ptr<void> old_addr, size_t old_num_bytes,
             remote_ptr<void> new_addr, size_t new_num_bytes);

  /**
   * Remove a |type| reference to the breakpoint at |addr|.  If
   * the removed reference was the last, the breakpoint is
   * destroyed.
   */
  void remove_breakpoint(remote_ptr<uint8_t> addr, TrapType type);

  /** Ensure a breakpoint of |type| is set at |addr|. */
  bool set_breakpoint(remote_ptr<uint8_t> addr, TrapType type);

  /**
   * Destroy all breakpoints in this VM, regardless of their
   * reference counts.
   */
  void destroy_all_breakpoints();

  /**
   * Manage watchpoints.  Analogous to breakpoint-managing
   * methods above, except that watchpoints can be set for an
   * address range.
   */
  void remove_watchpoint(remote_ptr<void> addr, size_t num_bytes,
                         WatchType type);
  bool set_watchpoint(remote_ptr<void> addr, size_t num_bytes, WatchType type);
  void destroy_all_watchpoints();

  /**
   * Make [addr, addr + num_bytes) inaccesible within this
   * address space.
   */
  void unmap(remote_ptr<void> addr, ssize_t num_bytes);

  /** Return the vdso mapping of this. */
  Mapping vdso() const;

  /** Return whether we have mapped libc.so.  */
  bool has_libc() const;

  /** Return the libc mapping of this. */
  Mapping libc() const;

  /** Return whether we have mapped libpthread.so. */
  bool has_libpthread() const;

  /** Return the libpthread mapping of this.  has_libpthread() must be true. */
  Mapping libpthread() const;

  /**
   * Verify that this cached address space matches what the
   * kernel thinks it should be.
   */
  void verify(Task* t) const;

  bool has_breakpoints() { return !breakpoints.empty(); }
  bool has_watchpoints() { return !watchpoints.empty(); }

  // Encoding of the |int $3| instruction.
  static const uint8_t breakpoint_insn = 0xCC;

  ScopedFd& mem_fd() { return child_mem_fd; }
  void set_mem_fd(ScopedFd&& fd) { child_mem_fd = std::move(fd); }

  /**
   * Call this when an exec replaces 'as' with 'this' for some process.
   */
  void is_replacing(AddressSpace& as) {
    // Take over as's child_mem_fd.
    child_mem_fd = std::move(as.child_mem_fd);
  }

private:
  AddressSpace(Task* t, const std::string& exe, Session& session);
  AddressSpace(const AddressSpace& o);

  /**
   * Construct a minimal set of watchpoints to be enabled based
   * on |set_watchpoint()| calls, and program them for each task
   * in this address space.
   */
  bool allocate_watchpoints();

  /**
   * Merge the mappings adjacent to |it| in memory that are
   * semantically "adjacent mappings" of the same resource as
   * well, for example have adjacent file offsets and the same
   * prot and flags.
   */
  void coalesce_around(MemoryMap::iterator it);

  /**
   * Erase |it| from |breakpoints| and restore any memory in
   * this it may have overwritten.
   */
  void destroy_breakpoint(BreakpointMap::const_iterator it);

  /**
   * For each mapped segment overlapping [addr, addr +
   * num_bytes), call |f|.  Pass |f| the overlapping mapping,
   * the mapped resource, and the range of addresses remaining
   * to be iterated over.
   *
   * Pass |ITERATE_CONTIGUOUS| to stop iterating when the last
   * contiguous mapping after |addr| within the region is seen.
   * Default is to iterate all mappings in the region.
   */
  enum {
    ITERATE_DEFAULT,
    ITERATE_CONTIGUOUS
  };
  void for_each_in_range(
      remote_ptr<void> addr, ssize_t num_bytes,
      std::function<void(const Mapping& m, const MappableResource& r,
                         const Mapping& rem)> f,
      int how = ITERATE_DEFAULT);

  /**
   * Map |m| of |r| into this address space, and coalesce any
   * mappings of |r| that are adjacent to |m|.
   */
  void map_and_coalesce(const Mapping& m, const MappableResource& r);

  /** Set the dynamic heap segment to |[start, end)| */
  void update_heap(remote_ptr<void> start, remote_ptr<void> end) {
    heap = Mapping(start, end, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, 0);
  }

  // All breakpoints set in this VM.
  BreakpointMap breakpoints;
  /* Path of the executable image this address space was
   * exec()'d with. */
  std::string exe;
  /* Track the special process-global heap in order to support
   * adjustments by brk(). */
  Mapping heap;
  /* Were we cloned from another address space? */
  bool is_clone;
  /* All segments mapped into this address space. */
  MemoryMap mem;
  // The session that created this.  We save a ref to it so that
  // we can notify it when we die.
  Session* session;
  /* First mapped byte of the vdso. */
  remote_ptr<void> vdso_start_addr;
  /* First mapped byte of libc's text section. */
  remote_ptr<void> libc_start_addr;
  /* First mapped byte of libpthread's text section.  May be null. */
  remote_ptr<void> libpthread_start_addr;
  // The watchpoints set for tasks in this VM.  Watchpoints are
  // programmed per Task, but we track them per address space on
  // behalf of debuggers that assume that model.
  WatchpointMap watchpoints;
  // Tracee memory is read and written through this fd, which is
  // opened for the tracee's magic /proc/[tid]/mem device.  The
  // advantage of this over ptrace is that we can access it even
  // when the tracee isn't at a ptrace-stop.  It's also
  // theoretically faster for large data transfers, which rr can
  // do often.
  //
  // Users of child_mem_fd should fall back to ptrace-based memory
  // access when child_mem_fd is not open.
  ScopedFd child_mem_fd;

  /**
   * Ensure that the cached mapping of |t| matches /proc/maps,
   * using adjancent-map-merging heuristics that are as lenient
   * as possible given the data available from /proc/maps.
   */
  static void check_segment_iterator(void* vasp, Task* t,
                                     const struct map_iterator_data* data);

  /**
   * After an exec, populate the new address space of |t| with
   * the existing mappings we find in /proc/maps.
   */
  static void populate_address_space(void* asp, Task* t,
                                     const struct map_iterator_data* data);

  AddressSpace operator=(const AddressSpace&) = delete;
};

#endif /* RR_ADDRESS_SPACE_H_ */
