/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#ifndef SYS_H_
#define SYS_H_

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include <sys/stat.h>

#include "types.h"

struct task;

void sys_close(int filedes);
FILE* sys_fopen(const char* path, const char* mode);
int sys_open(const char* path, int flags, mode_t mode);
int sys_mkpath(const char *path, mode_t mode);
void sys_fclose(FILE* file);
pid_t sys_fork(void);
int sys_open_child_mem(pid_t child_tid);
void sys_kill(int pid, int msg);
void sys_exit(void);
void sys_start_trace(char* executable, char** fake_argv, char** envp);

void goto_next_event(struct task *t);

long sys_ptrace(int request, pid_t pid, void* addr, void* data);
void sys_ptrace_setup(pid_t pid);
void sys_ptrace_singlestep(pid_t pid);
void sys_ptrace_singlestep_sig(pid_t pid, int sig);
void sys_ptrace_sysemu(pid_t pid);
void sys_ptrace_sysemu_sig(pid_t pid, int sig);
void sys_ptrace_sysemu_singlestep(pid_t pid);
void sys_ptrace_sysemu_singlestep_sig(pid_t pid, int sig);
void sys_ptrace_traceme(void);
void sys_ptrace_cont(pid_t pid);
void sys_ptrace_cont_sig(pid_t pid, int sig);
void sys_ptrace_syscall_sig(pid_t pid, int sig);
/* Return zero on success, -1 on error. */
int sys_ptrace_peekdata(pid_t pid, long addr, long* value);
unsigned long sys_ptrace_getmsg(pid_t pid);
void sys_ptrace_getsiginfo(pid_t pid, siginfo_t* sig);
void sys_ptrace_detach(pid_t pid);
void sys_ptrace_syscall(pid_t pid);

pid_t sys_waitpid(pid_t pid, int *status);
pid_t sys_waitpid_nonblock(pid_t pid, int *status);
void sys_fcntl(int fd, int cmd, long arg1);

void* sys_mmap(void* addr, size_t length, int prot, int flags, int filedes, off_t offset);
void sys_munmap(void* addr, size_t length);
void* sys_malloc(int size);
void* sys_memset(void * block, int c, size_t size);
void* sys_malloc_zero(int size);
void sys_free(void** ptr);
void sys_setpgid(pid_t pid, pid_t pgid);

#endif /* SYS_H_ */
