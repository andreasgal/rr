/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "rrutil.h"

int main(int argc, char *argv[]) {
	gid_t groups[1024];
	int num_groups = getgroups(ALEN(groups), groups);
	int i;

	atomic_printf("User %d belongs to %d groups:\n  ",
		      geteuid(), num_groups);
	for (i = 0; i < num_groups; ++i) {
		atomic_printf("%d,", groups[i]);
	}
	atomic_puts("");

	atomic_puts("EXIT-SUCCESS");
	return 0;
}
