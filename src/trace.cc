/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

//#define DEBUGTAG "Trace"

#include "trace.h"

#include <sysexits.h>

#include <fstream>
#include <string>
#include <sstream>

#include "log.h"
#include "util.h"

using namespace std;

//
// This represents the format and layout of recorded traces.  This
// version number doesn't track the rr version number, because changes
// to the trace format will be rare.
//
// NB: if you *do* change the trace format for whatever reason, you
// MUST increment this version number.  Otherwise users' old traces
// will become unreplayable and they won't know why.
//
#define TRACE_VERSION 8

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

static string default_rr_trace_dir()
{
	return string(getenv("HOME")) + "/.rr";
}

static string trace_save_dir()
{
	const char* output_dir = getenv("_RR_TRACE_DIR");
	return output_dir ? output_dir : default_rr_trace_dir();
}

static string latest_trace_symlink()
{
	return trace_save_dir() + "/latest-trace";
}

/**
 * Create the default ~/.rr directory if it doesn't already exist.
 */
static void ensure_default_rr_trace_dir()
{
	string dir = default_rr_trace_dir();
	struct stat st;
	if (0 == stat(dir.c_str(), &st)) {
		if (!(S_IFDIR & st.st_mode)) {
			FATAL() <<"`"<< dir <<"' exists but isn't a directory.";
		}
		if (access(dir.c_str(), W_OK)) {
			FATAL() <<"Can't write to `"<< dir <<"'.";
		}
		return;
	}
	int ret = mkdir(dir.c_str(), S_IRWXU | S_IRWXG);
	int err = errno;
	// Another rr process can be concurrently attempting to create
	// ~/.rr, so the directory may have come into existence since
	// we checked above.
	if (ret && EEXIST != err) {
		FATAL() <<"Failed to create directory `"<< dir <<"'";
	}
}

void
trace_frame::dump(FILE* out, bool raw_dump)
{
	out = out ? out : stdout;
	const Registers& r = recorded_regs;

	if (raw_dump) {
		fprintf(out, " %d %d %d %d",
			global_time, thread_time, tid, ev.encoded);
	} else {
		fprintf(out,
"{\n  global_time:%u, event:`%s' (state:%d), tid:%d, thread_time:%u",
			global_time, Event(ev).str().c_str(),
			ev.state, tid, thread_time);
	}
	if (!ev.has_exec_info) {
		fprintf(out, "\n");
		return;
	}

	if (raw_dump) {
		fprintf(out, " %lld %lld %lld %lld",
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
			hw_interrupts, page_faults, rbc, insts
#else
			// Don't force tools to detect our config.
			-1LL, -1LL, rbc, -1LL
#endif
			);
		r.print_register_file_for_trace(out, true);
		fprintf(out, "\n");
	} else {
		fprintf(out,
"\n"
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
"  hw_ints:%lld faults:%lld rbc:%lld insns:%lld\n"
#else
"  rbc:%lld\n"
#endif
"",
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
			hw_interrupts, page_faults, rbc, insts
#else
			rbc
#endif
			);
		r.print_register_file_for_trace(out, false);
	}
}

args_env::args_env(int argc, char* arg_v[], char** env_p, char* cwd)
	: exe_image(arg_v[0])
	, cwd(cwd)
{
	for (int i = 0; i < argc; ++i) {
		argv.push_back(strdup(arg_v[i]));
	}
	argv.push_back(nullptr);
	for (; *env_p; ++env_p) {
		envp.push_back(strdup(*env_p));
	}
	envp.push_back(nullptr);
}

args_env::~args_env()
{
	destroy();
}

args_env&
args_env::operator=(args_env&& o)
{
	swap(exe_image, o.exe_image);
	swap(argv, o.argv);
	swap(envp, o.envp);
	swap(cwd, o.cwd);
	return *this;
}

void
args_env::destroy()
{
	for (size_t i = 0; i < argv.size(); ++i) {
		free(argv[i]);
	}
	for (size_t i = 0; i < envp.size(); ++i) {
		free(envp[i]);
	}
}

bool
TraceOfstream::good() const
{
	return events.good() && data.good() && data_header.good() && mmaps.good();
}

bool
TraceIfstream::good() const
{
	return events.good() && data.good() && data_header.good() && mmaps.good();
}

TraceOfstream& operator<<(TraceOfstream& tof, const struct trace_frame& frame)
{
	const char* begin_data = (const char*)&frame.begin_event_info;
	ssize_t nbytes = sizeof_trace_frame_event_info();

	// TODO: only store exec info for non-async-sig events when
	// debugging assertions are enabled.
	if (frame.ev.has_exec_info) {
		nbytes += sizeof_trace_frame_exec_info();
	}
	tof.events.write(begin_data, nbytes);
	if (!tof.events.good()) {
		FATAL() <<"Tried to save "<< nbytes <<" bytes to the trace, but failed";
	}

	if (frame.ev.has_exec_info) {
		int extra_reg_bytes = frame.recorded_extra_regs.data_size();
		char extra_reg_format = (char)frame.recorded_extra_regs.format();
		tof.events.write(&extra_reg_format, sizeof(extra_reg_format));
		tof.events.write((char*)&extra_reg_bytes, sizeof(extra_reg_bytes));
		if (!tof.events.good()) {
			FATAL() <<"Tried to save "<< sizeof(extra_reg_bytes) + sizeof(extra_reg_format)
				<<" bytes to the trace, but failed";
		}
		if (extra_reg_bytes > 0) {
			tof.events.write((const char*)frame.recorded_extra_regs.data_bytes(),
					 extra_reg_bytes);
			if (!tof.events.good()) {
				FATAL() <<"Tried to save "<< extra_reg_bytes
					<<" bytes to the trace, but failed";
			}
		}
	}

	tof.tick_time();
	return tof;
}

TraceIfstream& operator>>(TraceIfstream& tif, struct trace_frame& frame)
{
	// Read the common event info first, to see if we also have
	// exec info to read.
	tif.events.read((char*)&frame.begin_event_info,
			sizeof_trace_frame_event_info());
	if (frame.ev.has_exec_info) {
		tif.events.read((char*)&frame.begin_exec_info,
				sizeof_trace_frame_exec_info());
		int extra_reg_bytes;
		char extra_reg_format;
		tif.events.read(&extra_reg_format, sizeof(extra_reg_format));
		tif.events.read((char*)&extra_reg_bytes, sizeof(extra_reg_bytes));
		if (extra_reg_bytes > 0) {
			std::vector<byte> data;
			data.resize(extra_reg_bytes);
			tif.events.read((char*)data.data(), extra_reg_bytes);
			frame.recorded_extra_regs.set_to_raw_data(
				(ExtraRegisters::Format)extra_reg_format, data);
		} else {
			assert(extra_reg_format == ExtraRegisters::NONE);
			frame.recorded_extra_regs = ExtraRegisters();
		}
	} else {
		memset(&frame.begin_exec_info, 0, sizeof_trace_frame_exec_info());
		frame.recorded_extra_regs = ExtraRegisters();
	}
	tif.tick_time();
	assert(tif.time() == frame.global_time);
	return tif;
}

template<typename T>
static CompressedWriter& operator<<(CompressedWriter& out, const T& value)
{
	out.write(&value, sizeof(value));
	return out;
}

static void write_string(CompressedWriter& out, const char* str)
{
	out.write(str, strlen(str) + 1);
}

template<typename T>
static CompressedReader& operator>>(CompressedReader& in, T& value)
{
	in.read(&value, sizeof(value));
	return in;
}

template<int N>
static void read_string(CompressedReader& in, char(&str)[N])
{
	int size = N;
	char* s = str;
	while (size > 0) {
		in.read(s, 1);
		if (*s == 0) {
			return;
		}
		++s;
		--size;
	}
	abort();
}

TraceOfstream& operator<<(TraceOfstream& tof, const struct mmapped_file& map)
{
	tof.mmaps << map.time << map.tid << map.copied;
	write_string(tof.mmaps, map.filename);
        tof.mmaps << map.stat << map.start << map.end;
	return tof;
}

TraceIfstream& operator>>(TraceIfstream& tif, struct mmapped_file& map)
{
	tif.mmaps >> map.time >> map.tid >> map.copied;
	read_string(tif.mmaps, map.filename);
	tif.mmaps >> map.stat >> map.start >> map.end;
	return tif;
}

static ostream& operator<<(ostream& out, const CharpVector& v)
{
	assert(!v.back());
	out << v.size() - 1 << endl;
	for (auto it = v.begin(); *it && it != v.end(); ++it) {
		out << *it << '\0';
	}
	return out;
}

static istream& operator>>(istream& in, CharpVector& v)
{
	size_t len;
	in >> len;
	in.ignore(1);
	v.reserve(len + 1);
	for (size_t i = 0; i < len; ++i) {
		char buf[PATH_MAX];
		in.getline(buf, sizeof(buf), '\0');
		v.push_back(strdup(buf));
	}
	v.push_back(nullptr);
	return in;
}

TraceOfstream& operator<<(TraceOfstream& tof, const struct args_env& ae)
{
	ofstream out(tof.args_env_path());

	assert(out.good());

	out << ae.cwd << '\0';
	out << ae.argv;
	out << ae.envp;
	return tof;
}

TraceIfstream& operator>>(TraceIfstream& tif, struct args_env& ae)
{
	ifstream in(tif.args_env_path());

	assert(in.good());

	char buf[PATH_MAX];
	in.getline(buf, sizeof(buf), '\0');
	ae.cwd = buf;

	in >> ae.argv;

	assert(in.good());

	ae.exe_image = ae.argv[0];
	in >> ae.envp;
	return tif;
}

TraceOfstream& operator<<(TraceOfstream& tof, const struct raw_data& d)
{
	tof.data_header << d.global_time << d.ev.encoded
			<< d.addr << d.data.size();
	tof.data.write((const char*)d.data.data(), d.data.size());
	return tof;
}

TraceIfstream& operator>>(TraceIfstream& tif, struct raw_data& d)
{
	size_t num_bytes;
	tif.data_header >> d.global_time >> d.ev.encoded >> d.addr
			>> num_bytes;
	d.data.resize(num_bytes);
	tif.data.read((char*)d.data.data(), num_bytes);
	return tif;
}

bool
TraceIfstream::read_raw_data_for_frame(const struct trace_frame& frame,
		                       struct raw_data& d)
{
	while (!data_header.at_end()) {
		uint32_t global_time;
		EncodedEvent ev;
		data_header.save_state();
		data_header >> global_time >> ev.encoded;
		data_header.restore_state();
		if (global_time == frame.global_time) {
			assert(ev == frame.ev);
			*this >> d;
			return true;
		}
		if (global_time > frame.global_time) {
			return false;
		}
	}
	return false;
}

void
TraceOfstream::close()
{
	events.close();
	data.close();
	data_header.close();
	mmaps.close();
}

/*static*/ TraceOfstream::shr_ptr
TraceOfstream::create(const string& exe_path)
{
	ensure_default_rr_trace_dir();

	// Find a unique trace directory name.
	int nonce = 0;
	int ret;
	string dir;
	do {
		stringstream ss;
		ss << trace_save_dir() << "/" << basename(exe_path.c_str())
		   << "-" << nonce++;
		dir = ss.str();
		ret = mkdir(dir.c_str(), S_IRWXU | S_IRWXG);
	} while (ret && EEXIST == errno);

	if (ret) {
		FATAL() <<"Unable to create trace directory `"<< dir <<"'";
	}

	shr_ptr trace(new TraceOfstream(dir));

	string version_path = trace->version_path();
	fstream version(version_path.c_str(), fstream::out);
	if (!version.good()) {
		FATAL() <<"Unable to create "<< version_path;
	}
	version << TRACE_VERSION << endl;

	string link_name = latest_trace_symlink();
	// Try to update the symlink to |trace|.  We only try attempt
	// to set the symlink once.  If the link is re-created after
	// we |unlink()| it, then another rr process is racing with us
	// and it "won".  The link is then valid and points at some
	// very-recent trace, so that's good enough.
	unlink(link_name.c_str());
	ret = symlink(trace->trace_dir.c_str(), link_name.c_str());
	if (!(0 == ret || EEXIST == ret)) {
		FATAL() <<"Failed to update symlink `"<< link_name
			<<"' to `"<< trace->trace_dir <<"'.";
	}

	if (!probably_not_interactive(STDOUT_FILENO)) {
		printf(
"rr: Saving the execution of `%s' to trace directory `%s'.\n",
			exe_path.c_str(), trace->trace_dir.c_str());
	}
	return trace;
}

TraceIfstream::shr_ptr
TraceIfstream::clone()
{
	shr_ptr stream(new TraceIfstream(*this));
	return stream;
}

struct trace_frame
TraceIfstream::peek_frame()
{
	struct trace_frame frame;
	events.save_state();
	auto saved_time = global_time;
	*this >> frame;
	events.restore_state();
	global_time = saved_time;
	return frame;
}

struct trace_frame
TraceIfstream::peek_to(pid_t pid, EventType type, int state)
{
	struct trace_frame frame;
	events.save_state();
	auto saved_time = global_time;
	while (good() && !at_end()) {
		*this >> frame;
		if (frame.tid == pid
		    && frame.ev.type == type
		    && frame.ev.state == state) {
			events.restore_state();
			global_time = saved_time;
			return frame;
		}
	}
	FATAL() <<"Unable to find requested frame in stream";
	// Unreachable
	return frame;
}

void
TraceIfstream::rewind()
{
	events.rewind();
	data.rewind();
	data_header.rewind();
	mmaps.rewind();
	global_time = 0;
	assert(good());
}

/*static*/ TraceIfstream::shr_ptr
TraceIfstream::open(int argc, char** argv)
{
	shr_ptr trace(new TraceIfstream(0 == argc ?
					latest_trace_symlink() : argv[0]));
	string path = trace->version_path();
	fstream vfile(path.c_str(), fstream::in);
	if (!vfile.good()) {
		fprintf(stderr,
"\n"
"rr: error: Version file for recorded trace `%s' not found.  Did you record\n"
"           `%s' with an older version of rr?  If so, you'll need to replay\n"
"           `%s' with that older version.  Otherwise, your trace is\n"
"           likely corrupted.\n"
"\n",
			path.c_str(), path.c_str(), path.c_str());
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
			path.c_str(), version, TRACE_VERSION,
			path.c_str(), path.c_str());
		exit(EX_DATAERR);
	}
	return trace;
}

uint64_t
TraceIfstream::uncompressed_bytes() const
{
	return events.uncompressed_bytes()
		+ data.uncompressed_bytes()
		+ data_header.uncompressed_bytes()
		+ mmaps.uncompressed_bytes();
}

uint64_t
TraceIfstream::compressed_bytes() const
{
	return events.compressed_bytes()
		+ data.compressed_bytes()
		+ data_header.compressed_bytes()
		+ mmaps.compressed_bytes();
}
