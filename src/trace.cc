/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

//#define DEBUGTAG "Trace"

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>

#include "preload/syscall_buffer.h"

#include "config.h"
#include "dbg.h"
#include "hpc.h"
#include "task.h"
#include "trace.h"
#include "util.h"

//
// This represents the format and layout of recorded traces.  This
// version number doesn't track the rr version number, because changes
// to the trace format will be rare.
//
// NB: if you *do* change the trace format for whatever reason, you
// MUST increment this version number.  Otherwise users' old traces
// will become unreplayable and they won't know why.
//
#define TRACE_VERSION 1

#define BUF_SIZE 1024;
#define LINE_SIZE 50;

using namespace std;

// The directory in which traces are saved.  Can be overridden by the
// environment variable "_RR_TRACE_DIR=/foo".
static char rr_trace_dir[PATH_MAX];
static char trace_path_[PATH_MAX];

static FILE *syscall_header;
static FILE *raw_data;
static int trace_file_fd = -1;
static FILE *mmaps_file;

/* Global time starts at "1" so that conditions like |global_time %
 * interval| don't have to consider the trivial case |global_time ==
 * 0|. */
static uint32_t global_time = 1;
static int read_first_trace_frame = 0;

// counts the number of raw bytes written, a new raw_data file is used when MAX_RAW_DATA_SIZE is reached
static long long overall_raw_bytes = 0;

static ssize_t sizeof_trace_frame_event_info(void)
{
	return offsetof(struct trace_frame, end_event_info) -
		offsetof(struct trace_frame, begin_event_info);
}

static ssize_t sizeof_trace_frame_exec_info(void)
{
	return offsetof(struct trace_frame, end_exec_info) -
		offsetof(struct trace_frame, begin_exec_info);
}

void flush_trace_files(void)
{
	fflush(syscall_header);
	fflush(raw_data);
	fflush(mmaps_file);
}

const char* get_trace_path()
{
	return trace_path_;
}

void dump_trace_frame(FILE* out, const struct trace_frame* f)
{
	bool raw_dump = rr_flags()->raw_dump;
	const struct user_regs_struct* r = &f->recorded_regs;

	if (raw_dump) {
		fprintf(out, " %d %d %d %d",
			f->global_time, f->thread_time, f->tid,	f->ev.encoded);
	} else {
		fprintf(out,
"{\n  global_time:%u, event:`%s' (state:%d), tid:%d, thread_time:%u",
			f->global_time, Event(f->ev).str().c_str(),
			f->ev.state, f->tid, f->thread_time);
	}
	if (!f->ev.has_exec_info) {
		if (!raw_dump) {
			fprintf(out, "\n}");
		}
		fprintf(out, "\n");
		return;
	}

	if (raw_dump) {
		fprintf(out,
			" %lld %lld %lld %lld"
			" %ld %ld %ld %ld %ld %ld %ld"
			" %ld %ld %ld %ld\n",
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
			f->hw_interrupts, f->page_faults, f->rbc, f->insts,
#else
			// Don't force tools to detect our config.
			-1LL, -1LL, f->rbc, -1LL,
#endif
			r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp,
			r->orig_eax, r->esp, r->eip, r->eflags);
	} else {
		fprintf(out,
"\n"
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
"  hw_ints:%lld faults:%lld rbc:%lld insns:%lld\n"
#else
"  rbc:%lld\n"
#endif
"  eax:0x%lx ebx:0x%lx ecx:0x%lx edx:0x%lx esi:0x%lx edi:0x%lx ebp:0x%lx\n"
"  eip:0x%lx esp:0x%lx eflags:0x%lx orig_eax:%ld xfs:0x%lx xgs:0x%lx\n"
"}\n",
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
			f->hw_interrupts, f->page_faults, f->rbc, f->insts,
#else
			f->rbc,
#endif
			r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp,
			r->eip, r->esp, r->eflags, r->orig_eax, r->xfs, r->xgs);
	}
}

unsigned int get_global_time(void)
{
	return global_time;
}

static string get_version_file_path()
{
	stringstream path;
	path << trace_path_ << "/version";
	return path.str();
}

static void set_default_rr_trace_dir()
{
	snprintf(rr_trace_dir, sizeof(rr_trace_dir), "%s/.rr", getenv("HOME"));
}

/**
 * Figure out where the saved-trace directory is, and store that
 * directory to |rr_trace_dir|.
 */
static void resolve_rr_trace_dir()
{
	const char* output_dir = getenv("_RR_TRACE_DIR");
	if (output_dir) {
		strncpy(rr_trace_dir, output_dir, sizeof(rr_trace_dir));
	} else {
		set_default_rr_trace_dir();
	}
}

/**
 * Create the default ~/.rr directory if it doesn't already exist.
 * Must be called *before* |resolve_rr_trace_dir()|.
 */
static void ensure_default_rr_trace_dir()
{
	set_default_rr_trace_dir();

	struct stat st;
	if (0 == stat(rr_trace_dir, &st)) {
		if (!(S_IFDIR & st.st_mode)) {
			fatal("`%s' exists but isn't a directory.",
			      rr_trace_dir);
		}
		if (access(rr_trace_dir, W_OK)) {
			fatal("Can't write to `%s'.", rr_trace_dir);
		}
		return;
	}
	int ret = mkdir(rr_trace_dir, S_IRWXU | S_IRWXG);
	// Another rr process can be concurrently attempting to create
	// ~/.rr, so the directory may have come into existence since
	// we checked above.
	if (ret && EEXIST != ret) {
		fatal("Failed to create directory `%s'", rr_trace_dir);
	}
}

/**
 * Return the name of the latest-trace symlink in the saved-trace dir.
 */
static string get_latest_trace_symlink()
{
	return string(rr_trace_dir) + "/latest-trace";
}

/**
 * Update the latest-trace symlink in the saved-trace dir to |dir|.
 */
static void set_latest_trace_dir(const char* dir)
{
	string link_name = get_latest_trace_symlink();
	// Try to update the symlink to |dir|.  We only try attempt to
	// set the symlink once.  If the link is re-created after we
	// |unlink()| it, then another rr process is racing with us
	// and it "won".  The link is then valid and points at some
	// very-recent trace, so that's good enough.
	unlink(link_name.c_str());
	int ret = symlink(dir, link_name.c_str());
	if (!(0 == ret || EEXIST == ret)) {
		fatal("Failed to update symlink `%s' to `%s'.",
		      link_name.c_str(), dir);
	}
}

void rec_set_up_trace_dir(const char* exe_path)
{
	int nonce = 0;
	int ret;

	ensure_default_rr_trace_dir();
	resolve_rr_trace_dir();

	// Find a unique trace directory name.
	do {
		snprintf(trace_path_, sizeof(trace_path_) - 1, "%s/%s-%d",
			 rr_trace_dir, basename(exe_path), nonce++);
		ret = mkdir(trace_path_, S_IRWXU | S_IRWXG);
	} while (ret && EEXIST == errno);

	if (ret) {
		fatal("Unable to create trace directory `%s'", trace_path_);
	}

	// Update the latest-trace symlink so that |rr replay| (with
	// no args) does what you expect.  We do this even if a
	// non-default saved-trace directory is being used, because
	// it's still convenient to be able to replay the latest trace
	// in that dir using |rr replay| (and in fact, the rr test
	// harness relies on being able to do that).
	set_latest_trace_dir(trace_path_);

	string path = get_version_file_path();
	fstream version(path.c_str(), fstream::out);
	if (!version.good()) {
		fatal("Unable to create %s", path.c_str());
	}
	version << TRACE_VERSION << endl;

	if (!probably_not_interactive(STDOUT_FILENO)) {
		printf(
"rr: Saving the execution of `%s' to trace directory `%s'.\n",
			exe_path, trace_path_);
	}
}

static string get_arg_env_file_path()
{
	stringstream arg_env_path;
	arg_env_path << trace_path_ << "/arg_env";
	return arg_env_path.str();
}

void record_argv_envp(int argc, char* argv[], char* envp[])
{
	FILE* arg_env = fopen(get_arg_env_file_path().c_str(), "a+");
	if (!arg_env) {
		fatal("Failed to open arg_env file in %s.", trace_path_);
	}

	/* print argc */
	fprintf(arg_env, "%d%c", argc, 0);

	/* print arguments to file */
	for (int i = 0; i < argc; ++i) {
		fprintf(arg_env, "%s%c", argv[i], 0);
	}

	int envp_len = 0;
	while (envp[envp_len]) ++envp_len;
	fprintf(arg_env, "%d%c", envp_len, 0);

	for (int i = 0; i < envp_len; i++) {
		fprintf(arg_env, "%s%c", envp[i], 0);
	}
	fclose(arg_env);
}

static void open_rawdata_file(void)
{
	assert(!raw_data);

	char path[PATH_MAX];

	overall_raw_bytes = 0;

	snprintf(path, sizeof(path) - 1, "%s/raw_data", trace_path_);
	raw_data = fopen64(path, "a+");
	if (!raw_data) {
		fatal("Failed to create new rawdata file %s", path);
	}
}

static void open_trace_file(void)
{
	char path[PATH_MAX];

	if (0 <= trace_file_fd) {
		return;
	}
	snprintf(path, sizeof(path) - 1, "%s/trace", trace_path_);
	trace_file_fd = open(path, O_APPEND | O_CLOEXEC | O_CREAT | O_RDWR,
			     0600);
}

static void ensure_compatible_trace_version()
{
	string path = get_version_file_path();
	fstream vfile(path.c_str(), fstream::in);
	if (!vfile.good()) {
		fprintf(stderr,
"\n"
"rr: error: Version file for recorded trace `%s' not found.  Did you record\n"
"           `%s' with an older version of rr?  If so, you'll need to replay\n"
"           `%s' with that older version.  Otherwise, your trace is\n"
"           likely corrupted.\n"
"\n",
			trace_path_, trace_path_, trace_path_);
		exit(EX_DATAERR);
	}
	int version = 0;
	vfile >> version;
	if (vfile.fail() || TRACE_VERSION != version) {
		fprintf(stderr,
"\n"
"rr: error: Recorded trace `%s' has an incompatible version %d; expected\n"
"           %d.  Did you record `%s' with an older version of rr?  If so,\n"
"           you'll need to replay `%s' with that older version.  Otherwise,\n"
"           your trace is likely corrupted.\n"
"\n",
			trace_path_, version, TRACE_VERSION,
			trace_path_, trace_path_);
		exit(EX_DATAERR);
	}
}

void open_trace_files(void)
{
	ensure_compatible_trace_version();

	char path[PATH_MAX];

	open_trace_file();

	snprintf(path, sizeof(path) - 1, "%s/syscall_input", trace_path_);
	syscall_header = fopen64(path, "a+");
	if (!syscall_header) {
		fatal("Failed to open syscall header file %s", path);
	}

	open_rawdata_file();

	snprintf(path, sizeof(path) - 1, "%s/mmaps", trace_path_);
	mmaps_file = fopen64(path, "a+");
	if (!mmaps_file) {
		fatal("Failed to open mmaps record file %s", path);
	}
}

void rec_init_trace_files(void)
{
	/* print human readable header */
	fprintf(syscall_header, "%11s", "time");
	fprintf(syscall_header, "%11s", "syscall");
	fprintf(syscall_header, "%11s", "addr");
	fprintf(syscall_header, "%11s\n", "size");

	fprintf(mmaps_file, "%11s", "time");
	fprintf(mmaps_file, "%11s", "tid");
	fprintf(mmaps_file, "%11s", "mmap_start");
	fprintf(mmaps_file, "%11s", "mmap_end");
	fprintf(mmaps_file, "%11s", "blksize");
	fprintf(mmaps_file, "%11s", "blocks");
	fprintf(mmaps_file, "%11s", "ctim.sec");
	fprintf(mmaps_file, "%11s", "ctim.nsec");
	fprintf(mmaps_file, "%11s", "dev");
	fprintf(mmaps_file, "%11s", "gid");
	fprintf(mmaps_file, "%11s", "ino");
	fprintf(mmaps_file, "%11s", "mode");
	fprintf(mmaps_file, "%11s", "mtim.sec");
	fprintf(mmaps_file, "%11s", "mtim.nsec");
	fprintf(mmaps_file, "%11s", "rdev");
	fprintf(mmaps_file, "%11s", "size");
	fprintf(mmaps_file, "%11s", "uid");
	fprintf(mmaps_file, "%11s\n", "filename");

	fflush(mmaps_file);
	fflush(syscall_header);
	fflush(raw_data);
}

void close_trace_files(void)
{
	close(trace_file_fd);
	if (syscall_header)
		fclose(syscall_header);
	if (raw_data)
		fclose(raw_data);
	if (mmaps_file)
		fclose(mmaps_file);
}

static void print_header(EncodedEvent ev, void* addr)
{
	fprintf(syscall_header, "%11u", global_time);
	fprintf(syscall_header, "%11d", ev.encoded);
	fprintf(syscall_header, "%11u", (uintptr_t)addr);
}

static void write_raw_data(Task *t, const void *buf, size_t to_write)
{
	size_t bytes_written;
	(void)bytes_written;

	bytes_written = fwrite(buf, 1, to_write, raw_data);
	assert(bytes_written == to_write);
	overall_raw_bytes += to_write;
}

static void save_data(Task *t, void* addr, ssize_t num_bytes, const void* buf)
{
	EncodedEvent ev = t->ev().encode();

	/* We shouldn't be recording a scratch address */
	assert_exec(t, !addr || addr != t->scratch_ptr, "");

	if (addr && num_bytes > 0) {
		write_raw_data(t, buf, num_bytes);
	}
	print_header(ev, addr);
	assert(num_bytes >= 0);
	fprintf(syscall_header, "%11d\n", num_bytes);
}

/**
 * Flush the syscallbuf to the trace, if there are any pending entries.
 */
static void maybe_flush_syscallbuf(Task *t)
{
	if (!t || !t->syscallbuf_hdr
	    || 0 == t->syscallbuf_hdr->num_rec_bytes 
	    || t->delay_syscallbuf_flush) {
		/* No context, no syscallbuf, or no records.  Nothing
		 * to do. */
		return;
	}
	/* Write the entire buffer in one shot without parsing it,
	 * since replay will take care of that. */
	t->push_event(Event(EV_SYSCALLBUF_FLUSH, NO_EXEC_INFO));
	save_data(t, t->syscallbuf_child,
		  /* Record the header for consistency checking. */
		  t->syscallbuf_hdr->num_rec_bytes + sizeof(*t->syscallbuf_hdr),
		  t->syscallbuf_hdr);
	record_event(t);
	t->pop_event(EV_SYSCALLBUF_FLUSH);

	/* Reset header. */
	assert(!t->syscallbuf_hdr->abort_commit);
	if (!t->delay_syscallbuf_reset) {
		t->syscallbuf_hdr->num_rec_bytes = 0;
	}
	t->flushed_syscallbuf = 1;
}

/**
 * Collect execution info about |t| that's relevant for replay.  For
 * example, the current rbc value must read and saved in order to
 * replay asynchronous signals.
 */
static void collect_execution_info(Task* t, struct trace_frame* frame)
{
	frame->rbc = read_rbc(t->hpc);
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
	frame->hw_interrupts = read_hw_int(t->hpc);
	frame->page_faults = read_page_faults(t->hpc);
	frame->insts = read_insts(t->hpc);
#endif
	frame->recorded_regs = t->regs();
}

/**
 * Translate |t|'s event |ev| into a trace frame that can be saved to
 * the log.
 */
static void encode_trace_frame(Task* t, const Event& ev,
			       struct trace_frame* frame)
{
	memset(frame, 0, sizeof(*frame));

	frame->global_time = global_time++;
	frame->thread_time = t->thread_time++;
	frame->tid = t->tid;
	frame->ev = ev.encode();
	if (frame->ev.has_exec_info) {
		collect_execution_info(t, frame);
	}
}

/**
 * Write |frame| to the log.  Succeed or don't return.
 */
static void write_trace_frame(const struct trace_frame* frame)
{
	void* begin_data = (void*)&frame->begin_event_info;
	ssize_t nbytes = sizeof_trace_frame_event_info();
	ssize_t nwritten;

	/* TODO: only store exec info for non-async-sig events when
	 * debugging assertions are enabled. */
	if (frame->ev.has_exec_info) {
		nbytes += sizeof_trace_frame_exec_info();
	}
	nwritten = write(trace_file_fd, begin_data, nbytes);
	if (nwritten != nbytes) {
		fatal("Tried to save %d bytes to the trace, but only wrote %d",
		      nbytes, nwritten);
	}
}

void record_event(Task *t)
{
	struct trace_frame frame;

	/* If there's buffered syscall data, we need to record a flush
	 * event before recording |frame|, so that they're replayed in
	 * the correct order. */
	if (EV_SYSCALLBUF_FLUSH != t->ev().type()) {
		maybe_flush_syscallbuf(t);
	}

	/* NB: we must encode the frame *after* flushing the
	 * syscallbuf, because encoding the frame has side effects on
	 * the global and thread clocks. */
	encode_trace_frame(t, t->ev(), &frame);

	if (should_dump_memory(t, frame)) {
		dump_process_memory(t, frame.global_time, "rec");
	}		
	if (should_checksum(t, frame)) {
		checksum_process_memory(t, frame.global_time);
	}

	write_trace_frame(&frame);

	if (frame.ev.has_exec_info) {
		reset_hpc(t, rr_flags()->max_rbc);
	}
}

void record_trace_termination_event(Task* t)
{
	struct trace_frame frame;
	memset(&frame, 0, sizeof(frame));
	frame.tid = t ? t->tid : 0;
	frame.global_time = global_time++;
	frame.ev.type = EV_TRACE_TERMINATION;
	frame.ev.data = 0;
	write_trace_frame(&frame);
}

void record_data(Task* t, void* addr, ssize_t num_bytes, const void* buf)
{
	maybe_flush_syscallbuf(t);
	save_data(t, addr, num_bytes, buf);
}

void record_mmapped_file_stats(struct mmapped_file* file)
{
	// XXX rewrite me
	fprintf(mmaps_file, "%11d", file->time);
	fprintf(mmaps_file, "%11d", file->tid);
	fprintf(mmaps_file, "%11d", file->copied);
	fprintf(mmaps_file, "%11x", (uintptr_t)file->start);
	fprintf(mmaps_file, "%11x", (uintptr_t)file->end);
	fprintf(mmaps_file, "%11lu", file->stat.st_blksize);
	fprintf(mmaps_file, "%11lu", file->stat.st_blocks);
	fprintf(mmaps_file, "%11lu", file->stat.st_ctim.tv_sec);
	fprintf(mmaps_file, "%11lu", file->stat.st_ctim.tv_nsec);
	fprintf(mmaps_file, "%11llu", file->stat.st_dev);
	fprintf(mmaps_file, "%11u", file->stat.st_gid);
	fprintf(mmaps_file, "%11lu", file->stat.st_ino);
	fprintf(mmaps_file, "%11u", file->stat.st_mode);
	fprintf(mmaps_file, "%11lu", file->stat.st_mtim.tv_sec);
	fprintf(mmaps_file, "%11lu", file->stat.st_mtim.tv_nsec);
	fprintf(mmaps_file, "%11llu", file->stat.st_rdev);
	fprintf(mmaps_file, "%11lu", file->stat.st_size);
	fprintf(mmaps_file, "%11d", file->stat.st_uid);
	fprintf(mmaps_file, "%s\n", file->filename);
}

void record_child_str(Task* t, void* child_ptr)
{
	EncodedEvent ev = t->ev().encode();
	print_header(ev, child_ptr);
	string str = t->read_c_str(child_ptr);
	size_t len = str.size() + 1;
	fprintf(syscall_header, "%11d\n", len);
	size_t bytes_written = fwrite(str.c_str(), 1, len, raw_data);
	(void)bytes_written;
	overall_raw_bytes += len;

	assert(bytes_written == len);
}

void rep_set_up_trace_dir(int argc, char** argv)
{
	resolve_rr_trace_dir();
	string trace_dir = argc > 0 ? argv[0] : get_latest_trace_symlink();
	strncpy(trace_path_, trace_dir.c_str(), sizeof(trace_path_) - 1);
}

void rep_init_trace_files(void)
{
	char line[1024];

	/* The first line of these files is a header, which we eat. */
	read_line(syscall_header, line, 1024, "syscall_input");
	read_line(mmaps_file, line, 1024, "stats");
}

void load_recorded_env(int* argc, string* exe_image,
		       CharpVector* argv, CharpVector* envp)
{
	FILE* arg_env = fopen(get_arg_env_file_path().c_str(), "r");
	if (!arg_env) {
		fprintf(stderr,
"rr: Error: Unable to load files from trace directory `%s'.\n",
			trace_path_);
		exit(EX_DATAERR);
	}
	char buf[8192];

	/* the first line contains argc */
	read_null_terminated(arg_env, buf, sizeof(buf), "arg_env");
	*argc = str2li(buf, LI_COLUMN_SIZE);

	/* followed by argv */
	for (int i = 0; i < *argc; ++i) {
		read_null_terminated(arg_env, buf, sizeof(buf), "arg_env");
		argv->push_back(strdup(buf));
	}

	/* do not forget write NULL to the last element */
	argv->push_back(NULL);
	*exe_image = argv->at(0);

	/* now, read the number of environment entries */
	read_null_terminated(arg_env, buf, sizeof(buf), "arg_env");
	int envc = str2li(buf, LI_COLUMN_SIZE);

	/* followed by argv */
	for (int i = 0; i < envc; i++) {
		read_null_terminated(arg_env, buf, sizeof(buf), "arg_env");
		envp->push_back(strdup(buf));
	}

	/* do not forget write NULL to the last element */
	envp->push_back(NULL);

	/* clean up */
	fclose(arg_env);
}

static size_t parse_raw_data_hdr(struct trace_frame* trace, void** addr)
{
	/* XXX rewrite me */
	char line[1024];
	char* tmp_ptr;
	uint32_t time;
	int size;
	EncodedEvent ev;

	read_line(syscall_header, line, 1024, "syscall_input");
	tmp_ptr = line;

	time = str2li(tmp_ptr, LI_COLUMN_SIZE);
	tmp_ptr += LI_COLUMN_SIZE;
	ev.encoded = str2li(tmp_ptr, LI_COLUMN_SIZE);
	tmp_ptr += LI_COLUMN_SIZE;
	*addr = (void*)str2li(tmp_ptr, LI_COLUMN_SIZE);
	tmp_ptr += LI_COLUMN_SIZE;
	size = str2li(tmp_ptr, LI_COLUMN_SIZE);

	if (!(time == trace->global_time
	      && (ev.type == trace->ev.type
		  || (trace->ev.type == EV_SYSCALL
		      && trace->ev.data == SYS_restart_syscall)))) {
		fatal("trace and syscall_input out of sync: trace is at (time=%d, %s), but input is for (time=%d, %s)",
		      trace->global_time, Event(trace->ev).str().c_str(),
		      time, Event(ev).str().c_str());
	}
	return size;
}

/**
 * Read |num_bytes| from the current rawdata file into |buf|, which
 * the caller must ensure is sized appropriately.  Skip to next
 * rawdata file if the current one is at eof.
 */
static void read_rawdata(void* buf, size_t num_bytes)
{
	size_t bytes_read = fread(buf, 1, num_bytes, raw_data);
	if (bytes_read != num_bytes) {
		fatal("rawdata read of %u requested, but %u read",
		      num_bytes, bytes_read);
	}
	overall_raw_bytes += bytes_read;
}

void* read_raw_data(struct trace_frame* trace, size_t* size_ptr, void** addr)
{
	size_t size = parse_raw_data_hdr(trace, addr);
	void* data = NULL;

	*size_ptr = size;
	if (!*addr) {
		return NULL;
	}

	data = malloc(size);
	read_rawdata(data, size);
	return data;
}

ssize_t read_raw_data_direct(struct trace_frame* trace,
			     void* buf, size_t buf_size, void** rec_addr)
{
	size_t data_size = parse_raw_data_hdr(trace, rec_addr);

	if (!*rec_addr) {
		return 0;
	}

	assert(data_size <= buf_size);
	read_rawdata(buf, data_size);
	return data_size;
}

pid_t get_recorded_main_thread(void)
{
	struct trace_frame frame;

	assert(1 == get_global_time());

	peek_next_trace(&frame);
	return frame.tid;
}

/**
 * Read next file stat buffer
 *
 * Returns file reader tid on success, -1 on failure.
 *
 */
void read_next_mmapped_file_stats(struct mmapped_file* file) {
	assert(!feof(mmaps_file));
	/* XXX this could be considerably faster, simpler, and
	 * memory-safer ... */
	char line0[1024], *line = line0;
	line = fgets(line, 1024, mmaps_file);
	assert(line);

	file->time = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->tid = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->copied = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->start = str2x(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->end = str2x(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_blksize = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_blocks = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_ctim.tv_sec = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_ctim.tv_nsec = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_dev = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_gid = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_ino = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_mode = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_mtim.tv_sec = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_mtim.tv_nsec = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_rdev = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_size = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	file->stat.st_uid = str2li(line,LI_COLUMN_SIZE);
	line += LI_COLUMN_SIZE;
	strcpy(file->filename,line);
	// get rid of the \n
	file->filename[strlen(file->filename) - 1] = '\0';
}

void peek_next_trace(struct trace_frame* trace)
{
	/* FIXME if peeking causes the trace file to roll over, then
	 * things will go haywire. */
	off_t pos = lseek(trace_file_fd, 0, SEEK_CUR);
	uint32_t saved_global_time = global_time;
	int saved_read_first_trace_frame = read_first_trace_frame;

	read_next_trace(trace);

	read_first_trace_frame = saved_read_first_trace_frame;
	global_time = saved_global_time;
	lseek(trace_file_fd, pos, SEEK_SET);
}

int try_read_next_trace(struct trace_frame *frame)
{
	ssize_t nread;

	memset(frame, 0, sizeof(*frame));

	/* This is the global time for the *next* frame, the one we're
	 * about to read.  For the first frame, the global time is
	 * already correct. */
	global_time += read_first_trace_frame ? 1 : 0;
	read_first_trace_frame = 1;

	/* Read the common event info first, to see if we also have
	 * exec info to read. */
	nread = read(trace_file_fd, &frame->begin_event_info,
		     sizeof_trace_frame_event_info());
	if (sizeof_trace_frame_event_info() != nread) {
		return 0;
	}

	if (frame->ev.has_exec_info) {
		nread = read(trace_file_fd, &frame->begin_exec_info,
			     sizeof_trace_frame_exec_info());
		if (sizeof_trace_frame_exec_info() != nread) {
			return 0;
		}
	}

	assert(global_time == frame->global_time);
	return 1;
}

void read_next_trace(struct trace_frame* frame)
{
	int read_ok = try_read_next_trace(frame);
	assert(read_ok);
}
