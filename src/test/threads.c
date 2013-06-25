/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>

long int counter = 0;

void catcher(int sig) {
	printf("Signal caught, Counter is %ld\n", counter);
	puts("EXIT-SUCCESS");
	fflush(stdout);
	_exit(0);
}

void* reciever(void* name) {
	struct sigaction sact;

	sigemptyset(&sact.sa_mask);
	sact.sa_flags = 0;
	sact.sa_handler = catcher;
	sigaction(SIGALRM, &sact, NULL);

	while (1) {
		counter++;
		if (counter % 100000 == 0) {
			write(1, ".", 1);
		}
	}
	return NULL;
}

void* sender(void* id) {
	sleep(1);
	pthread_kill(*((pthread_t*)id), SIGALRM);
	return NULL;
}

int main() {
	pthread_t thread1, thread2;

	/* Create independent threads each of which will execute
	 * function */
	pthread_create(&thread1, NULL, reciever, NULL);
	pthread_create(&thread2, NULL, sender, &thread1);

	/* Wait till threads are complete before main
	 * continues. Unless we wait we run the risk of executing an
	 * exit which will terminate the process and all threads
	 * before the threads have completed. */
	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);
	return 0;
}


