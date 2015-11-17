/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "rrutil.h"

#define FILENAME "foo.txt"

int main(void) {
  int fd;

  sync();

  fd = open(FILENAME, O_CREAT | O_RDWR, 0600);
  test_assert(fd >= 0);
  test_assert(0 == syncfs(fd));
  unlink(FILENAME);


  atomic_puts("EXIT-SUCCESS");
  return 0;
}
