/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_MONKEYPATCHER_H_
#define RR_MONKEYPATCHER_H_

#include <unordered_set>
#include <vector>

#include "preload/preload_interface.h"

#include "remote_ptr.h"
#include "remote_code_ptr.h"

class Task;

/**
 * A class encapsulating patching state. There is one instance of this
 * class per tracee address space. Currently this class performs the following
 * tasks:
 *
 * 1) Patch the VDSO's user-space-only implementation of certain system calls
 * (e.g. gettimeofday) to do a proper kernel system call instead, so rr can
 * trap and record it (x86-64 only).
 *
 * 2) Patch the VDSO __kernel_vsyscall fast-system-call stub to redirect to
 * our syscall hook in the preload library (x86 only).
 *
 * 3) Patch syscall instructions whose following instructions match a known
 * pattern to call the syscall hook.
 */
class Monkeypatcher {
public:
  Monkeypatcher() {}
  Monkeypatcher(const Monkeypatcher& o)
      : syscall_hooks(o.syscall_hooks),
        tried_to_patch_syscall_addresses(o.tried_to_patch_syscall_addresses) {}

  /**
   * Apply any necessary patching immediately after exec.
   * In this hook we patch everything that doesn't depend on the preload
   * library being loaded.
   */
  void patch_after_exec(Task* t);

  /**
   * During librrpreload initialization, apply patches that require the
   * preload library to be initialized.
   */
  void patch_at_preload_init(Task* t);

  /**
   * Try to patch the syscall instruction that |t| just entered. If this
   * returns false, patching failed and the syscall should be processed
   * as normal. If this returns true, patching succeeded and the syscall
   * was aborted; ip() has been reset to the start of the patched syscall,
   * and execution should resume normally to execute the patched code.
   */
  bool try_patch_syscall(Task* t);

  void init_dynamic_syscall_patching(
      Task* t, int syscall_patch_hook_count,
      remote_ptr<syscall_patch_hook> syscall_patch_hooks);

private:
  /**
   * The list of supported syscall patches obtained from the preload
   * library. Each one matches a specific byte signature for the instruction(s)
   * after a syscall instruction.
   */
  std::vector<syscall_patch_hook> syscall_hooks;
  /**
   * The addresses of the instructions following syscalls that we've tried
   * (or are currently trying) to patch.
   */
  std::unordered_set<remote_code_ptr> tried_to_patch_syscall_addresses;
};

#endif /* RR_MONKEYPATCHER_H_ */
