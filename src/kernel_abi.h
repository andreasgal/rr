/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#ifndef RR_KERNEL_ABI_H
#define RR_KERNEL_ABI_H

// Get all the kernel definitions so we can verify our alternative versions.
#include <arpa/inet.h>
#include <asm/ldt.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/net.h>
#include <linux/sockios.h>
#include <linux/wireless.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/quota.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <termios.h>

#include <assert.h>

#include "types.h"

namespace rr {

#if defined(__i386__)
#define RR_NATIVE_ARCH supported_arch::x86
#else
#error need to define new supported_arch enum
#endif

template<supported_arch a, typename system_type, typename rr_type>
struct verifier {
	// Optimistically say we are the same size.
	static const bool same_size = true;
};

template<typename system_type, typename rr_type>
struct verifier<RR_NATIVE_ARCH, system_type, rr_type> {
	static const bool same_size = sizeof(system_type) == sizeof(rr_type);
};

template<typename T>
struct verifier<RR_NATIVE_ARCH, T, T> {
	// Prevent us from accidentally verifying the size of rr's structure
	// with itself or (unlikely) the system's structure with itself.
};

// For instances where the system type and the rr type are named differently.
#define RR_VERIFY_TYPE_EXPLICIT(system_type_, rr_type_)	\
  static_assert(verifier<arch, system_type_, rr_type_>::same_size, \
		"type " #system_type_ " not correctly defined");

// For instances where the system type and the rr type are named identically.
#define RR_VERIFY_TYPE(type_) RR_VERIFY_TYPE_EXPLICIT(struct ::type_, type_)

struct kernel_constants {
	static const ::size_t SIGINFO_MAX_SIZE = 128;

	// These types are the same size everywhere.
	typedef int32_t pid_t;
	typedef uint32_t uid_t;

	typedef uint32_t socklen_t;
};

struct wordsize32_defs : public kernel_constants {
	static const ::size_t SIGINFO_PAD_SIZE = (SIGINFO_MAX_SIZE / sizeof(int32_t)) - 3;

	typedef int16_t signed_short;
	typedef uint16_t unsigned_short;

	typedef int32_t signed_int;
	typedef uint32_t unsigned_int;

	typedef int32_t signed_long;
	typedef uint32_t unsigned_long;

	typedef int32_t signed_word;
	typedef uint32_t unsigned_word;

	typedef uint32_t size_t;

	// These really only exist as proper abstractions so that adding x32
	// (x86-64's ILP32 ABI) support is relatively easy.
	typedef int32_t syscall_slong_t;
	typedef int32_t sigchld_clock_t;
};

template<supported_arch arch, typename wordsize>
struct base_arch : public wordsize {
	typedef typename wordsize::syscall_slong_t syscall_slong_t;
	typedef typename wordsize::signed_int signed_int;
	typedef typename wordsize::unsigned_int unsigned_int;
	typedef typename wordsize::signed_short signed_short;
	typedef typename wordsize::unsigned_short unsigned_short;
	typedef typename wordsize::signed_long signed_long;
	typedef typename wordsize::unsigned_long unsigned_long;
	typedef typename wordsize::unsigned_word unsigned_word;
	typedef typename wordsize::sigchld_clock_t sigchld_clock_t;

	typedef syscall_slong_t time_t;
	typedef syscall_slong_t suseconds_t;
	typedef syscall_slong_t off_t;

	typedef syscall_slong_t clock_t;

	template<typename T>
	struct ptr {
		unsigned_word val;
		operator T*() const {
			return reinterpret_cast<T*>(val);
		};
		T* operator=(T* p) {
			val = reinterpret_cast<uintptr_t>(p);
			// check that val is wide enough to hold the value of p
			assert(val == reinterpret_cast<uintptr_t>(p));
			return p;
		}
	};

	union sigval_t {
		signed_int sival_int;
		ptr<void> sival_ptr;
	};

	struct sockaddr {
		unsigned_short sa_family;
		char sa_data[14];
	};
	RR_VERIFY_TYPE(sockaddr);

	struct timeval {
		time_t tv_sec;
		suseconds_t tv_usec;
	};
	RR_VERIFY_TYPE(timeval);

	struct timespec {
		time_t tv_sec;
		syscall_slong_t tv_nsec;
	};
	RR_VERIFY_TYPE(timespec);

	struct pollfd {
		signed_int fd;
		signed_short events;
		signed_short revents;
	};
	RR_VERIFY_TYPE(pollfd);

	struct iovec {
		ptr<void> iov_base;
		size_t iov_len;
	};
	RR_VERIFY_TYPE(iovec);

	struct msghdr {
		ptr<void> msg_name;
		socklen_t msg_namelen;

		ptr<iovec> msg_iov;
		size_t msg_iovlen;

		ptr<void> msg_control;
		size_t msg_controllen;

		signed_int msg_flags;
	};
	RR_VERIFY_TYPE(msghdr);

	struct mmsghdr {
		msghdr msg_hdr;
		unsigned_int msg_len;
	};
	RR_VERIFY_TYPE(mmsghdr);

	// XXX what to do about __EPOLL_PACKED?  Explicit specialization?
	struct epoll_event {
		union epoll_data {
			ptr<void> ptr_;
			signed_int fd;
			uint32_t u32;
			uint64_t u64;
		};

		uint32_t events;
		epoll_data data;
	};
	RR_VERIFY_TYPE(epoll_event);

	struct rusage {
		timeval ru_utime;
		timeval ru_stime;
		signed_long ru_maxrss;
		signed_long ru_ixrss;
		signed_long ru_idrss;
		signed_long ru_isrss;
		signed_long ru_minflt;
		signed_long ru_majflt;
		signed_long ru_nswap;
		signed_long ru_inblock;
		signed_long ru_oublock;
		signed_long ru_msgnsd;
		signed_long ru_msgrcv;
		signed_long ru_nsignals;
		signed_long ru_nvcsw;
		signed_long ru_nivcsw;
	};
	RR_VERIFY_TYPE(rusage);

	struct siginfo_t {
		signed_int si_signo;
		signed_int si_errno;
		signed_int si_code;
		union {
			signed_int padding[wordsize::SIGINFO_PAD_SIZE];
			// <bits/siginfo.h> #defines all the field names belong due to X/Open
			// requirements, so we append '_'.
			struct {
				pid_t si_pid_;
				uid_t si_uid_;
			} _kill;
			struct {
				signed_int si_tid_;
				signed_int si_overrun_;
				sigval_t si_sigval_;
			} _timer;
			struct {
				pid_t si_pid_;
				uid_t si_uid_;
				sigval_t si_sigval_;
			} _rt;
			struct {
				pid_t si_pid_;
				uid_t si_uid_;
				signed_int si_status_;
				sigchld_clock_t si_utime_;
				sigchld_clock_t si_stime_;
			} _sigchld;
			struct {
				ptr<void> si_addr_;
				signed_short si_addr_lsb_;
			} _sigfault;
			struct {
				signed_long si_band_;
				signed_int si_fd_;
			} _sigpoll;
			struct {
				ptr<void> _call_addr;
				signed_int _syscall;
				unsigned_int _arch;
			} _sigsys;
		} _sifields;
	};
	RR_VERIFY_TYPE_EXPLICIT(siginfo_t, ::siginfo_t)

	// rr uses several types merely for their sizeof() properties
	// and completely ignores the fields inside of them.
	// Replicating them here with perfect fidelity seems
	// unnecessary.
	//
	// For some of these structures, then, we're going to define
	// them purely for sizeof() equivalence.  We're only going to do
	// that where the size of the structure couldn't vary between
	// architectures, though; if a structure depends on types like
	// unsigned_long or an architecture's packing rules, we spell it
	// out in full.
	struct termios {
		unsigned_int c_iflag;
		unsigned_int c_oflag;
		unsigned_int c_cflag;
		unsigned_int c_lflag;
		unsigned char c_line;
		unsigned char c_cc[32];
		unsigned_int c_ispeed;
		unsigned_int c_ospeed;
	};
	RR_VERIFY_TYPE(termios);

	struct winsize {
		char dummy[8];
	};
	RR_VERIFY_TYPE(winsize);

	struct ipc64_perm {
		signed_int key;
		uid_t uid;
		gid_t gid;
		uid_t cuid;
		gid_t cgid;
		unsigned_int mode; // __kernel_mode_t + padding, really.
		unsigned_short seq;
		unsigned_short pad2;
		unsigned_long unused1;
		unsigned_long unused2;
	};
	RR_VERIFY_TYPE(ipc64_perm);

	struct msqid64_ds {
		struct ipc64_perm msg_perm;
		// These msg*time fields are really __kernel_time_t plus
		// appropiate padding.  We don't touch the fields, though.
		//
		// We do, however, suffix them with _only_little_endian to
		// urge anybody who does touch them to make sure the right
		// thing is done for big-endian systems.
		uint64_t msg_stime_only_little_endian;
		uint64_t msg_rtime_only_little_endian;
		uint64_t msg_ctime_only_little_endian;
		unsigned_long msg_cbytes;
		unsigned_long msg_qnum;
		unsigned_long msg_qbytes;
		pid_t msg_lspid;
		pid_t msg_lrpid;
		unsigned_long unused1;
		unsigned_long unused2;
	};
	RR_VERIFY_TYPE(msqid64_ds);

	struct msginfo {
		signed_int msgpool;
		signed_int msgmap;
		signed_int msgmax;
		signed_int msgmnb;
		signed_int msgmni;
		signed_int msgssz;
		signed_int msgtql;
		unsigned_short msgseg;
	};
	RR_VERIFY_TYPE(msginfo);

	struct user_desc {
		char dummy[16];
	};
	RR_VERIFY_TYPE(user_desc);

	// This structure uses fixed-size fields, but the padding rules
	// for 32-bit vs. 64-bit architectures dictate that it be
	// defined in full.
	struct dqblk {
		uint64_t dqb_bhardlimit;
		uint64_t dqb_bsoftlimit;
		uint64_t dqb_curspace;
		uint64_t dqb_ihardlimit;
		uint64_t dqb_isoftlimit;
		uint64_t dqb_curinodes;
		uint64_t dqb_btime;
		uint64_t dqb_itime;
		uint32_t dqb_valid;
	};
	RR_VERIFY_TYPE(dqblk);

	struct dqinfo {
		char dummy[24];
	};
	RR_VERIFY_TYPE(dqinfo);

	struct ifreq {
		char ifreq_name[16];
		union {
			char dummy[16];
			ptr<void> data;
		} ifreq_union;
	};
	RR_VERIFY_TYPE(ifreq);

	struct ifconf {
		signed_int ifc_len;
		union {
			ptr<char> ifcu_buf;
			ptr<ifreq> ifcu_req;
		} ifc_ifcu;
	};
	RR_VERIFY_TYPE(ifconf);

	struct iwreq {
		char dummy[32];
	};
	RR_VERIFY_TYPE(iwreq);

	struct ethtool_cmd {
		char dummy[44];
	};
	RR_VERIFY_TYPE(ethtool_cmd);

	struct flock {
		unsigned_short l_type;
		unsigned_short l_whence;
		unsigned_int l_start;
		unsigned_int l_len;
		pid_t l_pid;
	};
	RR_VERIFY_TYPE(flock);

	struct flock64 {
		unsigned_short l_type;
		unsigned_short l_whence;
		uint64_t l_start;
		uint64_t l_len;
		pid_t l_pid;
	};
	RR_VERIFY_TYPE(flock64);

	struct f_owner_ex {
		signed_int type;
		pid_t pid;
	};
	RR_VERIFY_TYPE(f_owner_ex);

	// Define various structures that package up syscall arguments.
	// The types of their members are part of the ABI, and defining
	// them here makes their definitions more concise.
	struct accept_args {
		signed_int sockfd;
		ptr<sockaddr> addr;
		ptr<socklen_t> addrlen;
	};

	struct accept4_args {
		accept_args _;
		signed_long flags;
	};

	struct getsockname_args {
		signed_int sockfd;
		ptr<sockaddr> addr;
		ptr<socklen_t> addrlen;
	};

	struct getsockopt_args {
		signed_int sockfd;
		signed_int level;
		signed_int optname;
		ptr<void> optval;
		ptr<socklen_t> optlen;
	};

	struct recv_args {
		signed_int sockfd;
		ptr<void> buf;
		size_t len;
		signed_int flags;
	};

	struct recvfrom_args {
		signed_long sockfd;
		ptr<void> buf;
		size_t len;
		signed_long flags;
		ptr<sockaddr> src_addr;
		ptr<socklen_t> addrlen;
	};

	struct recvmsg_args {
		signed_int fd;
		ptr<msghdr> msg;
		signed_int flags;
	};

	struct recvmmsg_args {
		signed_int sockfd;
		ptr<mmsghdr> msgvec;
		unsigned_int vlen;
		unsigned_int flags;
		ptr<timespec> timeout;
	};

	struct sendmmsg_args {
		signed_int sockfd;
		ptr<mmsghdr> msgvec;
		unsigned_int vlen;
		unsigned_int flags;
	};

	struct socketpair_args {
		signed_int domain;
		signed_int type;
		signed_int protocol;
		ptr<signed_int> sv; // int sv[2]
	};

	struct mmap_args {
		ptr<void> addr;
		size_t len;
		signed_int prot;
		signed_int flags;
		signed_int fd;
		off_t offset;
	};
};

struct x86_arch : public base_arch<supported_arch::x86, wordsize32_defs> {
};

} // namespace rr

#endif /* RR_KERNEL_ABI_H */
