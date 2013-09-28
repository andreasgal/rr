/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "rrutil.h"

int main(int argc, char *argv[]) {
	int navail;
	int ret;

	ret = ioctl(STDIN_FILENO, TIOCINQ, &navail);
	atomic_printf("TIOCINQ returned navail=%d (ret:%d)\n", navail, ret);

	atomic_puts("EXIT-SUCCESS");
	return 0;
}
