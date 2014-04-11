/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

//#define DEBUGTAG "Task"

#include "task.h"

#include <errno.h>
#include <linux/kdev_t.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <limits>
#include <set>

#include "preload/syscall_buffer.h"

#include "dbg.h"
#include "hpc.h"
#include "util.h"

using namespace std;

static Task::Map tasks;
static Task::PrioritySet tasks_by_priority;

/*static*/ const byte AddressSpace::breakpoint_insn;
/*static*/ AddressSpace::Set AddressSpace::sas;

dev_t
FileId::dev_major() const { return is_real_device() ? MAJOR(device) : 0; }

dev_t
FileId::dev_minor() const { return is_real_device() ? MINOR(device) : 0; }

ino_t
FileId::disp_inode() const { return is_real_device() ? inode : 0; } 

const char*
FileId::special_name() const {
	switch (psdev) {
	case PSEUDODEVICE_ANONYMOUS: return "";
	case PSEUDODEVICE_HEAP: return "(heap)";
	case PSEUDODEVICE_NONE: return "";
	case PSEUDODEVICE_SCRATCH: return "";
	case PSEUDODEVICE_STACK: return "(stack)";
	case PSEUDODEVICE_SYSCALLBUF: return "(syscallbuf)";
	case PSEUDODEVICE_VDSO: return "(vdso)";
	}
	fatal("Not reached");
	return nullptr;
}

void
HasTaskSet::insert_task(Task* t)
{
	debug("adding %d to task set %p", t->tid, this);
	tasks.insert(t);
}

void
HasTaskSet::erase_task(Task* t) {
	debug("removing %d from task group %p", t->tid, this);
	tasks.erase(t);
}

FileId::FileId(dev_t dev_major, dev_t dev_minor, ino_t ino, PseudoDevice psdev)
	: device(MKDEV(dev_major, dev_minor)), inode(ino), psdev(psdev) { }

/*static*/ MappableResource
MappableResource::syscallbuf(pid_t tid, int fd)
 {
	 char path[PATH_MAX];
	 format_syscallbuf_shmem_path(tid, path);
	 struct stat st;
	 if (fstat(fd, &st)) {
		 fatal("Failed to fstat(%d) (%s)", fd, path);
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

	shr_ptr clone() {
		return shr_ptr(new Breakpoint(*this));
	}

	void ref(TrapType which) {
		assert(internal_count >= 0 && user_count >= 0);
		++*counter(which);
	}
	int unref(TrapType which) {
		assert(internal_count >= 0 && user_count >= 0
		       && (internal_count > 0 || user_count > 0));
		--*counter(which);
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

	static shr_ptr create() {
		return shr_ptr(new Breakpoint());
	}

	// "Refcounts" of breakpoints set at |addr|.  The breakpoint
	// object must be unique since we have to save the overwritten
	// data, and we can't enforce the order in which breakpoints
	// are set/removed.
	int internal_count, user_count;
	byte overwritten_data;
	static_assert(sizeof(overwritten_data) ==
		      sizeof(AddressSpace::breakpoint_insn), 
		      "Must have the same size.");
private:
	Breakpoint() : internal_count(), user_count() { }
	Breakpoint(const Breakpoint& o)
		: internal_count(o.internal_count), user_count(o.user_count)
		, overwritten_data(o.overwritten_data) { }

	int* counter(TrapType which) {
		assert(TRAP_BKPT_INTERNAL == which || TRAP_BKPT_USER == which);
		int* p = TRAP_BKPT_USER == which ?
			 &user_count : &internal_count;
		assert(*p >= 0);
		return p;
	}
};

void
AddressSpace::brk(void* addr)
{
	debug("[%d] brk(%p)", get_global_time(), addr);

	assert(heap.start <= addr);
	if (addr == heap.end) {
		return;
	}

	update_heap(heap.start, addr);
	map(heap.start, heap.num_bytes(), heap.prot, heap.flags, heap.offset,
	    MappableResource::heap());
}

AddressSpace::shr_ptr
AddressSpace::clone()
{
	return shr_ptr(new AddressSpace(*this));
}

void
AddressSpace::dump() const
{
	fprintf(stderr, "  (heap: %p-%p)\n", heap.start, heap.end);
	for (auto it = mem.begin(); it != mem.end(); ++it) {
		const Mapping& m = it->first;
		const MappableResource& r = it->second;
		fprintf(stderr,
			"%08lx-%08lx %c%c%c%c %08llx %02llx:%02llx %-10ld %s %s (f:0x%x d:0x%llx i:%ld)\n",
			reinterpret_cast<long>(m.start),
			reinterpret_cast<long>(m.end),
			(PROT_READ & m.prot) ? 'r' : '-',
			(PROT_WRITE & m.prot) ? 'w' : '-',
			(PROT_EXEC & m.prot) ? 'x' : '-',
			(MAP_SHARED & m.flags) ? 's' : 'p',
			m.offset,
			r.id.dev_major(), r.id.dev_minor(), r.id.disp_inode(),
			r.fsname.c_str(), r.id.special_name(),
			m.flags, r.id.device, r.id.inode);
	}
}

TrapType
AddressSpace::get_breakpoint_type_at_ip(void* ip)
{
	void* addr = (byte*)ip - sizeof(breakpoint_insn);
	auto it = breakpoints.find(addr);
	return it == breakpoints.end() ? TRAP_NONE : it->second->type();
}

void
AddressSpace::map(void* addr, size_t num_bytes, int prot, int flags,
		  off64_t offset_bytes, const MappableResource& res)
{
	debug("[%d] mmap(%p, %u, %#x, %#x, %#llx)", get_global_time(),
	      addr, num_bytes, prot, flags, offset_bytes);

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
}

typedef AddressSpace::MemoryMap::value_type MappingResourcePair;
MappingResourcePair
AddressSpace::mapping_of(void* addr, size_t num_bytes) const
{
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
			     off64_t delta)
{
	return r.id.is_real_device() ? m.offset + delta : 0;
}

void
AddressSpace::protect(void* addr, size_t num_bytes, int prot)
{
	debug("[%d] mprotect(%p, %u, 0x%x)", get_global_time(),
	      addr, num_bytes, prot);

	Mapping last_overlap;
	auto protector = [this, prot, &last_overlap](
		const Mapping& m, const MappableResource& r,
		const Mapping& rem) {
		debug("  protecting (%p, %u) ...", rem.start, rem.num_bytes());

		mem.erase(m);
		debug("  erased (%p, %u, %#x) ...",
		      m.start, m.num_bytes(), m.prot);

		// If the first segment we protect underflows the
		// region, remap the underflow region with previous
		// prot.
		if (m.start < rem.start) {
			Mapping underflow(m.start, rem.start, m.prot, m.flags,
					  m.offset);
			mem[underflow] = r;
		}
		// Remap the overlapping region with the new prot.
		void* new_end = min(rem.end, m.end);
		Mapping overlap(rem.start, new_end, prot, m.flags,
				adjust_offset(r, m,
					      (byte*)rem.start - (byte*)m.start));
		mem[overlap] = r;
		last_overlap = overlap;

		// If the last segment we protect overflows the
		// region, remap the overflow region with previous
		// prot.
		if (rem.end < m.end) {
			Mapping overflow(rem.end, m.end, m.prot, m.flags,
					 adjust_offset(r, m,
						       (byte*)rem.end - (byte*)m.start));
			mem[overflow] = r;
		}
	};
	for_each_in_range(addr, num_bytes, protector, ITERATE_CONTIGUOUS);
	// All mappings that we altered which might need coalescing
	// are adjacent to |last_overlap|.
	coalesce_around(mem.find(last_overlap));
}

void
AddressSpace::remap(void* old_addr, size_t old_num_bytes,
		    void* new_addr, size_t new_num_bytes)
{
	debug("[%d] mremap(%p, %u, %p, %u)", get_global_time(),
	      old_addr, old_num_bytes, new_addr, new_num_bytes);

	auto mr = mapping_of(old_addr, old_num_bytes);
	const Mapping& m = mr.first;
	const MappableResource& r = mr.second;

	unmap(old_addr, old_num_bytes);
	if (0 == new_num_bytes) {
		return;
	}

	map_and_coalesce(Mapping((byte*)new_addr, new_num_bytes,
				 m.prot, m.flags,
				 adjust_offset(r, m,
					       ((byte*)old_addr - (byte*)m.start))),
			 r);
}

void
AddressSpace::remove_breakpoint(void* addr, TrapType type)
{
	auto it = breakpoints.find(addr);
	if (it == breakpoints.end() || !it->second
	    || it->second->unref(type) > 0) {
		return;
	}
	destroy_breakpoint(it);
}

void
AddressSpace::set_breakpoint(void* addr, TrapType type)
{
	auto it = breakpoints.find(addr);
	if (it == breakpoints.end()) {
		auto bp = Breakpoint::create();
		// Grab a random task from the VM so we can use its
		// read/write_mem() helpers.
		Task* t = *task_set().begin();
		t->read_mem(addr, &bp->overwritten_data);
		t->write_mem(addr, breakpoint_insn);

		auto it_and_is_new = breakpoints.insert(make_pair(addr, bp));
		assert(it_and_is_new.second);
		it = it_and_is_new.first;
	}
	it->second->ref(type);
}

void
AddressSpace::destroy_all_breakpoints()
{
	while (!breakpoints.empty()) {
		destroy_breakpoint(breakpoints.begin());
	}
}

void
AddressSpace::unmap(void* addr, ssize_t num_bytes)
{
	debug("[%d] munmap(%p, %u)", get_global_time(), addr, num_bytes);

	auto unmapper = [this](const Mapping& m, const MappableResource& r,
			       const Mapping& rem) {
		debug("  unmapping (%p, %u) ...", rem.start, rem.num_bytes());

		mem.erase(m);
		debug("  erased (%p, %u) ...", m.start, m.num_bytes());

		// If the first segment we unmap underflows the unmap
		// region, remap the underflow region.
		if (m.start < rem.start) {
			Mapping underflow(m.start, rem.start, m.prot, m.flags,
					  m.offset);
			mem[underflow] = r;
		}
		// If the last segment we unmap overflows the unmap
		// region, remap the overflow region.
		if (rem.end < m.end) {
			Mapping overflow(rem.end, m.end, m.prot, m.flags,
					 adjust_offset(r, m,
						       (byte*)rem.end - (byte*)m.start));
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
				const MappingResourcePair& right)
{
	const Mapping& mleft = left.first;
	const Mapping& mright = right.first;
	if (mleft.end != mright.start) {
		debug("    (not adjacent in memory)");
		return false;
	}
	if (mleft.flags != mright.flags || mleft.prot != mright.prot) {
		debug("    (flags or prot differ)");
		return false;
	}
	const MappableResource& rleft = left.second;
	const MappableResource& rright = right.second;
	if (rright.fsname.substr(0, strlen(PREFIX_FOR_EMPTY_MMAPED_REGIONS)) ==
	    PREFIX_FOR_EMPTY_MMAPED_REGIONS) {
		return true;
	}
	if (rleft != rright) {
		debug("    (not the same resource)");
		return false;
	}
	if (rleft.id.is_real_device()
	    && mleft.offset + mleft.num_bytes() != mright.offset) {
		debug("    (%lld + %d != %lld: offsets into real device aren't adjacent)",
		      mleft.offset, mleft.num_bytes(), mright.offset);
		return false;
	}
	debug("    adjacent!");
	return true;
}

/**
 * If (*left_m, left_r), (right_m, right_r) are adjacent (see
 * |is_adjacent_mapping()|), write a merged segment descriptor to
 * |*left_m| and return true.  Otherwise return false.
 */
static bool try_merge_adjacent(Mapping* left_m,
			       const MappableResource& left_r,
			       const Mapping& right_m,
			       const MappableResource& right_r)
{
	if (is_adjacent_mapping(MappingResourcePair(*left_m, left_r),
				MappingResourcePair(right_m, right_r))) {
		*left_m = Mapping(left_m->start, right_m.end,
				  right_m.prot, right_m.flags,
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
		: as(as), it(as->mem.begin()), phase(NO_PHASE) { }

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
		NO_PHASE, MERGING_CACHED, INITING_KERNEL, MERGING_KERNEL
	} phase;
};

void
VerifyAddressSpace::assert_segments_match(Task* t)
{
	assert(MERGING_KERNEL == phase);
	bool same_mapping = (m.start == km.start && m.end == km.end
			     && m.prot == km.prot
			     && m.flags == km.flags);
	if (!same_mapping) {
		log_err("cached mmap:");
		as->dump();
		log_err("/proc/%d/mmaps:", t->tid);
		print_process_mmap(t);

		assert_exec(t, same_mapping,
			    "\nCached mapping '%p-%p 0x%x f:0x%x'\n"
			    "    should be '%p-%p 0x%x f:0x%x'",
			    m.start, m.end, m.prot, m.flags,
			    km.start, km.end, km.prot, km.flags);
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
/*static*/ int
AddressSpace::check_segment_iterator(void* pvas, Task* t,
				     const struct map_iterator_data* data)
{
	VerifyAddressSpace* vas = static_cast<VerifyAddressSpace*>(pvas);
	const AddressSpace* as = vas->as;
	const struct mapped_segment_info& info = data->info;

	debug("examining /proc/maps segment '%p-%p 0x%x f:0x%x'",
	      info.start_addr, info.end_addr, info.prot, info.flags);

	// Merge adjacent cached mappings.
	if (vas->NO_PHASE == vas->phase) {
		assert(vas->it != as->mem.end());

		vas->phase = vas->MERGING_CACHED;
		// Start of next segment range to match.
		vas->m = vas->it->first.to_kernel();
		vas->r = vas->it->second.to_kernel();
		do {
			++vas->it;
		} while (vas->it != as->mem.end()
			 && try_merge_adjacent(&vas->m, vas->r,
					       vas->it->first.to_kernel(),
					       vas->it->second.to_kernel()));
		vas->phase = vas->INITING_KERNEL;
	}

	debug("  merged cached seg: '%p-%p 0x%x f:0x%x'",
	      vas->m.start, vas->m.end, vas->m.prot, vas->m.flags);

	// Merge adjacent kernel mappings.
	assert(info.flags == (info.flags & Mapping::checkable_flags_mask));
	Mapping km(info.start_addr, info.end_addr, info.prot, info.flags,
		   info.file_offset);
	MappableResource kr(FileId(info.dev_major, info.dev_minor,
				   info.inode), info.name);

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
		       || string::npos != kr.fsname.find(SHMEM_FS "/rr-emufs")
		       || string::npos != kr.fsname.find(SHMEM_FS2 "/rr-emufs"));
		vas->km = km;
		vas->phase = vas->MERGING_KERNEL;
		return CONTINUE_ITERATING;
	}
	if (vas->MERGING_KERNEL == vas->phase
	    && try_merge_adjacent(&vas->km, vas->r, km, kr)) {
		return CONTINUE_ITERATING;
	}

	// Merged as much as we can ... now the mappings must be
	// equal.
	vas->assert_segments_match(t);

	vas->phase = vas->NO_PHASE;
	return check_segment_iterator(pvas, t, data);
}

Mapping
AddressSpace::vdso() const
{
	assert(vdso_start_addr);
	return mapping_of(vdso_start_addr, 1).first;
}

void
AddressSpace::verify(Task* t) const
{
	assert(task_set().end() != task_set().find(t));

	VerifyAddressSpace vas(this);
	iterate_memory_map(t, check_segment_iterator, &vas,
			   kNeverReadSegment, NULL);

	assert(vas.MERGING_KERNEL == vas.phase);
	vas.assert_segments_match(t);
}

/*static*/ AddressSpace::shr_ptr
AddressSpace::create(Task* t)
{
	shr_ptr as(new AddressSpace());
	as->insert_task(t);
	iterate_memory_map(t, populate_address_space, as.get(),
			   kNeverReadSegment, NULL);
	assert(as->vdso_start_addr);
	return as;
}

AddressSpace::AddressSpace(const AddressSpace& o)
	// Whether the new VM wants our breakpoints our not,
	// it's going to inherit them.  This is pretty much
	// never what anyone wants, so a call to
	// |remove_all_breakpoints()| is expected soon after
	// the creation of this.
	: breakpoints(o.breakpoints)
	, exe(o.exe), heap(o.heap), is_clone(true)
	, mem(o.mem), vdso_start_addr(o.vdso_start_addr)
{
	for (auto it = breakpoints.begin(); it != breakpoints.end(); ++it) {
		it->second = it->second->clone();
	}
	sas.insert(this);
}

void
AddressSpace::coalesce_around(MemoryMap::iterator it)
{
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
		if (mem.end() == ++last_kv
		    || !is_adjacent_mapping(*prev, *last_kv)) {
			last_kv = prev;
			break;
		}
	}
	assert(last_kv != mem.end());
	if (first_kv == last_kv) {
		debug("  no mappings to coalesce");
		return;
	}

	Mapping c(first_kv->first.start, last_kv->first.end, m.prot, m.flags,
		  first_kv->first.offset);
	debug("  coalescing %p-%p", c.start, c.end);

	mem.erase(first_kv, ++last_kv);

	auto ins = mem.insert(MemoryMap::value_type(c, r));
	assert(ins.second);	// key didn't already exist
}

void
AddressSpace::destroy_breakpoint(BreakpointMap::const_iterator it)
{
	Task* t = *task_set().begin();
	t->write_mem(it->first, it->second->overwritten_data);
	breakpoints.erase(it);
}

void
AddressSpace::for_each_in_range(void* addr, ssize_t num_bytes,
				function<void (const Mapping& m,
					       const MappableResource& r,
					       const Mapping& rem)> f,
				int how)
{
	num_bytes = ceil_page_size(num_bytes);
	byte* last_unmapped_end = (byte*)addr;
	byte* region_end = (byte*)addr + num_bytes;
	while (last_unmapped_end < region_end) {
		// Invariant: |rem| is always exactly the region of
		// memory remaining to be examined for pages to be
		// unmapped.
		Mapping rem(last_unmapped_end, region_end);

		// The next page to iterate may not be contiguous with
		// the last one seen.
		auto it = mem.lower_bound(rem);
		if (mem.end() == it) {
			debug("  not found, done.");
			return;
		}

		Mapping m = it->first;
		if (rem.end <= m.start) {
			debug("  mapping at %p out of range, done.", m.start);
			return;
		}
		if (ITERATE_CONTIGUOUS == how &&
		    !(m.start < addr || rem.start == m.start)) {
			debug("  discontiguous mapping at %p, done.", m.start);
			return;
		}

		MappableResource r = it->second;
		f(m, r, rem);

		// Maintain the loop invariant.
		last_unmapped_end = (byte*)m.end;
	}
}

void
AddressSpace::map_and_coalesce(const Mapping& m, const MappableResource& r)
{
	debug("  mapping %p-%p (prot:0x%d flags:0x%x)",
	      m.start, m.end, m.prot, m.flags);

	auto ins = mem.insert(MemoryMap::value_type(m, r));
	assert(ins.second);	// key didn't already exist
	coalesce_around(ins.first);
}

/*static*/ int
AddressSpace::populate_address_space(void* asp, Task* t,
				     const struct map_iterator_data* data)
{
	AddressSpace* as = static_cast<AddressSpace*>(asp);
	const struct mapped_segment_info& info = data->info;

	if (!as->heap.start) {
		// We assume that the first mapped segment is the
		// program text segment.  It's possible, but not
		// probably, that the end of this segment is the
		// beginning of the dynamic heap segment.  We'll
		// determine that for sure in subsequent iterations.
		//
		// TODO: handle arbitrarily positioned text segments
		assert(info.prot & PROT_EXEC);
		assert(!as->exe.length());

		as->exe = info.name;

		as->update_heap(info.end_addr, info.end_addr);
		debug("  guessing heap starts at %p (end of text segment)",
		      as->heap.start);
	}

	bool is_dynamic_heap = !strcmp("[heap]", info.name);
	// This segment is adjacent to our previous guess at the start
	// of the dynamic heap, but it's still not an explicit heap
	// segment.  Update the guess.
	if (as->heap.end == info.start_addr) {
		assert(as->heap.start == as->heap.end);
		assert(!(info.prot & PROT_EXEC));
		as->update_heap(info.end_addr, info.end_addr);
		debug("  updating start-of-heap guess to %p (end of mapped-data segment)",
		      as->heap.start);
	}

	FileId id;
	if (is_dynamic_heap) {
		id.psdev = PSEUDODEVICE_HEAP;
		as->update_heap(as->heap.start, info.end_addr);
	} else if (!strcmp("[stack]", info.name)) {
		id.psdev = PSEUDODEVICE_STACK;
	} else if (!strcmp("[vdso]", info.name)) {
		assert(!as->vdso_start_addr);
		as->vdso_start_addr = info.start_addr;
		id.psdev = PSEUDODEVICE_VDSO;
	} else {
		id = FileId(MKDEV(info.dev_major, info.dev_minor), info.inode);
	}

	as->map(info.start_addr, (byte*)info.end_addr - (byte*)info.start_addr,
		info.prot, info.flags, info.file_offset,
		MappableResource(id, info.name));

	return CONTINUE_ITERATING;
}

/*static*/ ino_t MappableResource::nr_anonymous_maps;

/**
 * Stores the table of signal dispositions and metadata for an
 * arbitrary set of tasks.  Each of those tasks must own one one of
 * the |refcount|s while they still refer to this.
 */
struct Sighandler {
	Sighandler() : sa(), resethand() { }
	Sighandler(const struct kernel_sigaction& sa)
		: sa(sa), resethand(sa.sa_flags & SA_RESETHAND) { }

	bool ignored(int sig) const {
		return (SIG_IGN == sa.k_sa_handler ||
			(SIG_DFL == sa.k_sa_handler
			 && IGNORE == default_action(sig)));
	}
	bool is_default() const {
		return SIG_DFL == sa.k_sa_handler && !resethand;
	}
	bool is_user_handler() const {
		static_assert((void*)1 == SIG_IGN, "");
		return (uintptr_t)sa.k_sa_handler & ~(uintptr_t)SIG_IGN;
	}

	kernel_sigaction sa;
	bool resethand;
};
struct Sighandlers {
	typedef shared_ptr<Sighandlers> shr_ptr;

	shr_ptr clone() const {
		shr_ptr s(new Sighandlers());
		// NB: depends on the fact that Sighandler is for all
		// intents and purposes a POD type, though not
		// technically.
		memcpy(s->handlers, handlers, sizeof(handlers));
		return s;
	}

	Sighandler& get(int sig) {
		assert_valid(sig);
		return handlers[sig];
	}
	const Sighandler& get(int sig) const {
		assert_valid(sig);
		return handlers[sig];
	}

	void init_from_current_process() {
		for (int i = 0; i < ssize_t(ALEN(handlers)); ++i) {
			Sighandler& h = handlers[i];
			struct sigaction act;
			if (-1 == sigaction(i, NULL, &act)) {
				/* EINVAL means we're querying an
				 * unused signal number. */
				assert(EINVAL == errno);
				assert(h.is_default());
				continue;
			}
			struct kernel_sigaction ka;
			ka.k_sa_handler = act.sa_handler;
			ka.sa_flags = act.sa_flags;
			ka.sa_restorer = act.sa_restorer;
			ka.sa_mask = act.sa_mask;
			h = Sighandler(ka);
		}
	}

	/**
	 * For each signal in |table| such that is_user_handler() is
	 * true, reset the disposition of that signal to SIG_DFL, and
	 * clear the resethand flag if it's set.  SIG_IGN signals are
	 * not modified.
	 *
	 * (After an exec() call copies the original sighandler table,
	 * this is the operation required by POSIX to initialize that
	 * table copy.)
	 */
	void reset_user_handlers() {
		for (int i = 0; i < ssize_t(ALEN(handlers)); ++i) {
			Sighandler& h = handlers[i];
			// If the handler was a user handler, reset to
			// default.  If it was SIG_IGN or SIG_DFL,
			// leave it alone.
			if (h.is_user_handler()) {
				handlers[i] = Sighandler();
			}
		}
	}

	static void assert_valid(int sig) {
		assert(0 < sig && sig < ssize_t(ALEN(handlers)));
	}

	static shr_ptr create() {
		return shr_ptr(new Sighandlers());
	}

	Sighandler handlers[_NSIG];

private:
	Sighandlers() { }
	Sighandlers(const Sighandlers&);
	Sighandlers operator=(const Sighandlers&);
};

void
TaskGroup::destabilize()
{
	debug("destabilizing task group %d", tgid);
	for (auto it = task_set().begin(); it != task_set().end(); ++it) {
		Task* t = *it;
		t->unstable = 1;
		debug("  destabilized task %d", t->tid);
	}
}

/*static*/ TaskGroup::shr_ptr
TaskGroup::create(Task* t)
{
	shr_ptr tg(new TaskGroup(t->rec_tid, t->tid));
	tg->insert_task(t);
	return tg;
}

TaskGroup::TaskGroup(pid_t tgid, pid_t real_tgid)
	: tgid(tgid), real_tgid(real_tgid)
{
	debug("creating new task group %d (real tgid: %d)", tgid, real_tgid);
}

Task::Task(pid_t _tid, pid_t _rec_tid, int _priority)
	: thread_time(1)
	, switchable(), pseudo_blocked(), succ_event_counter(), unstable()
	, priority(_priority)
	, scratch_ptr(), scratch_size()
	, flushed_syscallbuf()
	, delay_syscallbuf_reset(), delay_syscallbuf_flush()
	  // These will be initialized when the syscall buffer is.
	, desched_fd(-1), desched_fd_child(-1)
	, seccomp_bpf_enabled()
	, child_sig(), stepped_into_syscall()
	, trace(), hpc()
	, tid(_tid), rec_tid(_rec_tid > 0 ? _rec_tid : _tid)
	, untraced_syscall_ip(), syscallbuf_lib_start(), syscallbuf_lib_end()
	, syscallbuf_hdr(), num_syscallbuf_bytes(), syscallbuf_child()
	, blocked_sigs()
	, child_mem_fd(open_mem_fd())
	, prname("???")
	, registers(), registers_known(false)
	, stashed_si(), stashed_wait_status()
	, tid_futex()
	, wait_status()
{
	if (RECORD != rr_flags()->option) {
		// This flag isn't meaningful outside recording.
		// Suppress output related to it outside recording.
		switchable = 1;
	}
	tasks_by_priority.insert(std::make_pair(priority, this));

	push_event(Event(EV_SENTINEL, NO_EXEC_INFO));

	init_hpc(this);

	tasks[rec_tid] = this;
}

Task::~Task()
{
	debug("task %d (rec:%d) is dying ...", tid, rec_tid);

	assert(this == Task::find(rec_tid));
	// We expect tasks to usually exit by a call to exit() or
	// exit_group(), so it's not helpful to warn about that.
	if (EV_SENTINEL != ev().type()
	    && (pending_events.size() > 2
		|| !(ev().type() == EV_SYSCALL
		     && (SYS_exit == ev().Syscall().no
			 || SYS_exit_group == ev().Syscall().no)))) {
		log_warn("%d still has pending events.  From top down:", tid);
		log_pending_events();
	}

	tasks.erase(rec_tid);
	tasks_by_priority.erase(std::make_pair(priority, this));
	tg->erase_task(this);
	as->erase_task(this);

	destroy_hpc(this);
	close(desched_fd);
	munmap(syscallbuf_hdr, num_syscallbuf_bytes);

	// We need the mem_fd in detach_and_reap().
	detach_and_reap();
	close(child_mem_fd);

	debug("  dead");
}

bool
Task::at_may_restart_syscall() const
{
	ssize_t depth = pending_events.size();
	const Event* prev_ev =
		depth > 2 ? &pending_events[depth - 2] : nullptr;
	return EV_SYSCALL_INTERRUPTION == ev().type()
		|| (EV_SIGNAL_DELIVERY == ev().type()
		    && prev_ev && EV_SYSCALL_INTERRUPTION == prev_ev->type());
}

Task*
Task::clone(int flags, void* stack, void* cleartid_addr,
	    pid_t new_tid, pid_t new_rec_tid)
{
	Task* t = new Task(new_tid, new_rec_tid, priority);

	t->syscallbuf_lib_start = syscallbuf_lib_start;
	t->syscallbuf_lib_end = syscallbuf_lib_end;
	t->blocked_sigs = blocked_sigs;
	if (CLONE_SHARE_SIGHANDLERS & flags) {
		t->sighandlers = sighandlers;
	} else {
		auto sh = Sighandlers::create();
		t->sighandlers.swap(sh);
	}
	if (CLONE_SHARE_TASK_GROUP & flags) {
		t->tg = tg;
		tg->insert_task(t);
	} else {
		auto g = TaskGroup::create(t);
		t->tg.swap(g);
	}
	if (CLONE_SHARE_VM & flags) {
		t->as = as;
	} else {
		t->as = as->clone();
	}
	if (stack) {
		const Mapping& m = 
			t->as->mapping_of((byte*)stack - page_size(),
					  page_size()).first;
		debug("mapping stack for %d at [%p, %p)",
		      new_tid, m.start, m.end);
		t->as->map(m.start, m.num_bytes(), m.prot, m.flags,
			   m.offset, MappableResource::stack(new_tid));
	}
	// Clone children, both thread and fork, inherit the parent
	// prname.
	t->prname = prname;
	if (CLONE_CLEARTID & flags) {
		debug("cleartid futex is %p", cleartid_addr);
		assert(cleartid_addr);
		t->tid_futex = cleartid_addr;
	} else {
		debug("(clone child not enabling CLEARTID)");
	}

	t->as->insert_task(t);
	return t;
}

const struct syscallbuf_record*
Task::desched_rec() const
{
	return (ev().is_syscall_event() ? ev().Syscall().desched_rec :
		(EV_DESCHED == ev().type()) ? ev().Desched().rec : nullptr);
}

/**
 * In recording, Task is notified of a destabilizing signal event
 * *after* it's been recorded, at the next trace-event-time.  In
 * replay though we're notified at the occurrence of the signal.  So
 * to display the same event time in logging across record/replay,
 * apply this offset.
 */
static int signal_delivery_event_offset()
{
	return RECORD == rr_flags()->option ? -1 : 0;
}

void
Task::destabilize_task_group()
{
	// Only print this helper warning if there's (probably) a
	// human around to see it.  This is done to avoid polluting
	// output from tests.
	if (EV_SIGNAL_DELIVERY == ev().type() && !probably_not_interactive()) {
		printf("[rr.%d] Warning: task %d (process %d) dying from fatal signal %s.\n",
		       trace_time() + signal_delivery_event_offset(),
		       rec_tid, tgid(), signalname(ev().Signal().no));
	}

	tg->destabilize();
}

void
Task::dump(FILE* out) const
{
	out = out ? out : LOG_FILE;
	fprintf(out, "  %s(tid:%d rec_tid:%d status:0x%x%s%s)<%p>\n",
		prname.c_str(), tid, rec_tid, wait_status,
		switchable ? "" : " UNSWITCHABLE",
		unstable ? " UNSTABLE" : "",
		this);
	if (RECORD == rr_flags()->option) {
		// TODO pending events are currently only meaningful
		// during recording.  We should change that
		// eventually, to have more informative output.
		log_pending_events();
	}
}

void
Task::set_priority(int value)
{
	if (priority == value) {
		// don't mess with task order
		return;
	}
	tasks_by_priority.erase(std::make_pair(priority, this));
	priority = value;
	tasks_by_priority.insert(std::make_pair(priority, this));
}

const Task::PrioritySet&
Task::get_priority_set()
{
	return tasks_by_priority;
}

bool
Task::fdstat(int fd, struct stat* st, char* buf, size_t buf_num_bytes)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path) - 1, "/proc/%d/fd/%d", tid, fd);
	ScopedOpen backing_fd(path, O_RDONLY);
	if (0 > backing_fd) {
		return false;
	}
	ssize_t nbytes = readlink(path, buf, buf_num_bytes);
	if (0 > nbytes) {
		return false;
	}
	buf[nbytes] = '\0';
	return 0 == fstat(backing_fd, st);
}

void
Task::futex_wait(void* futex, uint32_t val)
{
	static_assert(sizeof(val) == sizeof(long),
		      "Sorry, need to implement Task::read_int().");
	// Wait for *sync_addr == sync_val.  This implementation isn't
	// pretty, but it's pretty much the best we can do with
	// available kernel tools.
	//
	// TODO: find clever way to avoid busy-waiting.
	while (val != uint32_t(read_word(futex))) {
		// Try to give our scheduling slot to the kernel
		// thread that's going to write sync_addr.
		sched_yield();
	}
}

unsigned long
Task::get_ptrace_eventmsg()
{
	unsigned long msg;
	xptrace(PTRACE_GETEVENTMSG, nullptr, &msg);
	return msg;
}

void
Task::get_siginfo(siginfo_t* si)
{
	xptrace(PTRACE_GETSIGINFO, nullptr, si);
}

bool
Task::is_arm_desched_event_syscall()
{
	return (is_desched_event_syscall()
		&& PERF_EVENT_IOC_ENABLE == regs().ecx);
}

bool
Task::is_desched_event_syscall()
{
	return (SYS_ioctl == regs().orig_eax
		&& (desched_fd_child == regs().ebx
		    || desched_fd_child == REPLAY_DESCHED_EVENT_FD));
}

bool
Task::is_disarm_desched_event_syscall()
{
	return (is_desched_event_syscall()
		&& PERF_EVENT_IOC_DISABLE == regs().ecx);
}

bool
Task::is_ptrace_seccomp_event() const
{
	int event = ptrace_event();
	return (PTRACE_EVENT_SECCOMP_OBSOLETE == event ||
		PTRACE_EVENT_SECCOMP == event);
}

bool
Task::is_sig_blocked(int sig) const
{
	int sig_bit = sig - 1;
	return (blocked_sigs >> sig_bit) & 1;
}

bool
Task::is_sig_ignored(int sig) const
{
	return sighandlers->get(sig).ignored(sig);
}

bool
Task::is_syscall_restart()
{
	int syscallno = regs().orig_eax;
	bool must_restart = (SYS_restart_syscall == syscallno);
	bool is_restart = false;
	const struct user_regs_struct* old_regs;

	debug("  is syscall interruption of recorded %s? (now %s)",
	      syscallname(ev().Syscall().no), syscallname(syscallno));

	if (EV_SYSCALL_INTERRUPTION != ev().type()) {
		goto done;
	}

	old_regs = &ev().Syscall().regs;
	/* It's possible for the tracee to resume after a sighandler
	 * with a fresh syscall that happens to be the same as the one
	 * that was interrupted.  So we check here if the args are the
	 * same.
	 *
	 * Of course, it's possible (but less likely) for the tracee
	 * to incidentally resume with a fresh syscall that just
	 * happens to have the same *arguments* too.  But in that
	 * case, we would usually set up scratch buffers etc the same
	 * was as for the original interrupted syscall, so we just
	 * save a step here.
	 *
	 * TODO: it's possible for arg structures to be mutated
	 * between the original call and restarted call in such a way
	 * that it might change the scratch allocation decisions. */
	if (SYS_restart_syscall == syscallno) {
		must_restart = true;
		syscallno = ev().Syscall().no;
		debug("  (SYS_restart_syscall)");
	}
	if (ev().Syscall().no != syscallno) {
		debug("  interrupted %s != %s",
		      syscallname(ev().Syscall().no), syscallname(syscallno));
		goto done;
	}
	if (!(old_regs->ebx == regs().ebx
	      && old_regs->ecx == regs().ecx
	      && old_regs->edx == regs().edx
	      && old_regs->esi == regs().esi
	      && old_regs->edi == regs().edi
	      && old_regs->ebp == regs().ebp)) {
		debug("  regs different at interrupted %s",
		      syscallname(syscallno));
		goto done;
	}
	is_restart = true;

done:
	assert_exec(this, !must_restart || is_restart,
		    "Must restart %s but won't", syscallname(syscallno));
	if (is_restart) {
		debug("  restart of %s", syscallname(syscallno));
	}
	return is_restart;
}

void
Task::inited_syscallbuf()
{
	syscallbuf_hdr->locked = is_desched_sig_blocked();
}

void
Task::log_pending_events() const
{
	ssize_t depth = pending_events.size();

	assert(depth > 0);
	if (1 == depth) {
		log_info("(no pending events)");
		return;
	}

	/* The event at depth 0 is the placeholder event, which isn't
	 * useful to log.  Skip it. */
	for (auto it = pending_events.rbegin(); it != pending_events.rend();
	     ++it) {
		it->log();
	}
}

bool
Task::may_be_blocked() const
{
	return (EV_SYSCALL == ev().type()
		&& PROCESSING_SYSCALL == ev().Syscall().state)
		|| (EV_SIGNAL_DELIVERY == ev().type()
		    && ev().Signal().delivered);
}

void
Task::maybe_update_vm(int syscallno, int state)
{
	// We have to use the recorded_regs during replay because they
	// have the return value set in |eax|.  We may not have
	// advanced regs() to that point yet.
	const struct user_regs_struct& r = RECORD == rr_flags()->option ?
					   regs() : trace.recorded_regs;

	if (STATE_SYSCALL_EXIT != state
	    || (SYSCALL_FAILED(r.eax) && SYS_mprotect != syscallno)) {
		return;
	}
	switch (syscallno) {
	case SYS_brk: {
		void* addr = reinterpret_cast<void*>(r.ebx);
		if (!addr) {
			// A brk() update of NULL is observed with
			// libc, which apparently is its means of
			// finding out the initial brk().  We can
			// ignore that for the purposes of updating
			// our address space.
			return;
		}
		return vm()->brk(addr);
	}
	case SYS_mmap2: {
		debug("(mmap2 will receive / has received direct processing)");
		return;
	}
	case SYS_mprotect: {
		void* addr = reinterpret_cast<void*>(r.ebx);
		size_t num_bytes = r.ecx;
		int prot = r.edx;
		return vm()->protect(addr, num_bytes, prot);
	}
	case SYS_mremap: {
		if (SYSCALL_FAILED(r.eax) && -ENOMEM != r.eax) {
			return;
		}
		void* old_addr = reinterpret_cast<void*>(r.ebx);
		size_t old_num_bytes = r.ecx;
		void* new_addr = reinterpret_cast<void*>(r.eax);
		size_t new_num_bytes = r.edx;
		return vm()->remap(old_addr, old_num_bytes,
				   new_addr, new_num_bytes);
	}
	case SYS_munmap: {
		void* addr = reinterpret_cast<void*>(r.ebx);
		size_t num_bytes = r.ecx;
		return vm()->unmap(addr, num_bytes);
	}
	}
}

void
Task::move_ip_before_breakpoint()
{
	// TODO: assert that this is at a breakpoint trap.
	struct user_regs_struct r = regs();
	r.eip -= sizeof(AddressSpace::breakpoint_insn);
	set_regs(r);
}

static string prname_from_exe_image(const string& e)
{
	size_t last_slash = e.rfind('/');
	string basename =
		(last_slash != e.npos) ? e.substr(last_slash + 1) : e;
	return basename.substr(0, 15);
}

void
Task::post_exec()
{
	sighandlers = sighandlers->clone();
	sighandlers->reset_user_handlers();
	auto a = AddressSpace::create(this);
	as.swap(a);
	prname = prname_from_exe_image(as->exe_image());
}

void
Task::record_local(void* addr, ssize_t num_bytes, const void* buf)
{
	record_data(this, addr, num_bytes, buf);
}

// Max size we'll attempt to read into an inline buffer. Otherwise, we
// allocate a temporary heap buffer.
#define MAX_STACK_BUFFER_SIZE (1 << 17)

void
Task::record_remote(void* addr, ssize_t num_bytes)
{
	byte stack_buf[MAX_STACK_BUFFER_SIZE];
	byte* heap_buf = nullptr;
	byte* read_buf = nullptr;
	ssize_t read_bytes = 0;

	// We shouldn't be recording a scratch address.
	assert_exec(this, !addr || addr != scratch_ptr, "");

 	if (addr && num_bytes > 0) {
		if (num_bytes > ssize_t(sizeof(stack_buf))) {
			heap_buf = (byte*)malloc(num_bytes);
		}
		read_buf = heap_buf ? heap_buf : stack_buf;

		read_bytes_helper(addr, num_bytes, read_buf);
		read_bytes = num_bytes;
	}

	record_data(this, addr, read_bytes, read_buf);
	free(heap_buf);
}

void
Task::record_remote_str(void* str)
{
	string s = read_c_str(str);
	record_data(this, str, s.size(), s.c_str());
}

string
Task::read_c_str(void* child_addr)
{
	// XXX handle invalid C strings
	string str;
	while (true) {
		// We're only guaranteed that [child_addr,
		// end_of_page) is mapped.
		void* end_of_page = ceil_page_size((byte*)child_addr + 1);
		ssize_t nbytes = (byte*)end_of_page - (byte*)child_addr;
		char buf[nbytes];

		read_bytes_helper(child_addr, nbytes,
				  reinterpret_cast<byte*>(buf));
		for (int i = 0; i < nbytes; ++i) {
			if ('\0' == buf[i]) {
				return str;
			}
			str += buf[i];
		}
		child_addr = end_of_page;
	}
}

long
Task::read_word(void* child_addr)
{
	long word;
	read_mem(child_addr, &word);
	return word;
}

const struct user_regs_struct&
Task::regs()
{
	if (!registers_known) {
		debug("  (refreshing register cache)");
		xptrace(PTRACE_GETREGS, nullptr, &registers);
		registers_known = true;
	}
	return registers;
}

void
Task::remote_memcpy(void* dst, const void* src, size_t num_bytes)
{
	// XXX this could be more efficient
	byte buf[num_bytes];
	read_bytes_helper((void*)src, num_bytes, buf);
	write_bytes_helper(dst, num_bytes, buf);
}

bool
Task::resume_execution(ResumeRequest how, WaitRequest wait_how, int sig)
{
	debug("resuming execution with %s", ptrace_req_name(how));
	xptrace(how, nullptr, (void*)(uintptr_t)sig);
	registers_known = false;
	if (RESUME_NONBLOCKING == wait_how) {
		return true;
	}
	return wait();
}

ssize_t
Task::set_data_from_trace()
{
	size_t size;
	void* rec_addr;
	byte* data = (byte*)read_raw_data(&trace, &size, &rec_addr);
	if (data && size > 0) {
		write_bytes_helper(rec_addr, size, data);
		free(data);
	}
	return size;
}

void
Task::set_return_value_from_trace()
{
	struct user_regs_struct r = regs();
	r.eax = trace.recorded_regs.eax;
	set_regs(r);
}

void
Task::set_regs(const struct user_regs_struct& regs)
{
	registers = regs;
	xptrace(PTRACE_SETREGS, nullptr, (void*)&registers);
	registers_known = true;
}

void
Task::set_tid_addr(void* tid_addr)
{
	debug("updating cleartid futex to %p", tid_addr);
	tid_futex = tid_addr;
}

void
Task::signal_delivered(int sig)
{
	Sighandler& h = sighandlers->get(sig);
	if (h.resethand) {
		h = Sighandler();
	}
}

sig_handler_t
Task::signal_disposition(int sig) const
{
	return sighandlers->get(sig).sa.k_sa_handler;
}

bool
Task::signal_has_user_handler(int sig) const
{
	return sighandlers->get(sig).is_user_handler();
}

const kernel_sigaction&
Task::signal_action(int sig) const
{
	return sighandlers->get(sig).sa;
}

void
Task::stash_sig()
{
	assert(pending_sig());
	assert_exec(this, !has_stashed_sig(),
		    "Tried to stash %s when %s was already stashed.",
		    signalname(pending_sig()), signalname(stashed_si.si_signo));
	stashed_wait_status = wait_status;
	get_siginfo(&stashed_si);
}

const siginfo_t&
Task::pop_stash_sig()
{
	assert(has_stashed_sig());
	force_status(stashed_wait_status);
	stashed_wait_status = 0;
	return stashed_si;
}

uint32_t
Task::trace_time() const
{
	return get_global_time();
}

void
Task::update_prname(void* child_addr)
{
	struct { char chars[16]; } name;
	read_mem(child_addr, &name);
	name.chars[sizeof(name.chars) - 1] = '\0';
	prname = name.chars;
}

void
Task::update_sigaction()
{
	int sig = regs().ebx;
	void* new_sigaction = (void*)regs().ecx;
	if (0 == regs().eax && new_sigaction) {
		// A new sighandler was installed.  Update our
		// sighandler table.
		// TODO: discard attempts to handle or ignore signals
		// that can't be by POSIX
		struct kernel_sigaction sa;
		read_mem(new_sigaction, &sa);
		sighandlers->get(sig) = Sighandler(sa);
	}
}

void
Task::update_sigmask()
{
	int how = regs().ebx;
	void* setp = (void*)regs().ecx;

	if (SYSCALL_FAILED(regs().eax) || !setp) {
		return;
	}

	assert_exec(this, (!syscallbuf_hdr || !syscallbuf_hdr->locked
			   || is_desched_sig_blocked()),
		    "syscallbuf is locked but SIGSYS isn't blocked");

	sig_set_t set;
	read_mem(setp, &set);

	// Update the blocked signals per |how|.
	switch (how) {
	case SIG_BLOCK:
		blocked_sigs |= set;
		break;
	case SIG_UNBLOCK:
		blocked_sigs &= ~set;
		break;
	case SIG_SETMASK:
		blocked_sigs = set;
		break;
	default:
		fatal("Unknown sigmask manipulator %d", how);
	}

	// In the syscallbuf, we rely on SIGSYS being raised when
	// tracees are descheduled in blocked syscalls.  But
	// unfortunately, if tracees block SIGSYS, then we don't get
	// notification of the pending signal and deadlock.  If we did
	// get those notifications, this code would be unnecessary.
	//
	// So we lock the syscallbuf while the desched signal is
	// blocked, which prevents the tracee from attempting a
	// buffered call.
	if (syscallbuf_hdr) {
		syscallbuf_hdr->locked = is_desched_sig_blocked();
	}
}

// The Task currently being wait()d on, or nullptr.
// |waiter_was_interrupted| if a PTRACE_INTERRUPT had to be applied to
// |waiter| to get it to stop.
static Task* waiter;
static bool waiter_was_interrupted;

bool
Task::wait()
{
	debug("going into blocking waitpid(%d) ...", tid);

	// We only need this during recording.  If tracees go runaway
	// during replay, something else is at fault.
	bool enable_wait_interrupt = (RECORD == rr_flags()->option);
	if (enable_wait_interrupt) {
		waiter = this;
		// Where does the 3 seconds come from?  No especially
		// good reason.  We want this to be pretty high,
		// because it's a last-ditch recovery mechanism, not a
		// primary thread scheduler.  Though in theory the
		// PTRACE_INTERRUPT's shouldn't interfere with other
		// events, that's hard to test thoroughly so try to
		// avoid it.
		alarm(3);

		// Set the wait_status to a sentinel value so that we
		// can hopefully recognize race conditions in the
		// SIGALRM handler.
		wait_status = -1;
	}
	pid_t ret = waitpid(tid, &wait_status, __WALL);
	if (enable_wait_interrupt) {
		waiter = nullptr;
		alarm(0);
	}

	if (0 > ret && EINTR == errno) {
		debug("  waitpid(%d) interrupted!", tid);
		return false;
	}
	debug("  waitpid(%d) returns %d; status %#x", tid, ret, wait_status);
	assert_exec(this, tid == ret, "waitpid(%d) failed with %d",
		    tid, ret);
	// If some other ptrace-stop happened to race with our
	// PTRACE_INTERRUPT, then let the other event win.  We only
	// want to interrupt tracees stuck running in userspace.
	if (waiter_was_interrupted && PTRACE_EVENT_STOP == ptrace_event()
	    && (SIGTRAP == WSTOPSIG(wait_status)
		// We sometimes see SIGSTOP at interrupts, though the
		// docs don't mention that.
		|| SIGSTOP == WSTOPSIG(wait_status))) {
		log_warn("Forced to PTRACE_INTERRUPT tracee");
		stashed_wait_status = wait_status =
				      (HPC_TIME_SLICE_SIGNAL << 8) | 0x7f;
		memset(&stashed_si, 0, sizeof(stashed_si));
		stashed_si.si_signo = HPC_TIME_SLICE_SIGNAL;
		stashed_si.si_fd = hpc->rbc.fd;
		stashed_si.si_code = POLL_IN;
		// Starve the runaway task of CPU time.  It just got
		// the equivalent of hundreds of time slices.
		succ_event_counter = numeric_limits<int>::max() / 2;
	} else if (waiter_was_interrupted) {
		debug("  PTRACE_INTERRUPT raced with another event %#x",
		      wait_status);
	}
	waiter_was_interrupted = false;
	return true;
}

bool
Task::try_wait()
{
	pid_t ret = waitpid(tid, &wait_status, WNOHANG | __WALL | WSTOPPED);
	debug("waitpid(%d, NOHANG) returns %d, status %#x",
	      tid, ret, wait_status);
	assert_exec(this, 0 <= ret, "waitpid(%d, NOHANG) failed with %d",
		    tid, ret);
	return ret == tid;
}

/*static*/ Task::Map::const_iterator
Task::begin()
{
	return tasks.begin();
}

/*static*/ ssize_t
Task::count()
{
	return tasks.size();
}

/**
 * Prepare this process and its ancestors for recording/replay by
 * preventing direct access to sources of nondeterminism, and ensuring
 * that rr bugs don't adversely affect the underlying system.
 */
static void set_up_process(void)
{
	int orig_pers;

	/* TODO tracees can probably undo some of the setup below
	 * ... */

	/* Disable address space layout randomization, for obvious
	 * reasons, and ensure that the layout is otherwise well-known
	 * ("COMPAT").  For not-understood reasons, "COMPAT" layouts
	 * have been observed in certain recording situations but not
	 * in replay, which causes divergence. */
	if (0 > (orig_pers = personality(0xffffffff))) {
		fatal("error getting personaity");
	}
	if (0 > personality(orig_pers | ADDR_NO_RANDOMIZE |
			    ADDR_COMPAT_LAYOUT)) {
		fatal("error disabling randomization");
	}
	/* Trap to the rr process if a 'rdtsc' instruction is issued.
	 * That allows rr to record the tsc and replay it
	 * deterministically. */
	if (0 > prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0)) {
		fatal("error setting up prctl -- bailing out");
	}
	// If the rr process dies, prevent runaway tracee processes
	// from dragging down the underlying system.
	//
	// TODO: this isn't inherited across fork().
	if (0 > prctl(PR_SET_PDEATHSIG, SIGKILL)) {
		fatal("Couldn't set parent-death signal");
	}
}

/*static*/ Task*
Task::create(const std::string& exe, CharpVector& argv, CharpVector& envp,
	     pid_t rec_tid)
{
	assert(Task::count() == 0);

	pid_t tid = fork();
	if (0 == tid) {
		set_up_process();
		// Signal to tracer that we're configured.
		kill(getpid(), SIGSTOP);

		// We do a small amount of dummy work here to retire
		// some branches in order to ensure that the rbc is
		// non-zero.  The tracer can then check the rbc value
		// at the first ptrace-trap to see if it seems to be
		// working.
		int start = rand() % 5;
		int num_its = start + 5;
		int sum = 0;
		for (int i = start; i < num_its; ++i) {
			sum += i;
		}
		syscall(SYS_write, -1, &sum, sizeof(sum));

		execvpe(exe.c_str(), argv.data(), envp.data());
		fatal("Failed to exec %s", exe.c_str());
	}

	signal(SIGALRM, Task::handle_runaway);

	Task* t = new Task(tid, rec_tid, 0);
	// The very first task we fork inherits the signal
	// dispositions of the current OS process (which should all be
	// default at this point, but ...).  From there on, new tasks
	// will transitively inherit from this first task.
	auto sh = Sighandlers::create();
	sh->init_from_current_process();
	t->sighandlers.swap(sh);
	// Don't use the POSIX wrapper, because it doesn't necessarily
	// read the entire sigset tracked by the kernel.
	if (::syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL,
		      &t->blocked_sigs, sizeof(t->blocked_sigs))) {
		fatal("Failed to read blocked signals");
	}
	auto g = TaskGroup::create(t);
	t->tg.swap(g);
	auto as = AddressSpace::create(t);
	t->as.swap(as);

	// Sync with the child process.
	t->xptrace(PTRACE_SEIZE, nullptr,
		   (void*)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK |
			   PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
			   PTRACE_O_TRACEEXEC | PTRACE_O_TRACEVFORKDONE |
			   PTRACE_O_TRACEEXIT | PTRACE_O_TRACESECCOMP));
	// PTRACE_SEIZE is fundamentally racy by design.  We depend on
	// stopping the tracee at a known location, so raciness is
	// bad.  To resolve the race condition, we just keep running
	// the tracee until it reaches the known-safe starting point.
	//
	// Alternatively, it would be possible to remove the
	// requirement of the tracing beginning from a known point.
	while (true) {
		t->wait();
		if (SIGSTOP == t->stop_sig()) {
			break;
		}
		t->cont_nonblocking();
	}
	t->force_status(0);
	return t;
}

/*static*/ void
Task::dump_all(FILE* out)
{
	out = out ? out : LOG_FILE;

	auto sas = AddressSpace::set();
	for (auto ait = sas.begin(); ait != sas.end(); ++ait) {
		const AddressSpace* as = *ait;
		auto ts = as->task_set();
		Task* t = *ts.begin();
		// XXX assuming that address space == task group,
		// which is almost certainly what the kernel enforces
		// too.
		fprintf(out, "\nTask group %d, image '%s':\n",
			t->tgid(), as->exe_image().c_str());
		for (auto tsit = ts.begin(); tsit != ts.end(); ++tsit) {
			(*tsit)->dump(out);
		}
	}
}

/*static*/ Task::Map::const_iterator
Task::end()
{
	return tasks.end();
}

/*static*/Task*
Task::find(pid_t rec_tid)
{
	auto it = tasks.find(rec_tid);
	return tasks.end() != it ? it->second : NULL;
}

/** Send |sig| to task |tid| within group |tgid|. */
static int sys_tgkill(pid_t tgid, pid_t tid, int sig)
{
	return syscall(SYS_tgkill, tgid, tid, SIGKILL);
}

/*static*/ void
Task::killall()
{
	while (!tasks.empty()) {
		auto it = tasks.rbegin();
		Task* t = it->second;

		debug("sending SIGKILL to %d ...", t->tid);
		sys_tgkill(t->real_tgid(), t->tid, SIGKILL);

		t->wait();
		debug("  ... status %#x", t->status());

		int status = t->status();
		if (WIFSIGNALED(status)) {
			assert(SIGKILL == WTERMSIG(status));
			// The task is already dead and reaped, so
			// skip any waitpid()'ing during cleanup.
			t->unstable = 1;
		} else {
			// If the task participated in an unstable
			// exit, it's probably already dead by now.
			assert(t->unstable
			       || PTRACE_EVENT_EXIT == t->ptrace_event());
		}
		// Don't attempt to synchonize on the cleartid futex.
		// We won't be able to reliably read it, and it's
		// pointless anyway.
		t->tid_futex = nullptr;
		delete t;
	}
}

/*static*/int
Task::pending_sig_from_status(int status)
{
	if (status == 0) {
		return 0;
	}
	int sig = stop_sig_from_status(status);
	switch (sig) {
	case (SIGTRAP | 0x80):
		/* We ask for PTRACE_O_TRACESYSGOOD, so this was a
		 * trap for a syscall.  Pretend like it wasn't a
		 * signal. */
		return 0;
	case SIGTRAP:
		/* For a "normal" SIGTRAP, it's a ptrace trap if
		 * there's a ptrace event.  If so, pretend like we
		 * didn't get a signal.  Otherwise it was a genuine
		 * TRAP signal raised by something else (most likely a
		 * debugger breakpoint). */
		return ptrace_event_from_status(status) ? 0 : SIGTRAP;
	default:
		/* XXX do we really get the high bit set on some
		 * SEGVs? */
		return sig & ~0x80;
	}
}

void
Task::detach_and_reap()
{
	if (tid_futex) {
		static_assert(sizeof(int32_t) == sizeof(long),
			      "Sorry, need to add Task::read_int()");
		// This read also ensures that child_mem_fd is valid
		// before the tracee exits.  Otherwise we might not be
		// open the fd below.  See TODO comment in Task ctor.
		int32_t tid_addr_val = read_word(tid_futex);
		assert_exec(this, rec_tid == tid_addr_val,
			    "tid addr should be %d (tid), but is %d",
			    rec_tid, tid_addr_val);
	}

	// XXX: why do we detach before harvesting?
	fallible_ptrace(PTRACE_DETACH, nullptr, nullptr);
	if (unstable) {
		// In addition to problems described in the long
		// comment at the prototype of this function, unstable
		// exits may result in the kernel *not* clearing the
		// futex, for example for fatal signals.  So we would
		// deadlock waiting on the futex.
		log_warn("%d is unstable; not blocking on its termination",
			 tid);
		return;
	}

	debug("Joining with exiting %d ...", tid);
	while (true) {
		int err = waitpid(tid, &wait_status, __WALL);
		if (-1 == err && ECHILD == errno) {
			debug(" ... ECHILD");
			break;
		} else if (-1 == err) {
			assert(EINTR == errno);
		}
		if (err == tid && (exited() || signaled())) {
			debug(" ... exited with status 0x%x", wait_status);
			break;
		} else if (err == tid) {
			assert(PTRACE_EVENT_EXIT == ptrace_event());
		}
	}

	if (tid_futex && as->task_set().size() > 0) {
		// clone()'d tasks can have a pid_t* |ctid| argument
		// that's written with the new task's pid.  That
		// pointer can also be used as a futex: when the task
		// dies, the original ctid value is cleared and a
		// FUTEX_WAKE is done on the address. So
		// pthread_join() is basically a standard futex wait
		// loop.
		debug("  waiting for tid futex %p to be cleared ...",
		      tid_futex);
		futex_wait(tid_futex, 0);
	} else if(tid_futex) {
		// There are no other live tasks in this address
		// space, which means the address space just died
		// along with our exit.  So we can't read the futex.
		debug("  (can't futex_wait last task in vm)");
	}
}

long
Task::fallible_ptrace(int request, void* addr, void* data)
{
	return ptrace(__ptrace_request(request), tid, addr, data);
}

int
Task::open_mem_fd()
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path) - 1, "/proc/%d/mem", tid);
	int fd =open(path, O_RDWR);
	assert_exec(this, fd >= 0, "Failed to open %s", path);
	return fd;
}

void
Task::reopen_mem_fd()
{
	close(child_mem_fd);
	child_mem_fd = open_mem_fd();
}

bool
Task::is_desched_sig_blocked()
{
	return is_sig_blocked(SYSCALLBUF_DESCHED_SIGNAL);
}

static off64_t to_offset(void* addr)
{
	off64_t offset = (uintptr_t)addr;
	assert(offset <= off64_t(numeric_limits<unsigned long>::max()));
	return offset;
}

ssize_t
Task::read_bytes_fallible(void* addr, ssize_t buf_size, byte* buf)
{
	assert_exec(this, buf_size >= 0, "Invalid buf_size %d", buf_size);
	if (0 == buf_size) {
		return 0;
	}
	errno = 0;
	ssize_t nread = pread64(child_mem_fd, buf, buf_size, to_offset(addr));
	// We open the child_mem_fd just after being notified of
	// exec(), when the Task is created.  Trying to read from that
	// fd seems to return 0 with errno 0.  Reopening the mem fd
	// allows the pwrite to succeed.  It seems that the first mem
	// fd we open, very early in exec, refers to some resource
	// that's different than the one we see after reopening the
	// fd, after exec.
	if (0 == nread && 0 == errno) {
		reopen_mem_fd();
		return read_bytes_fallible(addr, buf_size, buf);
	}
	return nread;
}

void
Task::read_bytes_helper(void* addr, ssize_t buf_size, byte* buf)
{
	ssize_t nread = read_bytes_fallible(addr, buf_size, buf);
	assert_exec(this, nread == buf_size,
		    "Should have read %d bytes from %p, but only read %d",
		    buf_size, addr, nread);
}

void
Task::write_bytes_helper(void* addr, ssize_t buf_size, const byte* buf)
{
	assert_exec(this, buf_size >= 0, "Invalid buf_size %d", buf_size);
	if (0 == buf_size) {
		return;
	}
	errno = 0;
	ssize_t nwritten = pwrite64(child_mem_fd, buf, buf_size,
				    to_offset(addr));
	// See comment in read_bytes_helper().
	if (0 == nwritten && 0 == errno) {
		reopen_mem_fd();
		return write_bytes_helper(addr, buf_size, buf);
	}
	assert_exec(this, nwritten == buf_size,
		    "Should have written %d bytes to %p, but only wrote %d",
		    buf_size, addr, nwritten);
}

void
Task::xptrace(int request, void* addr, void* data)
{
	long ret = fallible_ptrace(request, addr, data);
	assert_exec(this, 0 == ret,
		    "ptrace(%s, %d, addr=%p, data=%p) failed",
		    ptrace_req_name(request), tid, addr, data);
}

/*static*/ void
Task::handle_runaway(int sig)
{
	debug("SIGALRM fired; runaway tracee");
	if (!waiter || -1 != waiter->wait_status) {
		debug("  ... false alarm, race condition");
		return;
	}
	waiter->xptrace(PTRACE_INTERRUPT, nullptr, nullptr);
	waiter_was_interrupted = true;
}
