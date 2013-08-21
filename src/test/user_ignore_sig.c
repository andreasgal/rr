/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define test_assert(cond)  assert("FAILED if not: " && (cond))

static void handle_usr1(int sig) {
	test_assert("Shouldn't have caught SIGUSR1" && 0);
}

int main(int argc, char *argv[]) {
	/* NB: unlike most other rr tests, this one verifies that rr
	 * can "intervene" in execution to block signals, for the
	 * purposes of unit tests.  This test *will* fail if not run
	 * under rr with the right command-line options. */
	signal(SIGUSR1, handle_usr1);
	raise(SIGUSR1);

	puts("EXIT-SUCCESS");
	return 0;
}
