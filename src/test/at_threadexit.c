/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "rrutil.h"

static pthread_key_t exit_key;

static void thread_exit(void* data) {
	atomic_puts("thread exit");
}

static void* thread(void* unused) {
	pthread_key_create(&exit_key, thread_exit);
	pthread_setspecific(exit_key, (void*)0x1);
	pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
	pthread_t t;

	pthread_create(&t, NULL, thread, NULL);
	pthread_join(t, NULL);

	atomic_puts("EXIT-SUCCESS");
	return 0;
}
