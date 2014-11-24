/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "Util"

//#define FIRST_INTERESTING_EVENT 10700
//#define LAST_INTERESTING_EVENT 10900

#include "util.h"

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/magic.h>
#include <string.h>
#include <stdlib.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "preload/preload_interface.h"

#include "Flags.h"
#include "kernel_abi.h"
#include "kernel_metadata.h"
#include "log.h"
#include "ReplaySession.h"
#include "task.h"
#include "TraceStream.h"

using namespace std;
using namespace rr;

// FIXME this function assumes that there's only one address space.
// Should instead only look at the address space of the task in
// question.
static bool is_start_of_scratch_region(Task* t, remote_ptr<void> start_addr) {
  for (auto& kv : t->session().tasks()) {
    Task* c = kv.second;
    if (start_addr == c->scratch_ptr) {
      return true;
    }
  }
  return false;
}

bool probably_not_interactive(int fd) {
  /* Eminently tunable heuristic, but this is guaranteed to be
   * true during rr unit tests, where we care most about this
   * check (to a first degree).  A failing test shouldn't
   * hang. */
  return !isatty(fd);
}

void maybe_mark_stdio_write(Task* t, int fd) {
  char buf[256];
  ssize_t len;

  if (!Flags::get().mark_stdio ||
      !(STDOUT_FILENO == fd || STDERR_FILENO == fd)) {
    return;
  }
  snprintf(buf, sizeof(buf) - 1, "[rr %d %d]", t->tgid(), t->trace_time());
  len = strlen(buf);
  if (write(fd, buf, len) != len) {
    FATAL() << "Couldn't write to " << fd;
  }
}

int clone_flags_to_task_flags(int flags_arg) {
  int flags = CLONE_SHARE_NOTHING;
  // See task.h for description of the flags.
  flags |= (CLONE_CHILD_CLEARTID & flags_arg) ? CLONE_CLEARTID : 0;
  flags |= (CLONE_SETTLS & flags_arg) ? CLONE_SET_TLS : 0;
  flags |= (CLONE_SIGHAND & flags_arg) ? CLONE_SHARE_SIGHANDLERS : 0;
  flags |= (CLONE_THREAD & flags_arg) ? CLONE_SHARE_TASK_GROUP : 0;
  flags |= (CLONE_VM & flags_arg) ? CLONE_SHARE_VM : 0;
  return flags;
}

size_t page_size() { return sysconf(_SC_PAGE_SIZE); }

size_t ceil_page_size(size_t sz) {
  size_t page_mask = ~(page_size() - 1);
  return (sz + page_size() - 1) & page_mask;
}

remote_ptr<void> ceil_page_size(remote_ptr<void> addr) {
  return remote_ptr<void>(ceil_page_size(addr.as_int()));
}

/**
 * Dump |buf_len| words in |buf| to |out|, starting with a line
 * containing |label|.  See |dump_binary_data()| for a description of
 * the remaining parameters.
 */
static void dump_binary_chunk(FILE* out, const char* label, const uint32_t* buf,
                              size_t buf_len, remote_ptr<void> start_addr) {
  int i;

  fprintf(out, "%s\n", label);
  for (i = 0; i < ssize_t(buf_len); i += 1) {
    uint32_t word = buf[i];
    fprintf(out, "0x%08x | [%p]\n", word,
            (void*)(start_addr.as_int() + i * sizeof(*buf)));
  }
}

void dump_binary_data(const char* filename, const char* label,
                      const uint32_t* buf, size_t buf_len,
                      remote_ptr<void> start_addr) {
  FILE* out = fopen64(filename, "w");
  if (!out) {
    return;
  }
  dump_binary_chunk(out, label, buf, buf_len, start_addr);
  fclose(out);
}

void format_dump_filename(Task* t, int global_time, const char* tag,
                          char* filename, size_t filename_size) {
  snprintf(filename, filename_size - 1, "%s/%d_%d_%s", t->trace_dir().c_str(),
           t->rec_tid, global_time, tag);
}

bool should_dump_memory(Task* t, const TraceFrame& f) {
  const Flags* flags = &Flags::get();

#if defined(FIRST_INTERESTING_EVENT)
  int is_syscall_exit = event >= 0 && state == STATE_SYSCALL_EXIT;
  if (is_syscall_exit && RECORD == Flags->option &&
      FIRST_INTERESTING_EVENT <= global_time &&
      global_time <= LAST_INTERESTING_EVENT) {
    return true;
  }
  if (global_time > LAST_INTERESTING_EVENT) {
    return false;
  }
#endif
  return flags->dump_on == Flags::DUMP_ON_ALL ||
         flags->dump_at == int(f.time());
}

void dump_process_memory(Task* t, int global_time, const char* tag) {
  char filename[PATH_MAX];
  FILE* dump_file;

  format_dump_filename(t, global_time, tag, filename, sizeof(filename));
  dump_file = fopen64(filename, "w");

  const AddressSpace& as = *(t->vm());
  for (auto& kv : as.memmap()) {
    const Mapping& first = kv.first;
    const MappableResource& second = kv.second;
    vector<uint8_t> mem;
    mem.resize(first.num_bytes());

    ssize_t mem_len =
        t->read_bytes_fallible(first.start, first.num_bytes(), mem.data());
    mem_len = max(ssize_t(0), mem_len);

    string label = first.str() + ' ' + second.str();

    if (!is_start_of_scratch_region(t, first.start)) {
      dump_binary_chunk(dump_file, label.c_str(), (const uint32_t*)mem.data(),
                        mem_len / sizeof(uint32_t), first.start);
    }
  }
  fclose(dump_file);
}

static void notify_checksum_error(Task* t, int global_time, unsigned checksum,
                                  unsigned rec_checksum,
                                  const string& raw_map_line) {
  char cur_dump[PATH_MAX];
  char rec_dump[PATH_MAX];

  dump_process_memory(t, global_time, "checksum_error");

  /* TODO: if the right recorder memory dump is present,
   * automatically compare them, taking the oddball
   * not-mapped-during-replay region(s) into account.  And if
   * not present, tell the user how to make one in a future
   * run. */
  format_dump_filename(t, global_time, "checksum_error", cur_dump,
                       sizeof(cur_dump));
  format_dump_filename(t, global_time, "rec", rec_dump, sizeof(rec_dump));

  Event ev(t->current_trace_frame().event());
  ASSERT(t, checksum == rec_checksum)
      << "Divergence in contents of memory segment after '" << ev << "':\n"
                                                                     "\n"
      << raw_map_line << "    (recorded checksum:" << HEX(rec_checksum)
      << "; replaying checksum:" << HEX(checksum) << ")\n"
                                                     "\n"
      << "Dumped current memory contents to " << cur_dump
      << ". If you've created a memory dump for\n"
      << "the '" << ev << "' event (line " << t->trace_time()
      << ") during recording by using, for example with\n"
      << "the args\n"
         "\n"
      << "$ rr --dump-at=" << t->trace_time() << " record ...\n"
                                                 "\n"
      << "then you can use the following to determine which memory cells "
         "differ:\n"
         "\n"
      << "$ diff -u " << rec_dump << " " << cur_dump << " > mem-diverge.diff\n";
}

/**
 * This helper does the heavy lifting of storing or validating
 * checksums.  The iterator data determines which behavior the helper
 * function takes on, and to/from which file it writes/read.
 */
enum ChecksumMode {
  STORE_CHECKSUMS,
  VALIDATE_CHECKSUMS
};
struct checksum_iterator_data {
  ChecksumMode mode;
  FILE* checksums_file;
  int global_time;
};

static bool checksum_segment_filter(const Mapping& m,
                                    const MappableResource& r) {
  struct stat st;
  int may_diverge;

  if (stat(r.fsname.c_str(), &st)) {
    /* If there's no persistent resource backing this
     * mapping, we should expect it to change. */
    LOG(debug) << "CHECKSUMMING unlinked '" << r.fsname << "'";
    return true;
  }
  /* If we're pretty sure the backing resource is effectively
   * immutable, skip checksumming, it's a waste of time.  Except
   * if the mapping is mutable, for example the rw data segment
   * of a system library, then it's interesting. */
  may_diverge = (should_copy_mmap_region(r.fsname.c_str(), &st, m.prot, m.flags,
                                         DONT_WARN_SHARED_WRITEABLE) ||
                 (PROT_WRITE & m.prot));
  LOG(debug) << (may_diverge ? "CHECKSUMMING" : "  skipping") << " '"
             << r.fsname << "'";
  return may_diverge;
}

/**
 * Either create and store checksums for each segment mapped in |t|'s
 * address space, or validate an existing computed checksum.  Behavior
 * is selected by |mode|.
 */
static void iterate_checksums(Task* t, ChecksumMode mode, int global_time) {
  struct checksum_iterator_data c;
  memset(&c, 0, sizeof(c));
  char filename[PATH_MAX];
  const char* fmode = (STORE_CHECKSUMS == mode) ? "w" : "r";

  c.mode = mode;
  snprintf(filename, sizeof(filename) - 1, "%s/%d_%d", t->trace_dir().c_str(),
           global_time, t->rec_tid);
  c.checksums_file = fopen64(filename, fmode);
  c.global_time = global_time;
  if (!c.checksums_file) {
    FATAL() << "Failed to open checksum file " << filename;
  }

  const AddressSpace& as = *(t->vm());
  for (auto& kv : as.memmap()) {
    const Mapping& first = kv.first;
    const MappableResource& second = kv.second;

    vector<uint8_t> mem;
    ssize_t valid_mem_len = 0;

    if (checksum_segment_filter(first, second)) {
      mem.resize(first.num_bytes());
      valid_mem_len =
          t->read_bytes_fallible(first.start, first.num_bytes(), mem.data());
      valid_mem_len = max(ssize_t(0), valid_mem_len);
    }

    unsigned* buf = (unsigned*)mem.data();
    unsigned checksum = 0;
    int i;

    if (second.fsname.find(SYSCALLBUF_SHMEM_PATH_PREFIX) != string::npos) {
      /* The syscallbuf consists of a region that's written
      * deterministically wrt the trace events, and a
      * region that's written nondeterministically in the
      * same way as trace scratch buffers.  The
      * deterministic region comprises committed syscallbuf
      * records, and possibly the one pending record
      * metadata.  The nondeterministic region starts at
      * the "extra data" for the possibly one pending
      * record.
      *
      * So here, we set things up so that we only checksum
      * the deterministic region. */
      auto child_hdr = first.start.cast<struct syscallbuf_hdr>();
      auto hdr = t->read_mem(child_hdr);
      valid_mem_len = !buf ? 0 : sizeof(hdr) + hdr.num_rec_bytes +
                                     sizeof(struct syscallbuf_record);
    }

    ASSERT(t, buf || valid_mem_len == 0);
    for (i = 0; i < ssize_t(valid_mem_len / sizeof(*buf)); ++i) {
      checksum += buf[i];
    }

    string raw_map_line = first.str() + ' ' + second.str();
    if (STORE_CHECKSUMS == c.mode) {
      fprintf(c.checksums_file, "(%x) %s\n", checksum, raw_map_line.c_str());
    } else {
      char line[1024];
      unsigned rec_checksum;
      unsigned long rec_start;
      unsigned long rec_end;
      int nparsed;

      fgets(line, sizeof(line), c.checksums_file);
      nparsed =
          sscanf(line, "(%x) %lx-%lx", &rec_checksum, &rec_start, &rec_end);
      remote_ptr<void> rec_start_addr = rec_start;
      remote_ptr<void> rec_end_addr = rec_end;
      ASSERT(t, 3 == nparsed) << "Only parsed " << nparsed << " items";

      ASSERT(t, rec_start_addr == first.start && rec_end_addr == first.end)
          << "Segment " << rec_start_addr << "-" << rec_end_addr
          << " changed to " << first << "??";

      if (is_start_of_scratch_region(t, rec_start_addr)) {
        /* Replay doesn't touch scratch regions, so
         * their contents are allowed to diverge.
         * Tracees can't observe those segments unless
         * they do something sneaky (or disastrously
         * buggy). */
        LOG(debug) << "Not validating scratch starting at 0x" << hex
                   << rec_start_addr << dec;
        continue;
      }
      if (checksum != rec_checksum) {
        notify_checksum_error(t, c.global_time, checksum, rec_checksum,
                              raw_map_line.c_str());
      }
    }
  }

  fclose(c.checksums_file);
}

bool should_checksum(Task* t, const TraceFrame& f) {
  int checksum = Flags::get().checksum;
  bool is_syscall_exit =
      EV_SYSCALL == f.event().type && SYSCALL_EXIT == f.event().state;

#if defined(FIRST_INTERESTING_EVENT)
  if (is_syscall_exit && FIRST_INTERESTING_EVENT <= global_time &&
      global_time <= LAST_INTERESTING_EVENT) {
    return true;
  }
  if (global_time > LAST_INTERESTING_EVENT) {
    return false;
  }
#endif
  if (Flags::CHECKSUM_NONE == checksum) {
    return false;
  }
  if (Flags::CHECKSUM_ALL == checksum) {
    return true;
  }
  if (Flags::CHECKSUM_SYSCALL == checksum) {
    return is_syscall_exit;
  }
  /* |checksum| is a global time point. */
  return checksum <= int(f.time());
}

void checksum_process_memory(Task* t, int global_time) {
  iterate_checksums(t, STORE_CHECKSUMS, global_time);
}

void validate_process_memory(Task* t, int global_time) {
  iterate_checksums(t, VALIDATE_CHECKSUMS, global_time);
}

signal_action default_action(int sig) {
  if (SIGRTMIN <= sig && sig <= SIGRTMAX) {
    return TERMINATE;
  }
  switch (sig) {
/* TODO: SSoT for signal defs/semantics. */
#define CASE(_sig, _act)                                                       \
  case SIG##_sig:                                                              \
    return _act
    CASE(HUP, TERMINATE);
    CASE(INT, TERMINATE);
    CASE(QUIT, DUMP_CORE);
    CASE(ILL, DUMP_CORE);
    CASE(ABRT, DUMP_CORE);
    CASE(FPE, DUMP_CORE);
    CASE(KILL, TERMINATE);
    CASE(SEGV, DUMP_CORE);
    CASE(PIPE, TERMINATE);
    CASE(ALRM, TERMINATE);
    CASE(TERM, TERMINATE);
    CASE(USR1, TERMINATE);
    CASE(USR2, TERMINATE);
    CASE(CHLD, IGNORE);
    CASE(CONT, CONTINUE);
    CASE(STOP, STOP);
    CASE(TSTP, STOP);
    CASE(TTIN, STOP);
    CASE(TTOU, STOP);
    CASE(BUS, DUMP_CORE);
    /*CASE(POLL, TERMINATE);*/
    CASE(PROF, TERMINATE);
    CASE(SYS, DUMP_CORE);
    CASE(TRAP, DUMP_CORE);
    CASE(URG, IGNORE);
    CASE(VTALRM, TERMINATE);
    CASE(XCPU, DUMP_CORE);
    CASE(XFSZ, DUMP_CORE);
    /*CASE(IOT, DUMP_CORE);*/
    /*CASE(EMT, TERMINATE);*/
    CASE(STKFLT, TERMINATE);
    CASE(IO, TERMINATE);
    CASE(PWR, TERMINATE);
    /*CASE(LOST, TERMINATE);*/
    CASE(WINCH, IGNORE);
    default:
      FATAL() << "Unknown signal " << sig;
      return TERMINATE; // not reached
#undef CASE
  }
}

bool possibly_destabilizing_signal(Task* t, int sig,
                                   SignalDeterministic deterministic) {
  signal_action action = default_action(sig);
  if (action != DUMP_CORE && action != TERMINATE) {
    // If the default action doesn't kill the process, it won't die.
    return false;
  }

  if (t->is_sig_ignored(sig)) {
    // Deterministic fatal signals can't be ignored.
    return deterministic == DETERMINISTIC_SIG;
  }
  if (!t->signal_has_user_handler(sig)) {
    // The default action is going to happen: killing the process.
    return true;
  }
  // If the signal's blocked, user handlers aren't going to run and the process
  // will die.
  return t->is_sig_blocked(sig);
}

static bool has_fs_name(const char* path) {
  struct stat dummy;
  return 0 == stat(path, &dummy);
}

static bool is_tmp_file(const char* path) {
  struct statfs sfs;
  statfs(path, &sfs);
  return (TMPFS_MAGIC == sfs.f_type
                         // In observed configurations of Ubuntu 13.10, /tmp is
                         // a folder in the / fs, not a separate tmpfs.
          ||
          path == strstr(path, "/tmp/"));
}

bool should_copy_mmap_region(const char* filename, const struct stat* stat,
                             int prot, int flags, int warn_shared_writeable) {
  bool private_mapping = (flags & MAP_PRIVATE);

  // TODO: handle mmap'd files that are unlinked during
  // recording.
  if (!has_fs_name(filename)) {
    LOG(debug) << "  copying unlinked file";
    return true;
  }
  if (is_tmp_file(filename)) {
    LOG(debug) << "  copying file on tmpfs";
    return true;
  }
  if (private_mapping && (prot & PROT_EXEC)) {
    /* We currently don't record the images that we
     * exec(). Since we're being optimistic there (*cough*
     * *cough*), we're doing no worse (in theory) by being
     * optimistic about the shared libraries too, most of
     * which are system libraries. */
    LOG(debug) << "  (no copy for +x private mapping " << filename << ")";
    return false;
  }
  if (private_mapping && (0111 & stat->st_mode)) {
    /* A private mapping of an executable file usually
     * indicates mapping data sections of object files.
     * Since we're already assuming those change very
     * infrequently, we can avoid copying the data
     * sections too. */
    LOG(debug) << "  (no copy for private mapping of +x " << filename << ")";
    return false;
  }

  // TODO: using "can the euid of the rr process write this
  // file" as an approximation of whether the tracee can write
  // the file.  If the tracee is messing around with
  // set*[gu]id(), the real answer may be different.
  bool can_write_file = (0 == access(filename, W_OK));

  if (!can_write_file && 0 == stat->st_uid) {
    // We would like to assert this, but on Ubuntu 13.10,
    // the file /lib/i386-linux-gnu/libdl-2.17.so is
    // writeable by root for unknown reasons.
    // assert(!(prot & PROT_WRITE));
    /* Mapping a file owned by root: we don't care if this
     * was a PRIVATE or SHARED mapping, because unless the
     * program is disastrously buggy or unlucky, the
     * mapping is effectively PRIVATE.  Bad luck can come
     * from this program running during a system update,
     * or a user being added, which is probably less
     * frequent than even system updates.
     *
     * XXX what about the fontconfig cache files? */
    LOG(debug) << "  (no copy for root-owned " << filename << ")";
    return false;
  }
  if (private_mapping) {
    /* Some programs (at least Firefox) have been observed
     * to use cache files that are expected to be
     * consistent and unchanged during the bulk of
     * execution, but may be destroyed or mutated at
     * shutdown in preparation for the next session.  We
     * don't otherwise know what to do with private
     * mappings, so err on the safe side.
     *
     * TODO: could get into dirty heuristics here like
     * trying to match "cache" in the filename ... */
    LOG(debug) << "  copying private mapping of non-system -x " << filename;
    return true;
  }
  if (!(0222 & stat->st_mode)) {
    /* We couldn't write the file because it's read only.
     * But it's not a root-owned file (therefore not a
     * system file), so it's likely that it could be
     * temporary.  Copy it. */
    LOG(debug) << "  copying read-only, non-system file";
    return true;
  }
  if (!can_write_file) {
    /* mmap'ing another user's (non-system) files?  Highly
     * irregular ... */
    FATAL() << "Unhandled mmap " << filename << "(prot:" << HEX(prot)
            << ((flags & MAP_SHARED) ? ";SHARED" : "")
            << "); uid:" << stat->st_uid << " mode:" << stat->st_mode;
  }
  /* Shared mapping that we can write.  Should assume that the
   * mapping is likely to change. */
  LOG(debug) << "  copying writeable SHARED mapping " << filename;
  if (PROT_WRITE | prot) {
    if (warn_shared_writeable) {
      LOG(debug) << filename << " is SHARED|WRITEABLE; that's not handled "
                                "correctly yet. Optimistically hoping it's not "
                                "written by programs outside the rr tracee "
                                "tree.";
    }
  }
  return true;
}

ScopedFd create_shmem_segment(const char* name, size_t num_bytes) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path) - 1, "%s/%s", SHMEM_FS, name);

  ScopedFd fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  if (0 > fd) {
    FATAL() << "Failed to create shmem segment " << path;
  }
  /* Remove the fs name so that we don't have to worry about
   * cleaning up this segment in error conditions. */
  unlink(path);
  resize_shmem_segment(fd, num_bytes);

  LOG(debug) << "created shmem segment " << path;
  return fd;
}

void resize_shmem_segment(ScopedFd& fd, size_t num_bytes) {
  if (ftruncate(fd, num_bytes)) {
    FATAL() << "Failed to resize shmem to " << num_bytes;
  }
}

// TODO de-dup
static void advance_syscall(Task* t) {
  do {
    t->cont_syscall();
  } while (t->is_ptrace_seccomp_event() ||
           ReplaySession::is_ignored_signal(t->pending_sig()));
  assert(t->ptrace_event() == 0);
}

void destroy_buffers(Task* t) {
  // NB: we have to pay all this complexity here because glibc
  // makes its SYS_exit call through an inline int $0x80 insn,
  // instead of going through the vdso.  There may be a deep
  // reason for why it does that, but if it starts going through
  // the vdso in the future, this code can be eliminated in
  // favor of a *much* simpler vsyscall SYS_exit hook in the
  // preload lib.
  Registers exit_regs = t->regs();
  ASSERT(t, is_exit_syscall(exit_regs.original_syscallno(), t->arch()))
      << "Tracee should have been at exit, but instead at "
      << t->syscallname(exit_regs.original_syscallno());

  // The tracee is at the entry to SYS_exit, but hasn't started
  // the call yet.  We can't directly start injecting syscalls
  // because the tracee is still in the kernel.  And obviously,
  // if we finish the SYS_exit syscall, the tracee isn't around
  // anymore.
  //
  // So hijack this SYS_exit call and rewrite it into a harmless
  // one that we can exit successfully, SYS_gettid here (though
  // that choice is arbitrary).
  exit_regs.set_original_syscallno(syscall_number_for_gettid(t->arch()));
  t->set_regs(exit_regs);
  // This exits the hijacked SYS_gettid.  Now the tracee is
  // ready to do our bidding.
  advance_syscall(t);

  // Restore these regs to what they would have been just before
  // the tracee trapped at SYS_exit.  When we've finished
  // cleanup, we'll restart the SYS_exit call.
  exit_regs.set_original_syscallno(-1);
  exit_regs.set_syscallno(syscall_number_for_exit(t->arch()));
  exit_regs.set_ip(exit_regs.ip() - syscall_instruction_length(t->arch()));
  ASSERT(t, is_at_syscall_instruction(t, exit_regs.ip()))
      << "Tracee should have entered through int $0x80.";

  // Do the actual buffer and fd cleanup.
  t->destroy_buffers(DESTROY_SCRATCH | DESTROY_SYSCALLBUF);

  // Restart the SYS_exit call.
  t->set_regs(exit_regs);
  advance_syscall(t);
}

template <typename Arch> struct ElfSymbols {
  vector<typename Arch::ElfSym> symbols;
  vector<char> strtab;
};

template <typename Arch>
static ElfSymbols<Arch> read_elf_symbols (Task* t, remote_ptr<void> library_start) {
  auto elfheader = t->read_mem(library_start.cast<typename Arch::ElfEhdr>());
  assert(elfheader.e_ident[EI_CLASS] == Arch::elfclass);
  assert(elfheader.e_ident[EI_DATA] == Arch::elfendian);
  assert(elfheader.e_machine == Arch::elfmachine);
  assert(elfheader.e_shentsize == sizeof(typename Arch::ElfShdr));

  auto sections_start = library_start + elfheader.e_shoff;
  typename Arch::ElfShdr sections[elfheader.e_shnum];
  t->read_bytes_helper(sections_start, sizeof(sections), sections);

  typename Arch::ElfShdr* dynsym = nullptr;
  typename Arch::ElfShdr* dynstr = nullptr;

  for (size_t i = 0; i < elfheader.e_shnum; ++i) {
    auto header = &sections[i];
    if (header->sh_type == SHT_DYNSYM) {
      assert(!dynsym && "multiple .dynsym sections?!");
      dynsym = header;
      continue;
    }
    if (header->sh_type == SHT_STRTAB && (header->sh_flags & SHF_ALLOC) &&
        i != elfheader.e_shstrndx) {
      assert(!dynstr && "multiple .dynstr sections?!");
      dynstr = header;
    }
  }

  if (!dynsym || !dynstr) {
    assert(0 && "Unable to locate vdso information");
  }

  assert(dynsym->sh_entsize == sizeof(typename Arch::ElfSym));
  remote_ptr<void> symbols_start = library_start + dynsym->sh_offset;
  size_t nsymbols = dynsym->sh_size / dynsym->sh_entsize;
  remote_ptr<void> strtab_start = library_start + dynstr->sh_offset;
  ElfSymbols<Arch> result;
  result.symbols =
      t->read_mem(symbols_start.cast<typename Arch::ElfSym>(), nsymbols);
  result.strtab = t->read_mem(strtab_start.cast<char>(), dynstr->sh_size);
  return result;
}

template <typename Arch>
static ElfSymbols<Arch> read_vdso_symbols(Task* t) {
  auto vdso_start = t->vm()->vdso().start;
  return read_elf_symbols<Arch>(t, vdso_start);
}

#include "AssemblyTemplates.generated"

/**
 * Return true iff |addr| points to a known |__kernel_vsyscall()|
 * implementation.
 */
static bool is_kernel_vsyscall(Task* t, remote_ptr<void> addr) {
  uint8_t impl[X86VsyscallImplementation::size];
  t->read_bytes(addr, impl);
  return X86VsyscallImplementation::match(impl);
}

/**
 * Return the address of a recognized |__kernel_vsyscall()|
 * implementation in |t|'s address space.
 */
static remote_ptr<void> locate_and_verify_kernel_vsyscall(Task* t) {
  auto syms = read_vdso_symbols<X86Arch>(t);

  remote_ptr<void> kernel_vsyscall = nullptr;
  // It is unlikely but possible that multiple, versioned __kernel_vsyscall
  // symbols will exist.  But we can't rely on setting |kernel_vsyscall| to
  // catch that case, because only one of the versioned symbols will
  // actually match what we expect to see, and the matching one might be
  // the last one.  Therefore, we have this separate flag to alert us to
  // this possbility.
  bool seen_kernel_vsyscall = false;

  for (auto& sym : syms.symbols) {
    const char* name = &syms.strtab[sym.st_name];
    if (strcmp(name, "__kernel_vsyscall") == 0) {
      assert(!seen_kernel_vsyscall);
      seen_kernel_vsyscall = true;
      // The ELF information in the VDSO assumes that the VDSO
      // is always loaded at a particular address.  The kernel,
      // however, subjects the VDSO to ASLR, which means that
      // we have to adjust the offsets properly.
      auto vdso_start = t->vm()->vdso().start;
      remote_ptr<void> candidate = sym.st_value;
      // The symbol values can be absolute or relative addresses.
      // The first part of the assertion is for absolute
      // addresses, and the second part is for relative.
      assert((candidate.as_int() & ~uintptr_t(0xfff)) == 0xffffe000 ||
             (candidate.as_int() & ~uintptr_t(0xfff)) == 0);
      uintptr_t candidate_offset = candidate.as_int() & uintptr_t(0xfff);
      candidate = vdso_start + candidate_offset;

      if (is_kernel_vsyscall(t, candidate)) {
        kernel_vsyscall = candidate;
      }
    }
  }

  return kernel_vsyscall;
}

template <typename Arch>
static void monkeypatch_vdso_after_preload_init_arch(Task* t);

template <typename Arch> static void monkeypatch_vdso_after_exec_arch(Task* t);

template <> void monkeypatch_vdso_after_exec_arch<X86Arch>(Task* t) {}

// Monkeypatch x86 vsyscall hook only after the preload library
// has initialized. The vsyscall hook expects to be able to use the syscallbuf.
// Before the preload library has initialized, the regular vsyscall code
// will trigger ptrace traps and be handled correctly by rr.
template <> void monkeypatch_vdso_after_preload_init_arch<X86Arch>(Task* t) {
  if (!t->regs().arg2()) {
    return;
  }

  auto kernel_vsyscall = locate_and_verify_kernel_vsyscall(t);
  if (!kernel_vsyscall) {
    FATAL() << "Failed to monkeypatch vdso: your __kernel_vsyscall() wasn't "
               "recognized.\n"
               "    Syscall buffering is now effectively disabled.  If you're "
               "OK with\n"
               "    running rr without syscallbuf, then run the recorder "
               "passing the\n"
               "    --no-syscall-buffer arg.\n"
               "    If you're *not* OK with that, file an issue.";
  }

  // Luckily, linux is happy for us to scribble directly over
  // the vdso mapping's bytes without mprotecting the region, so
  // we don't need to prepare remote syscalls here.
  remote_ptr<void> vsyscall_hook_trampoline_ptr = t->regs().arg1();
  uint32_t vsyscall_hook_trampoline = vsyscall_hook_trampoline_ptr.as_int();

  uint8_t patch[X86VsyscallMonkeypatch::size];
  X86VsyscallMonkeypatch::substitute(patch, vsyscall_hook_trampoline);

  t->write_bytes(kernel_vsyscall, patch);
  LOG(debug) << "monkeypatched __kernel_vsyscall to jump to "
             << vsyscall_hook_trampoline;
}

// x86-64 doesn't have a convenient vsyscall-esque function in the VDSO;
// syscalls happen directly with the |syscall| instruction and manual
// syscall restarting if necessary.  Its VDSO is filled with overhead
// critical functions related to getting the time and current CPU.  We
// need to ensure that these syscalls get redirected into actual
// trap-into-the-kernel syscalls so rr can intercept them.

enum SyscallBuffering {
  Supported,
  NotSupported,
};

struct named_syscall {
  const char* name;
  int syscall_number;
  // Indicates whether the syscall is supported by our LD_PRELOAD syscall
  // buffering library.
  SyscallBuffering syscall_buffering;
};

#define S(n, buffering)                         \
  { #n, X64Arch::n, buffering }
static const named_syscall syscalls_to_monkeypatch[] = {
  S(clock_gettime, Supported),
  S(gettimeofday, Supported),
  S(time, Supported),
  // getcpu isn't supported by rr, so any changes to this monkeypatching
  // scheme for efficiency's sake will have to ensure that getcpu gets
  // converted to an actual syscall so rr will complain appropriately.
  S(getcpu, NotSupported),
};
#undef S

enum NumSyscallArguments {
  ThreeOrFewerArguments,
  FourOrMoreArguments,
};

enum IsCancellationPoint {
  CancellationPoint,
  NotCancellationPoint,
};

struct patchable_libc_syscall {
  const char* name;
  NumSyscallArguments n_args;
  IsCancellationPoint is_cancellation_point;
};

// For now, we only patch those libc syscalls which follow a regular format,
// i.e. those generated from syscall-template.S.  So we don't catch all the
// syscalls handled by preload.c.
static const patchable_libc_syscall libc_syscalls_to_monkeypatch[] = {
  { "access", ThreeOrFewerArguments, NotCancellationPoint },
  // clock_gettime is handled by vdso monkeypatching.
  { "close", ThreeOrFewerArguments, CancellationPoint },
  { "creat", ThreeOrFewerArguments, CancellationPoint },
  // fcntl is irregular.
  // fstat is irregular.
  // gettimeofday is handled by vdso monkeypatching.
  { "lseek", ThreeOrFewerArguments, CancellationPoint },
  // lstat is irregular.
  { "madvise", ThreeOrFewerArguments, NotCancellationPoint },
  { "open", ThreeOrFewerArguments, CancellationPoint },
  { "poll", ThreeOrFewerArguments, CancellationPoint },
  { "read", ThreeOrFewerArguments, CancellationPoint },
  { "readlink", ThreeOrFewerArguments, NotCancellationPoint },
  // stat is irregular.
  // time is handled by vdso monkeypatching.
  { "write", ThreeOrFewerArguments, CancellationPoint },
  // writev is irregular.
};

// Monkeypatch x86-64 vdso syscalls immediately after exec. The vdso syscalls
// will cause replay to fail if called by the dynamic loader or some library's
// static constructors, so we can't wait for our preload library to be
// initialized. Fortunately we're just replacing the vdso code with real
// syscalls so there is no dependency on the preload library at all.
template <> void monkeypatch_vdso_after_exec_arch<X64Arch>(Task* t) {
  auto vdso_start = t->vm()->vdso().start;

  auto syms = read_vdso_symbols<X64Arch>(t);

  for (auto& sym : syms.symbols) {
    const char* symname = &syms.strtab[sym.st_name];
    for (size_t j = 0; j < array_length(syscalls_to_monkeypatch); ++j) {
      if (strcmp(symname, syscalls_to_monkeypatch[j].name) == 0) {
        // Absolutely-addressed symbols in the VDSO claim to start here.
        static const uint64_t vdso_static_base = 0xffffffffff700000LL;
        static const uintptr_t vdso_max_size = 0xffffLL;
        uintptr_t sym_address = uintptr_t(sym.st_value);
        // The symbol values can be absolute or relative addresses.
        // The first part of the assertion is for absolute
        // addresses, and the second part is for relative.
        assert((sym_address & ~vdso_max_size) == vdso_static_base ||
               (sym_address & ~vdso_max_size) == 0);
        uintptr_t sym_offset = sym_address & vdso_max_size;
        uintptr_t absolute_address = vdso_start.as_int() + sym_offset;

        uint8_t patch[X64VsyscallMonkeypatch::size];
        uint32_t syscall_number = syscalls_to_monkeypatch[j].syscall_number;
        X64VsyscallMonkeypatch::substitute(patch, syscall_number);

        t->write_bytes(absolute_address, patch);
        LOG(debug) << "monkeypatched " << symname << " to syscall "
                   << syscalls_to_monkeypatch[j].syscall_number;
      }
    }
  }
}

template <typename AssemblyTemplate, typename MonkeypatchTemplate>
static void monkeypatch_non_cancellable_function(Task* t,
                                                 remote_ptr<void> lib_start,
                                                 const char* name,
                                                 const typename X64Arch::ElfSym* elfsym) {
  remote_ptr<void> syscall_raw_trampoline = t->regs().arg1();

  auto symbol_address = lib_start + elfsym->st_value;

  // Verify that the assembly is what we expect to see.  Our trampoline scheme
  // depends on the exact sequence of instructions after the syscall, so it's
  // critical that the assembly has a particular format.
  uint8_t original_code[elfsym->st_size];
  t->read_bytes_helper(symbol_address, elfsym->st_size, original_code);

  uint32_t syscall_number;
  if (!AssemblyTemplate::match(original_code, &syscall_number)) {
    LOG(debug) << "Not patching " << name
               << " because its assembly code didn't match expectations";
    return;
  }

  // Our preloaded library (and therefore the syscall trampoline) should
  // be loaded within 2GB of libc/libpthread, and therefore we can use
  // a PC-relative CALL instruction to reach our trampoline.  Compute
  // the precise offset to place in the CALL instruction.
  remote_ptr<void> monkeypatched_call_end =
    symbol_address + AssemblyTemplate::monkeypatch_point_offset +
    MonkeypatchTemplate::syscall_trampoline_end();
  int64_t call_distance_to_trampoline =
    syscall_raw_trampoline - monkeypatched_call_end;

  // Bail if the distance doesn't fit into +/- 2GB.  We can work
  // around this with different monkeypatching approaches if need be,
  // but this style of monkeypatch is much simpler.
  if (call_distance_to_trampoline < INT32_MIN ||
      call_distance_to_trampoline > INT32_MAX) {
    LOG(debug) << "Not patching " << name
               << " because the trampoline is too far away";
    return;
  }

  // The monkeypatch is designed to be inserted at a particular place in the
  // assembly.  Verify that we're putting things in the correct place.
  static_assert(AssemblyTemplate::monkeypatch_point_offset + MonkeypatchTemplate::size ==
                AssemblyTemplate::jae_instruction_offset, "bogus monkeypatch!");

  // Create the patch and install.
  uint8_t patch[MonkeypatchTemplate::size];
  MonkeypatchTemplate::substitute(patch, call_distance_to_trampoline);
  t->write_bytes(symbol_address + AssemblyTemplate::monkeypatch_point_offset, patch);
  LOG(debug) << "monkeypatched " << name << " to jump to "
             << "trampoline at " << HEX(syscall_raw_trampoline.as_int());
}

template <typename AssemblyTemplate, typename MonkeypatchTemplateCancel,
          typename MonkeypatchTemplateNoCancel>
static void monkeypatch_cancellable_function(Task* t,
                                             remote_ptr<void> lib_start,
                                             const char* name,
                                             const typename X64Arch::ElfSym* elfsym) {
  remote_ptr<void> syscall_raw_trampoline = t->regs().arg1();
  remote_ptr<void> syscall_raw_cancellation_trampoline = t->regs().arg3();

  auto symbol_address = lib_start + elfsym->st_value;

  // Verify that the assembly is what we expect to see.  Our trampoline scheme
  // depends on the exact sequence of instructions after the syscall, so it's
  // critical that the assembly has a particular format.
  uint8_t original_code[elfsym->st_size];
  t->read_bytes_helper(symbol_address, elfsym->st_size, original_code);

  uint32_t syscall_number;
  // These aren't used, but do vary with the address of the function.
  uint32_t threaded_program_p, libc_enable_cancellation, libc_disable_cancellation;
  if (!AssemblyTemplate::match(original_code, &threaded_program_p,
                               &syscall_number, &libc_enable_cancellation,
                               &syscall_number, &libc_disable_cancellation)) {
    LOG(debug) << "Not patching " << name
               << " because its assembly code didn't match expectations";
    return;
  }

  // Our preloaded library (and therefore the syscall trampoline) should
  // be loaded within 2GB of libc/libpthread, and therefore we can use
  // a PC-relative CALL instruction to reach our trampoline.  Compute
  // the precise offset to place in the CALL instruction.
  remote_ptr<void> monkeypatched_call_end =
    symbol_address + AssemblyTemplate::nocancel_monkeypatch_point_offset +
    MonkeypatchTemplateNoCancel::syscall_trampoline_end();
  int64_t call_distance_to_trampoline =
    syscall_raw_trampoline - monkeypatched_call_end;

  // Bail if the distance doesn't fit into +/- 2GB.  We can work
  // around this with different monkeypatching approaches if need be,
  // but this style of monkeypatch is much simpler.
  if (call_distance_to_trampoline < INT32_MIN ||
      call_distance_to_trampoline > INT32_MAX) {
    LOG(debug) << "Not patching " << name
               << " because the trampoline is too far away";
    return;
  }

  // We have to do the same check for the cancellation trampoline, too.
  remote_ptr<void> cancellation_monkeypatched_call_end =
    symbol_address + AssemblyTemplate::cancel_monkeypatch_point_offset +
    MonkeypatchTemplateCancel::syscall_trampoline_end();
  int64_t call_distance_to_cancellation_trampoline =
    syscall_raw_cancellation_trampoline - cancellation_monkeypatched_call_end;

  if (call_distance_to_cancellation_trampoline < INT32_MIN ||
      call_distance_to_cancellation_trampoline > INT32_MAX) {
    LOG(debug) << "Not patching " << name
               << " because the cancellation trampoline is too far away";
    return;
  }

  // The monkeypatches are designed to be inserted at particular places in the
  // assembly.  Verify that we're putting things in the correct place.
  static_assert(AssemblyTemplate::nocancel_monkeypatch_point_offset + MonkeypatchTemplateNoCancel::size ==
                AssemblyTemplate::jae_instruction_offset, "bogus nocancel monkeypatch!");
  static_assert(AssemblyTemplate::cancel_monkeypatch_point_offset + MonkeypatchTemplateCancel::size ==
                AssemblyTemplate::begin_disable_call_offset, "bogus cancel monkeypatch!");

  // Create the patches and install.
  uint8_t nocancel_patch[MonkeypatchTemplateNoCancel::size];
  MonkeypatchTemplateNoCancel::substitute(nocancel_patch, call_distance_to_trampoline);
  t->write_bytes(symbol_address + AssemblyTemplate::nocancel_monkeypatch_point_offset, nocancel_patch);

  uint8_t cancel_patch[MonkeypatchTemplateCancel::size];
  MonkeypatchTemplateCancel::substitute(cancel_patch, call_distance_to_cancellation_trampoline);
  t->write_bytes(symbol_address + AssemblyTemplate::cancel_monkeypatch_point_offset, cancel_patch);

  LOG(debug) << "monkeypatched " << name << " to jump to "
             << "trampoline at " << HEX(syscall_raw_trampoline.as_int())
             << " and cancel trampoline at " << HEX(syscall_raw_cancellation_trampoline.as_int());
}

template <typename Arch>
static void monkeypatch_libc_alike(Task* t, remote_ptr<void> lib_start) {
  auto syms = read_elf_symbols<Arch>(t, lib_start);

  // libc and related libraries are big enough that we're going to
  // construct a table of sym->address for faster lookup before we start
  // patching things.
  map<std::string, typename Arch::ElfSym*> symbol_table;

  // TODO: what about versioned symbols?
  for (typename Arch::ElfSym& libsym : syms.symbols) {
    symbol_table[&syms.strtab[libsym.st_name]] = &libsym;
  }

  for (auto& patchable_symbol : libc_syscalls_to_monkeypatch) {
    auto name = patchable_symbol.name;
    auto sym_entry = symbol_table.find(name);
    if (sym_entry == symbol_table.end()) {
      LOG(debug) << "Not patching " << name << " because it's not in the symbol table";
      continue;
    }

    auto elfsym = (*sym_entry).second;
    if (patchable_symbol.is_cancellation_point == CancellationPoint) {
      if (patchable_symbol.n_args == FourOrMoreArguments) {
        monkeypatch_cancellable_function<X64CancellationPointSyscall4Arg,
                                         X64CancellationPointMonkeypatch,
                                         X64NotCancellationPointMonkeypatch>(t, lib_start,
                                                                             name, elfsym);
      } else {
        monkeypatch_cancellable_function<X64CancellationPointSyscall,
                                         X64CancellationPointMonkeypatch,
                                         X64NotCancellationPointMonkeypatch>(t, lib_start,
                                                                             name, elfsym);
      }
      continue;
    }

    if (patchable_symbol.n_args == FourOrMoreArguments) {
      monkeypatch_non_cancellable_function<X64NotCancellationPointSyscall4Arg,
                                           X64NotCancellationPoint4ArgMonkeypatch>(t, lib_start,
                                                                                   name, elfsym);
    } else {
      monkeypatch_non_cancellable_function<X64NotCancellationPointSyscall,
                                           X64NotCancellationPointMonkeypatch>(t, lib_start,
                                                                               name, elfsym);
    }
  }
}

template <> void monkeypatch_vdso_after_preload_init_arch<X64Arch>(Task* t) {
  remote_ptr<void> syscall_raw_trampoline = t->regs().arg1();
  auto vdso_start = t->vm()->vdso().start;

  auto syms = read_vdso_symbols<X64Arch>(t);

  for (auto& sym : syms.symbols) {
    const char* symname = &syms.strtab[sym.st_name];
    for (auto& syscall : syscalls_to_monkeypatch) {
      if (syscall.syscall_buffering == Supported &&
          strcmp(symname, syscall.name) == 0) {
          // Absolutely-addressed symbols in the VDSO claim to start here.
          static const uint64_t vdso_static_base = 0xffffffffff700000LL;
          static const uintptr_t vdso_max_size = 0xffffLL;
          uintptr_t sym_address = uintptr_t(sym.st_value);
          // The symbol values can be absolute or relative addresses.
          // The first part of the assertion is for absolute
          // addresses, and the second part is for relative.
          assert((sym_address & ~vdso_max_size) == vdso_static_base ||
                 (sym_address & ~vdso_max_size) == 0);
          uintptr_t sym_offset = sym_address & vdso_max_size;
          uintptr_t absolute_address = vdso_start.as_int() + sym_offset;

          uint32_t syscall_number = syscall.syscall_number;

          uint8_t patch[X64VsyscallSyscallbufMonkeypatch::size];
          X64VsyscallSyscallbufMonkeypatch::substitute(patch, syscall_number,
                                                       syscall_raw_trampoline.as_int());

          t->write_bytes(absolute_address, patch);
          LOG(debug) << "monkeypatched " << symname << " to jump to "
                     << "trampoline at " << HEX(syscall_raw_trampoline.as_int());
      }
    }
  }

  if (t->vm()->has_libc()) {
    monkeypatch_libc_alike<X64Arch>(t, t->vm()->libc().start);
  }
  if (t->vm()->has_libpthread()) {
    monkeypatch_libc_alike<X64Arch>(t, t->vm()->libpthread().start);
  }
}

void monkeypatch_vdso_after_exec(Task* t) {
  ASSERT(t, 1 == t->vm()->task_set().size())
      << "Can't have multiple threads immediately after exec!";

  RR_ARCH_FUNCTION(monkeypatch_vdso_after_exec_arch, t->arch(), t)
}

void monkeypatch_vdso_after_preload_init(Task* t) {
  ASSERT(t, 1 == t->vm()->task_set().size())
      << "TODO: monkeypatch multithreaded process";

  // NB: the tracee can't be interrupted with a signal while
  // we're processing the rrcall, because it's masked off all
  // signals.
  RR_ARCH_FUNCTION(monkeypatch_vdso_after_preload_init_arch, t->arch(), t)

  Registers r = t->regs();
  r.set_syscall_result(0);
  t->set_regs(r);
}

void cpuid(int code, int subrequest, unsigned int* a, unsigned int* c,
           unsigned int* d) {
  asm volatile("cpuid"
               : "=a"(*a), "=c"(*c), "=d"(*d)
               : "a"(code), "c"(subrequest)
               : "ebx");
}

void set_cpu_affinity(int cpu) {
  assert(cpu >= 0);

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  if (0 > sched_setaffinity(0, sizeof(mask), &mask)) {
    FATAL() << "Couldn't bind to CPU " << cpu;
  }
}

int get_num_cpus() {
  int cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
  return cpus > 0 ? cpus : 1;
}

template <typename Arch>
static void extract_clone_parameters_arch(const Registers& regs,
                                          remote_ptr<void>* stack,
                                          remote_ptr<int>* parent_tid,
                                          remote_ptr<void>* tls,
                                          remote_ptr<int>* child_tid) {
  switch (Arch::clone_parameter_ordering) {
    case Arch::FlagsStackParentTLSChild:
      if (stack) {
        *stack = regs.arg2();
      }
      if (parent_tid) {
        *parent_tid = regs.arg3();
      }
      if (tls) {
        *tls = regs.arg4();
      }
      if (child_tid) {
        *child_tid = regs.arg5();
      }
      break;
    case Arch::FlagsStackParentChildTLS:
      if (stack) {
        *stack = regs.arg2();
      }
      if (parent_tid) {
        *parent_tid = regs.arg3();
      }
      if (child_tid) {
        *child_tid = regs.arg4();
      }
      if (tls) {
        *tls = regs.arg5();
      }
      break;
  }
}

void extract_clone_parameters(Task* t, remote_ptr<void>* stack,
                              remote_ptr<int>* parent_tid,
                              remote_ptr<void>* tls,
                              remote_ptr<int>* child_tid) {
  RR_ARCH_FUNCTION(extract_clone_parameters_arch, t->arch(), t->regs(), stack,
                   parent_tid, tls, child_tid);
}
