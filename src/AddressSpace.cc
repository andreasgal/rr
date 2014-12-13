/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "AddressSpace"

#include "AddressSpace.h"

#include <linux/kdev_t.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>

#include "preload/preload_interface.h"

#include "AutoRemoteSyscalls.h"
#include "log.h"
#include "RecordSession.h"
#include "Session.h"
#include "task.h"

using namespace rr;
using namespace std;

/*static*/ ino_t MappableResource::nr_anonymous_maps;

/*static*/ const uint8_t AddressSpace::breakpoint_insn;

dev_t FileId::dev_major() const { return is_real_device() ? MAJOR(device) : 0; }

dev_t FileId::dev_minor() const { return is_real_device() ? MINOR(device) : 0; }

ino_t FileId::disp_inode() const { return is_real_device() ? inode : 0; }

const char* FileId::special_name() const {
  switch (psdev) {
    case PSEUDODEVICE_ANONYMOUS:
      return "";
    case PSEUDODEVICE_HEAP:
      return "(heap)";
    case PSEUDODEVICE_NONE:
      return "";
    case PSEUDODEVICE_SCRATCH:
      return "";
    case PSEUDODEVICE_SHARED_MMAP_FILE:
      return "(shmmap)";
    case PSEUDODEVICE_STACK:
      return "(stack)";
    case PSEUDODEVICE_SYSCALLBUF:
      return "(syscallbuf)";
    case PSEUDODEVICE_VDSO:
      return "(vdso)";
  }
  FATAL() << "Not reached";
  return nullptr;
}

void HasTaskSet::insert_task(Task* t) {
  LOG(debug) << "adding " << t->tid << " to task set " << this;
  tasks.insert(t);
}

void HasTaskSet::erase_task(Task* t) {
  LOG(debug) << "removing " << t->tid << " from task group " << this;
  tasks.erase(t);
}

FileId::FileId(dev_t dev_major, dev_t dev_minor, ino_t ino, PseudoDevice psdev)
    : device(MKDEV(dev_major, dev_minor)), inode(ino), psdev(psdev) {}

ostream& operator<<(ostream& o, const Mapping& m) {
  o << m.start << "-" << m.end << " " << HEX(m.prot) << " f:" << HEX(m.flags);
  return o;
}

/*static*/ MappableResource MappableResource::shared_mmap_file(
    const TraceMappedRegion& file) {
  return MappableResource(FileId(file.stat(), PSEUDODEVICE_SHARED_MMAP_FILE),
                          file.file_name().c_str());
}

/*static*/ MappableResource MappableResource::syscallbuf(pid_t tid, int fd,
                                                         const char* path) {
  struct stat st;
  if (fstat(fd, &st)) {
    FATAL() << "Failed to fstat(" << fd << ") (" << path << ")";
  }
  return MappableResource(FileId(st), path);
}

/**
 * Represents a refcount set on a particular address.  Because there
 * can be multiple refcounts of multiple types set on a single
 * address, Breakpoint stores explicit USER and INTERNAL breakpoint
 * refcounts.  Clients adding/removing breakpoints at this addr must
 * call ref()/unref() as appropropiate.
 */
struct Breakpoint {
  typedef shared_ptr<Breakpoint> shr_ptr;

  // AddressSpace::destroy_all_breakpoints() can cause this
  // destructor to be invoked while we have nonzero total
  // refcount, so the most we can assert is that the refcounts
  // are valid.
  ~Breakpoint() { assert(internal_count >= 0 && user_count >= 0); }

  shr_ptr clone() { return shr_ptr(new Breakpoint(*this)); }

  void ref(TrapType which) {
    assert(internal_count >= 0 && user_count >= 0);
    ++*counter(which);
  }
  int unref(TrapType which) {
    assert(internal_count > 0 || user_count > 0);
    --*counter(which);
    assert(internal_count >= 0 && user_count >= 0);
    return internal_count + user_count;
  }

  TrapType type() const {
    // NB: USER breakpoints need to be processed before
    // INTERNAL ones.  We want to give the debugger a
    // chance to dispatch commands before we attend to the
    // internal rr business.  So if there's a USER "ref"
    // on the breakpoint, treat it as a USER breakpoint.
    return user_count > 0 ? TRAP_BKPT_USER : TRAP_BKPT_INTERNAL;
  }

  static shr_ptr create() { return shr_ptr(new Breakpoint()); }

  // "Refcounts" of breakpoints set at |addr|.  The breakpoint
  // object must be unique since we have to save the overwritten
  // data, and we can't enforce the order in which breakpoints
  // are set/removed.
  int internal_count, user_count;
  uint8_t overwritten_data;
  static_assert(sizeof(overwritten_data) ==
                    sizeof(AddressSpace::breakpoint_insn),
                "Must have the same size.");

private:
  Breakpoint() : internal_count(), user_count() {}
  Breakpoint(const Breakpoint& o) = default;

  int* counter(TrapType which) {
    assert(TRAP_BKPT_INTERNAL == which || TRAP_BKPT_USER == which);
    int* p = TRAP_BKPT_USER == which ? &user_count : &internal_count;
    assert(*p >= 0);
    return p;
  }
};

enum {
  EXEC_BIT = 1 << 0,
  READ_BIT = 1 << 1,
  WRITE_BIT = 1 << 2
};

/** Return the access bits above needed to watch |type|. */
static int access_bits_of(WatchType type) {
  switch (type) {
    case WATCH_EXEC:
      return EXEC_BIT;
    case WATCH_WRITE:
      return WRITE_BIT;
    case WATCH_READWRITE:
      return READ_BIT | WRITE_BIT;
    default:
      FATAL() << "Unknown watchpoint type " << type;
      return 0; // not reached
  }
}

/**
 * Track the watched accesses of a contiguous range of memory
 * addresses.
 */
class Watchpoint {
public:
  typedef shared_ptr<Watchpoint> shr_ptr;

  ~Watchpoint() { assert_valid(); }

  shr_ptr clone() { return shr_ptr(new Watchpoint(*this)); }

  void watch(int which) {
    assert_valid();
    exec_count += (EXEC_BIT & which) != 0;
    read_count += (READ_BIT & which) != 0;
    write_count += (WRITE_BIT & which) != 0;
  }
  int unwatch(int which) {
    assert_valid();
    if (EXEC_BIT & which) {
      assert(exec_count > 0);
      --exec_count;
    }
    if (READ_BIT & which) {
      assert(read_count > 0);
      --read_count;
    }
    if (WRITE_BIT & which) {
      assert(write_count > 0);
      --write_count;
    }
    return exec_count + read_count + write_count;
  }

  int watched_bits() const {
    return (exec_count > 0 ? EXEC_BIT : 0) | (read_count > 0 ? READ_BIT : 0) |
           (write_count > 0 ? WRITE_BIT : 0);
  }

  static shr_ptr create() { return shr_ptr(new Watchpoint()); }

private:
  Watchpoint() : exec_count(), read_count(), write_count() {}
  Watchpoint(const Watchpoint&) = default;

  void assert_valid() const {
    assert(exec_count >= 0 && read_count >= 0 && write_count >= 0);
  }

  // Watchpoints stay alive until all watched access typed have
  // been cleared.  We track refcounts of each watchable access
  // separately.
  int exec_count, read_count, write_count;
};

/**
 * Advance *str to skip leading blank characters.
 */
static const char* trim_leading_blanks(const char* str) {
  const char* trimmed = str;
  while (isblank(*trimmed)) {
    ++trimmed;
  }
  return trimmed;
}

/**
 * Collecion of data describing a mapped memory segment, as parsed
 * from /proc/[tid]/maps on linux.
 */
struct mapped_segment_info {
  mapped_segment_info()
      : prot(0),
        flags(0),
        file_offset(0),
        inode(0),
        dev_major(0),
        dev_minor(0) {}
  /* Name of the segment, which isn't necessarily an fs entry
   * anywhere. */
  std::string name;
  remote_ptr<void> start_addr;
  remote_ptr<void> end_addr;
  int prot;
  int flags;
  int64_t file_offset;
  int64_t inode;
  int dev_major;
  int dev_minor;
};

ostream& operator<<(ostream& o, const mapped_segment_info& m) {
  o << m.start_addr << "-" << m.end_addr << " " << HEX(m.prot)
    << " f:" << HEX(m.flags);
  return o;
}

/**
 * The following helpers are used to iterate over a tracee's memory
 * maps.  Clients call |iterate_memory_map()|, passing an iterator
 * function that's invoked for each mapping.
 *
 * For each map, a |struct map_iterator_data| object is provided which
 * contains segment info, the size of the mapping, and the raw
 * /proc/maps line the data was parsed from.
 *
 * Any pointers passed transitively to the iterator function are
 * *owned by |iterate_memory_map()||*.  Iterator functions must copy
 * the data they wish to save beyond the scope of the iterator
 * function invocation.
 */
struct map_iterator_data {
  map_iterator_data() : raw_map_line(nullptr) {}
  struct mapped_segment_info info;
  const char* raw_map_line;
};
typedef void (*memory_map_iterator_t)(void* it_data, Task* t,
                                      const struct map_iterator_data* data);

static void iterate_memory_map(Task* t, memory_map_iterator_t it,
                               void* it_data) {
  FILE* maps_file;
  char line[PATH_MAX * 2];
  {
    char maps_path[PATH_MAX];
    snprintf(maps_path, sizeof(maps_path) - 1, "/proc/%d/maps", t->tid);
    ASSERT(t, (maps_file = fopen(maps_path, "r"))) << "Failed to open "
                                                   << maps_path;
  }
  while (fgets(line, sizeof(line), maps_file)) {
    struct map_iterator_data data;
    data.raw_map_line = line;
    uint64_t start, end;
    char flags[32];
    int chars_scanned;
    int nparsed = sscanf(
        line, "%" SCNx64 "-%" SCNx64 " %31s %" SCNx64 " %x:%x %" SCNu64 " %n",
        &start, &end, flags, &data.info.file_offset, &data.info.dev_major,
        &data.info.dev_minor, &data.info.inode, &chars_scanned);
    ASSERT(t, (8 /*number of info fields*/ == nparsed ||
               7 /*num fields if name is blank*/ == nparsed))
        << "Only parsed " << nparsed << " fields of segment info from\n"
        << data.raw_map_line;

    // trim trailing newline, if any
    int last_char = strlen(line) - 1;
    if (line[last_char] == '\n') {
      line[last_char] = 0;
    }
    data.info.name = trim_leading_blanks(line + chars_scanned);
#if defined(__i386__)
    if (start > numeric_limits<uint32_t>::max() ||
        end > numeric_limits<uint32_t>::max() ||
        data.info.name == "[vsyscall]") {
      // We manually read the exe link here because
      // this helper is used to set
      // |t->vm()->exe_image()|, so we can't rely on
      // that being correct yet.
      char proc_exe[PATH_MAX];
      char exe[PATH_MAX];
      snprintf(proc_exe, sizeof(proc_exe), "/proc/%d/exe", t->tid);
      readlink(proc_exe, exe, sizeof(exe));
      FATAL() << "Sorry, tracee " << t->tid << " has x86-64 image " << exe
              << " and that's not supported with a 32-bit rr.";
    }
#endif
    data.info.start_addr = start;
    data.info.end_addr = end;

    data.info.prot |= strchr(flags, 'r') ? PROT_READ : 0;
    data.info.prot |= strchr(flags, 'w') ? PROT_WRITE : 0;
    data.info.prot |= strchr(flags, 'x') ? PROT_EXEC : 0;
    data.info.flags |= strchr(flags, 'p') ? MAP_PRIVATE : 0;
    data.info.flags |= strchr(flags, 's') ? MAP_SHARED : 0;

    it(it_data, t, &data);
  }
  fclose(maps_file);
}

static void print_process_mmap_iterator(void* unused, Task* t,
                                        const struct map_iterator_data* data) {
  fputs(data->raw_map_line, stderr);
  fputc('\n', stderr);
}

/**
 * Cat the /proc/[t->tid]/maps file to stdout, line by line.
 */
static void print_process_mmap(Task* t) {
  iterate_memory_map(t, print_process_mmap_iterator, nullptr);
}

AddressSpace::~AddressSpace() { session->on_destroy(this); }

void AddressSpace::after_clone() { allocate_watchpoints(); }

struct RrVdso {
  remote_ptr<uint8_t> start;
  remote_ptr<uint8_t> end;
};

static void find_rr_vdso_map(void* it_data, Task* t,
                             const struct map_iterator_data* data) {
  if (data->info.name == "[vdso]") {
    auto result = static_cast<RrVdso*>(it_data);
    result->start = data->info.start_addr.cast<uint8_t>();
    result->end = data->info.end_addr.cast<uint8_t>();
  }
}

static remote_ptr<void> find_rr_vdso(Task* t, size_t* len) {
  RrVdso result;
  iterate_memory_map(t, find_rr_vdso_map, &result);
  ASSERT(t, result.start) << "rr VDSO not found?";
  *len = result.end - result.start;
  ASSERT(t, uint32_t(*len) == *len) << "VDSO more than 4GB???";
  return result.start;
}

static uint32_t find_offset_of_syscall_instruction_in(SupportedArch arch,
                                                      uint8_t* vdso_data,
                                                      size_t vdso_len) {
  auto instruction = syscall_instruction(arch);
  for (uint32_t i = 1; i < vdso_len - instruction.size(); ++i) {
    if (memcmp(vdso_data + i, instruction.data(), instruction.size()) == 0) {
      return i;
    }
  }
  return 0;
}

uint32_t AddressSpace::offset_to_syscall_in_vdso[SupportedArch_MAX + 1];

remote_ptr<uint8_t> AddressSpace::find_syscall_instruction(Task* t) {
  SupportedArch arch = t->arch();
  if (!offset_to_syscall_in_vdso[arch]) {
    auto vdso_data =
        t->read_mem(vdso().start.cast<uint8_t>(), vdso().num_bytes());
    offset_to_syscall_in_vdso[arch] = find_offset_of_syscall_instruction_in(
        arch, vdso_data.data(), vdso_data.size());
    ASSERT(t, offset_to_syscall_in_vdso[arch])
        << "No syscall instruction found in VDSO";
  }
  return vdso().start.cast<uint8_t>() + offset_to_syscall_in_vdso[arch];
}

static void write_rr_page(Task* t, ScopedFd& fd) {
  switch (t->arch()) {
    case x86: {
      static const uint8_t x86_data[] = { 0x90, 0x90, // padding
                                          // rr_page_untraced_syscall_ip:
                                          0xcd, 0x80, // int 0x80
                                          // rr_page_ip_in_untraced_syscall:
                                          0xc3, // ret
                                          0x90, 0x90, 0x90, 0x90, 0x90,
                                          0x90, 0x90, 0x90, 0x90, // padding
                                          // rr_page_traced_syscall_ip:
                                          0xcd, 0x80, // int 0x80
                                          // rr_page_ip_in_traced_syscall:
                                          0xc3 // ret
      };
      ASSERT(t, sizeof(x86_data) == write(fd, x86_data, sizeof(x86_data)));
      break;
    }
    case x86_64:
      static const uint8_t x86_64_data[] = {
        0x90, 0x90, // padding
        // rr_page_untraced_syscall_ip:
        0x0f, 0x05, // syscall
        // rr_page_ip_in_untraced_syscall:
        0x49, 0x81, 0xe3, 0xff,
        0xfe, 0xff, 0xff, // and $0xfffffffffffffeff,%r11
        0xc3,             // ret
        0x90, 0x90,       // padding
        // rr_page_traced_syscall_ip:
        0x0f, 0x05, // syscall
        // rr_page_ip_in_traced_syscall:
        0xc3 // ret
      };
      ASSERT(t, sizeof(x86_64_data) ==
                    write(fd, x86_64_data, sizeof(x86_64_data)));
      break;
  }
}

void AddressSpace::map_rr_page(Task* t) {
  // We initialize the rr page by creating a temporary file with the data
  // and mapping it PROT_READ | PROT_EXEC. There are simpler ways to do it,
  // but this is the only way allowed by PaX.
  char path[] = "/tmp/rr-page-XXXXXX";
  ScopedFd fd(mkstemp(path));
  ASSERT(t, fd.is_open());
  write_rr_page(t, fd);

  int prot = PROT_EXEC | PROT_READ;
  int flags = MAP_PRIVATE | MAP_FIXED;

  {
    AutoRemoteSyscalls remote(t);
    SupportedArch arch = t->arch();

    AutoRestoreMem child_path(remote, reinterpret_cast<uint8_t*>(path),
                              sizeof(path));
    int child_fd = remote.syscall(syscall_number_for_open(arch),
                                  child_path.get(), O_RDONLY);
    ASSERT(t, child_fd >= 0);

    auto result = remote.mmap_syscall(rr_page_start(), rr_page_size(), prot,
                                      flags, child_fd, 0);
    ASSERT(t, result == rr_page_start());

    remote.syscall(syscall_number_for_close(arch), child_fd);
  }

  unlink(path);

  struct stat stat_buf;
  ASSERT(t, fstat(fd, &stat_buf) == 0);
  map(rr_page_start(), rr_page_size(), prot, flags, 0,
      MappableResource(FileId(stat_buf), path));

  untraced_syscall_ip_ = rr_page_untraced_syscall_ip(t->arch());
  traced_syscall_ip_ = rr_page_traced_syscall_ip(t->arch());
}

void AddressSpace::post_exec_syscall(Task* t) {
  // First locate a syscall instruction we can use for remote syscalls.
  traced_syscall_ip_ = find_syscall_instruction(t);
  // Now remote syscalls work, we can open_mem_fd.
  t->open_mem_fd();
  // Now we can set up the "rr page" at its fixed address. This gives
  // us traced and untraced syscall instructions at known, fixed addresses.
  map_rr_page(t);
  monkeypatcher().patch_after_exec(t);
}

void AddressSpace::brk(remote_ptr<void> addr) {
  LOG(debug) << "brk(" << addr << ")";

  assert(heap.start <= addr);
  if (addr == heap.end) {
    return;
  }

  update_heap(heap.start, addr);
  map(heap.start, heap.num_bytes(), heap.prot, heap.flags, heap.offset,
      MappableResource::heap());
}

void AddressSpace::dump() const {
  fprintf(stderr, "  (heap: %p-%p)\n", (void*)heap.start.as_int(),
          (void*)heap.end.as_int());
  for (auto it = mem.begin(); it != mem.end(); ++it) {
    const Mapping& m = it->first;
    const MappableResource& r = it->second;
    fprintf(stderr, "%s %s\n", m.str().c_str(), r.str().c_str());
  }
}

TrapType AddressSpace::get_breakpoint_type_for_retired_insn(
    remote_ptr<uint8_t> ip) {
  remote_ptr<uint8_t> addr = ip - sizeof(breakpoint_insn);
  return get_breakpoint_type_at_addr(addr);
}

TrapType AddressSpace::get_breakpoint_type_at_addr(remote_ptr<uint8_t> addr) {
  auto it = breakpoints.find(addr);
  return it == breakpoints.end() ? TRAP_NONE : it->second->type();
}

void AddressSpace::map(remote_ptr<void> addr, size_t num_bytes, int prot,
                       int flags, off64_t offset_bytes,
                       const MappableResource& res) {
  LOG(debug) << "mmap(" << addr << ", " << num_bytes << ", " << HEX(prot)
             << ", " << HEX(flags) << ", " << HEX(offset_bytes);

  num_bytes = ceil_page_size(num_bytes);

  Mapping m(addr, num_bytes, prot, flags, offset_bytes);
  if (mem.end() != mem.find(m)) {
    // The mmap() man page doesn't specifically describe
    // what should happen if an existing map is
    // "overwritten" by a new map (of the same resource).
    // In testing, the behavior seems to be as if the
    // overlapping region is unmapped and then remapped
    // per the arguments to the second call.
    unmap(addr, num_bytes);
  }

  map_and_coalesce(m, res);

  if ((prot & PROT_EXEC) &&
      (res.fsname.find(SYSCALLBUF_LIB_FILENAME) != string::npos ||
       res.fsname.find(SYSCALLBUF_LIB_FILENAME_32) != string::npos)) {
    syscallbuf_lib_start_ = addr;
    syscallbuf_lib_end_ = addr + num_bytes;
  }
}

template <typename Arch> void AddressSpace::at_preload_init_arch(Task* t) {
  auto params = t->read_mem(
      remote_ptr<rrcall_init_preload_params<Arch> >(t->regs().arg1()));

  ASSERT(t, !t->session().as_record() ||
                t->session().as_record()->use_syscall_buffer() ==
                    params.syscallbuf_enabled)
      << "Tracee thinks syscallbuf is "
      << (params.syscallbuf_enabled ? "en" : "dis")
      << "abled, but tracer thinks "
      << (t->session().as_record()->use_syscall_buffer() ? "en" : "dis")
      << "abled";

  if (!params.syscallbuf_enabled) {
    return;
  }

  monkeypatch_state.patch_at_preload_init(t);
}

void AddressSpace::at_preload_init(Task* t) {
  ASSERT(t, syscallbuf_lib_start_)
      << "should have found preload library already";
  RR_ARCH_FUNCTION(at_preload_init_arch, t->arch(), t);
}

typedef AddressSpace::MemoryMap::value_type MappingResourcePair;
MappingResourcePair AddressSpace::mapping_of(remote_ptr<void> addr,
                                             size_t num_bytes) const {
  auto it = mem.find(Mapping(addr, num_bytes));
  assert(it != mem.end());
  // TODO callers assume [addr, addr + num_bytes] doesn't cross
  // resource boundaries
  assert(it->first.has_subset(Mapping(addr, num_bytes)));
  return *it;
}

/**
 * Return the offset of |m| into |r| updated by |delta|, unless |r| is
 * a pseudo-device that doesn't have offsets, in which case the
 * updated offset 0 is returned.
 */
static off64_t adjust_offset(const MappableResource& r, const Mapping& m,
                             off64_t delta) {
  return r.id.is_real_device() ? m.offset + delta : 0;
}

void AddressSpace::protect(remote_ptr<void> addr, size_t num_bytes, int prot) {
  LOG(debug) << "mprotect(" << addr << ", " << num_bytes << ", " << HEX(prot)
             << ")";

  Mapping last_overlap;
  auto protector = [this, prot, &last_overlap](
      const Mapping& m, const MappableResource& r, const Mapping& rem) {
    LOG(debug) << "  protecting (" << rem << ") ...";

    mem.erase(m);
    LOG(debug) << "  erased (" << m << ")";

    // If the first segment we protect underflows the
    // region, remap the underflow region with previous
    // prot.
    if (m.start < rem.start) {
      Mapping underflow(m.start, rem.start, m.prot, m.flags, m.offset);
      mem[underflow] = r;
    }
    // Remap the overlapping region with the new prot.
    remote_ptr<void> new_end = min(rem.end, m.end);
    Mapping overlap(rem.start, new_end, prot, m.flags,
                    adjust_offset(r, m, rem.start - m.start));
    mem[overlap] = r;
    last_overlap = overlap;

    // If the last segment we protect overflows the
    // region, remap the overflow region with previous
    // prot.
    if (rem.end < m.end) {
      Mapping overflow(rem.end, m.end, m.prot, m.flags,
                       adjust_offset(r, m, rem.end - m.start));
      mem[overflow] = r;
    }
  };
  for_each_in_range(addr, num_bytes, protector, ITERATE_CONTIGUOUS);
  // All mappings that we altered which might need coalescing
  // are adjacent to |last_overlap|.
  coalesce_around(mem.find(last_overlap));
}

void AddressSpace::remap(remote_ptr<void> old_addr, size_t old_num_bytes,
                         remote_ptr<void> new_addr, size_t new_num_bytes) {
  LOG(debug) << "mremap(" << old_addr << ", " << old_num_bytes << ", "
             << new_addr << ", " << new_num_bytes << ")";

  auto mr = mapping_of(old_addr, old_num_bytes);
  const Mapping& m = mr.first;
  const MappableResource& r = mr.second;

  unmap(old_addr, old_num_bytes);
  if (0 == new_num_bytes) {
    return;
  }

  map_and_coalesce(Mapping(new_addr, new_num_bytes, m.prot, m.flags,
                           adjust_offset(r, m, old_addr - m.start)),
                   r);
}

void AddressSpace::remove_breakpoint(remote_ptr<uint8_t> addr, TrapType type) {
  auto it = breakpoints.find(addr);
  if (it == breakpoints.end() || !it->second || it->second->unref(type) > 0) {
    return;
  }
  destroy_breakpoint(it);
}

bool AddressSpace::set_breakpoint(remote_ptr<uint8_t> addr, TrapType type) {
  auto it = breakpoints.find(addr);
  if (it == breakpoints.end()) {
    auto bp = Breakpoint::create();
    // Grab a random task from the VM so we can use its
    // read/write_mem() helpers.
    Task* t = *task_set().begin();
    if (sizeof(bp->overwritten_data) !=
        t->read_bytes_fallible(addr, sizeof(bp->overwritten_data),
                               &bp->overwritten_data)) {
      return false;
    }
    t->write_mem(addr, breakpoint_insn);

    auto it_and_is_new = breakpoints.insert(make_pair(addr, bp));
    assert(it_and_is_new.second);
    it = it_and_is_new.first;
  }
  it->second->ref(type);
  return true;
}

void AddressSpace::destroy_all_breakpoints() {
  while (!breakpoints.empty()) {
    destroy_breakpoint(breakpoints.begin());
  }
}

void AddressSpace::remove_watchpoint(remote_ptr<void> addr, size_t num_bytes,
                                     WatchType type) {
  auto it = watchpoints.find(MemoryRange(addr, num_bytes));
  if (it != watchpoints.end() &&
      0 == it->second->unwatch(access_bits_of(type))) {
    watchpoints.erase(it);
  }
  allocate_watchpoints();
}

bool AddressSpace::set_watchpoint(remote_ptr<void> addr, size_t num_bytes,
                                  WatchType type) {
  MemoryRange key(addr, num_bytes);
  auto it = watchpoints.find(key);
  if (it == watchpoints.end()) {
    auto it_and_is_new =
        watchpoints.insert(make_pair(key, Watchpoint::create()));
    assert(it_and_is_new.second);
    it = it_and_is_new.first;
  }
  it->second->watch(access_bits_of(type));
  return allocate_watchpoints();
}

void AddressSpace::destroy_all_watchpoints() {
  watchpoints.clear();
  allocate_watchpoints();
}

void AddressSpace::unmap(remote_ptr<void> addr, ssize_t num_bytes) {
  LOG(debug) << "munmap(" << addr << ", " << num_bytes << ")";

  auto unmapper = [this](const Mapping& m, const MappableResource& r,
                         const Mapping& rem) {
    LOG(debug) << "  unmapping (" << rem << ") ...";

    mem.erase(m);
    LOG(debug) << "  erased (" << m << ") ...";

    // If the first segment we unmap underflows the unmap
    // region, remap the underflow region.
    if (m.start < rem.start) {
      Mapping underflow(m.start, rem.start, m.prot, m.flags, m.offset);
      mem[underflow] = r;
    }
    // If the last segment we unmap overflows the unmap
    // region, remap the overflow region.
    if (rem.end < m.end) {
      Mapping overflow(rem.end, m.end, m.prot, m.flags,
                       adjust_offset(r, m, rem.end - m.start));
      mem[overflow] = r;
    }
  };
  for_each_in_range(addr, num_bytes, unmapper);
}

/**
 * Return true iff |left| and |right| are located adjacently in memory
 * with the same metadata, and map adjacent locations of the same
 * underlying (real) device.
 */
static bool is_adjacent_mapping(const MappingResourcePair& left,
                                const MappingResourcePair& right) {
  const Mapping& mleft = left.first;
  const Mapping& mright = right.first;
  if (mleft.end != mright.start) {
    LOG(debug) << "    (not adjacent in memory)";
    return false;
  }
  if (mleft.flags != mright.flags || mleft.prot != mright.prot) {
    LOG(debug) << "    (flags or prot differ)";
    return false;
  }
  const MappableResource& rleft = left.second;
  const MappableResource& rright = right.second;
  if (rright.fsname.substr(0, strlen(PREFIX_FOR_EMPTY_MMAPED_REGIONS)) ==
      PREFIX_FOR_EMPTY_MMAPED_REGIONS) {
    return true;
  }
  if (rleft != rright) {
    LOG(debug) << "    (not the same resource)";
    return false;
  }
  if (rleft.id.is_real_device() &&
      mleft.offset + off64_t(mleft.num_bytes()) != mright.offset) {
    LOG(debug) << "    (" << mleft.offset << " + " << mleft.num_bytes()
               << " != " << mright.offset
               << ": offsets into real device aren't adjacent)";
    return false;
  }
  LOG(debug) << "    adjacent!";
  return true;
}

/**
 * If (*left_m, left_r), (right_m, right_r) are adjacent (see
 * |is_adjacent_mapping()|), write a merged segment descriptor to
 * |*left_m| and return true.  Otherwise return false.
 */
static bool try_merge_adjacent(Mapping* left_m, const MappableResource& left_r,
                               const Mapping& right_m,
                               const MappableResource& right_r) {
  if (is_adjacent_mapping(MappingResourcePair(*left_m, left_r),
                          MappingResourcePair(right_m, right_r))) {
    *left_m = Mapping(left_m->start, right_m.end, right_m.prot, right_m.flags,
                      left_m->offset);
    return true;
  }
  return false;
}

/**
 * Iterate over /proc/maps segments for a task and verify that the
 * task's cached mapping matches the kernel's (given a lenient fuzz
 * factor).
 */
struct VerifyAddressSpace {
  typedef AddressSpace::MemoryMap::const_iterator const_iterator;

  VerifyAddressSpace(const AddressSpace* as)
      : as(as), it(as->mem.begin()), phase(NO_PHASE) {}

  /**
   * |km| and |m| are the same mapping of the same resource, or
   * don't return.
   */
  void assert_segments_match(Task* t);

  /* Current kernel Mapping we're merging and trying to
   * match. */
  Mapping km;
  /* Current cached Mapping we've merged and are trying to
   * match. */
  Mapping m;
  /* The resource that |km| and |m| map. */
  MappableResource r;
  const AddressSpace* as;
  /* Iterator over mappings in |as|. */
  const_iterator it;
  /* Which mapping-checking phase we're in.  See below. */
  enum {
    NO_PHASE,
    MERGING_CACHED,
    INITING_KERNEL,
    MERGING_KERNEL
  } phase;
};

void VerifyAddressSpace::assert_segments_match(Task* t) {
  assert(MERGING_KERNEL == phase);
  bool same_mapping = (m.start == km.start && m.end == km.end &&
                       m.prot == km.prot && m.flags == km.flags);
  if (!same_mapping) {
    LOG(error) << "cached mmap:";
    as->dump();
    LOG(error) << "/proc/" << t->tid << "/mmaps:";
    print_process_mmap(t);

    ASSERT(t, same_mapping) << "\nCached mapping " << m << " should be " << km;
  }
}

/**
 * Iterate over the segments that are parsed from
 * |/proc/[t->tid]/maps| and ensure that they match up with the cached
 * segments for |t|.
 *
 * This implementation does the following
 *  1. Merge as many adjacent cached mappings as it can.
 *  2. Merge as many adjacent /proc/maps mappings as it can.
 *  3. Ensure that the two merged mappings are the same.
 *  4. Move on to the next mapping region, goto 1.
 *
 * There are two subtleties of this implementation.  The first is that
 * the kernel and rr have (only very slightly! argh) different
 * heuristics for merging adjacent memory mappings.  That means we
 * can't simply iterate through /proc/maps and assert that a cached
 * mapping corresponds to it, though we sure would like to.  Instead,
 * we reduce the rr mappings to the lowest common denonminator that
 * can be parsed from /proc/maps, and assume that adjacent mappings
 * should be merged if they're equal per common lax criteria (i.e.,
 * not honoring either rr or kernel criteria).  That means that the
 * mapped segments that this helper compares may look nothing like the
 * segments you would see in a /proc/maps dump or |as->dump()|.
 *
 * The second subtlety is that rr's /proc/maps iterator uses a C-style
 * callback iterator, whereas the cached map iterator uses a C++
 * iterator in a loop.  That means we have to do a bit of fancy
 * footwork to make the two styles iterate over the same mappings.
 * Since C++ iterators are more flexible, we do the C++ iteration
 * first, and then force a state machine to make the matchin required
 * C-iterator calls.
 *
 * TODO: replace iterate_memory_map()
 */
/*static*/ void AddressSpace::check_segment_iterator(
    void* pvas, Task* t, const struct map_iterator_data* data) {
  VerifyAddressSpace* vas = static_cast<VerifyAddressSpace*>(pvas);
  const AddressSpace* as = vas->as;
  const struct mapped_segment_info& info = data->info;

  LOG(debug) << "examining /proc/maps segment " << info;

  // Merge adjacent cached mappings.
  if (vas->NO_PHASE == vas->phase) {
    assert(vas->it != as->mem.end());

    vas->phase = vas->MERGING_CACHED;
    // Start of next segment range to match.
    vas->m = vas->it->first.to_kernel();
    vas->r = vas->it->second.to_kernel();
    do {
      ++vas->it;
    } while (vas->it != as->mem.end() &&
             try_merge_adjacent(&vas->m, vas->r, vas->it->first.to_kernel(),
                                vas->it->second.to_kernel()));
    vas->phase = vas->INITING_KERNEL;
  }

  LOG(debug) << "  merged cached seg: " << vas->m;

  // Merge adjacent kernel mappings.
  assert(info.flags == (info.flags & Mapping::checkable_flags_mask));
  Mapping km(info.start_addr, info.end_addr, info.prot, info.flags,
             info.file_offset);
  MappableResource kr(FileId(info.dev_major, info.dev_minor, info.inode),
                      info.name);

  if (vas->INITING_KERNEL == vas->phase) {
    assert(kr == vas->r
                 // XXX not-so-pretty hack.  If the mapped file
                 // lives in our replayer's emulated fs, then it
                 // will have a real system device/inode
                 // descriptor.  We /could/ initialize the
                 // MappableResource with that descriptor, but
                 // we rely on quick access to the recorded
                 // (i.e. emulated in replay) device/inode for
                 // gc.  So this suffices for now.
           ||
           string::npos != kr.fsname.find(SHMEM_FS "/rr-emufs") ||
           string::npos != kr.fsname.find(SHMEM_FS2 "/rr-emufs"));
    vas->km = km;
    vas->phase = vas->MERGING_KERNEL;
    return;
  }
  if (vas->MERGING_KERNEL == vas->phase &&
      try_merge_adjacent(&vas->km, vas->r, km, kr)) {
    return;
  }

  // Merged as much as we can ... now the mappings must be
  // equal.
  vas->assert_segments_match(t);

  vas->phase = vas->NO_PHASE;
  check_segment_iterator(pvas, t, data);
}

Mapping AddressSpace::vdso() const {
  assert(!vdso_start_addr.is_null());
  return mapping_of(vdso_start_addr, 1).first;
}

void AddressSpace::verify(Task* t) const {
  assert(task_set().end() != task_set().find(t));

  VerifyAddressSpace vas(this);
  iterate_memory_map(t, check_segment_iterator, &vas);

  assert(vas.MERGING_KERNEL == vas.phase);
  vas.assert_segments_match(t);
}

AddressSpace::AddressSpace(Task* t, const string& exe, Session& session)
    : exe(exe), is_clone(false), session(&session), child_mem_fd(-1) {
  // TODO: this is a workaround of
  // https://github.com/mozilla/rr/issues/1113 .
  if (session.can_validate()) {
    iterate_memory_map(t, populate_address_space, this);
    assert(!vdso_start_addr.is_null());
  } else {
    // Find the location of the VDSO in the just-spawned process. This will
    // match the VDSO in rr itself since we haven't execed yet. So, speed
    // things up by search rr's own VDSO for a syscall instruction.
    size_t rr_vdso_len;
    remote_ptr<void> rr_vdso = find_rr_vdso(t, &rr_vdso_len);
    // Here we rely on the VDSO location in the spawned tracee being the same
    // as in rr itself.
    uint8_t* local_vdso = reinterpret_cast<uint8_t*>(rr_vdso.as_int());
    auto offset = find_offset_of_syscall_instruction_in(
        NativeArch::arch(), local_vdso, rr_vdso_len);
    offset_to_syscall_in_vdso[NativeArch::arch()] = offset;
    // Setup traced_syscall_ip_ now because we need to do AutoRemoteSyscalls
    // (for open_mem_fd) before the first exec.
    traced_syscall_ip_ = rr_vdso.cast<uint8_t>() + offset;
  }
}

AddressSpace::AddressSpace(const AddressSpace& o)
    // Whether the new VM wants our breakpoints our not,
    // it's going to inherit them.  This is pretty much
    // never what anyone wants, so a call to
    // |remove_all_breakpoints()| is expected soon after
    // the creation of this.
    : breakpoints(o.breakpoints),
      exe(o.exe),
      heap(o.heap),
      is_clone(true),
      mem(o.mem),
      session(nullptr),
      vdso_start_addr(o.vdso_start_addr),
      monkeypatch_state(o.monkeypatch_state),
      traced_syscall_ip_(o.traced_syscall_ip_),
      untraced_syscall_ip_(o.untraced_syscall_ip_),
      syscallbuf_lib_start_(o.syscallbuf_lib_start_),
      syscallbuf_lib_end_(o.syscallbuf_lib_end_) {
  for (auto it = breakpoints.begin(); it != breakpoints.end(); ++it) {
    it->second = it->second->clone();
  }
}

bool AddressSpace::allocate_watchpoints() {
  Task::DebugRegs regs;
  for (auto kv : watchpoints) {
    const MemoryRange& r = kv.first;
    int watching = kv.second->watched_bits();
    if (EXEC_BIT & watching) {
      regs.push_back(WatchConfig(r.addr, r.num_bytes, WATCH_EXEC));
    }
    if (!(READ_BIT & watching) && (WRITE_BIT & watching)) {
      regs.push_back(WatchConfig(r.addr, r.num_bytes, WATCH_WRITE));
    }
    if (READ_BIT & watching) {
      regs.push_back(WatchConfig(r.addr, r.num_bytes, WATCH_READWRITE));
    }
  }
  for (auto t : task_set()) {
    if (!t->set_debug_regs(regs)) {
      return false;
    }
  }
  return true;
}

void AddressSpace::coalesce_around(MemoryMap::iterator it) {
  Mapping m = it->first;
  MappableResource r = it->second;

  auto first_kv = it;
  while (mem.begin() != first_kv) {
    auto next = first_kv;
    if (!is_adjacent_mapping(*--first_kv, *next)) {
      first_kv = next;
      break;
    }
  }
  auto last_kv = it;
  while (true) {
    auto prev = last_kv;
    if (mem.end() == ++last_kv || !is_adjacent_mapping(*prev, *last_kv)) {
      last_kv = prev;
      break;
    }
  }
  assert(last_kv != mem.end());
  if (first_kv == last_kv) {
    LOG(debug) << "  no mappings to coalesce";
    return;
  }

  Mapping c(first_kv->first.start, last_kv->first.end, m.prot, m.flags,
            first_kv->first.offset);
  LOG(debug) << "  coalescing " << c;

  mem.erase(first_kv, ++last_kv);

  auto ins = mem.insert(MemoryMap::value_type(c, r));
  assert(ins.second); // key didn't already exist
}

void AddressSpace::destroy_breakpoint(BreakpointMap::const_iterator it) {
  Task* t = *task_set().begin();
  t->write_mem(it->first, it->second->overwritten_data);
  breakpoints.erase(it);
}

void AddressSpace::for_each_in_range(
    remote_ptr<void> addr, ssize_t num_bytes,
    function<void(const Mapping& m, const MappableResource& r,
                  const Mapping& rem)> f,
    int how) {
  num_bytes = ceil_page_size(num_bytes);
  remote_ptr<void> last_unmapped_end = addr;
  remote_ptr<void> region_end = addr + num_bytes;
  while (last_unmapped_end < region_end) {
    // Invariant: |rem| is always exactly the region of
    // memory remaining to be examined for pages to be
    // unmapped.
    Mapping rem(last_unmapped_end, region_end);

    // The next page to iterate may not be contiguous with
    // the last one seen.
    auto it = mem.lower_bound(rem);
    if (mem.end() == it) {
      LOG(debug) << "  not found, done.";
      return;
    }

    Mapping m = it->first;
    if (rem.end <= m.start) {
      LOG(debug) << "  mapping at " << m.start << " out of range, done.";
      return;
    }
    if (ITERATE_CONTIGUOUS == how &&
        !(m.start < addr || rem.start == m.start)) {
      LOG(debug) << "  discontiguous mapping at " << m.start << ", done.";
      return;
    }

    MappableResource r = it->second;
    f(m, r, rem);

    // Maintain the loop invariant.
    last_unmapped_end = m.end;
  }
}

void AddressSpace::map_and_coalesce(const Mapping& m,
                                    const MappableResource& r) {
  LOG(debug) << "  mapping " << m;

  auto ins = mem.insert(MemoryMap::value_type(m, r));
  assert(ins.second); // key didn't already exist
  coalesce_around(ins.first);
}

/*static*/ void AddressSpace::populate_address_space(
    void* asp, Task* t, const struct map_iterator_data* data) {
  AddressSpace* as = static_cast<AddressSpace*>(asp);
  const struct mapped_segment_info& info = data->info;

  if (!as->heap.start && as->exe == info.name && !(info.prot & PROT_EXEC) &&
      (info.prot & (PROT_READ | PROT_WRITE))) {
    as->update_heap(info.end_addr, info.end_addr);
    LOG(debug) << "  guessing heap starts at " << as->heap.start
               << " (end of text segment)";
  }

  bool is_dynamic_heap = "[heap]" == info.name;
  // This segment is adjacent to our previous guess at the start of
  // the dynamic heap, but it's still not an explicit heap segment.
  // Or, in corner cases, the segment is the final mapping of the data
  // segment of the exe image, but is not adjacent to the prior mapped
  // segment of the exe.  (This is seen with x86-64 bash on Fedora
  // Core 20.)  Update the guess.
  if (!(info.prot & PROT_EXEC)
      && (as->heap.end == info.start_addr || as->exe == info.name)) {
    assert(as->heap.start == as->heap.end || as->exe == info.name);
    as->update_heap(info.end_addr, info.end_addr);
    LOG(debug) << "  updating start-of-heap guess to " << as->heap.start
               << " (end of mapped-data segment)";
  }

  FileId id;
  if (is_dynamic_heap) {
    id = FileId(PSEUDODEVICE_HEAP);
    if (!as->heap.start) {
      // No guess for the heap start. Assume it's just the [heap] segment.
      as->update_heap(info.start_addr, info.end_addr);
    } else {
      as->update_heap(as->heap.start, info.end_addr);
    }
  } else if ("[stack]" == info.name) {
    id = FileId(PSEUDODEVICE_STACK);
  } else if ("[vdso]" == info.name) {
    assert(!as->vdso_start_addr);
    as->vdso_start_addr = info.start_addr;
    id = FileId(PSEUDODEVICE_VDSO);
  } else {
    id = FileId(MKDEV(info.dev_major, info.dev_minor), info.inode);
  }

  as->map(info.start_addr, info.end_addr - info.start_addr, info.prot,
          info.flags, info.file_offset, MappableResource(id, info.name));
}
