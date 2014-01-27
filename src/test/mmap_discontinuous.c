/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "rrutil.h"

static int create_segment(size_t num_bytes) {
	char filename[] = "/dev/shm/rr-test-XXXXXX";
	int fd = mkstemp(filename);
	unlink(filename);
	test_assert(fd >= 0);
	ftruncate(fd, num_bytes);
	return fd;
}

int main(int argc, char *argv[]) {
	size_t page_size = sysconf(_SC_PAGESIZE);
	int fd = create_segment(3 * page_size);

	byte* wpage1 = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, fd,
			    0);
	byte* wpage2 = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, fd,
			    2 * page_size);

	test_assert(wpage1 != (void*)-1 && wpage2 != (void*)-1);
	test_assert(wpage1 != wpage2);
	test_assert(wpage2 - wpage1 == page_size
		    || wpage1 - wpage2 == page_size);

	wpage1 = mmap(NULL, page_size, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		      -1, 0);
	wpage2 = mmap(NULL, page_size, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		      -1, 2 * page_size);

	test_assert(wpage1 != (void*)-1 && wpage2 != (void*)-1);
	test_assert(wpage1 != wpage2);
	test_assert(wpage2 - wpage1 == page_size
		    || wpage1 - wpage2 == page_size);

#if 0
	{
		char cmd[4096];
		snprintf(cmd, sizeof(cmd) - 1, "cat /proc/%d/maps", getpid());
		system(cmd);
	}
#endif

	atomic_puts(" done");

	return 0;
}
