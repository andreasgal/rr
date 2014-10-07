/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_UTIL_H_
#define RR_UTIL_H_

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <unistd.h>

#include <array>
#include <ostream>

#include "ExtraRegisters.h"
#include "kernel_abi.h"
#include "kernel_supplement.h"
#include "Registers.h"
#include "ScopedFd.h"

class AutoRemoteSyscalls;
class Task;
class TraceFrame;
struct Flags;

template <typename T, size_t N> constexpr size_t array_length(T (&array)[N]) {
  return N;
}

template <typename T, size_t N>
constexpr size_t array_length(std::array<T, N>& array) {
  return N;
}

#define SHMEM_FS "/dev/shm"
#define SHMEM_FS2 "/run/shm"

/* The syscallbuf shared with tracees is created with this prefix
 * followed by the tracee tid, then immediately unlinked and shared
 * anonymously. */
#define SYSCALLBUF_SHMEM_NAME_PREFIX "rr-tracee-shmem-"
#define SYSCALLBUF_SHMEM_PATH_PREFIX SHMEM_FS "/" SYSCALLBUF_SHMEM_NAME_PREFIX

#define PREFIX_FOR_EMPTY_MMAPED_REGIONS "/tmp/rr-emptyfile-"

class Task;

enum Completion {
  COMPLETE,
  INCOMPLETE
};

/**
 * Return true if |reg1| matches |reg2|.  Passing EXPECT_MISMATCHES
 * indicates that the caller is using this as a general register
 * compare and nothing special should be done if the register files
 * mismatch.  Passing LOG_MISMATCHES will log the registers that don't
 * match.  Passing BAIL_ON_MISMATCH will additionally abort on
 * mismatch.
 */
enum {
  EXPECT_MISMATCHES = 0,
  LOG_MISMATCHES,
  BAIL_ON_MISMATCH
};
bool compare_register_files(Task* t, const char* name1, const Registers* reg1,
                            const char* name2, const Registers* reg2,
                            int mismatch_behavior);

void assert_child_regs_are(Task* t, const Registers* regs);

/**
 * Create a file named |filename| and dump |buf_len| words in |buf| to
 * that file, starting with a line containing |label|.  |start_addr|
 * is the client address at which |buf| resides, if meaningful.
 * |start_addr| is used to compute the output lines of words, which
 * look like "0xValue | [0xAddr]".
 */
void dump_binary_data(const char* filename, const char* label,
                      const uint32_t* buf, size_t buf_len,
                      remote_ptr<void> start_addr);

/**
 * Format a suitable filename within the trace directory for dumping
 * information about |t| at the current global time, to a file that
 * contains |tag|.  The constructed filename is returned through
 * |filename|.  For example, a filename for a task with tid 12345 at
 * time 111, for a file tagged "foo", would be something like
 * "trace_0/12345_111_foo".  The returned name is not guaranteed to be
 * unique, caveat emptor.
 */
void format_dump_filename(Task* t, int global_time, const char* tag,
                          char* filename, size_t filename_size);

/**
 * Return true if the user requested memory be dumped for |t| at
 * |event| at |global_time|.
 */
bool should_dump_memory(Task* t, const TraceFrame& f);
/**
 * Dump all of the memory in |t|'s address to the file
 * "[trace_dir]/[t->tid]_[global_time]_[tag]".
 */
void dump_process_memory(Task* t, int global_time, const char* tag);

/**
 * Return true if the user has requested |t|'s memory be
 * checksummed at |event| at |global_time|.
 */
bool should_checksum(Task* t, const TraceFrame& f);
/**
 * Write a checksum of each mapped region in |t|'s address space to a
 * special log, where it can be read by |validate_process_memory()|
 * during replay.
 */
void checksum_process_memory(Task* t, int global_time);
/**
 * Validate the checksum of |t|'s address space that was written
 * during recording.
 */
void validate_process_memory(Task* t, int global_time);

/**
 * Open a temporary debugging connection for |t| and service requests
 * until the user quits or requests execution to resume.
 *
 * You probably don't want to use this directly; instead, use
 * |assert_exec()| from dbg.h.
 *
 * This function does not return.
 */
void emergency_debug(Task* t);

/**
 * Get the current time from the preferred monotonic clock in units of
 * seconds, relative to an unspecific point in the past.
 */
double now_sec(void);

/**
 * Sleep for the duration of time specified in |ts|.  Continue
 * sleeping until |ts| has elapsed, even if a signal is received.  If
 * an error occurs, -1 is returned and errno is set appropriately.
 */
void nanosleep_nointr(const struct timespec* ts);

/**
 * Return nonzero if the rr session is probably not interactive (that
 * is, there's probably no user watching or interacting with rr), and
 * so asking for user input or other actions is probably pointless.
 */
bool probably_not_interactive(int fd = STDERR_FILENO);

/**
 * If |child_fd| is a stdio fd and stdio-marking is enabled, prepend
 * the stdio write with "[rr.<global-time>]".  This allows users to
 * more easily correlate stdio with trace event numbers.
 */
void maybe_mark_stdio_write(Task* t, int child_fd);

/**
 * Return the symbolic name of the PTRACE_EVENT_* |event|, or
 * "???EVENT" if unknown.
 */
const char* ptrace_event_name(int event);

/**
 * Return the symbolic name of the PTRACE_ |request|, or "???REQ" if
 * unknown.
 */
const char* ptrace_req_name(int request);

/**
 * Return the symbolic name of |sig|, f.e. "SIGILL", or "???signal" if
 * unknown.
 */
const char* signalname(int sig);

/**
 * Return true iff replaying |syscall| will never ever require
 * actually executing it, i.e. replay of |syscall| is always
 * emulated.
 */
bool is_always_emulated_syscall(int syscall, SupportedArch arch);

/**
 * Convert the flags passed to the clone() syscall, |flags_arg|, into
 * the format understood by Task::clone().
 */
int clone_flags_to_task_flags(int flags_arg);

/**
 * Return the argument rounded up to the nearest multiple of the
 * system |page_size()|.
 */
size_t ceil_page_size(size_t sz);
remote_ptr<void> ceil_page_size(remote_ptr<void> addr);

/** Return the system page size. */
size_t page_size();

/**
 * Copy the registers used for syscall arguments (not including
 * syscall number) from |from| to |to|.
 */
void copy_syscall_arg_regs(Registers* to, const Registers* from);

/**
 * Return true if a FUTEX_LOCK_PI operation on |futex| done by |t|
 * will transition the futex into the contended state.  (This results
 * in the kernel atomically setting the FUTEX_WAITERS bit on the futex
 * value.)  The new value of the futex after the kernel updates it is
 * returned in |next_val|.
 */
bool is_now_contended_pi_futex(Task* t, remote_ptr<int> futex, int* next_val);

/** Return the default action of |sig|. */
enum signal_action {
  DUMP_CORE,
  TERMINATE,
  CONTINUE,
  STOP,
  IGNORE
};
signal_action default_action(int sig);

/**
 * Return true if |sig| may cause the status of other tasks to change
 * unpredictably beyond rr's observation.
 * 'deterministic' is true when the signal was delivered deterministically,
 * i.e. due to code execution as opposed to an asynchronous signal sent by some
 * process.
 */
bool possibly_destabilizing_signal(Task* t, int sig, bool deterministic);

/**
 * Return nonzero if a mapping of |filename| with metadata |stat|,
 * using |flags| and |prot|, should almost certainly be copied to
 * trace; i.e., the file contents are likely to change in the interval
 * between recording and replay.  Zero is returned /if we think we can
 * get away/ with not copying the region.  That doesn't mean it's
 * necessarily safe to skip copying!
 */
enum {
  DONT_WARN_SHARED_WRITEABLE = 0,
  WARN_DEFAULT
};
bool should_copy_mmap_region(const char* filename, const struct stat* stat,
                             int prot, int flags, int warn_shared_writeable);

/**
 * Return an fd referring to a new shmem segment with descriptive
 * |name| of size |num_bytes|.
 */
ScopedFd create_shmem_segment(const char* name, size_t num_bytes);

/**
 * Ensure that the shmem segment referred to by |fd| has exactly the
 * size |num_bytes|.
 */
void resize_shmem_segment(ScopedFd& fd, size_t num_bytes);

/**
 * At thread exit time, undo the work that init_buffers() did.
 *
 * Call this when the tracee has already entered SYS_exit. The
 * tracee will be returned at a state in which it has entered (or
 * re-entered) SYS_exit.
 */
void destroy_buffers(Task* t);

/**
 * Locate |t|'s |__kernel_vsyscall()| helper and then monkey-patch it
 * to jump to the preload lib's hook function.
 */
void monkeypatch_vdso(Task* t);

enum cpuid_requests {
  CPUID_GETVENDORSTRING,
  CPUID_GETFEATURES,
  CPUID_GETTLB,
  CPUID_GETSERIAL,
  CPUID_GETXSAVE = 0x0D,
  CPUID_INTELEXTENDED = 0x80000000,
  CPUID_INTELFEATURES,
  CPUID_INTELBRANDSTRING,
  CPUID_INTELBRANDSTRINGMORE,
  CPUID_INTELBRANDSTRINGEND,
};

/** issue a single request to CPUID. Fits 'intel features', for instance
 *  note that even if only "eax" and "edx" are of interest, other registers
 *  will be modified by the operation, so we need to tell the compiler about it.
 *  'code' is placed in EAX. 'subrequest' is placed in ECX.
 *  *a, *c and *d receive EAX, ECX and EDX respectively.
 */
void cpuid(int code, int subrequest, unsigned int* a, unsigned int* c,
           unsigned int* d);

/**
 * Force this process (and its descendants) to only use the cpu with the given
 * index.
 */
void set_cpu_affinity(int cpu);

/**
 * Return the number of available CPUs in the system.
 */
int get_num_cpus();

#endif /* RR_UTIL_H_ */
