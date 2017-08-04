/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "TraceStream.h"

#include <inttypes.h>
#include <limits.h>
#include <sysexits.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include "AddressSpace.h"
#include "RecordSession.h"
#include "RecordTask.h"
#include "TaskishUid.h"
#include "kernel_supplement.h"
#include "log.h"
#include "util.h"

using namespace std;

namespace rr {

//
// This represents the format and layout of recorded traces.  This
// version number doesn't track the rr version number, because changes
// to the trace format will be rare.
//
// NB: if you *do* change the trace format for whatever reason, you
// MUST increment this version number.  Otherwise users' old traces
// will become unreplayable and they won't know why.
//
#define TRACE_VERSION 82

struct SubstreamData {
  const char* name;
  size_t block_size;
  int threads;
};

static SubstreamData substreams[TraceStream::SUBSTREAM_COUNT] = {
  { "events", 1024 * 1024, 1 }, { "data_header", 1024 * 1024, 1 },
  { "data", 1024 * 1024, 0 },   { "mmaps", 64 * 1024, 1 },
  { "tasks", 64 * 1024, 1 },    { "generic", 64 * 1024, 1 },
};

static const SubstreamData& substream(TraceStream::Substream s) {
  if (!substreams[TraceStream::RAW_DATA].threads) {
    substreams[TraceStream::RAW_DATA].threads = min(8, get_num_cpus());
  }
  return substreams[s];
}

static TraceStream::Substream operator++(TraceStream::Substream& s) {
  s = (TraceStream::Substream)(s + 1);
  return s;
}

static bool file_exists(const string& file) {
  struct stat dummy;
  return !file.empty() && stat(file.c_str(), &dummy) == 0
      && S_ISREG(dummy.st_mode);
}

static bool dir_exists(const string& dir) {
  struct stat dummy;
  return !dir.empty() && stat(dir.c_str(), &dummy) == 0
      && S_ISDIR(dummy.st_mode);
}

static string default_rr_trace_dir() {
  static string cached_dir;

  if (!cached_dir.empty()) {
    return cached_dir;
  }

  string dot_dir;
  const char* home = getenv("HOME");
  if (home) {
    dot_dir = string(home) + "/.rr";
  }
  string xdg_dir;
  const char* xdg_data_home = getenv("XDG_DATA_HOME");
  if (xdg_data_home) {
    xdg_dir = string(xdg_data_home) + "/rr";
  } else if (home) {
    xdg_dir = string(home) + "/.local/share/rr";
  }

  // If XDG dir does not exist but ~/.rr does, prefer ~/.rr for backwards
  // compatibility.
  if (dir_exists(xdg_dir)) {
    cached_dir = xdg_dir;
  } else if (dir_exists(dot_dir)) {
    cached_dir = dot_dir;
  } else if (!xdg_dir.empty()) {
    cached_dir = xdg_dir;
  } else {
    cached_dir = "/tmp/rr";
  }

  return cached_dir;
}

static string trace_save_dir() {
  const char* output_dir = getenv("_RR_TRACE_DIR");
  return output_dir ? output_dir : default_rr_trace_dir();
}

static string latest_trace_symlink() {
  return trace_save_dir() + "/latest-trace";
}

static void ensure_dir(const string& dir, mode_t mode) {
  string d = dir;
  while (!d.empty() && d[d.length() - 1] == '/') {
    d = d.substr(0, d.length() - 1);
  }

  struct stat st;
  if (0 > stat(d.c_str(), &st)) {
    if (errno != ENOENT) {
      FATAL() << "Error accessing trace directory `" << dir << "'";
    }

    size_t last_slash = d.find_last_of('/');
    if (last_slash == string::npos || last_slash == 0) {
      FATAL() << "Can't find trace directory `" << dir << "'";
    }
    ensure_dir(d.substr(0, last_slash), mode);

    // Allow for a race condition where someone else creates the directory
    if (0 > mkdir(d.c_str(), mode) && errno != EEXIST) {
      FATAL() << "Can't create trace directory `" << dir << "'";
    }
    if (0 > stat(d.c_str(), &st)) {
      FATAL() << "Can't stat trace directory `" << dir << "'";
    }
  }

  if (!(S_IFDIR & st.st_mode)) {
    FATAL() << "`" << dir << "' exists but isn't a directory.";
  }
  if (access(d.c_str(), W_OK)) {
    FATAL() << "Can't write to `" << dir << "'.";
  }
}

/**
 * Create the default ~/.rr directory if it doesn't already exist.
 */
static void ensure_default_rr_trace_dir() {
  ensure_dir(default_rr_trace_dir(), S_IRWXU);
}

TraceStream::TraceStream(const string& trace_dir, FrameTime initial_time)
    : trace_dir(real_path(trace_dir)), global_time(initial_time) {}

string TraceStream::file_data_clone_file_name(const TaskUid& tuid) {
  stringstream ss;
  ss << trace_dir << "/cloned_data_" << tuid.tid() << "_" << tuid.serial();
  return ss.str();
}

string TraceStream::path(Substream s) {
  return trace_dir + "/" + substream(s).name;
}

size_t TraceStream::mmaps_block_size() { return substreams[MMAPS].block_size; }

bool TraceWriter::good() const {
  for (auto& w : writers) {
    if (!w->good()) {
      return false;
    }
  }
  return true;
}

bool TraceReader::good() const {
  for (auto& r : readers) {
    if (!r->good()) {
      return false;
    }
  }
  return true;
}

struct BasicInfo {
  FrameTime global_time;
  pid_t tid_;
  EncodedEvent ev;
  Ticks ticks_;
  double monotonic_sec;
};

void TraceWriter::write_frame(const TraceFrame& frame) {
  auto& events = writer(EVENTS);

  BasicInfo basic_info;
  memset(&basic_info, 0, sizeof(BasicInfo));
  basic_info.global_time = frame.time();
  basic_info.tid_ = frame.tid();
  basic_info.ev = frame.event().encode();
  basic_info.ticks_ = frame.ticks();
  basic_info.monotonic_sec = frame.monotonic_time();
  events << basic_info;
  if (!events.good()) {
    FATAL() << "Tried to save " << sizeof(basic_info)
            << " bytes to the trace, but failed";
  }
  if (frame.event().has_exec_info() == HAS_EXEC_INFO) {
    events << (char)frame.regs().arch();
    // Avoid dynamic allocation and copy
    auto raw_regs = frame.regs().get_ptrace_for_self_arch();
    events.write(raw_regs.data, raw_regs.size);
    if (!events.good()) {
      FATAL() << "Tried to save registers to the trace, but failed";
    }

    int extra_reg_bytes = frame.extra_regs().data_size();
    char extra_reg_format = (char)frame.extra_regs().format();
    events << extra_reg_format << extra_reg_bytes;
    if (!events.good()) {
      FATAL() << "Tried to save "
              << sizeof(extra_reg_bytes) + sizeof(extra_reg_format)
              << " bytes to the trace, but failed";
    }
    if (extra_reg_bytes > 0) {
      events.write((const char*)frame.extra_regs().data_bytes(),
                   extra_reg_bytes);
      if (!events.good()) {
        FATAL() << "Tried to save " << extra_reg_bytes
                << " bytes to the trace, but failed";
      }
    }
  }

  tick_time();
}

TraceFrame TraceReader::read_frame() {
  // Read the common event info first, to see if we also have
  // exec info to read.
  auto& events = reader(EVENTS);
  BasicInfo basic_info;
  events >> basic_info;
  TraceFrame frame(basic_info.global_time, basic_info.tid_,
                   Event(basic_info.ev), basic_info.ticks_,
                   basic_info.monotonic_sec);
  if (frame.event().has_exec_info() == HAS_EXEC_INFO) {
    char a;
    events >> a;
    uint8_t buf[sizeof(X64Arch::user_regs_struct)];
    frame.recorded_regs.set_arch((SupportedArch)a);
    switch (frame.recorded_regs.arch()) {
      case x86:
        events.read(buf, sizeof(X86Arch::user_regs_struct));
        frame.recorded_regs.set_from_ptrace_for_arch(
            x86, buf, sizeof(X86Arch::user_regs_struct));
        break;
      case x86_64:
        events.read(buf, sizeof(X64Arch::user_regs_struct));
        frame.recorded_regs.set_from_ptrace_for_arch(
            x86_64, buf, sizeof(X64Arch::user_regs_struct));
        break;
      default:
        FATAL() << "Unknown arch";
    }

    int extra_reg_bytes;
    char extra_reg_format;
    events >> extra_reg_format >> extra_reg_bytes;
    if (extra_reg_bytes > 0) {
      vector<uint8_t> data;
      data.resize(extra_reg_bytes);
      events.read((char*)data.data(), extra_reg_bytes);
      bool ok = frame.recorded_extra_regs.set_to_raw_data(
          frame.event().arch(), (ExtraRegisters::Format)extra_reg_format, data,
          xsave_layout_from_trace(cpuid_records()));
      if (!ok) {
        FATAL() << "Invalid XSAVE data in trace";
      }
    } else {
      assert(extra_reg_format == ExtraRegisters::NONE);
      frame.recorded_extra_regs = ExtraRegisters(frame.event().arch());
    }
  }

  tick_time();
  assert(time() == frame.time());
  return frame;
}

void TraceWriter::write_task_event(const TraceTaskEvent& event) {
  auto& tasks = writer(TASKS);
  tasks << global_time << (char)event.type() << event.tid();
  switch (event.type()) {
    case TraceTaskEvent::CLONE:
      tasks << event.parent_tid() << event.own_ns_tid() << event.clone_flags();
      break;
    case TraceTaskEvent::EXEC:
      tasks << event.file_name() << event.cmd_line();
      break;
    case TraceTaskEvent::EXIT:
      tasks << event.exit_status_;
      break;
    case TraceTaskEvent::NONE:
      assert(0 && "Writing NONE TraceTaskEvent");
      break;
  }
}

TraceTaskEvent TraceReader::read_task_event() {
  auto& tasks = reader(TASKS);
  TraceTaskEvent r;
  FrameTime time;
  char type = TraceTaskEvent::NONE;
  tasks >> time >> type >> r.tid_;
  r.type_ = (TraceTaskEvent::Type)type;
  switch (r.type()) {
    case TraceTaskEvent::CLONE:
      tasks >> r.parent_tid_ >> r.own_ns_tid_ >> r.clone_flags_;
      break;
    case TraceTaskEvent::EXEC:
      tasks >> r.file_name_ >> r.cmd_line_;
      break;
    case TraceTaskEvent::EXIT:
      tasks >> r.exit_status_;
      break;
    case TraceTaskEvent::NONE:
      // Should be EOF only
      assert(!tasks.good());
      break;
    default:
      assert(false && "Corrupt Trace?");
  }
  return r;
}

static string base_file_name(const string& file_name) {
  size_t last_slash = file_name.rfind('/');
  return (last_slash != file_name.npos) ? file_name.substr(last_slash + 1)
                                        : file_name;
}

string TraceWriter::try_hardlink_file(const string& file_name) {
  char count_str[20];
  sprintf(count_str, "%d", mmap_count);

  string path =
      string("mmap_hardlink_") + count_str + "_" + base_file_name(file_name);
  int ret = link(file_name.c_str(), (dir() + "/" + path).c_str());
  if (ret < 0) {
    // maybe tried to link across filesystems?
    return file_name;
  }
  return path;
}

bool TraceWriter::try_clone_file(RecordTask* t, const string& file_name,
                                 string* new_name) {
  if (!t->session().use_file_cloning()) {
    return false;
  }

  char count_str[20];
  sprintf(count_str, "%d", mmap_count);

  string path =
      string("mmap_clone_") + count_str + "_" + base_file_name(file_name);

  ScopedFd src(file_name.c_str(), O_RDONLY);
  if (!src.is_open()) {
    return false;
  }
  string dest_path = dir() + "/" + path;
  ScopedFd dest(dest_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0700);
  if (!dest.is_open()) {
    return false;
  }

  int ret = ioctl(dest, BTRFS_IOC_CLONE, src.get());
  if (ret < 0) {
    // maybe not on the same filesystem, or filesystem doesn't support clone?
    unlink(dest_path.c_str());
    return false;
  }

  *new_name = path;
  return true;
}

TraceWriter::RecordInTrace TraceWriter::write_mapped_region(
    RecordTask* t, const KernelMapping& km, const struct stat& stat,
    MappingOrigin origin) {
  auto& mmaps = writer(MMAPS);
  TraceReader::MappedDataSource source;
  string backing_file_name;
  if (origin == REMAP_MAPPING || origin == PATCH_MAPPING) {
    source = TraceReader::SOURCE_ZERO;
  } else if (km.fsname().find("/SYSV") == 0) {
    source = TraceReader::SOURCE_TRACE;
  } else if (origin == SYSCALL_MAPPING &&
             (km.inode() == 0 || km.fsname() == "/dev/zero (deleted)")) {
    source = TraceReader::SOURCE_ZERO;
  } else if (origin == RR_BUFFER_MAPPING) {
    source = TraceReader::SOURCE_ZERO;
  } else if ((km.flags() & MAP_PRIVATE) &&
             try_clone_file(t, km.fsname(), &backing_file_name)) {
    source = TraceReader::SOURCE_FILE;
  } else if (should_copy_mmap_region(km, stat) &&
             files_assumed_immutable.find(make_pair(
                 stat.st_dev, stat.st_ino)) == files_assumed_immutable.end()) {
    source = TraceReader::SOURCE_TRACE;
  } else {
    source = TraceReader::SOURCE_FILE;
    // should_copy_mmap_region's heuristics determined it was OK to just map
    // the file here even if it's MAP_SHARED. So try cloning again to avoid
    // the possibility of the file changing between recording and replay.
    if (!try_clone_file(t, km.fsname(), &backing_file_name)) {
      // Try hardlinking file into the trace directory. This will avoid
      // replay failures if the original file is deleted or replaced (but not
      // if it is overwritten in-place). If try_hardlink_file fails it
      // just returns the original file name.
      // A relative backing_file_name is relative to the trace directory.
      backing_file_name = try_hardlink_file(km.fsname());
      files_assumed_immutable.insert(make_pair(stat.st_dev, stat.st_ino));
    }
  }
  mmaps << global_time << source << km.start() << km.end() << km.fsname()
        << km.device() << km.inode() << km.prot() << km.flags()
        << km.file_offset_bytes() << backing_file_name << (uint32_t)stat.st_mode
        << (uint32_t)stat.st_uid << (uint32_t)stat.st_gid
        << (int64_t)stat.st_size << (int64_t)stat.st_mtime;
  ++mmap_count;
  return source == TraceReader::SOURCE_TRACE ? RECORD_IN_TRACE
                                             : DONT_RECORD_IN_TRACE;
}

void TraceWriter::write_mapped_region_to_alternative_stream(
    CompressedWriter& mmaps, const MappedData& data, const KernelMapping& km) {
  mmaps << data.time << data.source << km.start() << km.end() << km.fsname()
        << km.device() << km.inode() << km.prot() << km.flags()
        << km.file_offset_bytes() << data.file_name
        // Indicate that we have no statbuf-data
        << (uint32_t)0 << (uint32_t)0 << (uint32_t)0 << data.file_size_bytes
        << (int64_t)0;
}

KernelMapping TraceReader::read_mapped_region(MappedData* data, bool* found,
                                              ValidateSourceFile validate,
                                              TimeConstraint time_constraint) {
  if (found) {
    *found = false;
  }

  auto& mmaps = reader(MMAPS);
  if (mmaps.at_end()) {
    return KernelMapping();
  }

  FrameTime time;
  if (time_constraint == CURRENT_TIME_ONLY) {
    mmaps.save_state();
    mmaps >> time;
    mmaps.restore_state();
    if (time != global_time) {
      return KernelMapping();
    }
  }

  string original_file_name;
  string backing_file_name;
  MappedDataSource source;
  remote_ptr<void> start, end;
  dev_t device;
  ino_t inode;
  int prot, flags;
  uint32_t uid, gid, mode;
  uint64_t file_offset_bytes;
  int64_t mtime, file_size;
  mmaps >> time >> source >> start >> end >> original_file_name >> device >>
      inode >> prot >> flags >> file_offset_bytes >> backing_file_name >>
      mode >> uid >> gid >> file_size >> mtime;
  bool has_stat_buf = mode != 0 || uid != 0 || gid != 0 || mtime != 0;
  assert(time_constraint == ANY_TIME || time == global_time);
  if (data) {
    data->time = time;
    data->source = source;
    if (data->source == SOURCE_FILE) {
      static const string clone_prefix("mmap_clone_");
      bool is_clone =
          backing_file_name.substr(0, clone_prefix.size()) == clone_prefix;
      if (backing_file_name[0] != '/') {
        backing_file_name = dir() + "/" + backing_file_name;
      }
      if (!is_clone && validate == VALIDATE && has_stat_buf) {
        struct stat backing_stat;
        if (stat(backing_file_name.c_str(), &backing_stat)) {
          FATAL() << "Failed to stat " << backing_file_name
                  << ": replay is impossible";
        }
        if (backing_stat.st_ino != inode || backing_stat.st_mode != mode ||
            backing_stat.st_uid != uid || backing_stat.st_gid != gid ||
            backing_stat.st_size != file_size ||
            backing_stat.st_mtime != mtime) {
          LOG(error) << "Metadata of " << original_file_name
                     << " changed: replay divergence likely, but continuing "
                        "anyway. inode: "
                     << backing_stat.st_ino << "/" << inode
                     << "; mode: " << backing_stat.st_mode << "/" << mode
                     << "; uid: " << backing_stat.st_uid << "/" << uid
                     << "; gid: " << backing_stat.st_gid << "/" << gid
                     << "; size: " << backing_stat.st_size << "/" << file_size
                     << "; mtime: " << backing_stat.st_mtime << "/" << mtime;
        }
      }
      data->file_name = backing_file_name;
      data->data_offset_bytes = file_offset_bytes;
    } else {
      data->data_offset_bytes = 0;
    }
    data->file_size_bytes = file_size;
  }
  if (found) {
    *found = true;
  }
  return KernelMapping(start, end, original_file_name, device, inode, prot,
                       flags, file_offset_bytes);
}

void TraceWriter::write_raw(pid_t rec_tid, const void* d, size_t len,
                            remote_ptr<void> addr) {
  auto& data = writer(RAW_DATA);
  auto& data_header = writer(RAW_DATA_HEADER);
  data_header << global_time << rec_tid << addr.as_int() << len;
  data.write(d, len);
}

TraceReader::RawData TraceReader::read_raw_data() {
  auto& data = reader(RAW_DATA);
  auto& data_header = reader(RAW_DATA_HEADER);
  FrameTime time;
  RawData d;
  size_t num_bytes;
  data_header >> time >> d.rec_tid >> d.addr >> num_bytes;
  assert(time == global_time);
  d.data.resize(num_bytes);
  data.read((char*)d.data.data(), num_bytes);
  return d;
}

bool TraceReader::read_raw_data_for_frame(const TraceFrame& frame, RawData& d) {
  auto& data_header = reader(RAW_DATA_HEADER);
  if (data_header.at_end()) {
    return false;
  }
  FrameTime time;
  data_header.save_state();
  data_header >> time;
  data_header.restore_state();
  assert(time >= frame.time());
  if (time > frame.time()) {
    return false;
  }
  d = read_raw_data();
  return true;
}

void TraceWriter::write_generic(const void* d, size_t len) {
  auto& generic = writer(GENERIC);
  generic << global_time << len;
  generic.write(d, len);
}

void TraceReader::read_generic(vector<uint8_t>& out) {
  auto& generic = reader(GENERIC);
  FrameTime time;
  size_t num_bytes;
  generic >> time >> num_bytes;
  assert(time == global_time);
  out.resize(num_bytes);
  generic.read((char*)out.data(), num_bytes);
}

bool TraceReader::read_generic_for_frame(const TraceFrame& frame,
                                         vector<uint8_t>& out) {
  auto& generic = reader(GENERIC);
  if (generic.at_end()) {
    return false;
  }
  FrameTime time;
  generic.save_state();
  generic >> time;
  generic.restore_state();
  assert(time >= frame.time());
  if (time > frame.time()) {
    return false;
  }
  read_generic(out);
  return true;
}

void TraceWriter::close() {
  for (auto& w : writers) {
    w->close();
  }
}

static string make_trace_dir(const string& exe_path) {
  ensure_default_rr_trace_dir();

  // Find a unique trace directory name.
  int nonce = 0;
  int ret;
  string dir;
  do {
    stringstream ss;
    ss << trace_save_dir() << "/" << basename(exe_path.c_str()) << "-"
       << nonce++;
    dir = ss.str();
    ret = mkdir(dir.c_str(), S_IRWXU | S_IRWXG);
  } while (ret && EEXIST == errno);

  if (ret) {
    FATAL() << "Unable to create trace directory `" << dir << "'";
  }

  return dir;
}

TraceWriter::TraceWriter(const std::string& file_name, int bind_to_cpu,
                         bool has_cpuid_faulting)
    : TraceStream(make_trace_dir(file_name),
                  // Somewhat arbitrarily start the
                  // global time from 1.
                  1),
      mmap_count(0),
      supports_file_data_cloning_(false) {
  this->bind_to_cpu = bind_to_cpu;

  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    writers[s] = unique_ptr<CompressedWriter>(new CompressedWriter(
        path(s), substream(s).block_size, substream(s).threads));
  }

  // Add a random UUID to the trace metadata. This lets tools identify a trace
  // easily.
  uint32_t uuid[4];
  good_random(uuid, sizeof(uuid));

  string ver_path = version_path();
  ScopedFd version_fd(ver_path.c_str(), O_RDWR | O_CREAT, 0600);
  if (!version_fd.is_open()) {
    FATAL() << "Unable to create " << ver_path;
  }
  char buf[100];
  sprintf(buf, "%d\n%08x%08x%08x%08x\n", TRACE_VERSION, uuid[0], uuid[1],
          uuid[2], uuid[3]);
  ssize_t buf_len = strlen(buf);
  if (write(version_fd, buf, buf_len) != buf_len) {
    FATAL() << "Unable to write " << ver_path;
  }

  // Test if file data cloning is supported
  string version_clone_path = trace_dir + "/tmp_clone";
  ScopedFd version_clone_fd(version_clone_path.c_str(), O_WRONLY | O_CREAT,
                            0600);
  if (!version_clone_fd.is_open()) {
    FATAL() << "Unable to create " << version_clone_path;
  }
  btrfs_ioctl_clone_range_args clone_args;
  clone_args.src_fd = version_fd;
  clone_args.src_offset = 0;
  clone_args.src_length = buf_len;
  clone_args.dest_offset = 0;
  if (ioctl(version_clone_fd, BTRFS_IOC_CLONE_RANGE, &clone_args) == 0) {
    supports_file_data_cloning_ = true;
  }
  unlink(version_clone_path.c_str());

  if (!probably_not_interactive(STDOUT_FILENO)) {
    printf("rr: Saving execution to trace directory `%s'.\n",
           trace_dir.c_str());
  }

  write_generic(&bind_to_cpu, sizeof(bind_to_cpu));
  write_generic(&has_cpuid_faulting, sizeof(has_cpuid_faulting));
}

void TraceWriter::make_latest_trace() {
  string link_name = latest_trace_symlink();
  // Try to update the symlink to |this|.  We only try attempt
  // to set the symlink once.  If the link is re-created after
  // we |unlink()| it, then another rr process is racing with us
  // and it "won".  The link is then valid and points at some
  // very-recent trace, so that's good enough.
  unlink(link_name.c_str());
  int ret = symlink(trace_dir.c_str(), link_name.c_str());
  if (ret < 0 && errno != EEXIST) {
    FATAL() << "Failed to update symlink `" << link_name << "' to `"
            << trace_dir << "'.";
  }
}

TraceFrame TraceReader::peek_frame() {
  auto& events = reader(EVENTS);
  events.save_state();
  auto saved_time = global_time;
  TraceFrame frame;
  if (!at_end()) {
    frame = read_frame();
  }
  events.restore_state();
  global_time = saved_time;
  return frame;
}

void TraceReader::rewind() {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    reader(s).rewind();
  }
  global_time = 0;
  assert(good());
}

TraceReader::TraceReader(const string& dir)
    : TraceStream(dir.empty() ? latest_trace_symlink() : dir, 1) {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    readers[s] = unique_ptr<CompressedReader>(new CompressedReader(path(s)));
  }

  string path = version_path();
  if (!file_exists(path)) {
    fprintf(
        stderr,
        "rr: warning: No traces have been recorded so far.\n"
        "\n");
    exit(EX_DATAERR);
  }

  fstream vfile(path.c_str(), fstream::in);
  if (!vfile.good()) {
    fprintf(
        stderr,
        "\n"
        "rr: error: Version file for recorded trace `%s' not found.  Did you "
        "record\n"
        "           `%s' with an older version of rr?  If so, you'll need to "
        "replay\n"
        "           `%s' with that older version.  Otherwise, your trace is\n"
        "           likely corrupted.\n"
        "\n",
        path.c_str(), path.c_str(), path.c_str());
    exit(EX_DATAERR);
  }
  int version = 0;
  vfile >> version;
  if (vfile.fail() || TRACE_VERSION != version) {
    fprintf(stderr, "\n"
                    "rr: error: Recorded trace `%s' has an incompatible "
                    "version %d; expected\n"
                    "           %d.  Did you record `%s' with an older version "
                    "of rr?  If so,\n"
                    "           you'll need to replay `%s' with that older "
                    "version.  Otherwise,\n"
                    "           your trace is likely corrupted.\n"
                    "\n",
            path.c_str(), version, TRACE_VERSION, path.c_str(), path.c_str());
    exit(EX_DATAERR);
  }

  vector<uint8_t> bind_to_cpu_bytes;
  read_generic(bind_to_cpu_bytes);
  assert(bind_to_cpu_bytes.size() == sizeof(bind_to_cpu));
  memcpy(&bind_to_cpu, bind_to_cpu_bytes.data(), sizeof(bind_to_cpu));

  vector<uint8_t> uses_cpuid_faulting_bytes;
  read_generic(uses_cpuid_faulting_bytes);
  assert(uses_cpuid_faulting_bytes.size() == sizeof(trace_uses_cpuid_faulting));
  memcpy(&trace_uses_cpuid_faulting, uses_cpuid_faulting_bytes.data(),
         sizeof(trace_uses_cpuid_faulting));

  vector<uint8_t> cpuid_records_bytes;
  read_generic(cpuid_records_bytes);
  size_t len = cpuid_records_bytes.size() / sizeof(CPUIDRecord);
  assert(cpuid_records_bytes.size() == len * sizeof(CPUIDRecord));
  cpuid_records_.resize(len);
  memcpy(cpuid_records_.data(), cpuid_records_bytes.data(),
         cpuid_records_bytes.size());

  // Set the global time at 0, so that when we tick it for the first
  // event, it matches the initial global time at recording, 1.
  global_time = 0;
}

/**
 * Create a copy of this stream that has exactly the same
 * state as 'other', but for which mutations of this
 * clone won't affect the state of 'other' (and vice versa).
 */
TraceReader::TraceReader(const TraceReader& other)
    : TraceStream(other.dir(), other.time()) {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    readers[s] =
        unique_ptr<CompressedReader>(new CompressedReader(other.reader(s)));
  }

  bind_to_cpu = other.bind_to_cpu;
  trace_uses_cpuid_faulting = other.trace_uses_cpuid_faulting;
  cpuid_records_ = other.cpuid_records_;
}

TraceReader::~TraceReader() {}

uint64_t TraceReader::uncompressed_bytes() const {
  uint64_t total = 0;
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    total += reader(s).uncompressed_bytes();
  }
  return total;
}

uint64_t TraceReader::compressed_bytes() const {
  uint64_t total = 0;
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    total += reader(s).compressed_bytes();
  }
  return total;
}

} // namespace rr
