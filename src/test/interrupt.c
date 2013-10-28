/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "rrutil.h"

void spin(void) {
	int i;

	atomic_puts("spinning");
	for (i = 1; i < (1 << 30); ++i) {
		if (0 == i % (1 << 20)) {
			write(STDOUT_FILENO, ".", 1);
		}
		if (0 == i % (79 * (1 << 20))) {
			write(STDOUT_FILENO, "\n", 1);
		}
	}
}

int main(int argc, char *argv[]) {

	spin();
	atomic_puts("done");
	return 0;
}
