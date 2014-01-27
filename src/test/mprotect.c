/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "rrutil.h"

int main(int argc, char *argv[]) {
	size_t page_size = sysconf(_SC_PAGESIZE);
	byte* map1 = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
			 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	byte* map1_end = map1 + 2 * page_size;
	byte* map2;
	byte* map2_end;

	test_assert(map1 != (void*)-1);

	atomic_printf("map1 = [%p, %p)\n", map1, map1_end);

	mprotect(map1 + page_size, page_size, PROT_NONE);

	map2 = mmap(map1_end, 2 * page_size, PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	map2_end = map2 + page_size;
	test_assert(map2 != (void*)-1);
	test_assert(map2 == map1_end);

	atomic_printf("map2 = [%p, %p)\n", map2, map2_end);

	mprotect(map2, page_size, PROT_NONE);

	atomic_puts(" done");

	return 0;
}
